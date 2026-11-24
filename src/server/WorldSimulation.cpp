#include "server/WorldSimulation.h"

#include "dynamicObject.h"
#include "entity.h"
#include "model.h"
#include "systems/ApplyRotation.h"
#include "systems/ApplyTranslation.h"
#include "systems/Move.h"

#include <algorithm>
#include <cmath>

#include <SQLiteCpp/SQLiteCpp.h>
#include <spdlog/spdlog.h>
#include <exception>
#include <iterator>
#include <memory>

namespace server {

namespace {

constexpr float kDefaultVoxelSize = 0.1f;
constexpr float kPositionEpsilon = 0.0001f;

} // namespace

void
WorldSimulation::loadVoxelsLocked()
{
  if (!registry_ || !dynamicObjects_) {
    return;
  }
  SQLite::Database& db = registry_->getDatabase();
  db.exec("CREATE TABLE IF NOT EXISTS ServerVoxels ("
          "id INTEGER PRIMARY KEY AUTOINCREMENT, "
          "pos_x REAL, pos_y REAL, pos_z REAL, "
          "edge REAL, color_r REAL, color_g REAL, color_b REAL)");
  SQLite::Statement query(
    db,
    "SELECT pos_x, pos_y, pos_z, edge, color_r, color_g, color_b "
    "FROM ServerVoxels ORDER BY id");
  while (query.executeStep()) {
    const glm::vec3 position(query.getColumn(0).getDouble(),
                             query.getColumn(1).getDouble(),
                             query.getColumn(2).getDouble());
    const float edge = static_cast<float>(query.getColumn(3).getDouble());
    const glm::vec3 color(query.getColumn(4).getDouble(),
                          query.getColumn(5).getDouble(),
                          query.getColumn(6).getDouble());
    auto cube = std::make_shared<DynamicCube>(
      position, glm::vec3(edge > 0.0f ? edge : kDefaultVoxelSize), color);
    apiIds_.insert(cube->id());
    dynamicObjects_->addObject(cube);
  }
}

bool
WorldSimulation::save(std::string* error)
{
  std::lock_guard<std::mutex> lock(mutex_);
  if (!registry_ || !dynamicObjects_) {
    if (error) {
      *error = "world not loaded";
    }
    return false;
  }
  try {
    registry_->saveAll();

    SQLite::Database& db = registry_->getDatabase();
    db.exec("CREATE TABLE IF NOT EXISTS ServerVoxels ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT, "
            "pos_x REAL, pos_y REAL, pos_z REAL, "
            "edge REAL, color_r REAL, color_g REAL, color_b REAL)");
    SQLite::Transaction transaction(db);
    db.exec("DELETE FROM ServerVoxels");
    SQLite::Statement insert(
      db,
      "INSERT INTO ServerVoxels "
      "(pos_x, pos_y, pos_z, edge, color_r, color_g, color_b) "
      "VALUES (?, ?, ?, ?, ?, ?, ?)");
    for (int id : apiIds_) {
      auto cube = std::dynamic_pointer_cast<DynamicCube>(
        dynamicObjects_->getObjectById(id));
      if (!cube) {
        continue;
      }
      const auto position = cube->getPosition();
      const auto color = cube->getColor();
      insert.bind(1, position.x);
      insert.bind(2, position.y);
      insert.bind(3, position.z);
      insert.bind(4, cube->getSize().x);
      insert.bind(5, color.r);
      insert.bind(6, color.g);
      insert.bind(7, color.b);
      insert.exec();
      insert.reset();
    }
    transaction.commit();
    dirty_ = false;
    return true;
  } catch (const std::exception& exception) {
    if (error) {
      *error = exception.what();
    }
    spdlog::error("server world save failed: {}", exception.what());
    return false;
  }
}

bool
WorldSimulation::hasPendingChanges() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  return dirty_;
}

MultiPlayer::WorldSyncEntityTransform
WorldSimulation::toProto(std::uint32_t entityId,
                         const CachedTransform& transform)
{
  MultiPlayer::WorldSyncEntityTransform out;
  out.entityID = entityId;
  out.position = transform.position;
  out.origin = transform.origin;
  out.rotate = transform.rotate;
  out.scale = transform.scale;
  return out;
}

WorldSimulation::WorldSimulation() = default;
WorldSimulation::~WorldSimulation() = default;

void
WorldSimulation::setBroadcast(Broadcast broadcast)
{
  std::lock_guard<std::mutex> lock(mutex_);
  broadcast_ = std::move(broadcast);
}

void
WorldSimulation::setSendTo(SendTo sendTo)
{
  std::lock_guard<std::mutex> lock(mutex_);
  sendTo_ = std::move(sendTo);
}

bool
WorldSimulation::load(const std::string& databasePath, std::string& error)
{
  std::lock_guard<std::mutex> lock(mutex_);
  try {
    registry_ = std::make_shared<EntityRegistry>(databasePath);
    registry_->addPersister(
      std::make_shared<PositionablePersister>(registry_));
    // Model/Light persisters construct GL resources, so they are deliberately
    // excluded from the headless server for now. Visual data stays on clients.
    registry_->createTablesIfNeeded();
    registry_->loadAll();
    dynamicObjects_ = std::make_shared<DynamicObjectSpace>();
    loadVoxelsLocked();

    cachedTransforms_.clear();
    for (auto [entity, positionable] :
         registry_->view<Positionable>().each()) {
      if (positionable.damaged) {
        positionable.update();
      }
      CachedTransform cached;
      cached.position = positionable.pos;
      cached.origin = positionable.origin;
      cached.rotate = positionable.rotate;
      cached.scale = positionable.scale;
      cachedTransforms_[static_cast<std::uint32_t>(entity)] = cached;
    }
    seq_ = 0;
    dirty_ = false;
    return true;
  } catch (const std::exception& exception) {
    error = exception.what();
    registry_.reset();
    dynamicObjects_.reset();
    cachedTransforms_.clear();
    return false;
  }
}

void
WorldSimulation::tick()
{
  std::vector<std::vector<std::uint8_t>> outgoing;
  Broadcast broadcast;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!registry_) {
      return;
    }
    broadcast = broadcast_;
    for (auto [entity, positionable] :
         registry_->view<Positionable>().each()) {
      (void)entity;
      if (positionable.damaged) {
        positionable.update();
      }
    }
    systems::applyRotation(registry_);
    systems::applyTranslations(registry_);
    if (dynamicObjects_) {
      dynamicObjects_->flushQueuedRemovals();
    }
    scanEntitiesLocked(outgoing);
  }
  for (const auto& bytes : outgoing) {
    if (broadcast) {
      broadcast(bytes);
    }
  }
}

void
WorldSimulation::scanEntitiesLocked(
  std::vector<std::vector<std::uint8_t>>& outgoing)
{
  if (!registry_) {
    return;
  }
  for (auto [entity, positionable] :
       registry_->view<Positionable>().each()) {
    const std::uint32_t id = static_cast<std::uint32_t>(entity);
    CachedTransform next;
    next.position = positionable.pos;
    next.origin = positionable.origin;
    next.rotate = positionable.rotate;
    next.scale = positionable.scale;

    auto it = cachedTransforms_.find(id);
    if (it == cachedTransforms_.end()) {
      cachedTransforms_.emplace(id, next);
      continue;
    }
    const auto& current = it->second;
    if (current.position == next.position && current.origin == next.origin &&
        current.rotate == next.rotate && current.scale == next.scale) {
      continue;
    }
    it->second = next;
    dirty_ = true;
    outgoing.push_back(MultiPlayer::encodeWorldEntityTransform(
      MultiPlayer::kServerAuthorityID, ++seq_, toProto(id, next)));
  }
}

void
WorldSimulation::applyAddLocked(bool replace,
                                float size,
                                const std::vector<glm::vec3>& positions,
                                const std::vector<glm::vec3>& colors)
{
  if (!dynamicObjects_) {
    return;
  }
  if (replace && !apiIds_.empty()) {
    std::vector<int> ids(apiIds_.begin(), apiIds_.end());
    dynamicObjects_->queueRemoveObjectsById(ids);
    apiIds_.clear();
  }
  dirty_ = true;
  const float edge = size > 0.0f ? size : kDefaultVoxelSize;
  const glm::vec3 cubeSize(edge);
  for (std::size_t i = 0; i < positions.size(); ++i) {
    const glm::vec3 color = i < colors.size() ? colors[i] : glm::vec3(1.0f);
    auto cube = std::make_shared<DynamicCube>(
      positions[i], cubeSize, color);
    apiIds_.insert(cube->id());
    dynamicObjects_->addObject(cube);
  }
}

void
WorldSimulation::applyClearBoxLocked(const glm::vec3& min,
                                     const glm::vec3& max)
{
  if (!dynamicObjects_ || apiIds_.empty()) {
    return;
  }
  const glm::vec3 lower(std::min(min.x, max.x),
                        std::min(min.y, max.y),
                        std::min(min.z, max.z));
  const glm::vec3 upper(std::max(min.x, max.x),
                        std::max(min.y, max.y),
                        std::max(min.z, max.z));
  std::vector<int> removed;
  for (int id : apiIds_) {
    auto object = dynamicObjects_->getObjectById(id);
    if (!object) {
      continue;
    }
    const auto position = object->getPosition();
    if (position.x < lower.x || position.x > upper.x ||
        position.y < lower.y || position.y > upper.y ||
        position.z < lower.z || position.z > upper.z) {
      continue;
    }
    removed.push_back(id);
  }
  for (int id : removed) {
    apiIds_.erase(id);
  }
  if (!removed.empty()) {
    dynamicObjects_->queueRemoveObjectsById(removed);
    dirty_ = true;
  }
}

void
WorldSimulation::applyClearPositionsLocked(
  const std::vector<glm::vec3>& positions)
{
  if (!dynamicObjects_ || positions.empty() || apiIds_.empty()) {
    return;
  }
  std::vector<int> removed;
  for (int id : apiIds_) {
    auto object = dynamicObjects_->getObjectById(id);
    if (!object) {
      continue;
    }
    const auto position = object->getPosition();
    for (const auto& target : positions) {
      if (glm::length(position - target) <= kPositionEpsilon) {
        removed.push_back(id);
        break;
      }
    }
  }
  for (int id : removed) {
    apiIds_.erase(id);
  }
  if (!removed.empty()) {
    dynamicObjects_->queueRemoveObjectsById(removed);
    dirty_ = true;
  }
}

void
WorldSimulation::applyEntityTransformLocked(
  const MultiPlayer::WorldSyncEntityTransform& transform)
{
  if (!registry_) {
    return;
  }
  const entt::entity entity =
    static_cast<entt::entity>(transform.entityID);
  if (!registry_->valid(entity) ||
      !registry_->any_of<Positionable>(entity)) {
    return;
  }
  auto& positionable = registry_->get<Positionable>(entity);
  positionable.pos = transform.position;
  positionable.origin = transform.origin;
  positionable.rotate = transform.rotate;
  positionable.scale = transform.scale;
  positionable.damage();
  dirty_ = true;
}

std::vector<MultiPlayer::WorldSyncVoxel>
WorldSimulation::voxelSnapshotLocked() const
{
  std::vector<MultiPlayer::WorldSyncVoxel> voxels;
  if (!dynamicObjects_) {
    return voxels;
  }
  voxels.reserve(apiIds_.size());
  for (int id : apiIds_) {
    auto cube =
      std::dynamic_pointer_cast<DynamicCube>(dynamicObjects_->getObjectById(id));
    if (!cube) {
      continue;
    }
    MultiPlayer::WorldSyncVoxel voxel;
    voxel.position = cube->getPosition();
    voxel.edge = cube->getSize().x;
    voxel.color = cube->getColor();
    voxels.push_back(voxel);
  }
  return voxels;
}

void
WorldSimulation::makeSnapshotLocked(
  std::uint32_t peer,
  std::vector<std::vector<std::uint8_t>>& outgoing)
{
  const auto voxels = voxelSnapshotLocked();
  outgoing.push_back(MultiPlayer::encodeWorldSnapshot(
    peer, MultiPlayer::kServerAuthorityID, seq_, voxels));

  std::vector<MultiPlayer::WorldSyncEntityTransform> entities;
  entities.reserve(cachedTransforms_.size());
  for (const auto& [id, transform] : cachedTransforms_) {
    entities.push_back(toProto(id, transform));
  }
  outgoing.push_back(MultiPlayer::encodeWorldEntitySnapshot(
    peer, MultiPlayer::kServerAuthorityID, seq_, entities));
}

void
WorldSimulation::handleClientWorldSync(std::uint32_t sender,
                                       const std::uint8_t* data,
                                       std::size_t size)
{
  if (data == nullptr || size < MultiPlayer::kWorldSyncHeaderSize) {
    return;
  }
  MultiPlayer::WorldSyncHeader header;
  if (!MultiPlayer::decodeWorldSyncHeader(data, size, header)) {
    return;
  }

  std::vector<std::vector<std::uint8_t>> outgoing;
  std::vector<std::vector<std::uint8_t>> direct;
  Broadcast broadcast;
  SendTo sendTo;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!registry_) {
      return;
    }
    broadcast = broadcast_;
    sendTo = sendTo_;
    switch (header.type) {
      case MultiPlayer::WorldSyncType::Add: {
        bool replace = false;
        float voxelSize = kDefaultVoxelSize;
        std::vector<glm::vec3> positions;
        std::vector<glm::vec3> colors;
        if (MultiPlayer::decodeWorldAdd(
              data, size, replace, voxelSize, positions, colors)) {
          applyAddLocked(replace, voxelSize, positions, colors);
          outgoing.push_back(MultiPlayer::encodeWorldAdd(
            MultiPlayer::kServerAuthorityID,
            ++seq_,
            replace,
            voxelSize,
            positions,
            colors));
        }
        break;
      }
      case MultiPlayer::WorldSyncType::ClearBox: {
        glm::vec3 min;
        glm::vec3 max;
        if (MultiPlayer::decodeWorldClearBox(data, size, min, max)) {
          applyClearBoxLocked(min, max);
          outgoing.push_back(MultiPlayer::encodeWorldClearBox(
            MultiPlayer::kServerAuthorityID, ++seq_, min, max));
        }
        break;
      }
      case MultiPlayer::WorldSyncType::ClearPositions: {
        std::vector<glm::vec3> positions;
        if (MultiPlayer::decodeWorldClearPositions(data, size, positions)) {
          applyClearPositionsLocked(positions);
          outgoing.push_back(MultiPlayer::encodeWorldClearPositions(
            MultiPlayer::kServerAuthorityID, ++seq_, positions));
        }
        break;
      }
      case MultiPlayer::WorldSyncType::EntityMove: {
        std::uint32_t entityId = 0;
        glm::vec3 delta{ 0.0f };
        float unitsPerSecond = 1.0f;
        if (MultiPlayer::decodeWorldEntityMove(
              data, size, entityId, delta, unitsPerSecond)) {
          const entt::entity entity = static_cast<entt::entity>(entityId);
          if (registry_->valid(entity) &&
              registry_->any_of<Positionable>(entity)) {
            systems::translate(
              registry_, entity, delta, unitsPerSecond);
          }
        }
        break;
      }
      case MultiPlayer::WorldSyncType::EntityTransform: {
        MultiPlayer::WorldSyncEntityTransform transform;
        if (MultiPlayer::decodeWorldEntityTransform(
              data, size, transform)) {
          applyEntityTransformLocked(transform);
        }
        break;
      }
      case MultiPlayer::WorldSyncType::SnapshotRequest: {
        makeSnapshotLocked(sender, direct);
        break;
      }
      default:
        break;
    }
  }

  for (const auto& bytes : outgoing) {
    if (broadcast) {
      broadcast(bytes);
    }
  }
  for (const auto& bytes : direct) {
    if (sendTo) {
      sendTo(sender, bytes);
    }
  }
}

std::size_t
WorldSimulation::entityCount() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  if (!registry_) {
    return 0;
  }
  auto view = registry_->view<entt::entity>();
  return static_cast<std::size_t>(std::distance(view.begin(), view.end()));
}

std::size_t
WorldSimulation::positionableCount() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  if (!registry_) {
    return 0;
  }
  auto view = registry_->view<Positionable>();
  return static_cast<std::size_t>(std::distance(view.begin(), view.end()));
}

} // namespace server
