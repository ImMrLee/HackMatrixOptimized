// Unit tests for VoxelSpace: the spatial index must behave exactly like the
// previous linear scan, including the EPS tolerance and the size dimension.

#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <vector>

#include <glm/glm.hpp>

#include "Voxel/VoxelSpace.h"

TEST(VoxelSpaceTest, EmptyNeverContains)
{
  VoxelSpace space;
  EXPECT_TRUE(space.empty());
  EXPECT_FALSE(space.has(glm::vec3(0.0f)));
}

TEST(VoxelSpaceTest, FindsExactPosition)
{
  VoxelSpace space;
  space.add(glm::vec3(1.0f, 2.0f, 3.0f));
  EXPECT_TRUE(space.has(glm::vec3(1.0f, 2.0f, 3.0f)));
  EXPECT_FALSE(space.has(glm::vec3(2.0f, 2.0f, 3.0f)));
}

TEST(VoxelSpaceTest, MatchesWithinEpsilon)
{
  VoxelSpace space;
  space.add(glm::vec3(1.0f, 2.0f, 3.0f));
  // Half the epsilon of tolerance.
  EXPECT_TRUE(space.has(glm::vec3(1.00005f, 2.0f, 3.0f)));
  // Outside the tolerance.
  EXPECT_FALSE(space.has(glm::vec3(1.01f, 2.0f, 3.0f)));
}

TEST(VoxelSpaceTest, SizeMustMatch)
{
  VoxelSpace space;
  space.add(glm::vec3(1.0f, 1.0f, 1.0f), 1.0f);
  EXPECT_TRUE(space.has(glm::vec3(1.0f, 1.0f, 1.0f), 1.0f));
  EXPECT_FALSE(space.has(glm::vec3(1.0f, 1.0f, 1.0f), 2.0f));
}

TEST(VoxelSpaceTest, RemoveInBox)
{
  VoxelSpace space;
  space.add(glm::vec3(1.0f, 1.0f, 1.0f));
  space.add(glm::vec3(5.0f, 5.0f, 5.0f));

  const size_t removed =
    space.remove(glm::vec3(0.0f), glm::vec3(2.0f, 2.0f, 2.0f));
  EXPECT_EQ(removed, 1u);
  EXPECT_FALSE(space.has(glm::vec3(1.0f, 1.0f, 1.0f)));
  EXPECT_TRUE(space.has(glm::vec3(5.0f, 5.0f, 5.0f)));
}

TEST(VoxelSpaceTest, ClearEmpties)
{
  VoxelSpace space;
  space.add(glm::vec3(1.0f, 1.0f, 1.0f));
  space.clear();
  EXPECT_TRUE(space.empty());
  EXPECT_FALSE(space.has(glm::vec3(1.0f, 1.0f, 1.0f)));
}

TEST(VoxelSpaceTest, LargeSetUsesIndexAndFindsAll)
{
  // More than the linear-scan threshold so the hash path is exercised.
  VoxelSpace space;
  for (int x = 0; x < 20; ++x) {
    for (int y = 0; y < 20; ++y) {
      space.add(glm::vec3(static_cast<float>(x), static_cast<float>(y), 0.0f));
    }
  }
  EXPECT_TRUE(space.has(glm::vec3(0.0f, 0.0f, 0.0f)));
  EXPECT_TRUE(space.has(glm::vec3(19.0f, 19.0f, 0.0f)));
  EXPECT_TRUE(space.has(glm::vec3(10.0f, 5.0f, 0.0f)));
  EXPECT_FALSE(space.has(glm::vec3(100.0f, 100.0f, 100.0f)));
  EXPECT_FALSE(space.has(glm::vec3(10.5f, 5.0f, 0.0f)));
}

TEST(VoxelSpaceTest, LargeSetFindsAcrossCellBoundary)
{
  // Voxels sit just below cell boundaries; queries just above them are within
  // epsilon and must be found via the neighbouring-cell lookup.
  VoxelSpace space;
  for (int x = 0; x < 10; ++x) {
    for (int y = 0; y < 10; ++y) {
      space.add(glm::vec3(static_cast<float>(x) + 0.99999f,
                          static_cast<float>(y) + 0.99999f,
                          0.0f));
    }
  }
  EXPECT_TRUE(space.has(glm::vec3(0.99999f, 0.99999f, 0.0f)));
  EXPECT_TRUE(space.has(glm::vec3(1.0f, 1.0f, 0.0f)));
  EXPECT_FALSE(space.has(glm::vec3(1.5f, 1.5f, 0.0f)));
}

TEST(VoxelSpaceTest, IndexSurvivesMutation)
{
  VoxelSpace space;
  for (int i = 0; i < 100; ++i) {
    space.add(glm::vec3(static_cast<float>(i), 0.0f, 0.0f));
  }
  EXPECT_TRUE(space.has(glm::vec3(50.0f, 0.0f, 0.0f)));

  space.remove(glm::vec3(49.0f, -1.0f, -1.0f), glm::vec3(51.0f, 1.0f, 1.0f));
  EXPECT_FALSE(space.has(glm::vec3(50.0f, 0.0f, 0.0f)));
  EXPECT_TRUE(space.has(glm::vec3(52.0f, 0.0f, 0.0f)));

  space.add(glm::vec3(50.0f, 0.0f, 0.0f));
  EXPECT_TRUE(space.has(glm::vec3(50.0f, 0.0f, 0.0f)));
}

// Disabled by default (timing is environment-dependent). Run manually with:
//   ./matrix_unit_tests --gtest_also_run_disabled_tests \
//       --gtest_filter='*Benchmark*'
TEST(VoxelSpaceTest, DISABLED_BenchmarkLargeHas)
{
  constexpr int kVoxels = 20000;
  constexpr int kQueries = 100000;

  VoxelSpace space;
  std::vector<glm::vec3> all;
  all.reserve(kVoxels);
  for (int i = 0; i < kVoxels; ++i) {
    glm::vec3 p(static_cast<float>(i % 100),
                static_cast<float>((i / 100) % 100),
                static_cast<float>(i / 10000));
    space.add(p, 1.0f);
    all.push_back(p);
  }

  auto makeQuery = [](int i) {
    return glm::vec3(static_cast<float>((i * 37) % 100),
                     static_cast<float>((i * 53) % 100),
                     static_cast<float>((i * 71) % 100));
  };

  int hits = 0;
  const auto hashStart = std::chrono::steady_clock::now();
  for (int i = 0; i < kQueries; ++i) {
    hits += space.has(makeQuery(i), 1.0f) ? 1 : 0;
  }
  const auto hashEnd = std::chrono::steady_clock::now();

  int linearHits = 0;
  const auto linearStart = std::chrono::steady_clock::now();
  for (int i = 0; i < kQueries; ++i) {
    const glm::vec3 q = makeQuery(i);
    for (const auto& p : all) {
      if (glm::length(p - q) < 0.0001f) {
        ++linearHits;
        break;
      }
    }
  }
  const auto linearEnd = std::chrono::steady_clock::now();

  const double hashMs =
    std::chrono::duration<double, std::milli>(hashEnd - hashStart).count();
  const double linearMs =
    std::chrono::duration<double, std::milli>(linearEnd - linearStart).count();
  std::printf("VoxelSpace: hash %.1f ms (%d hits) vs linear %.1f ms (%d hits), "
              "speedup %.1fx\n",
              hashMs,
              hits,
              linearMs,
              linearHits,
              linearMs / hashMs);
  EXPECT_EQ(hits, linearHits);
}
