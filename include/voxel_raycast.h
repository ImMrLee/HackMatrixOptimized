#pragma once

#include <cmath>
#include <limits>

#include <glm/glm.hpp>

// Result of a DDA voxel traversal.
struct VoxelRaycastHit
{
  bool valid = false;
  int x = 0;
  int y = 0;
  int z = 0;
  glm::vec3 normal{ 0.0f };
};

// Amanatides & Woo voxel traversal (the "3D DDA" used for picking).
//
//   origin, direction : ray in voxel space (direction need not be normalised)
//   limit             : tMax distance cap
//   maxSteps          : <= 0 means unlimited
//   stepAtLeastOnce   : reproduce the do-while variant that always inspects at
//                       least one cell (getLookedAtCube) instead of checking the
//                       distance cap before the first step (raycastVoxelSpace)
//   isSolid(x, y, z)  : cell occupancy predicate
//
// Returns the first occupied cell along the ray, with the normal of the face it
// was entered through.
template<typename IsSolid>
VoxelRaycastHit
ddaVoxelRaycast(glm::vec3 origin,
                glm::vec3 direction,
                float limit,
                int maxSteps,
                bool stepAtLeastOnce,
                IsSolid&& isSolid)
{
  VoxelRaycastHit hit;

  int x = static_cast<int>(std::floor(origin.x));
  int y = static_cast<int>(std::floor(origin.y));
  int z = static_cast<int>(std::floor(origin.z));

  const int stepX = (direction.x > 0) ? 1 : -1;
  const int stepY = (direction.y > 0) ? 1 : -1;
  const int stepZ = (direction.z > 0) ? 1 : -1;

  // Distance along the ray to the next cell boundary on each axis.
  float tMaxX =
    direction.x != 0
      ? (x + ((stepX == 1) ? 1 : 0) - origin.x) / direction.x
      : std::numeric_limits<float>::infinity();
  float tMaxY =
    direction.y != 0
      ? (y + ((stepY == 1) ? 1 : 0) - origin.y) / direction.y
      : std::numeric_limits<float>::infinity();
  float tMaxZ =
    direction.z != 0
      ? (z + ((stepZ == 1) ? 1 : 0) - origin.z) / direction.z
      : std::numeric_limits<float>::infinity();

  const float tDeltaX =
    direction.x != 0 ? 1.0f / std::fabs(direction.x)
                     : std::numeric_limits<float>::infinity();
  const float tDeltaY =
    direction.y != 0 ? 1.0f / std::fabs(direction.y)
                     : std::numeric_limits<float>::infinity();
  const float tDeltaZ =
    direction.z != 0 ? 1.0f / std::fabs(direction.z)
                     : std::numeric_limits<float>::infinity();

  const glm::vec3 normalX(stepX * -1, 0, 0);
  const glm::vec3 normalY(0, stepY * -1, 0);
  const glm::vec3 normalZ(0, 0, stepZ * -1);

  glm::vec3 normal(0.0f);
  int steps = 0;

  auto withinLimit = [&]() {
    return tMaxX < limit || tMaxY < limit || tMaxZ < limit;
  };

  if (!stepAtLeastOnce && !withinLimit()) {
    return hit;
  }

  for (;;) {
    // Advance to the next cell along the axis whose boundary is closest.
    if (tMaxX < tMaxY) {
      if (tMaxX < tMaxZ) {
        tMaxX += tDeltaX;
        x += stepX;
        normal = normalX;
      } else {
        tMaxZ += tDeltaZ;
        z += stepZ;
        normal = normalZ;
      }
    } else {
      if (tMaxY < tMaxZ) {
        tMaxY += tDeltaY;
        y += stepY;
        normal = normalY;
      } else {
        tMaxZ += tDeltaZ;
        z += stepZ;
        normal = normalZ;
      }
    }

    if (isSolid(x, y, z)) {
      hit.valid = true;
      hit.x = x;
      hit.y = y;
      hit.z = z;
      hit.normal = normal;
      return hit;
    }

    ++steps;
    if (maxSteps > 0 && steps >= maxSteps) {
      break;
    }
    if (!withinLimit()) {
      break;
    }
  }

  return hit;
}
