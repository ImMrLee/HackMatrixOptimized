#pragma once

#include <tuple>
#include <vector>
#include <cstddef>
#include <cstdint>
#include <unordered_map>

#include <glm/vec3.hpp>

class RenderedVoxelSpace;

class Voxel
{
public:
  Voxel();
  Voxel(glm::vec3 position, float size, glm::vec3 color);
  std::tuple<std::vector<glm::vec3>,
             std::vector<glm::vec3>,
             std::vector<glm::vec3>> buildVertices() const;
  glm::vec3 getPosition() const { return position; }
  float getSize() const { return size; }
  glm::vec3 getColor() const { return color; }

private:
  glm::vec3 position{ 0.0f };
  float size = 1.0f;
  glm::vec3 color{ 1.0f };
};

class RenderedVoxelSpace
{
public:
  RenderedVoxelSpace();
  explicit RenderedVoxelSpace(
    const std::vector<glm::vec3>& positions,
    const std::vector<glm::vec3>& barycentrics,
    const std::vector<glm::vec3>& colors);
  RenderedVoxelSpace(RenderedVoxelSpace&& other) noexcept;
  RenderedVoxelSpace& operator=(RenderedVoxelSpace&& other) noexcept;
  RenderedVoxelSpace(const RenderedVoxelSpace&) = delete;
  RenderedVoxelSpace& operator=(const RenderedVoxelSpace&) = delete;
  ~RenderedVoxelSpace();

  void upload(const std::vector<glm::vec3>& positions,
              const std::vector<glm::vec3>& barycentrics,
              const std::vector<glm::vec3>& colors);
  void draw() const;

private:
  void destroy();
  unsigned int vao = 0;
  unsigned int vboPositions = 0;
  unsigned int vboBarycentrics = 0;
  unsigned int vboColors = 0;
  int vertexCount = 0;
};

class VoxelSpace
{
public:
  void add(glm::vec3 position, float size = 1.0f, glm::vec3 color = glm::vec3(1.0f));
  bool has(glm::vec3 position, float size = 1.0f) const;
  size_t remove(glm::vec3 min, glm::vec3 max);
  RenderedVoxelSpace render() const;
  void clear();
  bool empty() const { return voxels.empty(); }

private:
  // Spatial hash over 1-unit cells. Queries check the 3x3x3 neighbourhood so
  // points within EPS of a cell boundary are still found.
  struct CellKey
  {
    int x = 0;
    int y = 0;
    int z = 0;
    bool operator==(const CellKey& other) const
    {
      return x == other.x && y == other.y && z == other.z;
    }
  };
  struct CellHash
  {
    std::size_t operator()(const CellKey& k) const noexcept
    {
      std::size_t h =
        static_cast<std::size_t>(static_cast<std::uint32_t>(k.x)) * 73856093u;
      h ^= static_cast<std::size_t>(static_cast<std::uint32_t>(k.y)) * 19349663u;
      h ^= static_cast<std::size_t>(static_cast<std::uint32_t>(k.z)) * 83492791u;
      return h;
    }
  };
  static CellKey cellFor(glm::vec3 position);
  void ensureIndex() const;

  std::vector<Voxel> voxels;
  mutable std::unordered_map<CellKey, std::vector<std::size_t>, CellHash> index;
  mutable bool indexDirty = true;
};
