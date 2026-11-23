#include "MultiPlayer/Server.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <iostream>
#include <glm/glm.hpp>
#include <unordered_map>
#include <vector>

#include "webrtc_protocol.h"
#include "window_share_protocol.h"
#include "world_sync_protocol.h"
#include <unistd.h>
#include <spdlog/spdlog.h>

namespace MultiPlayer {

Server::Server()
  : server(nullptr)
  , isRunning(false)
{
  if (enet_initialize() != 0) {
    spdlog::error("MultiPlayer: failed to initialize enet (server)");
  }
}

Server::~Server()
{
  Stop();
  enet_deinitialize();
}

bool
Server::Start(int port)
{
  if (isRunning.load()) {
    return false;
  }
  if (pollThread.joinable()) {
    pollThread.join();
  }

  ENetAddress address;
  address.host = ENET_HOST_ANY;
  address.port = port;

  server = enet_host_create(&address, 32, 10, 0, 0);
  if (server == NULL) {
    return false;
  }
  isRunning = true;
  enet_host_flush(server);

  pollThread = std::thread([this]() { PollLoop(); });

  return true;
}

PlayerUpdate
Server::getPlayerUpdateFromEvent(ENetEvent& event)
{
  PlayerUpdate update;
  const auto* bytes =
    static_cast<const unsigned char*>(event.packet->data);
  std::memcpy(&update.position, bytes, sizeof(glm::vec3));
  std::memcpy(
    &update.front, bytes + sizeof(glm::vec3), sizeof(glm::vec3));
  update.playerID = event.peer->connectID;
  return update;
}

void
Server::broadcastPlayerUpdate(PlayerUpdate update)
{
  ENetPacket* packet =
    enet_packet_create(NULL,
                       sizeof(glm::vec3) * 2 + sizeof(uint32_t),
                       ENET_PACKET_FLAG_UNSEQUENCED);

  memcpy(packet->data, &update.position, sizeof(glm::vec3));
  memcpy(static_cast<unsigned char*>(packet->data) + sizeof(glm::vec3),
         &update.front,
         sizeof(glm::vec3));
  memcpy(static_cast<unsigned char*>(packet->data) + sizeof(glm::vec3) * 2,
         &update.playerID,
         sizeof(uint32_t));

  enet_host_broadcast(server, 1, packet);
  enet_host_flush(server);
}

void
Server::broadcastSceneInfo(uint32_t playerID,
                           const SceneFingerprint& fingerprint)
{
  const auto bytes = encodeSceneInfo(playerID, fingerprint);
  ENetPacket* packet = enet_packet_create(
    bytes.data(), bytes.size(), ENET_PACKET_FLAG_RELIABLE);
  if (packet == nullptr) {
    return;
  }
  enet_host_broadcast(server, SCENE_INFO, packet);
  enet_host_flush(server);
}

void
Server::PollLoop()
{
  struct CachedShare
  {
    std::vector<uint8_t> start;
    std::vector<uint8_t> transform;
    std::vector<uint8_t> controlState;
    bool hasTransform = false;
    bool hasControlState = false;
  };

  std::vector<uint32_t> clients;
  std::unordered_map<uint32_t, SceneFingerprint> peerScenes;
  std::unordered_map<uint32_t, ENetPeer*> peers;
  std::unordered_map<ENetPeer*, uint32_t> peerIDs;
  std::unordered_map<uint64_t, CachedShare> cachedShares;

  auto shareKey = [](uint32_t owner, uint32_t shareId) -> uint64_t {
    return (static_cast<uint64_t>(owner) << 32) | shareId;
  };

  auto sendCached = [&](ENetPeer* peer, const std::vector<uint8_t>& bytes) {
    if (peer == nullptr || bytes.empty()) {
      return;
    }
    ENetPacket* packet = enet_packet_create(
      bytes.data(), bytes.size(), ENET_PACKET_FLAG_RELIABLE);
    if (packet == nullptr) {
      return;
    }
    if (enet_peer_send(peer, WINDOW_STREAM, packet) != 0) {
      enet_packet_destroy(packet);
    }
  };

  auto replayCachedShares = [&](ENetPeer* peer) {
    for (const auto& [key, cached] : cachedShares) {
      (void)key;
      sendCached(peer, cached.start);
      if (cached.hasTransform) {
        sendCached(peer, cached.transform);
      }
      if (cached.hasControlState) {
        sendCached(peer, cached.controlState);
      }
    }
    enet_host_flush(server);
  };

  // F14a: the first connected client is the world authority. On its departure
  // the next remaining client takes over so world sync keeps a single source.
  uint32_t authorityID = 0;
  auto broadcastAuthority = [&]() {
    const auto bytes = encodeWorldAuthority(authorityID);
    ENetPacket* packet = enet_packet_create(
      bytes.data(), bytes.size(), ENET_PACKET_FLAG_RELIABLE);
    if (packet != nullptr) {
      enet_host_broadcast(server, WORLD_SYNC, packet);
      enet_host_flush(server);
    }
  };

  ENetEvent event;
  auto lastPing = std::chrono::steady_clock::now();
  while (isRunning) {
    // ENet only arms its peer timeout while reliable commands are in flight.
    // Ping every 2 s so a crashed client is detected instead of lingering as a
    // phantom player (and, for F14a, as a phantom world authority).
    const auto now = std::chrono::steady_clock::now();
    if (now - lastPing >= std::chrono::seconds(2)) {
      lastPing = now;
      spdlog::debug("MultiPlayer: pinging {} peer(s)", peers.size());
      for (auto& [id, peer] : peers) {
        (void)id;
        if (peer != nullptr) {
          enet_peer_ping(peer);
        }
      }
      enet_host_flush(server);
    }
    if (enet_host_service(server, &event, 0) > 0) {
      switch (event.type) {
        case ENET_EVENT_TYPE_CONNECT: {
          enet_host_flush(server);
          uint32_t playerId = event.peer->connectID;
          // ENet resets connectID to 0 before dispatching DISCONNECT, so keep
          // our own copy for cleanup and authority re-election.
          peerIDs[event.peer] = playerId;
          event.peer->data =
            reinterpret_cast<void*>(static_cast<uintptr_t>(playerId));
          // Detect a crashed client in ~5-15 s instead of ENet's 30 s default,
          // so phantom players / world authorities do not linger.
          enet_peer_timeout(event.peer, 10, 5000, 15000);

          // Send the newcomer every scene fingerprint we already know about.
          // Its own fingerprint will arrive on channel 3 and is then relayed
          // to the other peers.
          for (const auto& [id, fingerprint] : peerScenes) {
            const auto bytes = encodeSceneInfo(id, fingerprint);
            ENetPacket* scenePacket = enet_packet_create(
              bytes.data(), bytes.size(), ENET_PACKET_FLAG_RELIABLE);
            if (scenePacket != nullptr) {
              enet_peer_send(event.peer, SCENE_INFO, scenePacket);
            }
          }

          ENetPacket* packet = enet_packet_create(
            &playerId, sizeof(uint32_t), ENET_PACKET_FLAG_RELIABLE);
          enet_host_broadcast(server, PLAYER_JOIN, packet);
          enet_host_flush(server);

          for (auto client : clients) {
            ENetPacket* joinPacket = enet_packet_create(
              &client, sizeof(uint32_t), ENET_PACKET_FLAG_RELIABLE);
            enet_peer_send(event.peer, PLAYER_JOIN, joinPacket);
          }
          enet_host_flush(server);
          clients.push_back(playerId);
          peers[playerId] = event.peer;

          // Elect the world authority (first client) and inform everyone.
          if (authorityID == 0) {
            authorityID = playerId;
          }
          broadcastAuthority();
        } break;
        case ENET_EVENT_TYPE_RECEIVE:
          if (event.channelID == PLAYER_UPDATE) {
            if (event.packet->dataLength >= sizeof(glm::vec3) * 2) {
              auto update = getPlayerUpdateFromEvent(event);
              broadcastPlayerUpdate(update);
            } else {
              spdlog::warn(
                "MultiPlayer: ignored short player update ({} bytes)",
                event.packet->dataLength);
            }
          } else if (event.channelID == SCENE_INFO) {
            uint32_t playerID = event.peer->connectID;
            SceneFingerprint fingerprint;
            if (decodeSceneInfo(event.packet->data,
                                event.packet->dataLength,
                                playerID,
                                fingerprint)) {
              // Never trust a peer-supplied identity; use the ENet peer ID.
              playerID = event.peer->connectID;
              peerScenes[playerID] = fingerprint;
              broadcastSceneInfo(playerID, fingerprint);
              // Now that the newcomer has exchanged scene info, replay the
              // active shares it missed while connecting.
              replayCachedShares(event.peer);
            }
          } else if (event.channelID == WINDOW_STREAM) {
            // Relay window metadata/frames after stamping the real sender ID.
            // The payload is opaque to the server; clients validate the scene.
            if (event.packet->dataLength >= kWindowShareHeaderSize) {
              setWindowShareOwner(static_cast<uint8_t*>(event.packet->data),
                                  event.packet->dataLength,
                                  event.peer->connectID);

              uint32_t shareId = 0;
              if (windowShareId(event.packet->data,
                                event.packet->dataLength,
                                shareId)) {
                const auto key =
                  shareKey(event.peer->connectID, shareId);
                const auto type = static_cast<WindowShareMessageType>(
                  static_cast<uint8_t*>(event.packet->data)[0]);
                if (type == WindowShareMessageType::Start) {
                  CachedShare cached;
                  const auto* bytes =
                    static_cast<const uint8_t*>(event.packet->data);
                  cached.start.assign(bytes,
                                      bytes + event.packet->dataLength);
                  cachedShares[key] = std::move(cached);
                } else if (type == WindowShareMessageType::Transform) {
                  auto& cached = cachedShares[key];
                  const auto* bytes =
                    static_cast<const uint8_t*>(event.packet->data);
                  cached.transform.assign(bytes,
                                          bytes + event.packet->dataLength);
                  cached.hasTransform = true;
                } else if (type == WindowShareMessageType::ControlState) {
                  auto& cached = cachedShares[key];
                  const auto* bytes =
                    static_cast<const uint8_t*>(event.packet->data);
                  cached.controlState.assign(
                    bytes, bytes + event.packet->dataLength);
                  cached.hasControlState = true;
                } else if (type == WindowShareMessageType::Stop) {
                  cachedShares.erase(key);
                } else if (type == WindowShareMessageType::OwnerLeft) {
                  for (auto it = cachedShares.begin();
                       it != cachedShares.end();) {
                    if ((it->first >> 32) == event.peer->connectID) {
                      it = cachedShares.erase(it);
                    } else {
                      ++it;
                    }
                  }
                }
              }

              const bool isFrame =
                static_cast<WindowShareMessageType>(
                  static_cast<uint8_t*>(event.packet->data)[0]) ==
                WindowShareMessageType::Frame;
              ENetPacket* relay = enet_packet_create(
                event.packet->data,
                event.packet->dataLength,
                isFrame ? ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT
                        : ENET_PACKET_FLAG_RELIABLE);
              if (relay != nullptr) {
                enet_host_broadcast(server, WINDOW_STREAM, relay);
                enet_host_flush(server);
              }
            }
          } else if (event.channelID == WINDOW_VIDEO) {
            // H.264 access units use a reliable ordered channel so a lost
            // packet cannot corrupt the decoder state. The server only stamps
            // the sender and relays; no video history is cached.
            if (event.packet->dataLength >= kWindowShareHeaderSize) {
              setWindowShareOwner(static_cast<uint8_t*>(event.packet->data),
                                  event.packet->dataLength,
                                  event.peer->connectID);
              ENetPacket* relay = enet_packet_create(event.packet->data,
                                                     event.packet->dataLength,
                                                     ENET_PACKET_FLAG_RELIABLE);
              if (relay != nullptr) {
                enet_host_broadcast(server, WINDOW_VIDEO, relay);
                enet_host_flush(server);
              }
            }
          } else if (event.channelID == WEBRTC_SIGNAL) {
            if (event.packet->dataLength >= kWebRtcHeaderSize) {
              uint32_t targetOwner = 0;
              if (webRtcTargetOwner(event.packet->data,
                                    event.packet->dataLength,
                                    targetOwner)) {
                auto target = peers.find(targetOwner);
                if (target != peers.end() && target->second != nullptr) {
                  setWebRtcSender(
                    static_cast<uint8_t*>(event.packet->data),
                    event.packet->dataLength,
                    event.peer->connectID);
                  ENetPacket* relay = enet_packet_create(
                    event.packet->data,
                    event.packet->dataLength,
                    ENET_PACKET_FLAG_RELIABLE);
                  if (relay != nullptr &&
                      enet_peer_send(target->second, WEBRTC_SIGNAL, relay) ==
                        0) {
                    enet_host_flush(server);
                  } else if (relay != nullptr) {
                    enet_packet_destroy(relay);
                  }
                }
              }
            }
          } else if (event.channelID == WINDOW_CONTROL) {
            // Control input is unicast to the target window owner.
            if (event.packet->dataLength >= kControlHeaderSize) {
              uint32_t targetOwner = 0;
              if (controlTargetOwner(event.packet->data,
                                     event.packet->dataLength,
                                     targetOwner)) {
                auto target = peers.find(targetOwner);
                if (target != peers.end() && target->second != nullptr) {
                  setControlSender(
                    static_cast<uint8_t*>(event.packet->data),
                    event.packet->dataLength,
                    event.peer->connectID);
                  const auto type = static_cast<ControlMessageType>(
                    static_cast<uint8_t*>(event.packet->data)[0]);
                  const bool lowLatency =
                    type == ControlMessageType::PointerMotion ||
                    type == ControlMessageType::PointerAxis;
                  ENetPacket* relay = enet_packet_create(
                    event.packet->data,
                    event.packet->dataLength,
                    lowLatency ? ENET_PACKET_FLAG_UNSEQUENCED
                               : ENET_PACKET_FLAG_RELIABLE);
                  if (relay != nullptr &&
                      enet_peer_send(target->second, WINDOW_CONTROL, relay) ==
                        0) {
                    enet_host_flush(server);
                  } else if (relay != nullptr) {
                    enet_packet_destroy(relay);
                  }
                }
              }
            }
          } else if (event.channelID == WORLD_SYNC) {
            // F14a world sync: stamp the real sender, then broadcast
            // (targetOwner==0) or unicast to the authority/requester.
            if (event.packet->dataLength >= kWorldSyncHeaderSize) {
              setWorldSyncSender(static_cast<uint8_t*>(event.packet->data),
                                 event.packet->dataLength,
                                 event.peer->connectID);
              uint32_t targetOwner = 0;
              if (worldSyncTargetOwner(event.packet->data,
                                       event.packet->dataLength,
                                       targetOwner)) {
                ENetPacket* relay = enet_packet_create(
                  event.packet->data,
                  event.packet->dataLength,
                  ENET_PACKET_FLAG_RELIABLE);
                if (relay != nullptr) {
                  if (targetOwner == 0) {
                    enet_host_broadcast(server, WORLD_SYNC, relay);
                    enet_host_flush(server);
                  } else {
                    auto target = peers.find(targetOwner);
                    if (target != peers.end() && target->second != nullptr &&
                        enet_peer_send(target->second, WORLD_SYNC, relay) ==
                          0) {
                      enet_host_flush(server);
                    } else {
                      enet_packet_destroy(relay);
                    }
                  }
                }
              }
            }
          }
          enet_packet_destroy(event.packet);
          break;
        case ENET_EVENT_TYPE_DISCONNECT:
        case ENET_EVENT_TYPE_DISCONNECT_TIMEOUT: {
          uint32_t playerId =
            static_cast<uint32_t>(reinterpret_cast<uintptr_t>(event.peer->data));
          auto idIt = peerIDs.find(event.peer);
          if (idIt != peerIDs.end()) {
            playerId = idIt->second;
            peerIDs.erase(idIt);
          }
          event.peer->data = nullptr;
          spdlog::info("MultiPlayer: player {} disconnected", playerId);
          peerScenes.erase(playerId);
          peers.erase(playerId);
          for (auto it = cachedShares.begin(); it != cachedShares.end();) {
            if ((it->first >> 32) == playerId) {
              it = cachedShares.erase(it);
            } else {
              ++it;
            }
          }
          clients.erase(
            std::remove(clients.begin(), clients.end(), playerId),
            clients.end());

          // Re-elect the world authority if the departing peer held it.
          if (playerId == authorityID) {
            authorityID = clients.empty() ? 0 : clients.front();
            broadcastAuthority();
          }

          // Tell the remaining peers to drop every window shared by this
          // player, so remote windows do not outlive their owner.
          const auto left = encodeWindowShareOwnerLeft(playerId);
          ENetPacket* leftPacket = enet_packet_create(
            left.data(), left.size(), ENET_PACKET_FLAG_RELIABLE);
          if (leftPacket != nullptr) {
            enet_host_broadcast(server, WINDOW_STREAM, leftPacket);
            enet_host_flush(server);
          }
          break;
        }
        default:
          break;
      }
    } else {
      usleep(10000);
    }
  }
}

void
Server::Stop()
{
  isRunning = false;
  // The poll loop only sleeps for 10 ms, so joining here is cheap and avoids
  // destroying the ENet host while the thread is still using it.
  if (pollThread.joinable()) {
    pollThread.join();
  }
  if (server != nullptr) {
    enet_host_destroy(server);
    server = nullptr;
  }
}

bool
Server::IsRunning()
{
  return isRunning;
}
}
