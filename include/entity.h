#pragma once

#include <SQLiteCpp/SQLiteCpp.h>
#include <entt.hpp>
#include "persister.h"
#include <vector>
#include <memory>
#include <optional>
#include <string>

class EntityRegistry : public entt::registry
{
  std::shared_ptr<SQLite::Database> db;
  std::string databaseFile;
  std::vector<std::shared_ptr<SQLPersister>> persisters;
  std::map<int, entt::entity> entityLocator;

public:
  EntityRegistry();
  // Open an explicit scene database (used by the headless server).
  explicit EntityRegistry(const std::string& databaseFile);
  SQLite::Database &getDatabase();
  const std::string& getDatabaseFile() const { return databaseFile; }
  void addPersister(std::shared_ptr<SQLPersister>);
  void depersist(entt::entity);
  entt::entity createPersistent();
  void createTablesIfNeeded();
  void saveAll();
  void save(entt::entity);
  void loadAll();
  void load(entt::entity);
  template<typename T>
  void removePersistent(entt::entity entity)
  {
    remove<T>(entity);
    for (auto persister : persisters) {
      persister->depersistIfGone(entity);
    }
  };

  std::optional<entt::entity> locateEntity(int entityIdForDB);
};
