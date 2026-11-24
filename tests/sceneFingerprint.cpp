// Unit tests for the multiplayer scene fingerprint: identical scene inputs
// must hash identically, and a change to the database or a referenced model
// must change the hash.

#include <gtest/gtest.h>

#include <SQLiteCpp/SQLiteCpp.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "scene_fingerprint.h"

namespace fs = std::filesystem;
using namespace MultiPlayer;

namespace {

class SceneFingerprintTest : public ::testing::Test
{
protected:
  fs::path dir;
  fs::path databaseFile;

  void SetUp() override
  {
    dir = fs::temp_directory_path() / "hackmatrix-scene-fingerprint-test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    databaseFile = dir / "matrix.db";
    writeFile(databaseFile, "scene-a");
  }

  void TearDown() override { fs::remove_all(dir); }

  static void writeFile(const fs::path& path, const std::string& contents)
  {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
  }

  static void createModelTable(SQLite::Database& db)
  {
    db.exec("CREATE TABLE Model (entity_id INTEGER PRIMARY KEY, path TEXT)");
  }

  static void addModel(SQLite::Database& db, int id, const std::string& path)
  {
    SQLite::Statement query(
      db, "INSERT INTO Model (entity_id, path) VALUES (?, ?)");
    query.bind(1, id);
    query.bind(2, path);
    query.exec();
  }
};

} // namespace

TEST_F(SceneFingerprintTest, IsDeterministic)
{
  SQLite::Database db(":memory:", SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
  createModelTable(db);

  const auto first = computeSceneFingerprint(db, databaseFile.string());
  const auto second = computeSceneFingerprint(db, databaseFile.string());

  EXPECT_TRUE(first.valid);
  EXPECT_EQ(first, second);
  EXPECT_FALSE(first.hex().empty());
  EXPECT_NE(first.hex(), "(none)");
}

TEST_F(SceneFingerprintTest, StaticSceneChangeChangesFingerprint)
{
  SQLite::Database db(":memory:", SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
  createModelTable(db);
  db.exec("CREATE TABLE Light (entity_id INTEGER PRIMARY KEY, color_r REAL)");

  const auto before = computeSceneFingerprint(db, databaseFile.string());
  db.exec("INSERT INTO Light (entity_id, color_r) VALUES (1, 0.5)");
  const auto after = computeSceneFingerprint(db, databaseFile.string());

  EXPECT_NE(before, after);
}

TEST_F(SceneFingerprintTest, RuntimeWorldStateDoesNotChangeFingerprint)
{
  SQLite::Database db(":memory:", SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
  createModelTable(db);

  const auto before = computeSceneFingerprint(db, databaseFile.string());
  db.exec("CREATE TABLE Positionable (entity_id INTEGER PRIMARY KEY, pos_x REAL)");
  db.exec("INSERT INTO Positionable (entity_id, pos_x) VALUES (1, 42.0)");
  db.exec("CREATE TABLE ServerVoxels (id INTEGER PRIMARY KEY, pos_x REAL)");
  db.exec("INSERT INTO ServerVoxels (pos_x) VALUES (1.0)");
  const auto after = computeSceneFingerprint(db, databaseFile.string());

  EXPECT_EQ(before, after);
}

TEST_F(SceneFingerprintTest, FileLayoutDoesNotAffectFingerprint)
{
  const auto dbFileA = dir / "a.db";
  const auto dbFileB = dir / "b.db";
  {
    SQLite::Database db(dbFileA.string(),
                        SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
    createModelTable(db);
  }
  {
    SQLite::Database db(dbFileB.string(),
                        SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
    createModelTable(db);
    // Force a different page layout/WAL history while keeping the same rows.
    db.exec("CREATE TABLE Scratch (id INTEGER PRIMARY KEY)");
    db.exec("INSERT INTO Scratch DEFAULT VALUES");
    db.exec("DROP TABLE Scratch");
  }
  SQLite::Database a(dbFileA.string(), SQLite::OPEN_READWRITE);
  SQLite::Database b(dbFileB.string(), SQLite::OPEN_READWRITE);
  const auto first = computeSceneFingerprint(a, dbFileA.string());
  const auto second = computeSceneFingerprint(b, dbFileB.string());
  EXPECT_EQ(first, second);
}

TEST_F(SceneFingerprintTest, DifferentInstallRootsMatchForSameAsset)
{
  const auto rootA = dir / "install-a";
  const auto rootB = dir / "install-b";
  fs::create_directories(rootA);
  fs::create_directories(rootB);
  writeFile(rootA / "model.obj", "v 0 0 0\n");
  writeFile(rootB / "model.obj", "v 0 0 0\n");

  SQLite::Database dbA(rootA / "matrix.db",
                       SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
  SQLite::Database dbB(rootB / "matrix.db",
                       SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
  createModelTable(dbA);
  createModelTable(dbB);
  addModel(dbA, 1, (rootA / "model.obj").string());
  addModel(dbB, 1, (rootB / "model.obj").string());

  const auto first =
    computeSceneFingerprint(dbA, (rootA / "matrix.db").string());
  const auto second =
    computeSceneFingerprint(dbB, (rootB / "matrix.db").string());
  EXPECT_EQ(first, second);
}

TEST_F(SceneFingerprintTest, VolatileBootablePidIsIgnored)
{
  SQLite::Database db(":memory:", SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
  db.exec("CREATE TABLE Bootable (entity_id INTEGER PRIMARY KEY, cmd TEXT, "
          "pid INTEGER)");
  db.exec("INSERT INTO Bootable (entity_id, cmd, pid) VALUES (1, 'foot', 1234)");
  const auto before = computeSceneFingerprint(db, databaseFile.string());
  db.exec("UPDATE Bootable SET pid = 9999 WHERE entity_id = 1");
  const auto after = computeSceneFingerprint(db, databaseFile.string());
  EXPECT_EQ(before, after);
}

TEST_F(SceneFingerprintTest, AddedModelChangesFingerprint)
{
  SQLite::Database db(":memory:", SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
  createModelTable(db);

  const auto before = computeSceneFingerprint(db, databaseFile.string());
  addModel(db, 1, (dir / "missing.obj").string());
  const auto after = computeSceneFingerprint(db, databaseFile.string());

  EXPECT_NE(before, after);
}

TEST_F(SceneFingerprintTest, ModelContentsChangeChangesFingerprint)
{
  SQLite::Database db(":memory:", SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
  createModelTable(db);

  const auto model = dir / "model.obj";
  writeFile(model, "v 0 0 0\n");
  addModel(db, 1, model.string());

  const auto before = computeSceneFingerprint(db, databaseFile.string());
  writeFile(model, "v 1 0 0\n");
  const auto after = computeSceneFingerprint(db, databaseFile.string());

  EXPECT_NE(before, after);
}

TEST_F(SceneFingerprintTest, MaterialTextureChangeChangesFingerprint)
{
  SQLite::Database db(":memory:", SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
  createModelTable(db);

  const auto model = dir / "model.obj";
  const auto material = dir / "model.mtl";
  const auto texture = dir / "model.png";
  writeFile(model, "mtllib model.mtl\nv 0 0 0\n");
  writeFile(material, "newmtl palette\nmap_Kd model.png\n");
  writeFile(texture, "texture-a");
  addModel(db, 1, model.string());

  const auto before = computeSceneFingerprint(db, databaseFile.string());
  writeFile(texture, "texture-b");
  const auto after = computeSceneFingerprint(db, databaseFile.string());

  EXPECT_NE(before, after);
}

TEST(SceneInfoPacketTest, RoundTrips)
{
  SceneFingerprint fingerprint;
  fingerprint.hi = 0x1122334455667788ULL;
  fingerprint.lo = 0x99aabbccddeeff00ULL;
  fingerprint.valid = true;

  const auto bytes = encodeSceneInfo(42, fingerprint);
  uint32_t playerID = 0;
  SceneFingerprint decoded;
  ASSERT_TRUE(
    decodeSceneInfo(bytes.data(), bytes.size(), playerID, decoded));
  EXPECT_EQ(playerID, 42u);
  EXPECT_EQ(decoded, fingerprint);
}

TEST(SceneInfoPacketTest, RejectsInvalidMagic)
{
  SceneFingerprint fingerprint;
  fingerprint.valid = true;
  auto bytes = encodeSceneInfo(7, fingerprint);
  bytes[0] = 'X';

  uint32_t playerID = 0;
  SceneFingerprint decoded;
  EXPECT_FALSE(
    decodeSceneInfo(bytes.data(), bytes.size(), playerID, decoded));
}
