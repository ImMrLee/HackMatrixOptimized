#include "server/WorldSimulation.h"

#include "entity.h"
#include "model.h"
#include "systems/ApplyRotation.h"
#include "systems/ApplyTranslation.h"

#include <exception>
#include <iterator>

namespace server {

WorldSimulation::WorldSimulation() = default;
WorldSimulation::~WorldSimulation() = default;

bool
WorldSimulation::load(const std::string& databasePath, std::string& error)
{
  try {
    registry_ = std::make_shared<EntityRegistry>(databasePath);
    registry_->addPersister(
      std::make_shared<PositionablePersister>(registry_));
    // Model/Light persistens construct GL resources, so they are deliberately
    // excluded from the headless server for now. Visual data stays on clients.
    registry_->createTablesIfNeeded();
    registry_->loadAll();
    for (auto [entity, positionable] : registry_->view<Positionable>().each()) {
      (void)entity;
      if (positionable.damaged) {
        positionable.update();
      }
    }
    return true;
  } catch (const std::exception& exception) {
    error = exception.what();
    registry_.reset();
    return false;
  }
}

void
WorldSimulation::tick()
{
  if (!registry_) {
    return;
  }
  for (auto [entity, positionable] : registry_->view<Positionable>().each()) {
    (void)entity;
    if (positionable.damaged) {
      positionable.update();
    }
  }
  systems::applyRotation(registry_);
  systems::applyTranslations(registry_);
}

std::size_t
WorldSimulation::entityCount() const
{
  if (!registry_) {
    return 0;
  }
  auto view = registry_->view<entt::entity>();
  return static_cast<std::size_t>(std::distance(view.begin(), view.end()));
}

std::size_t
WorldSimulation::positionableCount() const
{
  if (!registry_) {
    return 0;
  }
  auto view = registry_->view<Positionable>();
  return static_cast<std::size_t>(std::distance(view.begin(), view.end()));
}

} // namespace server
