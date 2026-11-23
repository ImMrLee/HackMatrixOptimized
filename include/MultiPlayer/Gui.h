#pragma once

#include <imgui.h>
#include <string>
#include <optional>
#include <memory>
#include <unordered_map>
#include "./Client.h"
#include "./Server.h"

class Engine;
namespace MultiPlayer {

class Gui
{
public:
  Gui(Engine*);
  ~Gui();

  void Render();

  bool IsConnectButtonClicked() const;
  const std::string& GetAddress() const;
  int GetPort() const;

private:
  entt::entity createLoopbackPreview(entt::entity source);
  void destroyLoopbackPreview(entt::entity source);
  void clearLoopbackPreviews();
  void pruneLoopbackPreviews();

  bool connect;
  std::string address;
  int port;
  Engine* engine;
  std::shared_ptr<MultiPlayer::Client> client;
  std::shared_ptr<MultiPlayer::Server> server;
  // Headless UI test hooks.
  std::string debugConnect;
  bool debugConnectAttempted = false;
  bool streamSettingsInitialized = false;
  int pendingShareFps = 5;
  int pendingShareQuality = 65;
  // Local-only F14c-1 scaffolding: source window -> RemoteApp entity.
  std::unordered_map<entt::entity, entt::entity> loopbackPreviews;
};

}
