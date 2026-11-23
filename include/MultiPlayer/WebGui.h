#pragma once

#include <imgui.h>
#include <atomic>
#include <memory>
#include <optional>
#include <string>

#ifdef True
#undef True
#endif

#ifdef False
#undef False
#endif

#include <crow.h>

#include <thread>

class Engine;
namespace MultiPlayer {

class WebGui
{
public:
  WebGui(Engine*);
  ~WebGui();

  void Render();

private:
  std::thread serverThread;
  crow::SimpleApp app;
  std::atomic<bool> started{ false };
  int port;
  Engine* engine;
};

}
