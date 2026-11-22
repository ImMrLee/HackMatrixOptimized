// Unit tests for the shared DDA voxel traversal used by picking
// (raycastVoxelSpace and getLookedAtCube). Pure maths: no World, renderer or GL.

#include <gtest/gtest.h>

#include "voxel_raycast.h"

namespace {

bool
cellIs(int x, int y, int z, int sx, int sy, int sz)
{
  return x == sx && y == sy && z == sz;
}

} // namespace

TEST(DdaRaycast, HitsAlongPositiveX)
{
  auto hit = ddaVoxelRaycast(glm::vec3(0.5f, 0.5f, 0.5f),
                             glm::vec3(1.0f, 0.0f, 0.0f),
                             20.0f,
                             256,
                             false,
                             [](int x, int y, int z) {
                               return cellIs(x, y, z, 3, 0, 0);
                             });

  ASSERT_TRUE(hit.valid);
  EXPECT_EQ(hit.x, 3);
  EXPECT_EQ(hit.y, 0);
  EXPECT_EQ(hit.z, 0);
  EXPECT_FLOAT_EQ(hit.normal.x, -1.0f);
  EXPECT_FLOAT_EQ(hit.normal.y, 0.0f);
  EXPECT_FLOAT_EQ(hit.normal.z, 0.0f);
}

TEST(DdaRaycast, HitsAlongNegativeX)
{
  auto hit = ddaVoxelRaycast(glm::vec3(0.5f, 0.5f, 0.5f),
                             glm::vec3(-1.0f, 0.0f, 0.0f),
                             20.0f,
                             256,
                             false,
                             [](int x, int y, int z) {
                               return cellIs(x, y, z, -3, 0, 0);
                             });

  ASSERT_TRUE(hit.valid);
  EXPECT_EQ(hit.x, -3);
  EXPECT_FLOAT_EQ(hit.normal.x, 1.0f);
}

TEST(DdaRaycast, HitsAlongPositiveY)
{
  auto hit = ddaVoxelRaycast(glm::vec3(0.5f, 0.5f, 0.5f),
                             glm::vec3(0.0f, 1.0f, 0.0f),
                             20.0f,
                             256,
                             false,
                             [](int x, int y, int z) {
                               return cellIs(x, y, z, 0, 2, 0);
                             });

  ASSERT_TRUE(hit.valid);
  EXPECT_EQ(hit.y, 2);
  EXPECT_FLOAT_EQ(hit.normal.y, -1.0f);
}

TEST(DdaRaycast, MissesWhenNothingIsSolid)
{
  auto hit = ddaVoxelRaycast(glm::vec3(0.5f, 0.5f, 0.5f),
                             glm::vec3(1.0f, 0.0f, 0.0f),
                             20.0f,
                             256,
                             false,
                             [](int, int, int) { return false; });

  EXPECT_FALSE(hit.valid);
}

TEST(DdaRaycast, RespectsMaxSteps)
{
  auto isSolid = [](int x, int, int) { return x == 100; };

  auto capped = ddaVoxelRaycast(glm::vec3(0.5f, 0.5f, 0.5f),
                                glm::vec3(1.0f, 0.0f, 0.0f),
                                1000.0f,
                                5,
                                false,
                                isSolid);
  EXPECT_FALSE(capped.valid) << "5 steps must not reach x=100";

  auto uncapped = ddaVoxelRaycast(glm::vec3(0.5f, 0.5f, 0.5f),
                                  glm::vec3(1.0f, 0.0f, 0.0f),
                                  1000.0f,
                                  0, // unlimited
                                  false,
                                  isSolid);
  ASSERT_TRUE(uncapped.valid);
  EXPECT_EQ(uncapped.x, 100);
}

TEST(DdaRaycast, RespectsDistanceLimit)
{
  auto hit = ddaVoxelRaycast(glm::vec3(0.5f, 0.5f, 0.5f),
                             glm::vec3(1.0f, 0.0f, 0.0f),
                             2.0f, // only the first two cells are in range
                             256,
                             false,
                             [](int x, int, int) { return x == 100; });

  EXPECT_FALSE(hit.valid);
}

TEST(DdaRaycast, StepAtLeastOnceDiffersFromPreCheck)
{
  auto isSolid = [](int x, int y, int z) { return cellIs(x, y, z, 1, 0, 0); };

  // limit 0 means the pre-check fails immediately...
  auto preCheck = ddaVoxelRaycast(glm::vec3(0.5f, 0.5f, 0.5f),
                                  glm::vec3(1.0f, 0.0f, 0.0f),
                                  0.0f,
                                  256,
                                  false,
                                  isSolid);
  EXPECT_FALSE(preCheck.valid);

  // ...but the do-while variant always inspects the first cell.
  auto doWhile = ddaVoxelRaycast(glm::vec3(0.5f, 0.5f, 0.5f),
                                 glm::vec3(1.0f, 0.0f, 0.0f),
                                 0.0f,
                                 256,
                                 true,
                                 isSolid);
  ASSERT_TRUE(doWhile.valid);
  EXPECT_EQ(doWhile.x, 1);
}
