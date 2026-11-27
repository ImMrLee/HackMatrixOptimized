#pragma once
#include "MultiPlayer/Client.h"
#include "MultiPlayer/Server.h"
#include "imgui/imgui.h"
#include "imgui/imgui_impl_opengl3.h"
#include <vector>
#include "model.h"

#include "logger.h"
#include "entity.h"

using namespace std;

class Engine;
class EngineGui
{
  Engine* engine;
  shared_ptr<EntityRegistry> registry;
  shared_ptr<LoggerVector> loggerVector;
  bool imguiGlInitialized = false;
  // wlroots presents the output FBO top-down; ImGui draws for the opposite
  // orientation. Override with HACKMATRIX_IMGUI_FLIP=0 if it renders correctly.
  bool imguiFlipY = true;
  void addComponentPanel(entt::entity, bool&);
  void renderComponentPanel(entt::entity);
  void renderEntities();

  // Multiplayer chat overlay (T to open, Enter send, Esc close).
  bool chatOpen = false;
  bool chatJustOpened = false;
  bool chatScrollToBottom = false;
  std::size_t lastChatCount = 0;
  char chatInput[1024] = {};
  void renderChat();

  // Esc-to-exit confirmation dialog.
  bool exitConfirmOpen = false;
  void renderExitConfirm();

public:
  bool isChatOpen() const { return chatOpen; }
  bool isExitConfirmOpen() const { return exitConfirmOpen; }
  void openExitConfirm() { exitConfirmOpen = true; }
  EngineGui(Engine* engine,
            shared_ptr<EntityRegistry> registry,
            shared_ptr<LoggerVector>);
  void render(double&, int, vector<double>&);
  shared_ptr<LoggerVector> getLoggerVector();
  void createNewEntity();
};
