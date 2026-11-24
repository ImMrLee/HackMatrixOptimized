#include "model.h"

#include <sstream>

#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/transform.hpp>

#include <spdlog/spdlog.h>

void
Positionable::update()
{
  modelMatrix = glm::mat4(1.0f);
  modelMatrix = glm::translate(modelMatrix, pos);

  // Create individual rotation quaternions
  glm::quat finalRotation = glm::quat(glm::radians(rotate));

  modelMatrix = modelMatrix * glm::mat4_cast(finalRotation);
  modelMatrix = glm::translate(modelMatrix, origin * glm::vec3(-1));
  modelMatrix = glm::scale(modelMatrix, glm::vec3(scale, scale, scale));
  glm::mat4 inverseModelMatrix = glm::inverse(modelMatrix);
  glm::mat4 transposedInverse = glm::transpose(inverseModelMatrix);
  normalMatrix = glm::mat3(transposedInverse);
  damaged = false;
}

void
Positionable::damage()
{
  damaged = true;
}

Positionable::Positionable(glm::vec3 pos,
                           glm::vec3 origin,
                           glm::vec3 rotate,
                           float scale)
  : pos(pos)
  , origin(origin)
  , scale(scale)
  , rotate(rotate)
{
}

Positionable::Positionable(Positionable* p) {
  Positionable(p->pos, p->origin, p->rotate, p->scale);
}

void
PositionablePersister::createTablesIfNeeded()
{
  stringstream queryStream;
  queryStream << "CREATE TABLE IF NOT EXISTS " << entityName << " ("
              << "entity_id INTEGER PRIMARY KEY, "
              << "pos_x REAL, pos_y REAL, pos_z REAL, "
              << "origin_x REAL, origin_y REAL, origin_z REAL, scale REAL, "
              << "rot_x REAL, rot_y REAL, rot_z REAL, "
              << "FOREIGN KEY(entity_id) REFERENCES Entity(id))";
  registry->getDatabase().exec(queryStream.str());
}

void
PositionablePersister::save(entt::entity entity)
{
  auto& pos = registry->get<Positionable>(entity);
  auto& persistable = registry->get<Persistable>(entity);
  auto& db = registry->getDatabase();
  stringstream queryStream;
  queryStream << "INSERT OR REPLACE INTO " << entityName
              << " (entity_id, pos_x, pos_y, pos_z, "
              << "origin_x, origin_y, origin_z, "
              << "rot_x, rot_y, rot_z, scale)"
              << " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";
  SQLite::Statement query(db, queryStream.str());
  query.bind(1, persistable.entityId);
  query.bind(2, pos.pos.x);
  query.bind(3, pos.pos.y);
  query.bind(4, pos.pos.z);
  query.bind(5, pos.origin.x);
  query.bind(6, pos.origin.y);
  query.bind(7, pos.origin.z);
  query.bind(8, pos.rotate.x);
  query.bind(9, pos.rotate.y);
  query.bind(10, pos.rotate.z);
  query.bind(11, pos.scale);
  query.exec();
}

void
PositionablePersister::saveAll()
{
  auto view = registry->view<Persistable, Positionable>();

  SQLite::Database& db = registry->getDatabase(); // Get database reference

  stringstream queryStream;
  queryStream << "INSERT OR REPLACE INTO " << entityName
              << " (entity_id, pos_x, pos_y, pos_z, "
              << "origin_x, origin_y, origin_z, "
              << "rot_x, rot_y, rot_z, scale)"
              << " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";
  SQLite::Statement query(db, queryStream.str());
  // Use a transaction for efficiency
  db.exec("BEGIN TRANSACTION");

  for (auto [entity, persist, positionable] : view.each()) {
    query.bind(1, persist.entityId);
    query.bind(2, positionable.pos.x);
    query.bind(3, positionable.pos.y);
    query.bind(4, positionable.pos.z);
    query.bind(5, positionable.origin.x);
    query.bind(6, positionable.origin.y);
    query.bind(7, positionable.origin.z);
    query.bind(8, positionable.rotate.x);
    query.bind(9, positionable.rotate.y);
    query.bind(10, positionable.rotate.z);
    query.bind(11, positionable.scale);
    query.exec();
    query.reset(); // Important for reusing the prepared statement
  }

  db.exec("COMMIT");
}

void
PositionablePersister::load(entt::entity entity)
{
  auto peristable = registry->get<Persistable>(entity);
  std::stringstream queryStream;
  queryStream << "SELECT pos_x, pos_y, pos_z, "
              << "origin_x, origin_y, origin_z, "
              << "rot_x, rot_y, rot_z, scale FROM " << entityName
              << " WHERE entity_id = ?";
  SQLite::Statement query(registry->getDatabase(), queryStream.str());
  query.bind(1, peristable.entityId);

  if (query.executeStep()) {
    float x = query.getColumn(0).getDouble();
    float y = query.getColumn(1).getDouble();
    float z = query.getColumn(2).getDouble();
    float origin_x = query.getColumn(3).getDouble();
    float origin_y = query.getColumn(4).getDouble();
    float origin_z = query.getColumn(5).getDouble();
    float rotx = query.getColumn(6).getDouble();
    float roty = query.getColumn(7).getDouble();
    float rotz = query.getColumn(8).getDouble();
    float scale = query.getColumn(9).getDouble();

    registry->emplace<Positionable>(entity,
                                    glm::vec3(x, y, z),
                                    glm::vec3(origin_x, origin_y, origin_z),
                                    glm::vec3(rotx, roty, rotz),
                                    scale);
  }
}

void
PositionablePersister::loadAll()
{
  auto view = registry->view<Persistable>();
  SQLite::Database& db = registry->getDatabase();

  // Cache query data
  std::unordered_map<int, std::array<float, 10>> positionDataCache;

  std::stringstream queryStream;
  queryStream << "SELECT entity_id, pos_x, pos_y, pos_z, "
              << "origin_x, origin_y, origin_z, "
              << "rot_x, rot_y, rot_z, scale FROM " << entityName;
  SQLite::Statement query(db, queryStream.str());
  while (query.executeStep()) {
    int dbId = query.getColumn(0).getInt();
    float x = query.getColumn(1).getDouble();
    float y = query.getColumn(2).getDouble();
    float z = query.getColumn(3).getDouble();
    float origin_x = query.getColumn(4).getDouble();
    float origin_y = query.getColumn(5).getDouble();
    float origin_z = query.getColumn(6).getDouble();
    float rotx = query.getColumn(7).getDouble();
    float roty = query.getColumn(8).getDouble();
    float rotz = query.getColumn(9).getDouble();
    float scale = query.getColumn(10).getDouble();

    std::array<float, 10> data = {
      x, y, z, origin_x, origin_y, origin_z, rotx, roty, rotz, scale
    }; // Using temporaries
    positionDataCache[dbId] = data;
  }

  // Iterate and emplace
  for (auto [entity, persistable] : view.each()) {
    auto it = positionDataCache.find(persistable.entityId);
    if (it != positionDataCache.end()) {
      auto& [x, y, z, originX, originY, originZ, rotx, roty, rotz, scale] =
        it->second;
      registry->emplace<Positionable>(entity,
                                      glm::vec3(x, y, z),
                                      glm::vec3(originX, originY, originZ),
                                      glm::vec3(rotx, roty, rotz),
                                      scale);
    }
  }
}

void
PositionablePersister::depersistIfGone(entt::entity entity)
{
  depersistIfGoneTyped<Positionable>(entity);
}
