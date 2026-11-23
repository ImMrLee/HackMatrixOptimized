#define ENET_IMPLEMENTATION
#include "MultiPlayer/Server.h"
#include <unistd.h>

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

int
main()
{
  // The standalone relay has no Engine logger. Without this, every
  // spdlog::info in Server.cpp is silently discarded.
  auto logger = spdlog::stdout_color_mt("bootServer");
  logger->set_level(spdlog::level::info);
  logger->flush_on(spdlog::level::info);
  spdlog::set_default_logger(logger);

  auto server = MultiPlayer::Server();
  if (server.Start(1234)) {
    spdlog::info("MultiPlayer relay listening on UDP 1234");
  } else {
    spdlog::error("MultiPlayer relay failed to start on UDP 1234");
  }
  while(true) {
    sleep(5);
  }
}
