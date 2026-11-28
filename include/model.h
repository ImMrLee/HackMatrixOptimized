#pragma once

#include "components/BoundingSphere.h"
#include "mesh.h"
#include "SQLPersisterImpl.h"
#include "shader.h"
#include <assimp/mesh.h>
#include <assimp/scene.h>
#include "entity.h"

unsigned int
TextureFromFile(const char* path, const string& directory, bool gamma = false);

struct Positionable
{
  Positionable(glm::vec3 pos, glm::vec3 origin, glm::vec3 rotate, float scale);
  Positionable(Positionable* p);
  glm::vec3 pos;
  glm::vec3 origin;
  glm::vec3 rotate;
  float scale;
  void update();
  glm::mat4 modelMatrix;
  glm::mat3 normalMatrix;
  bool damaged = true;
  bool isDragging = false;
  void damage();
};

class PositionablePersister : public SQLPersisterImpl
{
public:
  PositionablePersister(std::shared_ptr<EntityRegistry> registry)
    : SQLPersisterImpl("Positionable", registry){};
  void createTablesIfNeeded() override;
  void saveAll() override;
  void save(entt::entity) override;
  void loadAll() override;
  void load(entt::entity) override;
  void depersistIfGone(entt::entity) override;
};

struct ModelData
{
  vector<Mesh> meshes;
  vector<MeshTexture> textures_loaded;
  vector<Vertex> vertices;
  string directory;
};

class Model
{
public:
  string path;
  string node; // optional Assimp node name for scene-part imports
  Model(string path, string node = "");
  void Draw(Shader& shader);

  BoundingSphere getBoundingSphere(float scale);
  const std::shared_ptr<const ModelData>& data() const { return modelData; }

private:
  std::shared_ptr<const ModelData> modelData;

  // Shared cache: one GPU copy per model path, reused by every entity.
  static std::shared_ptr<const ModelData> loadCached(const string& path,
                                                     const string& node);
  static void loadModel(const string& path,
                        const string& node,
                        ModelData& data);
  static void processNode(aiNode* node, const aiScene* scene, ModelData& data);
  static Mesh processMesh(aiMesh* mesh, const aiScene* scene, ModelData& data);
  static vector<MeshTexture> loadMaterialTextures(aiMaterial* mat,
                                                  aiTextureType type,
                                                  string typeName,
                                                  ModelData& data,
                                                  const aiScene* scene);
};

class ModelPersister : public SQLPersisterImpl
{
public:
  ModelPersister(std::shared_ptr<EntityRegistry> registry)
    : SQLPersisterImpl("Model", registry){};
  void createTablesIfNeeded() override;
  void saveAll() override;
  void save(entt::entity) override;
  void loadAll() override;
  void load(entt::entity) override;
  void depersistIfGone(entt::entity) override;
};
