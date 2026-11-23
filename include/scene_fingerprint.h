#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

class EntityRegistry;
namespace SQLite {
class Database;
}

namespace MultiPlayer {

// Stable fingerprint of the scene inputs that both peers must agree on before
// they can share windows: the SQLite entity database and the model files it
// references (OBJ/MTL/textures).
//
// This is a consistency check, not a cryptographic signature.
struct SceneFingerprint
{
  uint64_t hi = 0;
  uint64_t lo = 0;
  bool valid = false;

  bool operator==(const SceneFingerprint& other) const
  {
    return valid == other.valid &&
           (!valid || (hi == other.hi && lo == other.lo));
  }
  bool operator!=(const SceneFingerprint& other) const
  {
    return !(*this == other);
  }

  std::string hex() const;
};

SceneFingerprint computeSceneFingerprint(SQLite::Database& database,
                                         const std::string& databaseFile);
SceneFingerprint computeSceneFingerprint(EntityRegistry& registry,
                                         const std::string& databaseFile);

// Wire format used on the relay channel (channel 3). Fixed size and explicit
// little-endian fields so both ends agree regardless of struct padding.
// Header-only so the lightweight relay server does not need the SQLite/entity
// dependencies that the fingerprint computation pulls in.
constexpr std::size_t kSceneInfoPacketSize = 24;

inline std::array<uint8_t, kSceneInfoPacketSize> encodeSceneInfo(
  uint32_t playerID,
  const SceneFingerprint& fingerprint)
{
  std::array<uint8_t, kSceneInfoPacketSize> out{};
  out[0] = 'S';
  out[1] = 'F';
  out[2] = 'P';
  out[3] = '1';

  for (int i = 0; i < 4; ++i) {
    out[4 + i] = static_cast<uint8_t>((playerID >> (8 * i)) & 0xff);
  }
  for (int i = 0; i < 8; ++i) {
    out[8 + i] = static_cast<uint8_t>((fingerprint.hi >> (8 * i)) & 0xff);
    out[16 + i] = static_cast<uint8_t>((fingerprint.lo >> (8 * i)) & 0xff);
  }
  return out;
}

inline bool decodeSceneInfo(const uint8_t* data,
                            std::size_t size,
                            uint32_t& playerID,
                            SceneFingerprint& fingerprint)
{
  if (!data || size < kSceneInfoPacketSize ||
      std::memcmp(data, "SFP1", 4) != 0) {
    return false;
  }

  playerID = 0;
  for (int i = 0; i < 4; ++i) {
    playerID |= static_cast<uint32_t>(data[4 + i]) << (8 * i);
  }

  fingerprint = SceneFingerprint{};
  for (int i = 0; i < 8; ++i) {
    fingerprint.hi |= static_cast<uint64_t>(data[8 + i]) << (8 * i);
    fingerprint.lo |= static_cast<uint64_t>(data[16 + i]) << (8 * i);
  }
  fingerprint.valid = true;
  return true;
}

} // namespace MultiPlayer
