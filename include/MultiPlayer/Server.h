#pragma once

#include <enet/enet.h>
#include <thread>
#include <atomic>
#include <functional>
#include <mutex>
#include <vector>
#include <glm/glm.hpp>
#include <unordered_map>
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
  WORLD_SYNC = 8,
  PLAYER_LEAVE = 9
};

class Server
{
public:
  // authoritative=true: the server owns the world authority (dedicated
  // server). false: the first connected client is elected (relay/bootServer
  // and client-hosted sessions).
  explicit Server(bool authoritative = false);
  ~Server();

  bool Start(int port);
  void Stop();
  void PollLoop();
  bool IsRunning();
  // Number of currently connected peers (relay sessions).
  uint32_t clientCount() const { return connectedClients.load(); }

  // Dedicated-server hooks. The handler runs on the ENet poll thread; it must
  // be thread-safe with the server simulation tick. Outbound world-sync
  // messages are queued and flushed by the poll thread.
  using WorldSyncHandler =
    std::function<void(uint32_t sender, const uint8_t* data, size_t size)>;
  void setWorldSyncHandler(WorldSyncHandler handler);
  void broadcastWorldSync(const std::vector<uint8_t>& bytes);
  void sendWorldSyncTo(uint32_t target, const std::vector<uint8_t>& bytes);

private:
  ENetHost* server;
  std::atomic<bool> isRunning;
  bool authoritative = false;
  std::atomic<uint32_t> connectedClients{ 0 };
  std::thread pollThread;
  PlayerUpdate getPlayerUpdateFromEvent(ENetEvent&);
  void broadcastPlayerUpdate(PlayerUpdate);
  void sendPlayerUpdate(ENetPeer* peer, const PlayerUpdate& update);
  void broadcastSceneInfo(uint32_t playerID,
                          const SceneFingerprint& fingerprint);

  struct OutboundWorldSync
  {
    uint32_t target = 0; // 0 = broadcast
    std::vector<uint8_t> bytes;
  };
  void flushWorldOutbox(const std::unordered_map<uint32_t, ENetPeer*>& peers);
  WorldSyncHandler worldSyncHandler_;
  std::mutex worldOutMutex_;
  std::vector<OutboundWorldSync> worldOutbox_;
};

}
