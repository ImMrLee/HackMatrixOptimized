#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "world_sync_protocol.h"

using namespace MultiPlayer;

TEST(WorldSyncProtocol, HeaderRoundTrip)
{
  std::vector<std::uint8_t> bytes(kWorldSyncHeaderSize, 0);
  bytes[0] = static_cast<std::uint8_t>(WorldSyncType::Add);
  // targetOwner = 0x01020304, sender = 0x0a0b0c0d, seq = 0x1122334455667788
  const std::uint32_t target = 0x01020304;
  const std::uint32_t sender = 0x0a0b0c0d;
  const std::uint64_t seq = 0x1122334455667788ULL;
  for (int i = 0; i < 4; ++i) {
    bytes[kWorldSyncTargetOffset + i] =
      static_cast<std::uint8_t>((target >> (8 * i)) & 0xff);
    bytes[kWorldSyncSenderOffset + i] =
      static_cast<std::uint8_t>((sender >> (8 * i)) & 0xff);
  }
  for (int i = 0; i < 8; ++i) {
    bytes[9 + i] = static_cast<std::uint8_t>((seq >> (8 * i)) & 0xff);
  }

  WorldSyncHeader header;
  ASSERT_TRUE(decodeWorldSyncHeader(bytes.data(), bytes.size(), header));
  EXPECT_EQ(header.type, WorldSyncType::Add);
  EXPECT_EQ(header.targetOwner, target);
  EXPECT_EQ(header.senderID, sender);
  EXPECT_EQ(header.seq, seq);
  EXPECT_FALSE(decodeWorldSyncHeader(bytes.data(), 8, header));
}

TEST(WorldSyncProtocol, AuthorityAndSenderStamp)
{
  auto bytes = encodeWorldAuthority(42);
  std::uint32_t authority = 0;
  ASSERT_TRUE(decodeWorldAuthority(bytes.data(), bytes.size(), authority));
  EXPECT_EQ(authority, 42u);

  setWorldSyncSender(bytes.data(), bytes.size(), 99);
  std::uint32_t target = 1234;
  ASSERT_TRUE(worldSyncTargetOwner(bytes.data(), bytes.size(), target));
  EXPECT_EQ(target, 0u);
  WorldSyncHeader header;
  ASSERT_TRUE(decodeWorldSyncHeader(bytes.data(), bytes.size(), header));
  EXPECT_EQ(header.senderID, 99u);
}

TEST(WorldSyncProtocol, AddRoundTrip)
{
  std::vector<glm::vec3> positions = {
    { 1.0f, 2.0f, 3.0f }, { -4.5f, 0.25f, 8.0f }
  };
  std::vector<glm::vec3> colors = {
    { 1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }
  };
  auto bytes = encodeWorldAdd(7, 11, true, 0.2f, positions, colors);

  bool replace = false;
  float size = 0.0f;
  std::vector<glm::vec3> decodedPositions;
  std::vector<glm::vec3> decodedColors;
  ASSERT_TRUE(decodeWorldAdd(bytes.data(),
                             bytes.size(),
                             replace,
                             size,
                             decodedPositions,
                             decodedColors));
  EXPECT_TRUE(replace);
  EXPECT_FLOAT_EQ(size, 0.2f);
  ASSERT_EQ(decodedPositions.size(), 2u);
  EXPECT_EQ(decodedPositions[0], positions[0]);
  EXPECT_EQ(decodedColors[1], colors[1]);
}

TEST(WorldSyncProtocol, ClearAndSnapshotRoundTrip)
{
  auto box = encodeWorldClearBox(3, 5, { 0.0f, 1.0f, 2.0f },
                                 { 4.0f, 5.0f, 6.0f });
  glm::vec3 min;
  glm::vec3 max;
  ASSERT_TRUE(decodeWorldClearBox(box.data(), box.size(), min, max));
  EXPECT_EQ(min, glm::vec3(0.0f, 1.0f, 2.0f));
  EXPECT_EQ(max, glm::vec3(4.0f, 5.0f, 6.0f));

  std::vector<glm::vec3> positions = { { 1.0f, 2.0f, 3.0f } };
  auto clear = encodeWorldClearPositions(3, 6, positions);
  std::vector<glm::vec3> decoded;
  ASSERT_TRUE(decodeWorldClearPositions(clear.data(), clear.size(), decoded));
  ASSERT_EQ(decoded.size(), 1u);
  EXPECT_EQ(decoded[0], positions[0]);

  std::vector<WorldSyncVoxel> voxels(2);
  voxels[0].position = { 1.0f, 2.0f, 3.0f };
  voxels[0].edge = 0.1f;
  voxels[0].color = { 0.2f, 0.3f, 0.4f };
  voxels[1].position = { -1.0f, 0.0f, 5.0f };
  voxels[1].edge = 0.25f;
  voxels[1].color = { 1.0f, 1.0f, 1.0f };
  auto snapshot = encodeWorldSnapshot(9, 3, 77, voxels);
  std::uint64_t seq = 0;
  std::vector<WorldSyncVoxel> decodedVoxels;
  ASSERT_TRUE(
    decodeWorldSnapshot(snapshot.data(), snapshot.size(), seq, decodedVoxels));
  EXPECT_EQ(seq, 77u);
  ASSERT_EQ(decodedVoxels.size(), 2u);
  EXPECT_EQ(decodedVoxels[0].position, voxels[0].position);
  EXPECT_FLOAT_EQ(decodedVoxels[1].edge, 0.25f);
  EXPECT_EQ(decodedVoxels[1].color, voxels[1].color);
}

TEST(WorldSyncProtocol, EntityTransformRoundTrip)
{
  WorldSyncEntityTransform transform;
  transform.entityID = 42;
  transform.position = { 1.0f, 2.0f, 3.0f };
  transform.origin = { -1.0f, 0.5f, 0.25f };
  transform.rotate = { 0.0f, 90.0f, 0.0f };
  transform.scale = 1.5f;

  auto broadcast = encodeWorldEntityTransform(7, 12, transform);
  WorldSyncHeader header;
  ASSERT_TRUE(
    decodeWorldSyncHeader(broadcast.data(), broadcast.size(), header));
  EXPECT_EQ(header.type, WorldSyncType::EntityTransform);
  EXPECT_EQ(header.targetOwner, 0u);
  EXPECT_EQ(header.seq, 12u);

  WorldSyncEntityTransform decoded;
  ASSERT_TRUE(decodeWorldEntityTransform(
    broadcast.data(), broadcast.size(), decoded));
  EXPECT_EQ(decoded.entityID, 42u);
  EXPECT_EQ(decoded.position, transform.position);
  EXPECT_EQ(decoded.origin, transform.origin);
  EXPECT_EQ(decoded.rotate, transform.rotate);
  EXPECT_FLOAT_EQ(decoded.scale, 1.5f);

  auto forward = encodeWorldEntityEdit(9, 7, transform);
  ASSERT_TRUE(decodeWorldSyncHeader(forward.data(), forward.size(), header));
  EXPECT_EQ(header.targetOwner, 9u);
  EXPECT_EQ(header.senderID, 7u);
  EXPECT_EQ(header.seq, 0u);
}

TEST(WorldSyncProtocol, EntityMoveRoundTrip)
{
  auto bytes = encodeWorldEntityMove(
    5, 3, 77, glm::vec3(0.25f, -1.0f, 2.0f), 4.0f);
  WorldSyncHeader header;
  ASSERT_TRUE(decodeWorldSyncHeader(bytes.data(), bytes.size(), header));
  EXPECT_EQ(header.type, WorldSyncType::EntityMove);
  EXPECT_EQ(header.targetOwner, 5u);
  EXPECT_EQ(header.senderID, 3u);

  std::uint32_t entityID = 0;
  glm::vec3 delta;
  float unitsPerSecond = 0.0f;
  ASSERT_TRUE(decodeWorldEntityMove(
    bytes.data(), bytes.size(), entityID, delta, unitsPerSecond));
  EXPECT_EQ(entityID, 77u);
  EXPECT_EQ(delta, glm::vec3(0.25f, -1.0f, 2.0f));
  EXPECT_FLOAT_EQ(unitsPerSecond, 4.0f);
}

TEST(WorldSyncProtocol, EntitySnapshotRoundTrip)
{
  std::vector<WorldSyncEntityTransform> entities(2);
  entities[0].entityID = 1;
  entities[0].position = { 0.0f, 1.0f, 2.0f };
  entities[0].origin = { 0.0f, 0.0f, 0.0f };
  entities[0].rotate = { 0.0f, 0.0f, 0.0f };
  entities[0].scale = 1.0f;
  entities[1].entityID = 2;
  entities[1].position = { -1.0f, 2.0f, 3.0f };
  entities[1].origin = { 0.1f, 0.2f, 0.3f };
  entities[1].rotate = { 5.0f, 6.0f, 7.0f };
  entities[1].scale = 0.5f;

  auto bytes = encodeWorldEntitySnapshot(8, 4, 33, entities);
  std::uint64_t seq = 0;
  std::vector<WorldSyncEntityTransform> decoded;
  ASSERT_TRUE(
    decodeWorldEntitySnapshot(bytes.data(), bytes.size(), seq, decoded));
  EXPECT_EQ(seq, 33u);
  ASSERT_EQ(decoded.size(), 2u);
  EXPECT_EQ(decoded[0].entityID, 1u);
  EXPECT_EQ(decoded[1].position, entities[1].position);
  EXPECT_FLOAT_EQ(decoded[1].scale, 0.5f);
  EXPECT_FALSE(
    decodeWorldEntitySnapshot(bytes.data(), bytes.size() - 1, seq, decoded));
}

TEST(WorldSyncProtocol, TruncatedPayloadsAreRejected)
{
  auto add = encodeWorldAdd(
    1, 2, false, 0.1f, { { 0.0f, 0.0f, 0.0f } }, { { 1.0f, 1.0f, 1.0f } });
  bool replace = false;
  float size = 0.0f;
  std::vector<glm::vec3> positions;
  std::vector<glm::vec3> colors;
  EXPECT_FALSE(decodeWorldAdd(add.data(),
                              add.size() - 1,
                              replace,
                              size,
                              positions,
                              colors));

  auto snapshot = encodeWorldSnapshot(1, 2, 3, std::vector<WorldSyncVoxel>{});
  std::uint64_t seq = 0;
  std::vector<WorldSyncVoxel> voxels;
  EXPECT_TRUE(decodeWorldSnapshot(
    snapshot.data(), snapshot.size(), seq, voxels));
  EXPECT_TRUE(voxels.empty());
  EXPECT_FALSE(decodeWorldSnapshot(snapshot.data(), 16, seq, voxels));
}
