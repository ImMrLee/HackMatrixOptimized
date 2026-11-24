#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include <glm/glm.hpp>

namespace MultiPlayer {

// F14a authoritative world sync travels on channel 8.
// The relay elects the first connected client as the world authority and
// broadcasts Authority. The authority applies local edits and broadcasts
// sequenced ops; other clients forward their edits to the authority and apply
// the authoritative broadcast. Late joiners request a Snapshot.
//
// Header: type u8, targetOwner u32, senderID u32, seq u64.
// targetOwner == 0 means broadcast; otherwise the relay unicasts to that peer.
// Reserved authority ID used when the dedicated server owns the world. ENet
// connect IDs are random; this sentinel is never treated as a peer target.
constexpr std::uint32_t kServerAuthorityID = 0xFFFFFFFFu;

constexpr std::size_t kWorldSyncHeaderSize = 17;
constexpr std::size_t kWorldSyncTargetOffset = 1;
constexpr std::size_t kWorldSyncSenderOffset = 5;

enum class WorldSyncType : std::uint8_t
{
  Authority = 1,
  Add = 2,
  ClearBox = 3,
  ClearPositions = 4,
  SnapshotRequest = 5,
  Snapshot = 6,
  // v2: runtime entity transforms (DB-persisted scene entities).
  EntityTransform = 7,
  EntityMove = 8,
  EntitySnapshot = 9,
};

// Entity transform payload shared by EntityTransform and EntitySnapshot.
struct WorldSyncEntityTransform
{
  std::uint32_t entityID = 0;
  glm::vec3 position{ 0.0f };
  glm::vec3 origin{ 0.0f };
  glm::vec3 rotate{ 0.0f };
  float scale = 1.0f;
};

struct WorldSyncHeader
{
  WorldSyncType type = WorldSyncType::Authority;
  std::uint32_t targetOwner = 0;
  std::uint32_t senderID = 0;
  std::uint64_t seq = 0;
};

struct WorldSyncVoxel
{
  glm::vec3 position{ 0.0f };
  float edge = 0.1f;
  glm::vec3 color{ 1.0f };
};

namespace world_sync_detail {
inline void appendU8(std::vector<std::uint8_t>& out, std::uint8_t v)
{
  out.push_back(v);
}
inline void appendU32(std::vector<std::uint8_t>& out, std::uint32_t v)
{
  for (int i = 0; i < 4; ++i) {
    out.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xff));
  }
}
inline void appendU64(std::vector<std::uint8_t>& out, std::uint64_t v)
{
  for (int i = 0; i < 8; ++i) {
    out.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xff));
  }
}
inline void appendF32(std::vector<std::uint8_t>& out, float v)
{
  std::uint32_t bits = 0;
  std::memcpy(&bits, &v, 4);
  appendU32(out, bits);
}
inline void appendVec3(std::vector<std::uint8_t>& out, const glm::vec3& v)
{
  appendF32(out, v.x);
  appendF32(out, v.y);
  appendF32(out, v.z);
}
inline void appendHeader(std::vector<std::uint8_t>& out,
                         WorldSyncType type,
                         std::uint32_t targetOwner,
                         std::uint32_t senderID,
                         std::uint64_t seq)
{
  appendU8(out, static_cast<std::uint8_t>(type));
  appendU32(out, targetOwner);
  appendU32(out, senderID);
  appendU64(out, seq);
}
struct Reader
{
  const std::uint8_t* data = nullptr;
  std::size_t size = 0;
  std::size_t offset = 0;
  bool u8(std::uint8_t& v)
  {
    if (offset + 1 > size) return false;
    v = data[offset++];
    return true;
  }
  bool u32(std::uint32_t& v)
  {
    if (offset + 4 > size) return false;
    v = 0;
    for (int i = 0; i < 4; ++i) {
      v |= static_cast<std::uint32_t>(data[offset + i]) << (8 * i);
    }
    offset += 4;
    return true;
  }
  bool u64(std::uint64_t& v)
  {
    if (offset + 8 > size) return false;
    v = 0;
    for (int i = 0; i < 8; ++i) {
      v |= static_cast<std::uint64_t>(data[offset + i]) << (8 * i);
    }
    offset += 8;
    return true;
  }
  bool f32(float& v)
  {
    std::uint32_t bits = 0;
    if (!u32(bits)) return false;
    std::memcpy(&v, &bits, 4);
    return true;
  }
  bool vec3(glm::vec3& v)
  {
    return f32(v.x) && f32(v.y) && f32(v.z);
  }
};
} // namespace world_sync_detail

inline bool
decodeWorldSyncHeader(const std::uint8_t* data,
                      std::size_t size,
                      WorldSyncHeader& header)
{
  if (data == nullptr || size < kWorldSyncHeaderSize) {
    return false;
  }
  world_sync_detail::Reader r{ data, size, 0 };
  std::uint8_t type = 0;
  if (!r.u8(type)) return false;
  header.type = static_cast<WorldSyncType>(type);
  return r.u32(header.targetOwner) && r.u32(header.senderID) &&
         r.u64(header.seq);
}

inline void
setWorldSyncSender(std::uint8_t* data, std::size_t size, std::uint32_t senderID)
{
  if (data == nullptr || size < kWorldSyncHeaderSize) {
    return;
  }
  for (int i = 0; i < 4; ++i) {
    data[kWorldSyncSenderOffset + i] =
      static_cast<std::uint8_t>((senderID >> (8 * i)) & 0xff);
  }
}

inline bool
worldSyncTargetOwner(const std::uint8_t* data,
                     std::size_t size,
                     std::uint32_t& targetOwner)
{
  WorldSyncHeader header;
  if (!decodeWorldSyncHeader(data, size, header)) {
    return false;
  }
  targetOwner = header.targetOwner;
  return true;
}

inline std::vector<std::uint8_t>
encodeWorldAuthority(std::uint32_t authorityID)
{
  std::vector<std::uint8_t> out;
  world_sync_detail::appendHeader(
    out, WorldSyncType::Authority, 0, 0, 0);
  world_sync_detail::appendU32(out, authorityID);
  return out;
}

inline bool
decodeWorldAuthority(const std::uint8_t* data,
                     std::size_t size,
                     std::uint32_t& authorityID)
{
  WorldSyncHeader header;
  if (!decodeWorldSyncHeader(data, size, header) ||
      header.type != WorldSyncType::Authority) {
    return false;
  }
  world_sync_detail::Reader r{ data, size, kWorldSyncHeaderSize };
  return r.u32(authorityID);
}

inline std::vector<std::uint8_t>
encodeWorldAdd(std::uint32_t senderID,
               std::uint64_t seq,
               bool replace,
               float size,
               const std::vector<glm::vec3>& positions,
               const std::vector<glm::vec3>& colors)
{
  using namespace world_sync_detail;
  std::vector<std::uint8_t> out;
  appendHeader(out, WorldSyncType::Add, 0, senderID, seq);
  appendU8(out, replace ? 1 : 0);
  appendF32(out, size);
  appendU32(out, static_cast<std::uint32_t>(positions.size()));
  for (std::size_t i = 0; i < positions.size(); ++i) {
    appendVec3(out, positions[i]);
    appendVec3(out, i < colors.size() ? colors[i] : glm::vec3(1.0f));
  }
  return out;
}

inline bool
decodeWorldAdd(const std::uint8_t* data,
               std::size_t size,
               bool& replace,
               float& voxelSize,
               std::vector<glm::vec3>& positions,
               std::vector<glm::vec3>& colors)
{
  WorldSyncHeader header;
  if (!decodeWorldSyncHeader(data, size, header) ||
      header.type != WorldSyncType::Add) {
    return false;
  }
  world_sync_detail::Reader r{ data, size, kWorldSyncHeaderSize };
  std::uint8_t rep = 0;
  std::uint32_t count = 0;
  if (!r.u8(rep) || !r.f32(voxelSize) || !r.u32(count)) {
    return false;
  }
  replace = rep != 0;
  positions.clear();
  colors.clear();
  positions.reserve(count);
  colors.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    glm::vec3 p;
    glm::vec3 c;
    if (!r.vec3(p) || !r.vec3(c)) {
      return false;
    }
    positions.push_back(p);
    colors.push_back(c);
  }
  return true;
}

inline std::vector<std::uint8_t>
encodeWorldClearBox(std::uint32_t senderID,
                    std::uint64_t seq,
                    const glm::vec3& min,
                    const glm::vec3& max)
{
  using namespace world_sync_detail;
  std::vector<std::uint8_t> out;
  appendHeader(out, WorldSyncType::ClearBox, 0, senderID, seq);
  appendVec3(out, min);
  appendVec3(out, max);
  return out;
}

inline bool
decodeWorldClearBox(const std::uint8_t* data,
                    std::size_t size,
                    glm::vec3& min,
                    glm::vec3& max)
{
  WorldSyncHeader header;
  if (!decodeWorldSyncHeader(data, size, header) ||
      header.type != WorldSyncType::ClearBox) {
    return false;
  }
  world_sync_detail::Reader r{ data, size, kWorldSyncHeaderSize };
  return r.vec3(min) && r.vec3(max);
}

inline std::vector<std::uint8_t>
encodeWorldClearPositions(std::uint32_t senderID,
                          std::uint64_t seq,
                          const std::vector<glm::vec3>& positions)
{
  using namespace world_sync_detail;
  std::vector<std::uint8_t> out;
  appendHeader(out, WorldSyncType::ClearPositions, 0, senderID, seq);
  appendU32(out, static_cast<std::uint32_t>(positions.size()));
  for (const auto& p : positions) {
    appendVec3(out, p);
  }
  return out;
}

inline bool
decodeWorldClearPositions(const std::uint8_t* data,
                          std::size_t size,
                          std::vector<glm::vec3>& positions)
{
  WorldSyncHeader header;
  if (!decodeWorldSyncHeader(data, size, header) ||
      header.type != WorldSyncType::ClearPositions) {
    return false;
  }
  world_sync_detail::Reader r{ data, size, kWorldSyncHeaderSize };
  std::uint32_t count = 0;
  if (!r.u32(count)) {
    return false;
  }
  positions.clear();
  positions.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    glm::vec3 p;
    if (!r.vec3(p)) {
      return false;
    }
    positions.push_back(p);
  }
  return true;
}

inline std::vector<std::uint8_t>
encodeWorldSnapshotRequest(std::uint32_t targetOwner, std::uint32_t senderID)
{
  std::vector<std::uint8_t> out;
  world_sync_detail::appendHeader(
    out, WorldSyncType::SnapshotRequest, targetOwner, senderID, 0);
  return out;
}

inline std::vector<std::uint8_t>
encodeWorldSnapshot(std::uint32_t targetOwner,
                    std::uint32_t senderID,
                    std::uint64_t seq,
                    const std::vector<WorldSyncVoxel>& voxels)
{
  using namespace world_sync_detail;
  std::vector<std::uint8_t> out;
  appendHeader(out, WorldSyncType::Snapshot, targetOwner, senderID, seq);
  appendU32(out, static_cast<std::uint32_t>(voxels.size()));
  for (const auto& v : voxels) {
    appendVec3(out, v.position);
    appendF32(out, v.edge);
    appendVec3(out, v.color);
  }
  return out;
}

inline bool
decodeWorldSnapshot(const std::uint8_t* data,
                    std::size_t size,
                    std::uint64_t& seq,
                    std::vector<WorldSyncVoxel>& voxels)
{
  WorldSyncHeader header;
  if (!decodeWorldSyncHeader(data, size, header) ||
      header.type != WorldSyncType::Snapshot) {
    return false;
  }
  seq = header.seq;
  world_sync_detail::Reader r{ data, size, kWorldSyncHeaderSize };
  std::uint32_t count = 0;
  if (!r.u32(count)) {
    return false;
  }
  voxels.clear();
  voxels.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    WorldSyncVoxel v;
    if (!r.vec3(v.position) || !r.f32(v.edge) || !r.vec3(v.color)) {
      return false;
    }
    voxels.push_back(v);
  }
  return true;
}

// --- v2: entity transforms ------------------------------------------------

inline void
appendEntityTransform(std::vector<std::uint8_t>& out,
                      const WorldSyncEntityTransform& t)
{
  using namespace world_sync_detail;
  appendU32(out, t.entityID);
  appendVec3(out, t.position);
  appendVec3(out, t.origin);
  appendVec3(out, t.rotate);
  appendF32(out, t.scale);
}

inline bool
readEntityTransform(world_sync_detail::Reader& r,
                    WorldSyncEntityTransform& t)
{
  return r.u32(t.entityID) && r.vec3(t.position) && r.vec3(t.origin) &&
         r.vec3(t.rotate) && r.f32(t.scale);
}

inline std::vector<std::uint8_t>
encodeWorldEntityTransform(std::uint32_t senderID,
                           std::uint64_t seq,
                           const WorldSyncEntityTransform& transform)
{
  std::vector<std::uint8_t> out;
  world_sync_detail::appendHeader(
    out, WorldSyncType::EntityTransform, 0, senderID, seq);
  appendEntityTransform(out, transform);
  return out;
}

// Forwarded from a non-authority to the authority (absolute transform).
inline std::vector<std::uint8_t>
encodeWorldEntityEdit(std::uint32_t targetOwner,
                      std::uint32_t senderID,
                      const WorldSyncEntityTransform& transform)
{
  std::vector<std::uint8_t> out;
  world_sync_detail::appendHeader(
    out, WorldSyncType::EntityTransform, targetOwner, senderID, 0);
  appendEntityTransform(out, transform);
  return out;
}

inline bool
decodeWorldEntityTransform(const std::uint8_t* data,
                           std::size_t size,
                           WorldSyncEntityTransform& transform)
{
  WorldSyncHeader header;
  if (!decodeWorldSyncHeader(data, size, header) ||
      header.type != WorldSyncType::EntityTransform) {
    return false;
  }
  world_sync_detail::Reader r{ data, size, kWorldSyncHeaderSize };
  return readEntityTransform(r, transform);
}

inline std::vector<std::uint8_t>
encodeWorldEntityMove(std::uint32_t targetOwner,
                      std::uint32_t senderID,
                      std::uint32_t entityID,
                      const glm::vec3& delta,
                      float unitsPerSecond)
{
  using namespace world_sync_detail;
  std::vector<std::uint8_t> out;
  appendHeader(out, WorldSyncType::EntityMove, targetOwner, senderID, 0);
  appendU32(out, entityID);
  appendVec3(out, delta);
  appendF32(out, unitsPerSecond);
  return out;
}

inline bool
decodeWorldEntityMove(const std::uint8_t* data,
                      std::size_t size,
                      std::uint32_t& entityID,
                      glm::vec3& delta,
                      float& unitsPerSecond)
{
  WorldSyncHeader header;
  if (!decodeWorldSyncHeader(data, size, header) ||
      header.type != WorldSyncType::EntityMove) {
    return false;
  }
  world_sync_detail::Reader r{ data, size, kWorldSyncHeaderSize };
  return r.u32(entityID) && r.vec3(delta) && r.f32(unitsPerSecond);
}

inline std::vector<std::uint8_t>
encodeWorldEntitySnapshot(
  std::uint32_t targetOwner,
  std::uint32_t senderID,
  std::uint64_t seq,
  const std::vector<WorldSyncEntityTransform>& entities)
{
  using namespace world_sync_detail;
  std::vector<std::uint8_t> out;
  appendHeader(out, WorldSyncType::EntitySnapshot, targetOwner, senderID, seq);
  appendU32(out, static_cast<std::uint32_t>(entities.size()));
  for (const auto& t : entities) {
    appendEntityTransform(out, t);
  }
  return out;
}

inline bool
decodeWorldEntitySnapshot(
  const std::uint8_t* data,
  std::size_t size,
  std::uint64_t& seq,
  std::vector<WorldSyncEntityTransform>& entities)
{
  WorldSyncHeader header;
  if (!decodeWorldSyncHeader(data, size, header) ||
      header.type != WorldSyncType::EntitySnapshot) {
    return false;
  }
  seq = header.seq;
  world_sync_detail::Reader r{ data, size, kWorldSyncHeaderSize };
  std::uint32_t count = 0;
  if (!r.u32(count)) {
    return false;
  }
  entities.clear();
  entities.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    WorldSyncEntityTransform t;
    if (!readEntityTransform(r, t)) {
      return false;
    }
    entities.push_back(t);
  }
  return true;
}

} // namespace MultiPlayer