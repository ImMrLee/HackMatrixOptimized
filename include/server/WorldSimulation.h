#pragma once

#include <cstddef>
#include <memory>
#include <string>

class EntityRegistry;

namespace server {

// Headless world state. Phase 1 loads the persistable scene and ticks the
// GL-free movement systems; voxels/players/snapshots will be added here.
class WorldSimulation
{
public:
  WorldSimulation();
  ~WorldSimulation();

  bool load(const std::string& databasePath, std::string& error);
  // Fixed-step simulation tick. Safe without a Renderer or GL context.
  void tick();

  std::size_t entityCount() const;
  std::size_t positionableCount() const;

private:
  std::shared_ptr<EntityRegistry> registry_;
};

} // namespace server
