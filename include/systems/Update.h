#pragma once

#include "entity.h"
#include <memory>
class Renderer;
namespace systems {
void
updateAll(std::shared_ptr<EntityRegistry>,
          Renderer* renderer,
          bool applyMovement = true);
void update(std::shared_ptr<EntityRegistry>, entt::entity);
}
