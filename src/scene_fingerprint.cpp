#include "scene_fingerprint.h"

#include <SQLiteCpp/SQLiteCpp.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "entity.h"

namespace fs = std::filesystem;

namespace MultiPlayer {
namespace {

constexpr std::string_view kFingerprintDomain =
  "hackmatrix-scene-fingerprint-v6";

// Two independent 64-bit streaming hashes are enough to detect accidental
// scene divergence (a single changed byte flips both). This is deliberately
// not a security primitive: a malicious peer can fake its packet anyway.
class Hasher
{
public:
  void update(const void* data, std::size_t size)
  {
    const auto* bytes = static_cast<const uint8_t*>(data);
    for (std::size_t i = 0; i < size; ++i) {
      const uint8_t byte = bytes[i];
      hi_ ^= byte;
      hi_ *= 1099511628211ULL;
      lo_ ^= static_cast<uint64_t>(byte) + 0x9e3779b97f4a7c15ULL;
      lo_ *= 14029467366897019727ULL;
      lo_ ^= lo_ >> 29;
    }
  }

  void updateString(std::string_view text)
  {
    // Length prefix avoids concatenation ambiguities ("ab" + "c" vs "a" + "bc").
    std::array<uint8_t, 8> length{};
    uint64_t n = text.size();
    for (int i = 0; i < 8; ++i) {
      length[i] = static_cast<uint8_t>((n >> (8 * i)) & 0xff);
    }
    update(length.data(), length.size());
    update(text.data(), text.size());
  }

  SceneFingerprint finish() const
  {
    SceneFingerprint fp;
    fp.hi = hi_;
    fp.lo = lo_;
    fp.valid = true;
    return fp;
  }

private:
  uint64_t hi_ = 14695981039346656037ULL;
  uint64_t lo_ = 0x6a09e667f3bcc909ULL;
};

bool
hashFile(Hasher& hasher, const fs::path& path)
{
  std::error_code ec;
  if (!fs::is_regular_file(path, ec)) {
    hasher.updateString("missing");
    return false;
  }

  std::ifstream input(path, std::ios::binary);
  if (!input) {
    hasher.updateString("unreadable");
    return false;
  }

  const uint64_t size = static_cast<uint64_t>(fs::file_size(path, ec));
  std::array<uint8_t, 8> sizeBytes{};
  for (int i = 0; i < 8; ++i) {
    sizeBytes[i] = static_cast<uint8_t>((size >> (8 * i)) & 0xff);
  }
  hasher.updateString("file");
  hasher.update(sizeBytes.data(), sizeBytes.size());

  std::array<char, 64 * 1024> buffer{};
  while (input) {
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const auto count = input.gcount();
    if (count > 0) {
      hasher.update(buffer.data(), static_cast<std::size_t>(count));
    }
  }
  hasher.updateString("eof");
  return true;
}

std::string
canonicalKey(std::string_view kind, const fs::path& path)
{
  std::error_code ec;
  fs::path absolute = fs::weakly_canonical(path, ec);
  if (ec) {
    absolute = fs::absolute(path, ec);
  }
  if (ec) {
    absolute = path;
  }
  return std::string(kind) + ":" + absolute.lexically_normal().string();
}

void
hashTexture(Hasher& hasher,
            const fs::path& texture,
            std::unordered_set<std::string>& seen)
{
  const auto key = canonicalKey("tex", texture);
  if (!seen.insert(key).second) {
    return;
  }
  hasher.updateString("texture");
  hashFile(hasher, texture);
}

void
hashMaterial(Hasher& hasher,
             const fs::path& material,
             std::unordered_set<std::string>& seen)
{
  const auto key = canonicalKey("mtl", material);
  if (!seen.insert(key).second) {
    return;
  }

  hasher.updateString("material");
  if (!hashFile(hasher, material)) {
    return;
  }

  std::ifstream input(material);
  std::string line;
  while (std::getline(input, line)) {
    std::istringstream fields(line);
    std::string directive;
    std::string file;
    if (!(fields >> directive >> file)) {
      continue;
    }
    if (directive == "map_Kd" || directive == "map_Ka" ||
        directive == "map_Ks" || directive == "map_Bump" ||
        directive == "map_bump" || directive == "bump" ||
        directive == "disp" || directive == "decal") {
      hashTexture(hasher, material.parent_path() / file, seen);
    }
  }
}

void
hashModel(Hasher& hasher, const fs::path& model,
          std::unordered_set<std::string>& seen)
{
  const auto key = canonicalKey("model", model);
  if (!seen.insert(key).second) {
    return;
  }

  hasher.updateString("model");
  const bool exists = hashFile(hasher, model);
  if (!exists) {
    return;
  }

  // OBJ files usually name their material library explicitly.
  if (model.extension() == ".obj" || model.extension() == ".OBJ") {
    std::ifstream input(model);
    std::string line;
    while (std::getline(input, line)) {
      std::istringstream fields(line);
      std::string directive;
      std::string file;
      if ((fields >> directive >> file) && directive == "mtllib") {
        hashMaterial(hasher, model.parent_path() / file, seen);
      }
    }
  }

  // MagicaVoxel exports keep the material/texture next to the model even when
  // the OBJ does not reference it. Include those when present.
  fs::path stem = model;
  stem.replace_extension();
  hashMaterial(hasher, fs::path(stem.string() + ".mtl"), seen);
  hashTexture(hasher, fs::path(stem.string() + ".png"), seen);
  hashTexture(hasher, fs::path(stem.string() + ".jpg"), seen);
}

std::string
normalizeAssetPath(const std::string& text, const fs::path& projectRoot)
{
  if (text.empty()) {
    return {};
  }
  std::error_code ec;
  fs::path path(text);
  fs::path absolute = fs::weakly_canonical(path, ec);
  if (ec) {
    absolute = fs::absolute(path, ec);
  }
  if (ec) {
    absolute = path;
  }
  const fs::path relative = fs::relative(absolute, projectRoot, ec);
  if (ec || relative.empty() ||
      relative.native().rfind("..", 0) == 0) {
    return path.filename().generic_string();
  }
  return relative.generic_string();
}

bool
isRuntimeStateTable(std::string_view table)
{
  // These rows are authoritative runtime state, not scene inputs: the server
  // broadcasts entity transforms/voxel snapshots and every client converges on
  // that state. Hashing them made two otherwise identical installs reject
  // window sharing just because their last local save differed.
  return table == "Positionable" || table == "ServerVoxels" ||
         table == "Door" || table == "Lock" || table == "Key" ||
         table == "RotateMovement" || table == "Bootable";
}

std::vector<std::string>
listTables(SQLite::Database& database)
{
  std::vector<std::string> tables;
  SQLite::Statement query(
    database,
    "SELECT name FROM sqlite_master "
    "WHERE type='table' AND name NOT LIKE 'sqlite_%' ORDER BY name");
  while (query.executeStep()) {
    tables.push_back(query.getColumn(0).getString());
  }
  return tables;
}

std::vector<std::string>
tableColumns(SQLite::Database& database, const std::string& table)
{
  std::vector<std::string> columns;
  SQLite::Statement query(database, "PRAGMA table_info(\"" + table + "\")");
  while (query.executeStep()) {
    columns.push_back(query.getColumn(1).getString());
  }
  return columns;
}

void
hashTableRows(Hasher& hasher,
              SQLite::Database& database,
              const std::string& table,
              const std::vector<std::string>& columns,
              bool skipPid)
{
  std::ostringstream sql;
  sql << "SELECT ";
  bool first = true;
  for (const auto& column : columns) {
    if (skipPid && column == "pid") {
      continue;
    }
    if (!first) {
      sql << ',';
    }
    sql << "quote(\"" << column << "\")";
    first = false;
  }
  if (first) {
    return;
  }
  sql << " FROM \"" << table << "\" ORDER BY rowid";

  hasher.updateString("table");
  hasher.updateString(table);
  for (const auto& column : columns) {
    if (!skipPid || column != "pid") {
      hasher.updateString(column);
    }
  }

  SQLite::Statement query(database, sql.str());
  while (query.executeStep()) {
    hasher.updateString("row");
    for (int i = 0; i < query.getColumnCount(); ++i) {
      hasher.updateString(query.getColumn(i).getString());
    }
  }
}

void
hashModelRows(Hasher& hasher,
              SQLite::Database& database,
              const fs::path& projectRoot,
              std::unordered_set<std::string>& seen)
{
  hasher.updateString("table");
  hasher.updateString("Model");
  hasher.updateString("entity_id");
  hasher.updateString("path");
  hasher.updateString("node");

  const std::vector<std::string> columns = tableColumns(database, "Model");
  const bool hasNode = std::find(columns.begin(), columns.end(), "node") !=
                       columns.end();
  SQLite::Statement query(
    database,
    hasNode ? "SELECT entity_id, path, node FROM Model ORDER BY entity_id"
            : "SELECT entity_id, path FROM Model ORDER BY entity_id");
  while (query.executeStep()) {
    const int64_t entityId = query.getColumn(0).getInt64();
    const std::string path = query.getColumn(1).getString();
    const std::string node = hasNode ? query.getColumn(2).getString() : "";
    hasher.updateString("model-row");
    hasher.updateString(std::to_string(entityId));
    const std::string normalized = normalizeAssetPath(path, projectRoot);
    hasher.updateString(normalized);
    hasher.updateString(node);
    if (path.empty()) {
      hasher.updateString("empty-model-path");
      continue;
    }
    hashModel(hasher, fs::path(path), seen);
  }
}

} // namespace

std::string
SceneFingerprint::hex() const
{
  if (!valid) {
    return "(none)";
  }
  std::ostringstream out;
  out << std::hex << std::setfill('0') << std::setw(16) << hi << std::setw(16)
      << lo;
  return out.str();
}

SceneFingerprint
computeSceneFingerprint(SQLite::Database& database,
                        const std::string& databaseFile)
{
  Hasher hasher;
  hasher.updateString(kFingerprintDomain);

  // Hash canonical row values instead of the raw SQLite file. The raw file
  // depends on page layout, WAL state and the absolute paths stored in the DB,
  // which makes two logically identical scenes mismatch across machines.
  fs::path projectRoot = fs::path(databaseFile).parent_path();
  if (projectRoot.filename() == "db") {
    projectRoot = projectRoot.parent_path();
  }
  if (projectRoot.empty()) {
    projectRoot = fs::current_path();
  }

  std::unordered_set<std::string> seen;
  // Do not swallow query failures: a partially hashed scene would look like a
  // mismatch to every peer. Let the caller report "fingerprint unavailable".
  for (const auto& table : listTables(database)) {
    if (isRuntimeStateTable(table)) {
      continue;
    }
    if (table == "Model") {
      hashModelRows(hasher, database, projectRoot, seen);
    } else {
      hashTableRows(hasher,
                    database,
                    table,
                    tableColumns(database, table),
                    table == "Bootable");
    }
  }

  return hasher.finish();
}

SceneFingerprint
computeSceneFingerprint(EntityRegistry& registry,
                        const std::string& databaseFile)
{
  return computeSceneFingerprint(registry.getDatabase(), databaseFile);
}

} // namespace MultiPlayer
