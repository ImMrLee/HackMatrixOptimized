#define ENET_IMPLEMENTATION
#include "MultiPlayer/Server.h"
#include "server/WorldSimulation.h"

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

  server::WorldSimulation simulation;
  std::string loadError;
  if (!simulation.load(databasePath, loadError)) {
    spdlog::error("failed to load scene database '{}': {}",
                  databasePath,
                  loadError);
    return 1;
  }
  spdlog::info("world loaded: {} entities, {} positionables",
               simulation.entityCount(),
               simulation.positionableCount());

  std::signal(SIGINT, handleSignal);
  std::signal(SIGTERM, handleSignal);

  MultiPlayer::Server server(/*authoritative=*/true);
  simulation.setBroadcast([&server](const std::vector<std::uint8_t>& bytes) {
    server.broadcastWorldSync(bytes);
  });
  simulation.setSendTo([&server](std::uint32_t target,
                                 const std::vector<std::uint8_t>& bytes) {
    server.sendWorldSyncTo(target, bytes);
  });
  server.setWorldSyncHandler(
    [&simulation](std::uint32_t sender,
                  const std::uint8_t* data,
                  std::size_t size) {
      simulation.handleClientWorldSync(sender, data, size);
    });
  if (!server.Start(port)) {
    spdlog::error("failed to start ENet server on UDP {}", port);
    return 1;
  }
  spdlog::info("matrix-server listening on UDP {} (db {})", port, databasePath);

  constexpr auto kTick = std::chrono::milliseconds(50); // 20 Hz
  auto nextTick = std::chrono::steady_clock::now();
  auto nextLog = nextTick + std::chrono::seconds(5);
  while (!g_stop.load()) {
    simulation.tick();
    nextTick += kTick;
    const auto now = std::chrono::steady_clock::now();
    if (now >= nextLog) {
      nextLog = now + std::chrono::seconds(5);
      spdlog::info("server tick: {} online, {} entities",
                   server.clientCount(),
                   simulation.entityCount());
    }
    if (nextTick > now) {
      std::this_thread::sleep_until(nextTick);
    } else {
      // Catch up without sleeping if a tick took longer than the interval.
      nextTick = now;
    }
  }

  spdlog::info("matrix-server stopping ({} client(s) online)",
               server.clientCount());
  server.Stop();
  return 0;
}
