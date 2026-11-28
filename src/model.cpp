#include "model.h"
#include <assimp/Importer.hpp>
#include <assimp/material.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>
#include <assimp/texture.h>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include "components/BoundingSphere.h"
#include "glm/trigonometric.hpp"
#include "persister.h"
#include "stb/stb_image.h"
#include "stb/stb_image_write.h"

#include "glm/ext/matrix_transform.hpp"
#include <glm/gtc/quaternion.hpp>
#include <spdlog/spdlog.h>

using namespace std;

namespace {

namespace fs = std::filesystem;

std::string
normalizeSlashes(std::string value)
{
  std::replace(value.begin(), value.end(), '\\', '/');
  return value;
}

std::string
modelCacheKey(const std::string& path)
{
  std::error_code ec;
  fs::path canonical = fs::weakly_canonical(fs::path(path), ec);
  if (ec) {
    canonical = fs::absolute(fs::path(path), ec);
  }
  return ec ? path : canonical.string();
}

unsigned int
uploadTexture(const unsigned char* data,
              int width,
              int height,
              int nrComponents)
{
  GLenum format = GL_RGBA;
  if (nrComponents == 1) {
    format = GL_RED;
  } else if (nrComponents == 2) {
    format = GL_RG;
  } else if (nrComponents == 3) {
    format = GL_RGB;
  } else if (nrComponents == 4) {
    format = GL_RGBA;
  }

  unsigned int textureID = 0;
  glGenTextures(1, &textureID);
  glBindTexture(GL_TEXTURE_2D, textureID);
  glTexImage2D(GL_TEXTURE_2D,
               0,
               format,
               width,
               height,
               0,
               format,
               GL_UNSIGNED_BYTE,
               data);
  glGenerateMipmap(GL_TEXTURE_2D);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  return textureID;
}

unsigned int
textureFromColor(const glm::vec4& color)
{
  const auto channel = [](float value) {
    return static_cast<unsigned char>(
      std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f);
  };
  const unsigned char pixel[4] = {
    channel(color.r), channel(color.g), channel(color.b), channel(color.a)
  };
  unsigned int textureID = 0;
  glGenTextures(1, &textureID);
  glBindTexture(GL_TEXTURE_2D, textureID);
  glTexImage2D(GL_TEXTURE_2D,
               0,
               GL_RGBA,
               1,
               1,
               0,
               GL_RGBA,
               GL_UNSIGNED_BYTE,
               pixel);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  return textureID;
}

unsigned int
textureFromEmbedded(const aiTexture* texture)
{
  if (!texture || texture->pcData == nullptr) {
    return textureFromColor(glm::vec4(1.0f, 0.0f, 1.0f, 1.0f));
  }

  if (texture->mHeight == 0) {
    // Compressed image bytes (PNG/JPEG/...), as used by GLB/FBX.
    int width = 0;
    int height = 0;
    int components = 0;
    stbi_set_flip_vertically_on_load(false);
    unsigned char* pixels = stbi_load_from_memory(
      reinterpret_cast<const stbi_uc*>(texture->pcData),
      texture->mWidth,
      &width,
      &height,
      &components,
      0);
    stbi_set_flip_vertically_on_load(true);
    if (!pixels) {
      return textureFromColor(glm::vec4(1.0f, 0.0f, 1.0f, 1.0f));
    }
    const unsigned int id = uploadTexture(pixels, width, height, components);
    stbi_image_free(pixels);
    return id;
  }

  // Uncompressed BGRA texels.
  const int width = static_cast<int>(texture->mWidth);
  const int height = static_cast<int>(texture->mHeight);
  std::vector<unsigned char> pixels(
    static_cast<std::size_t>(width) * height * 4);
  for (std::size_t i = 0;
       i < static_cast<std::size_t>(width) * height;
       ++i) {
    const aiTexel& texel = texture->pcData[i];
    pixels[i * 4 + 0] = texel.r;
    pixels[i * 4 + 1] = texel.g;
    pixels[i * 4 + 2] = texel.b;
    pixels[i * 4 + 3] = texel.a;
  }
  return uploadTexture(pixels.data(), width, height, 4);
}

aiNode*
findNodeByName(aiNode* node, const std::string& name)
{
  if (node == nullptr) {
    return nullptr;
  }
  if (name == node->mName.C_Str()) {
    return node;
  }
  for (unsigned int i = 0; i < node->mNumChildren; ++i) {
    if (aiNode* found = findNodeByName(node->mChildren[i], name)) {
      return found;
    }
  }
  return nullptr;
}

} // namespace

void
Model::Draw(Shader& shader)
{
  if (!modelData) {
    return;
  }
  for (unsigned int i = 0; i < modelData->meshes.size(); i++) {
    modelData->meshes[i].Draw(shader);
  }
}

void
Model::loadModel(const string& path, const string& node, ModelData& data)
{
  // Scene databases may reference model paths from another machine (the
  // upstream sample DB uses absolute /home/collin/... paths). Skip missing
  // files quietly instead of letting ASSIMP print the same multi-line error for
  // every entity that uses them.
  if (!fs::exists(path)) {
    static std::unordered_set<std::string> warnedMissing;
    if (warnedMissing.insert(path).second) {
      spdlog::warn("Model file missing, skipping: {}", path);
    }
    return;
  }

  const bool importNode = !node.empty();
  unsigned int flags = aiProcess_Triangulate | aiProcess_FlipUVs |
                       aiProcess_GenSmoothNormals |
                       aiProcess_JoinIdenticalVertices;
  if (!importNode) {
    // Flatten the whole file for normal model imports. Scene-part imports keep
    // node-local coordinates so their Positionable owns the world transform.
    flags |= aiProcess_PreTransformVertices;
  }

  Assimp::Importer import;
  const aiScene* scene = import.ReadFile(path, flags);

  if (!scene || scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE ||
      !scene->mRootNode) {
    spdlog::error("ASSIMP: {}", import.GetErrorString());
    return;
  }

  data.directory = fs::path(path).parent_path().string();
  if (importNode) {
    aiNode* target = findNodeByName(scene->mRootNode, node);
    if (target == nullptr) {
      spdlog::warn("Model node '{}' not found in {}", node, path);
      return;
    }
    for (unsigned int i = 0; i < target->mNumMeshes; ++i) {
      data.meshes.push_back(
        processMesh(scene->mMeshes[target->mMeshes[i]], scene, data));
    }
  } else {
    processNode(scene->mRootNode, scene, data);
  }

  for (const Mesh& mesh : data.meshes) {
    data.vertices.insert(
      data.vertices.end(), mesh.vertices.begin(), mesh.vertices.end());
  }
}

void
Model::processNode(aiNode* node, const aiScene* scene, ModelData& data)
{
  for (unsigned int i = 0; i < node->mNumMeshes; i++) {
    aiMesh* mesh = scene->mMeshes[node->mMeshes[i]];
    data.meshes.push_back(processMesh(mesh, scene, data));
  }
  for (unsigned int i = 0; i < node->mNumChildren; i++) {
    processNode(node->mChildren[i], scene, data);
  }
}

Mesh
Model::processMesh(aiMesh* mesh, const aiScene* scene, ModelData& data)
{
  vector<Vertex> vertices;
  vector<unsigned int> indices;
  vector<MeshTexture> textures;

  for (unsigned int i = 0; i < mesh->mNumVertices; i++) {
    Vertex vertex;
    glm::vec3 vector;
    vector.x = mesh->mVertices[i].x;
    vector.y = mesh->mVertices[i].y;
    vector.z = mesh->mVertices[i].z;
    vertex.Position = vector;

    if (mesh->mNormals != nullptr) {
      vector.x = mesh->mNormals[i].x;
      vector.y = mesh->mNormals[i].y;
      vector.z = mesh->mNormals[i].z;
      vertex.Normal = vector;
    } else {
      vertex.Normal = glm::vec3(0.0f, 1.0f, 0.0f);
    }

    if (mesh->mTextureCoords[0] != nullptr) {
      glm::vec2 vec;
      vec.x = mesh->mTextureCoords[0][i].x;
      vec.y = mesh->mTextureCoords[0][i].y;
      vertex.TexCoords = vec;
    } else {
      vertex.TexCoords = glm::vec2(0.0f, 0.0f);
    }

    vertices.push_back(vertex);
  }

  for (unsigned int i = 0; i < mesh->mNumFaces; i++) {
    aiFace face = mesh->mFaces[i];
    for (unsigned int j = 0; j < face.mNumIndices; j++) {
      indices.push_back(face.mIndices[j]);
    }
  }

  if (mesh->mMaterialIndex < scene->mNumMaterials) {
    aiMaterial* material = scene->mMaterials[mesh->mMaterialIndex];
    vector<MeshTexture> diffuseMaps = loadMaterialTextures(
      material, aiTextureType_DIFFUSE, "texture_diffuse", data, scene);
    if (diffuseMaps.empty()) {
      // glTF/GLB PBR materials store their base color here instead of the
      // legacy diffuse slot.
      diffuseMaps = loadMaterialTextures(
        material, aiTextureType_BASE_COLOR, "texture_diffuse", data, scene);
    }
    if (diffuseMaps.empty()) {
      aiColor4D baseColor(1.0f, 1.0f, 1.0f, 1.0f);
      if (material->Get(AI_MATKEY_BASE_COLOR, baseColor) != AI_SUCCESS) {
        material->Get(AI_MATKEY_COLOR_DIFFUSE, baseColor);
      }
      MeshTexture colorTexture;
      colorTexture.id = textureFromColor(
        glm::vec4(baseColor.r, baseColor.g, baseColor.b, baseColor.a));
      colorTexture.type = "texture_diffuse";
      colorTexture.path = "<material-color>";
      diffuseMaps.push_back(colorTexture);
    }
    textures.insert(textures.end(), diffuseMaps.begin(), diffuseMaps.end());

    vector<MeshTexture> specularMaps = loadMaterialTextures(
      material, aiTextureType_SPECULAR, "texture_specular", data, scene);
    textures.insert(textures.end(), specularMaps.begin(), specularMaps.end());
  }

  return Mesh(vertices, indices, textures);
}

vector<MeshTexture>
Model::loadMaterialTextures(aiMaterial* mat,
                            aiTextureType type,
                            string typeName,
                            ModelData& data,
                            const aiScene* scene)
{
  vector<MeshTexture> textures;
  for (unsigned int i = 0; i < mat->GetTextureCount(type); i++) {
    aiString str;
    mat->GetTexture(type, i, &str);

    bool skip = false;
    for (const MeshTexture& loaded : data.textures_loaded) {
      if (loaded.path == str.C_Str()) {
        textures.push_back(loaded);
        skip = true;
        break;
      }
    }
    if (skip) {
      continue;
    }

    MeshTexture texture;
    texture.path = str.C_Str();
    if (!texture.path.empty() && texture.path[0] == '*') {
      const int index = std::atoi(texture.path.c_str() + 1);
      if (scene != nullptr && index >= 0 &&
          index < static_cast<int>(scene->mNumTextures)) {
        texture.id = textureFromEmbedded(scene->mTextures[index]);
      } else {
        texture.id = textureFromColor(glm::vec4(1.0f, 0.0f, 1.0f, 1.0f));
      }
    } else {
      texture.id = TextureFromFile(str.C_Str(), data.directory);
    }
    texture.type = typeName;
    textures.push_back(texture);
    data.textures_loaded.push_back(texture);
  }
  return textures;
}

BoundingSphere
Model::getBoundingSphere(float scale)
{
  if (!modelData || modelData->vertices.empty()) {
    return BoundingSphere{ glm::vec3(0.0f), 0.0f };
  }

  const auto& vertices = modelData->vertices;
  glm::vec3 minBounds(std::numeric_limits<float>::max());
  glm::vec3 maxBounds(-std::numeric_limits<float>::max());

  for (const Vertex& vertex : vertices) {
    const glm::vec3 scaledPosition = vertex.Position * scale;
    minBounds = glm::min(minBounds, scaledPosition);
    maxBounds = glm::max(maxBounds, scaledPosition);
  }

  const glm::vec3 center = (minBounds + maxBounds) * 0.5f;
  float radius = 0.0f;
  for (const Vertex& vertex : vertices) {
    const glm::vec3 scaledPosition = vertex.Position * scale;
    radius = std::max(radius, glm::distance(scaledPosition, center));
  }
  return BoundingSphere{ center, radius };
}

unsigned int
TextureFromFile(const char* path, const string& directory, bool gamma)
{
  (void)gamma;
  const std::string requested = normalizeSlashes(path ? path : "");

  std::vector<fs::path> candidates;
  const fs::path requestedPath(requested);
  if (requestedPath.is_absolute()) {
    candidates.push_back(requestedPath);
  } else {
    if (!directory.empty()) {
      candidates.push_back(fs::path(directory) / requestedPath);
    }
    candidates.push_back(requestedPath);
    if (!directory.empty() && !requestedPath.filename().empty()) {
      candidates.push_back(fs::path(directory) / requestedPath.filename());
    }
  }

  fs::path resolved;
  for (const fs::path& candidate : candidates) {
    std::error_code ec;
    if (fs::exists(candidate, ec)) {
      resolved = candidate;
      break;
    }
  }

  if (resolved.empty()) {
    static std::unordered_set<std::string> warned;
    if (warned.insert(requested).second) {
      spdlog::warn("Texture missing, using placeholder: {}", requested);
    }
    return textureFromColor(glm::vec4(1.0f, 0.0f, 1.0f, 1.0f));
  }

  int width = 0;
  int height = 0;
  int components = 0;
  stbi_set_flip_vertically_on_load(false);
  unsigned char* pixels =
    stbi_load(resolved.string().c_str(), &width, &height, &components, 0);
  stbi_set_flip_vertically_on_load(true);
  if (!pixels) {
    spdlog::error("Texture failed to load at path: {}", resolved.string());
    return textureFromColor(glm::vec4(1.0f, 0.0f, 1.0f, 1.0f));
  }

  const unsigned int textureID =
    uploadTexture(pixels, width, height, components);
  stbi_image_free(pixels);
  return textureID;
}

std::shared_ptr<const ModelData>
Model::loadCached(const string& path, const string& node)
{
  static std::mutex cacheMutex;
  static std::unordered_map<std::string, std::shared_ptr<const ModelData>> cache;

  const std::string key = modelCacheKey(path) + "|" + node;
  {
    std::lock_guard<std::mutex> lock(cacheMutex);
    const auto existing = cache.find(key);
    if (existing != cache.end()) {
      return existing->second;
    }
  }

  auto data = std::make_shared<ModelData>();
  loadModel(path, node, *data);

  std::lock_guard<std::mutex> lock(cacheMutex);
  const auto existing = cache.find(key);
  if (existing != cache.end()) {
    return existing->second;
  }
  cache.emplace(key, data);
  return data;
}

Model::Model(string path, string node)
  : path(std::move(path))
  , node(std::move(node))
{
  modelData = loadCached(this->path, this->node);
}

void
ModelPersister::saveAll()
{
  auto view = registry->view<Persistable, Model>();
  SQLite::Database& db = registry->getDatabase();

  // Assuming you have a 'Model' table with 'entity_id' and 'path' columns
  std::stringstream queryStream;
  queryStream << "INSERT OR REPLACE INTO " << entityName
              << " (entity_id, path, node) VALUES (?, ?, ?)";
  SQLite::Statement query(db, queryStream.str());

  db.exec("BEGIN TRANSACTION"); // Initiate transaction

  for (auto [entity, persist, model] : view.each()) {
    query.bind(1, persist.entityId);
    query.bind(2, model.path);
    query.bind(3, model.node);
    query.exec();
    query.reset();
  }

  db.exec("COMMIT"); // Commit changes
}

void
ModelPersister::createTablesIfNeeded()
{
  SQLite::Database& db = registry->getDatabase();
  std::stringstream queryStream;
  queryStream << "CREATE TABLE IF NOT EXISTS " << entityName << " ("
              << "entity_id INTEGER PRIMARY KEY, "
              << "path TEXT, "
              << "node TEXT, "
              << "FOREIGN KEY(entity_id) REFERENCES Entity(id)) ";
  db.exec(queryStream.str());

  // Upgrade databases created before scene-part imports existed.
  bool hasNode = false;
  SQLite::Statement columns(db, "PRAGMA table_info(Model)");
  while (columns.executeStep()) {
    const std::string name = columns.getColumn(1).getString();
    if (name == "node") {
      hasNode = true;
      break;
    }
  }
  if (!hasNode) {
    db.exec("ALTER TABLE Model ADD COLUMN node TEXT");
  }
}

void
ModelPersister::loadAll()
{
  auto view = registry->view<Persistable>();
  SQLite::Database& db = registry->getDatabase();

  // Cache query data
  struct ModelRow
  {
    std::string path;
    std::string node;
  };
  std::unordered_map<int, ModelRow> modelDataCache;
  std::stringstream queryStream;
  queryStream << "SELECT entity_id, path, node FROM " << entityName;
  SQLite::Statement query(db, queryStream.str());

  while (query.executeStep()) {
    int dbId = query.getColumn(0).getInt();
    ModelRow row;
    row.path = query.getColumn(1).getText();
    row.node = query.getColumn(2).getText();
    modelDataCache[dbId] = std::move(row);
  }

  view.each([&modelDataCache, this](auto entity, auto& persistable) {
    auto it = modelDataCache.find(persistable.entityId);
    if (it != modelDataCache.end()) {
      std::unique_ptr<Model> model =
        make_unique<Model>(it->second.path, it->second.node);
      if (model) {
        registry->emplace<Model>(entity, std::move(*model));
      }
    }
  });
}

void
ModelPersister::load(entt::entity entity)
{
  auto& persistable = registry->get<Persistable>(entity);
  SQLite::Database& db = registry->getDatabase();

  std::stringstream queryStream;
  queryStream << "SELECT path, node FROM " << entityName << " WHERE entity_id = ?";
  SQLite::Statement query(db, queryStream.str());
  query.bind(1, persistable.entityId);

  if (query.executeStep()) {
    std::string path = query.getColumn(0).getText();
    std::string node = query.getColumn(1).getText();

    std::unique_ptr<Model> model = make_unique<Model>(path, node);
    if (model) {
      registry->emplace<Model>(entity, std::move(*model));
    }
  }
}

void
ModelPersister::save(entt::entity entity)
{
  auto& model = registry->get<Model>(entity);
  auto& persistable = registry->get<Persistable>(entity);
  SQLite::Database& db = registry->getDatabase();

  std::stringstream queryStream;
  queryStream << "INSERT OR REPLACE INTO " << entityName
              << " (entity_id, path, node) VALUES (?, ?, ?)";
  SQLite::Statement query(db, queryStream.str());
  query.bind(1, persistable.entityId);
  query.bind(2, model.path);
  query.bind(3, model.node);
  query.exec();
}

void
ModelPersister::depersistIfGone(entt::entity entity)
{
  depersistIfGoneTyped<Model>(entity);
}
