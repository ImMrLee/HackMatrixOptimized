#define ENET_IMPLEMENTATION
#include "MultiPlayer/Server.h"

#include <sqlite3.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

namespace {

std::atomic_bool g_stop{ false };

void
handleSignal(int)
{
  g_stop.store(true);
}

bool
tableExists(sqlite3* db, const char* table)
{
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db,
                         "SELECT 1 FROM sqlite_master WHERE type='table' AND "
                         "name=?1",
                         -1,
                         &stmt,
                         nullptr) != SQLITE_OK) {
    return false;
  }
  sqlite3_bind_text(stmt, 1, table, -1, SQLITE_STATIC);
  const bool exists = sqlite3_step(stmt) == SQLITE_ROW;
  sqlite3_finalize(stmt);
  return exists;
}

long long
tableCount(sqlite3* db, const char* table)
{
  if (!tableExists(db, table)) {
    return -1;
  }
  const std::string sql = std::string("SELECT COUNT(*) FROM \"") + table + "\"";
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
    return -1;
  }
  long long count = -1;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    count = sqlite3_column_int64(stmt, 0);
  }
  sqlite3_finalize(stmt);
  return count;
}

void
printUsage(const char* argv0)
{
  std::fprintf(stderr,
               "usage: %s [--port N] [--db PATH]\n"
               "  --port N    UDP port for ENet clients (default 1234)\n"
               "  --db PATH   scene database (default db/matrix.db)\n",
               argv0);
}

} // namespace

int
main(int argc, char** argv)
{
  int port = 1234;
  std::string databasePath = "db/matrix.db";

  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
      port = std::atoi(argv[++i]);
    } else if (std::strcmp(argv[i], "--db") == 0 && i + 1 < argc) {
      databasePath = argv[++i];
    } else if (std::strcmp(argv[i], "--help") == 0 ||
               std::strcmp(argv[i], "-h") == 0) {
      printUsage(argv[0]);
      return 0;
    } else {
      std::fprintf(stderr, "unknown argument: %s\n", argv[i]);
      printUsage(argv[0]);
      return 2;
    }
  }
  if (port <= 0 || port > 65535) {
    std::fprintf(stderr, "invalid port: %d\n", port);
    return 2;
  }

  auto logger = spdlog::stdout_color_mt("matrix-server");
  logger->set_level(spdlog::level::info);
  logger->flush_on(spdlog::level::info);
  spdlog::set_default_logger(logger);

  // Read-only DB inspection. The simulation module will own the real registry;
  // this first server target only validates the headless module boundary.
  sqlite3* db = nullptr;
  if (sqlite3_open_v2(databasePath.c_str(),
                      &db,
                      SQLITE_OPEN_READONLY,
                      nullptr) != SQLITE_OK) {
    spdlog::error("cannot open scene database '{}': {}",
                  databasePath,
                  db != nullptr ? sqlite3_errmsg(db) : "unknown error");
    if (db != nullptr) {
      sqlite3_close(db);
    }
    return 1;
  }

  for (const char* table :
       { "Entity", "Positionable", "Model", "Light", "Bootable",
         "Scriptable", "Door", "Key", "Lock" }) {
    const long long count = tableCount(db, table);
    if (count >= 0) {
      spdlog::info("db: {} = {}", table, count);
    } else {
      spdlog::warn("db: {} (missing)", table);
    }
  }
  sqlite3_close(db);

  std::signal(SIGINT, handleSignal);
  std::signal(SIGTERM, handleSignal);

  MultiPlayer::Server server;
  if (!server.Start(port)) {
    spdlog::error("failed to start ENet server on UDP {}", port);
    return 1;
  }
  spdlog::info("matrix-server listening on UDP {} (db {})", port, databasePath);

  while (!g_stop.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
  }

  spdlog::info("matrix-server stopping ({} client(s) online)",
               server.clientCount());
  server.Stop();
  return 0;
}
