#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <glm/glm.hpp>

#include "world_sync_protocol.h"

class EntityRegistry;
class DynamicObjectSpace;

namespace server {

// Headless authoritative world. It owns the scene entities, the runtime voxel
// overlay and the entity-transform sequence. The ENet poll thread forwards
// client edits here; the main thread ticks the simulation. Both paths lock
// mutex_.
class WorldSimulation
{
public:
  WorldSimulation();
  ~WorldSimulation();

  bool load(const std::string& databasePath, std::string& error);
  // Fixed-step simulation tick. Safe without a Renderer or GL context.
  void tick();
  // Persist Positionable rows and the runtime voxel overlay to SQLite.
  bool save(std::string* error = nullptr);
  bool hasPendingChanges() const;

  std::size_t entityCount() const;
  std::size_t positionableCount() const;

  using Broadcast = std::function<void(const std::vector<std::uint8_t>&)>;
  using SendTo =
    std::function<void(std::uint32_t, const std::vector<std::uint8_t>&)>;
  void setBroadcast(Broadcast broadcast);
  void setSendTo(SendTo sendTo);

  // Called from the ENet poll thread after the relay stamps the real sender.
  // The target owner is the server sentinel; apply and broadcast.
  void handleClientWorldSync(std::uint32_t sender,
                             const std::uint8_t* data,
                             std::size_t size);

private:
  struct CachedTransform
  {
    glm::vec3 position{ 0.0f };
    glm::vec3 origin{ 0.0f };
    glm::vec3 rotate{ 0.0f };
    float scale = 1.0f;
  };

  void loadVoxelsLocked();
  void applyAddLocked(bool replace,
                      float size,
                      const std::vector<glm::vec3>& positions,
                      const std::vector<glm::vec3>& colors);
  void applyClearBoxLocked(const glm::vec3& min, const glm::vec3& max);
  void applyClearPositionsLocked(const std::vector<glm::vec3>& positions);
  void applyEntityTransformLocked(
    const MultiPlayer::WorldSyncEntityTransform& transform);
  static MultiPlayer::WorldSyncEntityTransform toProto(
    std::uint32_t entityId,
    const CachedTransform& transform);
  void scanEntitiesLocked(std::vector<std::vector<std::uint8_t>>& outgoing);
  void makeSnapshotLocked(std::uint32_t peer,
                          std::vector<std::vector<std::uint8_t>>& outgoing);
  std::vector<MultiPlayer::WorldSyncVoxel> voxelSnapshotLocked() const;

  std::shared_ptr<EntityRegistry> registry_;
  std::shared_ptr<DynamicObjectSpace> dynamicObjects_;
  std::unordered_set<int> apiIds_;
  std::unordered_map<std::uint32_t, CachedTransform> cachedTransforms_;
  std::uint64_t seq_ = 0;
  bool dirty_ = false;
  Broadcast broadcast_;
  SendTo sendTo_;
  mutable std::mutex mutex_;
};

} // namespace server
