#include "assets.h"

#include <assimp/Importer.hpp>  // Include the Assimp header
#include <assimp/postprocess.h> // For useful post-processing steps
#include <assimp/scene.h>       // For working with the model data

#include <iostream>
#include <spdlog/spdlog.h>

aiMesh*
loadFbx(string modelPath)
{
  Assimp::Importer importer; // Create an Assimp importer object

  const aiScene* scene =
    importer.ReadFile(modelPath,
                      aiProcess_Triangulate |    // Triangulate all faces
                        aiProcess_GenNormals |   // Generate normals if missing
                        aiProcess_FlipUVs |      // Flip UVs if necessary
                        aiProcess_OptimizeMeshes // Optimize the meshes
    );

  if (!scene || scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE ||
      !scene->mRootNode) {
    spdlog::error("Error loading FBX: {}", importer.GetErrorString());
    return NULL;
  }

  // Process the loaded scene (meshes, materials, etc.)
  // Example: Accessing the first mesh:
  aiMesh* mesh = scene->mMeshes[0];

  return mesh;
  // ... (Your code to process mesh data, materials, etc.)
}
