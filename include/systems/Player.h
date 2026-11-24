#pragma once

#include "components/Player.h"
#include "entity.h"
#include <glm/glm.hpp>

namespace systems {
void
registerPlayer(std::shared_ptr<EntityRegistry> registry, uint32_t);
void
unregisterPlayer(std::shared_ptr<EntityRegistry> registry, uint32_t);
void
clearPlayers(std::shared_ptr<EntityRegistry> registry);
void
movePlayer(std::shared_ptr<EntityRegistry> registry,
           uint32_t,
           glm::vec3 position,
           glm::vec3 front,
           float time);
};
