#pragma once

#include <enet/enet.h>
#include <thread>
#include <atomic>
#include <glm/glm.hpp>
#include "scene_fingerprint.h"

namespace MultiPlayer {

struct PlayerUpdate
{
  uint32_t playerID;
  glm::vec3 position;
  glm::vec3 front;
};

enum CHANNEL_TYPE
{
  PLAYER_UPDATE = 1,
  PLAYER_JOIN = 2,
  SCENE_INFO = 3,
  WINDOW_STREAM = 4,
  WINDOW_CONTROL = 5,
  WINDOW_VIDEO = 6,
  WEBRTC_SIGNAL = 7,
  WORLD_SYNC = 8
};

class Server
{
public:
  Server();
  ~Server();

  bool Start(int port);
  void Stop();
  void PollLoop();
  bool IsRunning();
  // Number of currently connected peers (relay sessions).
  uint32_t clientCount() const { return connectedClients.load(); }

private:
  ENetHost* server;
  std::atomic<bool> isRunning;
  std::atomic<uint32_t> connectedClients{ 0 };
  std::thread pollThread;
  PlayerUpdate getPlayerUpdateFromEvent(ENetEvent&);
  void broadcastPlayerUpdate(PlayerUpdate);
  void broadcastSceneInfo(uint32_t playerID,
                          const SceneFingerprint& fingerprint);
};

}
