#include "MultiPlayer/Client.h"

#include <stb/stb_image.h>

#include <SQLiteCpp/SQLiteCpp.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <exception>
#include <iostream>
#include <thread>
#include <type_traits>
#include <vector>

#include <glm/glm.hpp>

#include "Config.h"
#include "camera.h"
#include "model.h"
#include "remote_app.h"
#include "systems/Player.h"
#include "time_utils.h"
#include "video_codec.h"
#include "wayland_app.h"
#include "webrtc_manager.h"
#include "webrtc_protocol.h"
#include "window_capture.h"
#include "window_encode.h"
#include "window_share_protocol.h"

#include <spdlog/spdlog.h>

namespace MultiPlayer {
namespace {

constexpr double kWindowAdaptationIntervalSeconds = 2.0;

} // namespace

struct Client::SceneHashJob
{
  std::mutex mutex;
  bool ready = false;
  bool failed = false;
  SceneFingerprint fingerprint;
  std::string error;
};

Client::Client(shared_ptr<EntityRegistry> registry, bool persistSettings)
  : registry(registry)
  , client(nullptr)
  , peer(nullptr)
  , persistSettings_(persistSettings)
{
  if (enet_initialize() != 0) {
    spdlog::error("MultiPlayer: failed to initialize enet");
  }
}

Client::~Client()
{
  stopAllWindowShares();
  clearRemoteWindows();
  clearWindowShares();
  cleanupConnectionState();
  if (peer != nullptr) {
    enet_peer_reset(peer);
    peer = nullptr;
  }
  if (client != nullptr) {
    enet_host_destroy(client);
    client = nullptr;
  }
  encodeWorker_.reset();
  enet_deinitialize();
}

WindowEncodeWorker*
Client::ensureEncodeWorker()
{
  if (!encodeWorker_) {
    encodeWorker_ = std::make_unique<WindowEncodeWorker>();
  }
  return encodeWorker_.get();
}

bool
Client::connect(const std::string& address, int port)
{
  if (connectionState_.load() != ConnectionState::Disconnected) {
    spdlog::warn("MultiPlayer: connect requested while {}",
                 connectionStateText());
    return false;
  }
  if (peer != nullptr) {
    enet_peer_reset(peer);
    peer = nullptr;
  }
  if (client != nullptr) {
    enet_host_destroy(client);
    client = nullptr;
  }
  connectionError_.clear();
  connectDeadline_ = 0.0;
  disconnectDeadline_ = 0.0;
  sceneInfoSent_ = false;
  sceneHashJob_.reset();
  {
    std::lock_guard<std::mutex> lock(sceneMutex);
    localScene = SceneFingerprint{};
    peerScenes.clear();
  }
  {
    std::lock_guard<std::mutex> lock(shareMutex);
    pendingWindowMessages.clear();
  }

  client = enet_host_create(NULL, 1, 10, 0, 0);
  if (client == NULL) {
    connectionError_ = "failed to create ENet client";
    spdlog::error("MultiPlayer: {}", connectionError_);
    return false;
  }

  ENetAddress enetAddress;
  enet_address_set_host(&enetAddress, address.c_str());
  enetAddress.port = port;

  peer = enet_host_connect(client, &enetAddress, 10, 0);
  if (peer == NULL) {
    connectionError_ = "failed to start ENet connection";
    spdlog::error("MultiPlayer: {}", connectionError_);
    enet_host_destroy(client);
    client = nullptr;
    return false;
  }

  connectionState_ = ConnectionState::Connecting;
  connectDeadline_ = nowSeconds() + 5.0;
  spdlog::info("MultiPlayer: connecting to {}:{}", address, port);
  return true;
}

void
Client::finishConnection()
{
  connectionState_ = ConnectionState::Connected;
  connectDeadline_ = 0.0;
  connectionError_.clear();
  spdlog::info("MultiPlayer: connected to server");
  enet_host_flush(client);
  loadWindowStreamSettings();
  startSceneHash();
}

void
Client::failConnection(const std::string& reason)
{
  connectionError_ = reason;
  spdlog::warn("MultiPlayer: {}", reason);
  connectionState_ = ConnectionState::Disconnected;
  cleanupConnectionState();
  if (peer != nullptr) {
    enet_peer_reset(peer);
    peer = nullptr;
  }
  if (client != nullptr) {
    enet_host_destroy(client);
    client = nullptr;
  }
}

void
Client::finishDisconnect()
{
  if (peer != nullptr) {
    enet_peer_reset(peer);
    peer = nullptr;
  }
  if (client != nullptr) {
    enet_host_destroy(client);
    client = nullptr;
  }
  connectionState_ = ConnectionState::Disconnected;
  cleanupConnectionState();
  spdlog::info("MultiPlayer: disconnected from server");
}

void
Client::cleanupConnectionState()
{
  sceneInfoSent_ = false;
  sceneHashJob_.reset();
  {
    std::lock_guard<std::mutex> lock(sceneMutex);
    localScene = SceneFingerprint{};
    peerScenes.clear();
  }
  {
    std::lock_guard<std::mutex> lock(shareMutex);
    pendingWindowMessages.clear();
  }
  webrtc.reset();
}

void
Client::poll()
{
  if (client == nullptr) {
    if (connectionState_.load() == ConnectionState::Connecting) {
      failConnection("ENet host disappeared");
    }
    return;
  }

  ENetEvent event;
  while (client != nullptr && enet_host_service(client, &event, 0) > 0) {
    switch (event.type) {
      case ENET_EVENT_TYPE_CONNECT:
        if (connectionState_.load() == ConnectionState::Connecting) {
          finishConnection();
        }
        break;
      case ENET_EVENT_TYPE_DISCONNECT:
        if (connectionState_.load() == ConnectionState::Disconnecting) {
          finishDisconnect();
        } else if (connectionState_.load() == ConnectionState::Connected ||
                   connectionState_.load() == ConnectionState::Connecting) {
          const bool wasConnected =
            connectionState_.load() == ConnectionState::Connected;
          connectionState_ = ConnectionState::Disconnected;
          connectionError_ = "disconnected from server";
          if (wasConnected) {
            stopAllWindowShares();
            clearRemoteWindows();
            clearWindowShares();
          }
          cleanupConnectionState();
          if (peer != nullptr) {
            enet_peer_reset(peer);
            peer = nullptr;
          }
          if (client != nullptr) {
            enet_host_destroy(client);
            client = nullptr;
          }
          spdlog::warn("MultiPlayer: server closed the connection");
        }
        break;
      case ENET_EVENT_TYPE_RECEIVE:
        if (connectionState_.load() == ConnectionState::Connected) {
          handleEnetEvent(event);
        }
        enet_packet_destroy(event.packet);
        break;
      default:
        break;
    }
  }

  const double now = nowSeconds();
  if (connectionState_.load() == ConnectionState::Connecting &&
      now >= connectDeadline_) {
    failConnection("connection timed out");
  }
  if (connectionState_.load() == ConnectionState::Disconnecting &&
      now >= disconnectDeadline_) {
    finishDisconnect();
  }
  if (connectionState_.load() == ConnectionState::Connected) {
    pollSceneHash();
    if (!sceneInfoSent_) {
      sendSceneInfo();
    }
    syncWindowShares();
  }
}

void
Client::handleEnetEvent(ENetEvent& event)
{
  if (peer == nullptr) {
    return;
  }

  switch (event.channelID) {
    case 1: {
      constexpr std::size_t kPlayerUpdateBytes =
        sizeof(glm::vec3) * 2 + sizeof(uint32_t);
      if (event.packet->dataLength < kPlayerUpdateBytes) {
        spdlog::warn("MultiPlayer: short player update packet ({} bytes)",
                     event.packet->dataLength);
        return;
      }
      const auto* packetData =
        static_cast<const unsigned char*>(event.packet->data);
      glm::vec3 position;
      std::memcpy(&position, packetData, sizeof(glm::vec3));
      glm::vec3 front;
      std::memcpy(&front, packetData + sizeof(glm::vec3), sizeof(glm::vec3));
      uint32_t playerID = 0;
      std::memcpy(&playerID,
                  packetData + sizeof(glm::vec3) * 2,
                  sizeof(uint32_t));
      if (peer->connectID != playerID) {
        systems::movePlayer(registry, playerID, position, front, 1.0 / 20.0);
      }
      break;
    }
    case 2: {
      if (event.packet->dataLength < sizeof(uint32_t)) {
        spdlog::warn("MultiPlayer: short player join packet ({} bytes)",
                     event.packet->dataLength);
        return;
      }
      uint32_t playerID = 0;
      std::memcpy(&playerID, event.packet->data, sizeof(uint32_t));
      if (peer->connectID != playerID) {
        systems::registerPlayer(registry, playerID);
      }
      break;
    }
    case 3:
      handleSceneInfo(event.packet->data, event.packet->dataLength);
      break;
    case 4:
      handleWindowStream(event.packet->data, event.packet->dataLength);
      break;
    case 5:
      handleControlMessage(event.packet->data, event.packet->dataLength);
      break;
    case 6:
      handleVideoFrame(event.packet->data, event.packet->dataLength);
      break;
    case 7:
      handleWebRtcSignal(event.packet->data, event.packet->dataLength);
      break;
    default:
      break;
  }
}

void
Client::disconnect()
{
  const auto state = connectionState_.load();
  if (state == ConnectionState::Disconnected) {
    stopAllWindowShares();
    clearRemoteWindows();
    clearWindowShares();
    cleanupConnectionState();
    return;
  }
  if (state == ConnectionState::Disconnecting) {
    return;
  }
  if (state == ConnectionState::Connecting) {
    connectionState_ = ConnectionState::Disconnected;
    cleanupConnectionState();
    if (peer != nullptr) {
      enet_peer_reset(peer);
      peer = nullptr;
    }
    if (client != nullptr) {
      enet_host_destroy(client);
      client = nullptr;
    }
    return;
  }

  stopAllWindowShares();
  clearRemoteWindows();
  clearWindowShares();
  if (peer != nullptr && client != nullptr) {
    enet_peer_disconnect(peer, 0);
    enet_host_flush(client);
    connectionState_ = ConnectionState::Disconnecting;
    disconnectDeadline_ = nowSeconds() + 1.0;
  } else {
    connectionState_ = ConnectionState::Disconnected;
    cleanupConnectionState();
    if (peer != nullptr) {
      enet_peer_reset(peer);
      peer = nullptr;
    }
    if (client != nullptr) {
      enet_host_destroy(client);
      client = nullptr;
    }
  }
}

bool
Client::sendSceneInfo()
{
  if (!isConnected() || peer == nullptr) {
    return false;
  }

  SceneFingerprint fingerprint;
  {
    std::lock_guard<std::mutex> lock(sceneMutex);
    if (!localScene.valid) {
      return false;
    }
    fingerprint = localScene;
  }

  const auto bytes = encodeSceneInfo(peer->connectID, fingerprint);
  ENetPacket* packet = enet_packet_create(
    bytes.data(), bytes.size(), ENET_PACKET_FLAG_RELIABLE);
  if (packet == nullptr) {
    return false;
  }
  if (enet_peer_send(peer, 3, packet) != 0) {
    enet_packet_destroy(packet);
    return false;
  }
  enet_host_flush(client);
  sceneInfoSent_ = true;
  return true;
}

void
Client::startSceneHash()
{
  sceneHashJob_.reset();
  if (!registry) {
    return;
  }
  const std::string databaseFile = registry->getDatabaseFile();
  if (databaseFile.empty()) {
    return;
  }

  auto job = std::make_shared<SceneHashJob>();
  sceneHashJob_ = job;
  spdlog::info("MultiPlayer: computing scene fingerprint in background");
  std::thread([job, databaseFile]() {
    std::string lastError;
    for (int attempt = 0; attempt < 3; ++attempt) {
      try {
        SQLite::Database database(databaseFile, SQLite::OPEN_READONLY);
        // The compositor may be writing the scene database while we hash it.
        database.setBusyTimeout(3000);
        const auto fingerprint =
          computeSceneFingerprint(database, databaseFile);
        std::lock_guard<std::mutex> lock(job->mutex);
        job->fingerprint = fingerprint;
        job->ready = true;
        return;
      } catch (const std::exception& error) {
        lastError = error.what();
        if (attempt + 1 < 3) {
          std::this_thread::sleep_for(std::chrono::milliseconds(300));
        }
      }
    }
    std::lock_guard<std::mutex> lock(job->mutex);
    job->error = lastError;
    job->failed = true;
  }).detach();
}

void
Client::pollSceneHash()
{
  auto job = sceneHashJob_;
  if (!job) {
    return;
  }

  bool ready = false;
  bool failed = false;
  SceneFingerprint fingerprint;
  std::string error;
  {
    std::lock_guard<std::mutex> lock(job->mutex);
    ready = job->ready;
    failed = job->failed;
    fingerprint = job->fingerprint;
    error = job->error;
  }
  if (!ready && !failed) {
    return;
  }

  sceneHashJob_.reset();
  if (ready && isConnected()) {
    {
      std::lock_guard<std::mutex> lock(sceneMutex);
      localScene = fingerprint;
    }
    sceneInfoSent_ = false;
    spdlog::info("MultiPlayer: local scene fingerprint {}", fingerprint.hex());
    sendSceneInfo();
  } else if (failed) {
    spdlog::warn("MultiPlayer: scene fingerprint failed: {}", error);
  }
}

void
Client::handleSceneInfo(const uint8_t* data, std::size_t size)
{
  uint32_t playerID = 0;
  SceneFingerprint fingerprint;
  if (!decodeSceneInfo(data, size, playerID, fingerprint)) {
    return;
  }
  if (peer != nullptr && playerID == peer->connectID) {
    // Our own fingerprint echoed by the relay.
    return;
  }

  bool mismatch = false;
  SceneFingerprint local;
  {
    std::lock_guard<std::mutex> lock(sceneMutex);
    peerScenes[playerID] = fingerprint;
    local = localScene;
    mismatch = local.valid && fingerprint.valid && local != fingerprint;
  }

  if (mismatch) {
    spdlog::warn(
      "MultiPlayer: scene mismatch with player {} (local={}, remote={}); "
      "window sharing is disabled",
      playerID,
      local.hex(),
      fingerprint.hex());
  } else {
    spdlog::info("MultiPlayer: scene matches player {} ({})",
                 playerID,
                 fingerprint.hex());
  }

  // Window starts/transforms may have arrived before the scene fingerprint.
  flushPendingWindowMessages(playerID);
}

bool
Client::hasSceneInfoFor(uint32_t playerID) const
{
  std::lock_guard<std::mutex> lock(sceneMutex);
  auto it = peerScenes.find(playerID);
  return it != peerScenes.end() && it->second.valid;
}

void
Client::flushPendingWindowMessages(uint32_t ownerID)
{
  std::vector<std::vector<uint8_t>> pending;
  {
    std::lock_guard<std::mutex> lock(shareMutex);
    auto it = pendingWindowMessages.find(ownerID);
    if (it == pendingWindowMessages.end()) {
      return;
    }
    pending = std::move(it->second);
    pendingWindowMessages.erase(it);
  }
  if (!sceneMatches(ownerID)) {
    return;
  }
  for (const auto& message : pending) {
    handleWindowStream(message.data(), message.size());
  }
}

SceneFingerprint
Client::localSceneFingerprint() const
{
  std::lock_guard<std::mutex> lock(sceneMutex);
  return localScene;
}

bool
Client::hasLocalScene() const
{
  std::lock_guard<std::mutex> lock(sceneMutex);
  return localScene.valid;
}

bool
Client::isComputingScene() const
{
  return sceneHashJob_ != nullptr;
}

bool
Client::hasSceneMismatch() const
{
  std::lock_guard<std::mutex> lock(sceneMutex);
  if (!localScene.valid) {
    // The local fingerprint is still being computed asynchronously; this is
    // not a mismatch, it is an unknown state.
    return false;
  }
  for (const auto& [playerID, fingerprint] : peerScenes) {
    (void)playerID;
    if (fingerprint.valid && fingerprint != localScene) {
      return true;
    }
  }
  return false;
}

bool
Client::hasPeerSceneInfo() const
{
  std::lock_guard<std::mutex> lock(sceneMutex);
  for (const auto& [playerID, fingerprint] : peerScenes) {
    (void)playerID;
    if (fingerprint.valid) {
      return true;
    }
  }
  return false;
}

bool
Client::sceneMatches(uint32_t playerID) const
{
  std::lock_guard<std::mutex> lock(sceneMutex);
  if (!localScene.valid) {
    return false;
  }
  auto it = peerScenes.find(playerID);
  return it != peerScenes.end() && it->second.valid &&
         it->second == localScene;
}

std::vector<uint32_t>
Client::mismatchedPeers() const
{
  std::lock_guard<std::mutex> lock(sceneMutex);
  std::vector<uint32_t> result;
  if (!localScene.valid) {
    for (const auto& [playerID, fingerprint] : peerScenes) {
      if (fingerprint.valid) {
        result.push_back(playerID);
      }
    }
    return result;
  }
  for (const auto& [playerID, fingerprint] : peerScenes) {
    if (fingerprint.valid && fingerprint != localScene) {
      result.push_back(playerID);
    }
  }
  return result;
}

std::size_t
Client::peerSceneCount() const
{
  std::lock_guard<std::mutex> lock(sceneMutex);
  return peerScenes.size();
}

void
Client::setWindowStreamFps(int fps)
{
  const int value = std::clamp(fps, 1, 30);
  if (windowStreamFps_.exchange(value) == value) {
    return;
  }
  persistWindowStreamFps(value);
}

void
Client::setWindowStreamQuality(int quality)
{
  const int value = std::clamp(quality, 1, 100);
  if (windowStreamQuality_.exchange(value) == value) {
    return;
  }
  persistWindowStreamQuality(value);
}

void
Client::loadWindowStreamSettings()
{
  if (!persistSettings_) {
    return;
  }
  try {
    auto config = Config::singleton();
    const int fps = config->get_or<int>("multiplayer_window_stream_fps", 5);
    const int quality =
      config->get_or<int>("multiplayer_window_stream_quality", 65);
    windowStreamFps_.store(std::clamp(fps, 1, 30));
    windowStreamQuality_.store(std::clamp(quality, 1, 100));
  } catch (const std::exception& error) {
    spdlog::warn("MultiPlayer: could not load stream settings: {}",
                 error.what());
  }
}

void
Client::persistWindowStreamFps(int fps)
{
  if (!persistSettings_) {
    return;
  }
  try {
    Config::singleton()->setIntAndSave("multiplayer_window_stream_fps", fps);
  } catch (const std::exception& error) {
    spdlog::warn("MultiPlayer: could not save stream FPS: {}", error.what());
  }
}

void
Client::persistWindowStreamQuality(int quality)
{
  if (!persistSettings_) {
    return;
  }
  try {
    Config::singleton()->setIntAndSave("multiplayer_window_stream_quality",
                                       quality);
  } catch (const std::exception& error) {
    spdlog::warn("MultiPlayer: could not save stream quality: {}",
                 error.what());
  }
}

WindowShareSettings
Client::windowShare(entt::entity entity) const
{
  std::lock_guard<std::mutex> lock(shareMutex);
  auto it = windowShares.find(entity);
  return it == windowShares.end() ? WindowShareSettings{} : it->second;
}

void
Client::setWindowShared(entt::entity entity, bool shared)
{
  if (!shared) {
    {
      std::lock_guard<std::mutex> lock(shareMutex);
      windowShares.erase(entity);
    }
    stopWindowShare(entity);
    return;
  }

  {
    std::lock_guard<std::mutex> lock(shareMutex);
    auto& settings = windowShares[entity];
    settings.shared = true;
    settings.allowControl = false;
  }

  // Local state is always recorded so the panel can remember the intent.
  // Actual streaming only starts when a matching peer is connected.
  if (!isConnected() || peer == nullptr || !hasPeerSceneInfo() ||
      !hasLocalScene() || hasSceneMismatch()) {
    return;
  }

  if (!startWindowShare(entity)) {
    std::lock_guard<std::mutex> lock(shareMutex);
    windowShares.erase(entity);
  }
}

void
Client::setWindowAllowControl(entt::entity entity, bool allowControl)
{
  uint32_t shareId = 0;
  uint32_t controllerID = 0;
  bool shared = false;
  {
    std::lock_guard<std::mutex> lock(shareMutex);
    auto it = windowShares.find(entity);
    if (it == windowShares.end() || !it->second.shared) {
      return;
    }
    it->second.allowControl = allowControl;
    auto shareIt = localShareIds.find(entity);
    if (shareIt != localShareIds.end()) {
      shareId = shareIt->second;
      shared = true;
      if (!allowControl) {
        localShareControllers.erase(shareId);
      } else {
        auto controllerIt = localShareControllers.find(shareId);
        if (controllerIt != localShareControllers.end()) {
          controllerID = controllerIt->second;
        }
      }
    }
  }

  if (shared) {
    sendControlState(shareId, allowControl, controllerID);
  }
}

std::vector<entt::entity>
Client::sharedWindows() const
{
  std::lock_guard<std::mutex> lock(shareMutex);
  std::vector<entt::entity> result;
  result.reserve(windowShares.size());
  for (const auto& [entity, settings] : windowShares) {
    if (settings.shared) {
      result.push_back(entity);
    }
  }
  return result;
}

void
Client::clearWindowShares()
{
  stopAllWindowShares();
  std::lock_guard<std::mutex> lock(shareMutex);
  windowShares.clear();
}

bool
Client::startWindowShare(entt::entity entity)
{
  if (!isConnected() || peer == nullptr || !registry) {
    return false;
  }
  auto* component = registry->try_get<WaylandApp::Component>(entity);
  auto* positionable = registry->try_get<Positionable>(entity);
  if (!component || !component->app || !positionable) {
    return false;
  }

  uint32_t shareId = 0;
  {
    std::lock_guard<std::mutex> lock(shareMutex);
    auto existing = localShareIds.find(entity);
    if (existing != localShareIds.end()) {
      return true;
    }
    shareId = nextWindowShareId++;
    if (nextWindowShareId == 0) {
      nextWindowShareId = 1;
    }
    while (localWindowShares.contains(shareId)) {
      shareId = nextWindowShareId++;
      if (nextWindowShareId == 0) {
        nextWindowShareId = 1;
      }
    }
    localShareIds[entity] = shareId;
    localWindowShares[shareId] = LocalWindowShare{ entity,
                                                   0.0,
                                                   positionable->pos,
                                                   positionable->origin,
                                                   positionable->rotate,
                                                   positionable->scale };
  }

  WindowShareStartData start;
  start.shareId = shareId;
  start.pos = positionable->pos;
  start.origin = positionable->origin;
  start.rotate = positionable->rotate;
  start.scale = positionable->scale;
  // A window can be shared before its first commit; using a 1x1 placeholder
  // keeps the remote window alive and the first real frame resizes it.
  start.width = std::max(1, component->app->getWidth());
  start.height = std::max(1, component->app->getHeight());
  start.name = component->app->getWindowName();

  const auto bytes = encodeWindowShareStart(peer->connectID, start);
  ENetPacket* packet = enet_packet_create(
    bytes.data(), bytes.size(), ENET_PACKET_FLAG_RELIABLE);
  if (packet == nullptr || enet_peer_send(peer, 4, packet) != 0) {
    if (packet) {
      enet_packet_destroy(packet);
    }
    std::lock_guard<std::mutex> lock(shareMutex);
    localShareIds.erase(entity);
    localWindowShares.erase(shareId);
    return false;
  }
  enet_host_flush(client);

  bool allowControl = false;
  {
    std::lock_guard<std::mutex> lock(shareMutex);
    auto settings = windowShares.find(entity);
    allowControl =
      settings != windowShares.end() && settings->second.allowControl;
  }
  if (allowControl) {
    sendControlState(shareId, true, 0);
  }

  spdlog::info("MultiPlayer: sharing window '{}' as share {}",
               start.name,
               shareId);
  return true;
}

void
Client::sendWindowShareStop(uint32_t shareId)
{
  if (!isConnected() || peer == nullptr) {
    return;
  }
  const auto bytes = encodeWindowShareStop(peer->connectID, shareId);
  ENetPacket* packet = enet_packet_create(
    bytes.data(), bytes.size(), ENET_PACKET_FLAG_RELIABLE);
  if (packet == nullptr) {
    return;
  }
  if (enet_peer_send(peer, 4, packet) != 0) {
    enet_packet_destroy(packet);
    return;
  }
  enet_host_flush(client);
}

void
Client::sendWindowShareTransform(uint32_t shareId,
                                 const glm::vec3& pos,
                                 const glm::vec3& origin,
                                 const glm::vec3& rotate,
                                 float scale)
{
  if (!isConnected() || peer == nullptr) {
    return;
  }
  WindowShareTransformData transform;
  transform.shareId = shareId;
  transform.pos = pos;
  transform.origin = origin;
  transform.rotate = rotate;
  transform.scale = scale;

  const auto bytes = encodeWindowShareTransform(peer->connectID, transform);
  ENetPacket* packet = enet_packet_create(
    bytes.data(), bytes.size(), ENET_PACKET_FLAG_RELIABLE);
  if (packet == nullptr) {
    return;
  }
  if (enet_peer_send(peer, 4, packet) != 0) {
    enet_packet_destroy(packet);
    return;
  }
  enet_host_flush(client);
}

void
Client::syncWindowShares()
{
  if (!isConnected() || peer == nullptr || !hasPeerSceneInfo() ||
      !hasLocalScene() || hasSceneMismatch()) {
    return;
  }

  std::vector<entt::entity> pending;
  {
    std::lock_guard<std::mutex> lock(shareMutex);
    for (const auto& [entity, settings] : windowShares) {
      if (settings.shared && !localShareIds.contains(entity)) {
        pending.push_back(entity);
      }
    }
  }
  for (entt::entity entity : pending) {
    startWindowShare(entity);
  }
}

void
Client::stopWindowShare(entt::entity entity)
{
  uint32_t shareId = 0;
  bool found = false;
  {
    std::lock_guard<std::mutex> lock(shareMutex);
    auto it = localShareIds.find(entity);
    if (it != localShareIds.end()) {
      shareId = it->second;
      localShareIds.erase(it);
      localWindowShares.erase(shareId);
      localShareControllers.erase(shareId);
      found = true;
    }
  }
  if (found) {
    if (encodeWorker_) {
      encodeWorker_->removeShare(shareId);
    }
    if (webrtc) {
      webrtc->removeSenderShare(shareId);
    }
    sendWindowShareStop(shareId);
    spdlog::info("MultiPlayer: stopped sharing window (share {})", shareId);
  }
}

void
Client::stopAllWindowShares()
{
  std::vector<uint32_t> shareIds;
  {
    std::lock_guard<std::mutex> lock(shareMutex);
    shareIds.reserve(localWindowShares.size());
    for (const auto& [shareId, share] : localWindowShares) {
      (void)share;
      shareIds.push_back(shareId);
    }
    localWindowShares.clear();
    localShareIds.clear();
    localShareControllers.clear();
  }
  for (uint32_t shareId : shareIds) {
    if (encodeWorker_) {
      encodeWorker_->removeShare(shareId);
    }
    if (webrtc) {
      webrtc->removeSenderShare(shareId);
    }
    sendWindowShareStop(shareId);
  }
}

void
Client::tickWindowStreams()
{
  if (!isConnected() || peer == nullptr || !registry) {
    return;
  }

  // Drain encoded frames produced by the off-thread encoder first.
  if (encodeWorker_) {
    for (auto& result : encodeWorker_->takeResults()) {
      if (!isConnected() || peer == nullptr) {
        break;
      }
      bool found = false;
      {
        std::lock_guard<std::mutex> lock(shareMutex);
        auto it = localWindowShares.find(result.shareId);
        if (it != localWindowShares.end()) {
          found = true;
          it->second.h264Unavailable = result.h264Unavailable;
        }
      }
      if (!found) {
        continue;
      }

      if (result.h264) {
        bool sent = false;
        for (const auto& h264Packet : result.packets) {
          if (h264Packet.empty() ||
              h264Packet.size() > kMaxWindowFrameBytes) {
            continue;
          }
          const auto bytes = encodeWindowShareH264Frame(peer->connectID,
                                                        result.shareId,
                                                        result.width,
                                                        result.height,
                                                        result.keyframe,
                                                        h264Packet.data(),
                                                        h264Packet.size());
          ENetPacket* packet = enet_packet_create(
            bytes.data(), bytes.size(), ENET_PACKET_FLAG_RELIABLE);
          if (packet == nullptr) {
            continue;
          }
          if (enet_peer_send(peer, 6, packet) != 0) {
            enet_packet_destroy(packet);
            continue;
          }
          sent = true;
        }
        if (sent) {
          enet_host_flush(client);
        }
        if (webrtc) {
          for (const auto& h264Packet : result.packets) {
            if (!h264Packet.empty() &&
                h264Packet.size() <= kMaxWindowFrameBytes) {
              webrtc->pushEncodedFrame(
                result.shareId, h264Packet.data(), h264Packet.size());
            }
          }
        }
      } else if (!result.jpeg.empty() &&
                 result.jpeg.size() <= kMaxWindowFrameBytes) {
        const auto bytes = encodeWindowShareFrame(peer->connectID,
                                                  result.shareId,
                                                  result.width,
                                                  result.height,
                                                  result.jpeg.data(),
                                                  result.jpeg.size());
        ENetPacket* packet = enet_packet_create(
          bytes.data(), bytes.size(), ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT);
        if (packet == nullptr) {
          continue;
        }
        if (enet_peer_send(peer, 4, packet) != 0) {
          enet_packet_destroy(packet);
          continue;
        }
        enet_host_flush(client);
      }
    }
  }

  const int configuredFps = std::clamp(windowStreamFps(), 1, 30);
  const int quality = std::clamp(windowStreamQuality(), 1, 100);
  const double now = nowSeconds();

  struct DueShare
  {
    uint32_t shareId = 0;
    entt::entity entity = entt::null;
    int fps = 5;
  };
  std::vector<DueShare> due;
  std::vector<std::pair<uint32_t, WindowShareTransformData>> transforms;
  {
    std::lock_guard<std::mutex> lock(shareMutex);
    for (auto& [shareId, share] : localWindowShares) {
      if (auto* positionable = registry->try_get<Positionable>(share.entity)) {
        if (positionable->pos != share.pos ||
            positionable->origin != share.origin ||
            positionable->rotate != share.rotate ||
            positionable->scale != share.scale) {
          share.pos = positionable->pos;
          share.origin = positionable->origin;
          share.rotate = positionable->rotate;
          share.scale = positionable->scale;
          WindowShareTransformData transform;
          transform.shareId = shareId;
          transform.pos = share.pos;
          transform.origin = share.origin;
          transform.rotate = share.rotate;
          transform.scale = share.scale;
          transforms.emplace_back(shareId, transform);
        }
      }

      const auto controllerIt = localShareControllers.find(shareId);
      share.controlActive = controllerIt != localShareControllers.end();
      share.controllerID = share.controlActive ? controllerIt->second : 0;
      // Remote control is much more usable at 15 FPS than at the default 5.
      const int shareFps =
        share.controlActive ? std::max(configuredFps, 15) : configuredFps;
      if (now - share.lastFrame >= 1.0 / static_cast<double>(shareFps)) {
        share.lastFrame = now;
        due.push_back(DueShare{ shareId, share.entity, shareFps });
      }
    }
  }

  for (const auto& [shareId, transform] : transforms) {
    sendWindowShareTransform(shareId,
                             transform.pos,
                             transform.origin,
                             transform.rotate,
                             transform.scale);
  }

  for (const auto& d : due) {
    auto* component = registry->try_get<WaylandApp::Component>(d.entity);
    auto* positionable = registry->try_get<Positionable>(d.entity);
    if (!component || !component->app || !positionable) {
      stopWindowShare(d.entity);
      continue;
    }

    const int width = component->app->getWidth();
    const int height = component->app->getHeight();
    if (width <= 0 || height <= 0) {
      continue;
    }

    int targetQuality = quality;
    int pointerX = -1;
    int pointerY = -1;
    std::shared_ptr<WindowCapture> capture;
    {
      std::lock_guard<std::mutex> lock(shareMutex);
      auto it = localWindowShares.find(d.shareId);
      if (it == localWindowShares.end()) {
        continue;
      }
      auto& share = it->second;
      if (!share.capture) {
        share.capture = std::make_shared<WindowCapture>();
      }
      capture = share.capture;

      if (share.configuredQuality != quality) {
        share.configuredQuality = quality;
        share.adaptiveQuality = quality;
        share.lastAdaptation = 0.0;
      }
      if (now - share.lastAdaptation >= kWindowAdaptationIntervalSeconds) {
        share.lastAdaptation = now;
        if (share.adaptiveQuality < 0) {
          share.adaptiveQuality = quality;
        }
        float loss = peer
                       ? static_cast<float>(peer->packetLoss) /
                           static_cast<float>(ENET_PEER_PACKET_LOSS_SCALE)
                       : 0.0f;
        uint32_t rtt = peer ? peer->roundTripTime : 0;
        // Prefer peer-to-peer RTCP feedback when it is fresh.
        if (share.webrtcLoss >= 0.0 &&
            now - share.webrtcStatsTime < 6.0) {
          loss = static_cast<float>(share.webrtcLoss);
          rtt = static_cast<uint32_t>(share.webrtcRtt * 1000.0);
        }
        int next = share.adaptiveQuality;
        if (loss > 0.02f || rtt > 200) {
          next = std::max(10, next - 10);
        } else if (loss < 0.005f && rtt < 80 && next < quality) {
          next = std::min(quality, next + 5);
        }
        if (next != share.adaptiveQuality) {
          spdlog::info(
            "MultiPlayer: adapting share {} quality {} -> {} "
            "(loss={:.2f}%, rtt={}ms)",
            d.shareId,
            share.adaptiveQuality,
            next,
            loss * 100.0f,
            rtt);
          share.adaptiveQuality = next;
        }
      }
      targetQuality = share.adaptiveQuality > 0
                        ? std::min(quality, share.adaptiveQuality)
                        : quality;
      if (share.controlActive) {
        pointerX = share.remotePointerX;
        pointerY = share.remotePointerY;
      }
    }

    auto* worker = ensureEncodeWorker();
    if (worker == nullptr || worker->isBusy(d.shareId)) {
      continue;
    }

    std::vector<uint8_t> rgba;
    int capturedWidth = 0;
    int capturedHeight = 0;
    if (!capture->readback(component->app->getTextureId(),
                           width,
                           height,
                           rgba,
                           capturedWidth,
                           capturedHeight)) {
      continue;
    }
    if (rgba.empty() || capturedWidth <= 0 || capturedHeight <= 0) {
      continue;
    }

    WindowEncodeJob job;
    job.shareId = d.shareId;
    job.width = capturedWidth;
    job.height = capturedHeight;
    job.fps = d.fps;
    job.quality = targetQuality;
    if (pointerX >= 0 && pointerY >= 0) {
      job.cursorX = std::clamp(pointerX * capturedWidth / std::max(1, width),
                               0,
                               capturedWidth - 1);
      job.cursorY = std::clamp(pointerY * capturedHeight / std::max(1, height),
                               0,
                               capturedHeight - 1);
    }
    job.rgba = std::move(rgba);
    worker->enqueue(std::move(job));
  }
}

void
Client::handleVideoFrame(const uint8_t* data, std::size_t size)
{
  if (!data || !registry) {
    return;
  }

  uint32_t owner = 0;
  if (!windowShareOwner(data, size, owner)) {
    return;
  }
  if (peer != nullptr && owner == peer->connectID) {
    return;
  }
  if (!sceneMatches(owner)) {
    return;
  }

  WindowShareH264FrameView frame;
  if (!decodeWindowShareH264Frame(data, size, frame)) {
    return;
  }

  const uint64_t key = (static_cast<uint64_t>(owner) << 32) | frame.shareId;
  entt::entity entity = entt::null;
  std::shared_ptr<H264Decoder> decoder;
  {
    std::lock_guard<std::mutex> lock(shareMutex);
    auto it = remoteWindowShares.find(key);
    if (it == remoteWindowShares.end()) {
      return;
    }
    // Prefer WebRTC while it is delivering frames. The owner keeps sending
    // the ENet fallback, so a stalled WebRTC session recovers automatically.
    // Keep this short: a silent WebRTC stall should show live ENet frames
    // within about a second instead of freezing the window.
    if (nowSeconds() - it->second.lastWebRtcFrame < 1.0) {
      return;
    }
    entity = it->second.entity;
    if (!it->second.decoder) {
      it->second.decoder = std::make_shared<H264Decoder>();
    }
    decoder = it->second.decoder;
  }
  if (!decoder) {
    return;
  }

  std::vector<uint8_t> rgba;
  int decodedWidth = 0;
  int decodedHeight = 0;
  if (decoder->decode(frame.data,
                      frame.size,
                      frame.keyframe,
                      rgba,
                      decodedWidth,
                      decodedHeight)) {
    auto* component = registry->try_get<RemoteApp::Component>(entity);
    if (component && component->app) {
      component->app->setRgbaFrame(
        rgba.data(), decodedWidth, decodedHeight);
    }
    return;
  }

  // No decoded frame yet (waiting for a keyframe or the decoder reset itself).
  bool requestKeyframe = false;
  const double now = nowSeconds();
  {
    std::lock_guard<std::mutex> lock(shareMutex);
    auto it = remoteWindowShares.find(key);
    if (it != remoteWindowShares.end() &&
        now - it->second.lastKeyframeRequest >= 1.0) {
      it->second.lastKeyframeRequest = now;
      requestKeyframe = true;
    }
  }
  if (requestKeyframe) {
    requestRemoteKeyframe(owner, frame.shareId);
  }
}

void
Client::handleWindowStream(const uint8_t* data, std::size_t size)
{
  if (!data || size < kWindowShareHeaderSize || !registry) {
    return;
  }

  const auto type = static_cast<WindowShareMessageType>(data[0]);
  uint32_t owner = 0;
  if (!windowShareOwner(data, size, owner)) {
    return;
  }
  if (peer != nullptr && owner == peer->connectID) {
    return;
  }

  if (type == WindowShareMessageType::OwnerLeft) {
    uint32_t leftOwner = 0;
    if (decodeWindowShareOwnerLeft(data, size, leftOwner)) {
      {
        std::lock_guard<std::mutex> lock(shareMutex);
        pendingWindowMessages.erase(leftOwner);
      }
      handleControllerDisconnect(leftOwner);
      removeRemoteWindowsForOwner(leftOwner);
    }
    return;
  }

  if (!hasSceneInfoFor(owner)) {
    if (type != WindowShareMessageType::Frame) {
      std::lock_guard<std::mutex> lock(shareMutex);
      auto& pending = pendingWindowMessages[owner];
      if (pending.size() < 64) {
        pending.emplace_back(data, data + size);
      }
    }
    return;
  }

  if (!sceneMatches(owner)) {
    return;
  }

  switch (type) {
    case WindowShareMessageType::Start: {
      WindowShareStartData start;
      if (!decodeWindowShareStart(data, size, start)) {
        return;
      }
      if (!sceneMatches(owner)) {
        spdlog::warn(
          "MultiPlayer: refusing shared window {} from player {} "
          "(scene mismatch or unknown scene)",
          start.shareId,
          owner);
        return;
      }
      if (start.width <= 0 || start.height <= 0 || start.width > 8192 ||
          start.height > 8192) {
        spdlog::warn("MultiPlayer: ignoring invalid shared window size {}x{}",
                     start.width,
                     start.height);
        return;
      }

      removeRemoteWindow(owner, start.shareId);

      auto remote = std::make_shared<RemoteApp>(nullptr, start.name);
      remote->createTexture();
      remote->resize(start.width, start.height);

      const entt::entity entity = registry->create();
      registry->emplace<Positionable>(entity,
                                      start.pos,
                                      start.origin,
                                      start.rotate,
                                      start.scale);
      if (auto* positionable = registry->try_get<Positionable>(entity)) {
        positionable->update();
      }
      registry->emplace<RemoteApp::Component>(entity, remote);

      const uint64_t key =
        (static_cast<uint64_t>(owner) << 32) | start.shareId;
      {
        std::lock_guard<std::mutex> lock(shareMutex);
        RemoteWindowShare share;
        share.entity = entity;
        share.ownerID = owner;
        share.shareId = start.shareId;
        share.name = start.name;
        share.webrtcRequested = peer != nullptr;
        share.webrtcRequestTime = nowSeconds();
        remoteWindowShares[key] = std::move(share);
      }
      spdlog::info("MultiPlayer: showing shared window '{}' from player {} "
                   "(share {}, {}x{})",
                   start.name,
                   owner,
                   start.shareId,
                   start.width,
                   start.height);
      requestRemoteKeyframe(owner, start.shareId);
      ensureWebRtc();
      webrtc->ensureReceiver(owner, start.shareId);
      if (peer != nullptr) {
        sendWebRtcSignal(
          encodeWebRtcRequest(owner, start.shareId, peer->connectID));
      }
      break;
    }
    case WindowShareMessageType::Stop: {
      uint32_t stopOwner = 0;
      uint32_t shareId = 0;
      if (decodeWindowShareStop(data, size, stopOwner, shareId)) {
        removeRemoteWindow(stopOwner, shareId);
      }
      break;
    }
    case WindowShareMessageType::Frame: {
      WindowShareFrameView frame;
      if (!decodeWindowShareFrame(data, size, frame)) {
        return;
      }
      if (frame.size == 0) {
        return;
      }

      const uint64_t key =
        (static_cast<uint64_t>(owner) << 32) | frame.shareId;
      entt::entity entity = entt::null;
      {
        std::lock_guard<std::mutex> lock(shareMutex);
        auto it = remoteWindowShares.find(key);
        if (it == remoteWindowShares.end()) {
          return;
        }
        entity = it->second.entity;
      }
      auto* component = registry->try_get<RemoteApp::Component>(entity);
      if (!component || !component->app) {
        return;
      }

      int decodedWidth = 0;
      int decodedHeight = 0;
      int channels = 0;
      // Other texture loaders set this global to true; JPEG frames are
      // captured top-row-first and must not be flipped again.
      stbi_set_flip_vertically_on_load(false);
      unsigned char* pixels = stbi_load_from_memory(
        frame.data,
        static_cast<int>(frame.size),
        &decodedWidth,
        &decodedHeight,
        &channels,
        4);
      if (!pixels) {
        return;
      }
      if (decodedWidth > 0 && decodedHeight > 0 && decodedWidth <= 8192 &&
          decodedHeight <= 8192) {
        component->app->setRgbaFrame(pixels, decodedWidth, decodedHeight);
      }
      stbi_image_free(pixels);
      break;
    }
    case WindowShareMessageType::ControlState: {
      handleControlState(data, size);
      break;
    }
    case WindowShareMessageType::Transform: {
      WindowShareTransformData transform;
      if (!decodeWindowShareTransform(data, size, transform)) {
        return;
      }
      const uint64_t key =
        (static_cast<uint64_t>(owner) << 32) | transform.shareId;
      entt::entity entity = entt::null;
      {
        std::lock_guard<std::mutex> lock(shareMutex);
        auto it = remoteWindowShares.find(key);
        if (it == remoteWindowShares.end()) {
          return;
        }
        entity = it->second.entity;
      }
      if (auto* positionable = registry->try_get<Positionable>(entity)) {
        positionable->pos = transform.pos;
        positionable->origin = transform.origin;
        positionable->rotate = transform.rotate;
        positionable->scale = transform.scale;
        positionable->update();
      }
      break;
    }
    case WindowShareMessageType::OwnerLeft:
      break;
  }
}

void
Client::sendControlPacket(const std::vector<uint8_t>& bytes, bool reliable)
{
  if (!isConnected() || peer == nullptr || bytes.empty()) {
    return;
  }
  ENetPacket* packet = enet_packet_create(
    bytes.data(),
    bytes.size(),
    reliable ? ENET_PACKET_FLAG_RELIABLE : ENET_PACKET_FLAG_UNSEQUENCED);
  if (packet == nullptr) {
    return;
  }
  if (enet_peer_send(peer, 5, packet) != 0) {
    enet_packet_destroy(packet);
    return;
  }
  enet_host_flush(client);
}

void
Client::sendControlState(uint32_t shareId,
                         bool allowControl,
                         uint32_t controllerID)
{
  if (!isConnected() || peer == nullptr) {
    return;
  }
  const auto bytes = encodeWindowShareControlState(
    peer->connectID, shareId, allowControl, controllerID);
  ENetPacket* packet = enet_packet_create(
    bytes.data(), bytes.size(), ENET_PACKET_FLAG_RELIABLE);
  if (packet == nullptr) {
    return;
  }
  if (enet_peer_send(peer, 4, packet) != 0) {
    enet_packet_destroy(packet);
    return;
  }
  enet_host_flush(client);
}

void
Client::handleControlState(const uint8_t* data, std::size_t size)
{
  uint32_t owner = 0;
  uint32_t shareId = 0;
  uint32_t controllerID = 0;
  bool allowControl = false;
  if (!windowShareOwner(data, size, owner) ||
      !decodeWindowShareControlState(
        data, size, shareId, allowControl, controllerID)) {
    return;
  }

  const uint64_t key = (static_cast<uint64_t>(owner) << 32) | shareId;
  bool selfControlling = false;
  {
    std::lock_guard<std::mutex> lock(shareMutex);
    auto it = remoteWindowShares.find(key);
    if (it == remoteWindowShares.end()) {
      return;
    }
    it->second.allowControl = allowControl;
    it->second.controllerID = controllerID;
    it->second.controlPending = false;
    selfControlling = peer != nullptr && controllerID == peer->connectID;
    if (selfControlling) {
      controlledOwnerID = owner;
      controlledShareId = shareId;
    } else if (controlledOwnerID == owner && controlledShareId == shareId) {
      controlledOwnerID = 0;
      controlledShareId = 0;
    }
  }

  spdlog::info("MultiPlayer: share {} from player {}: allowControl={} "
               "controller={}{}",
               shareId,
               owner,
               allowControl ? 1 : 0,
               controllerID,
               selfControlling ? " (you)" : "");
}

void
Client::ensureWebRtc()
{
  if (!webrtc) {
    webrtc = std::make_unique<WebRtcManager>();
  }
}

void
Client::sendWebRtcSignal(const std::vector<uint8_t>& bytes)
{
  if (!isConnected() || peer == nullptr || bytes.empty()) {
    return;
  }
  ENetPacket* packet = enet_packet_create(
    bytes.data(), bytes.size(), ENET_PACKET_FLAG_RELIABLE);
  if (packet == nullptr) {
    return;
  }
  if (enet_peer_send(peer, 7, packet) != 0) {
    enet_packet_destroy(packet);
    return;
  }
  enet_host_flush(client);
}

void
Client::handleWebRtcSignal(const uint8_t* data, std::size_t size)
{
  if (!data || size < kWebRtcHeaderSize || peer == nullptr) {
    return;
  }
  WebRtcHeader header;
  if (!decodeWebRtcHeader(data, size, header) ||
      header.targetOwner != peer->connectID ||
      !sceneMatches(header.senderID)) {
    return;
  }

  switch (header.type) {
    case WebRtcSignalType::Request:
    case WebRtcSignalType::Reconnect: {
      bool shared = false;
      {
        std::lock_guard<std::mutex> lock(shareMutex);
        shared = localWindowShares.contains(header.shareId);
      }
      if (!shared) {
        return;
      }
      ensureWebRtc();
      if (header.type == WebRtcSignalType::Reconnect) {
        webrtc->removeSender(header.senderID, header.shareId);
      }
      webrtc->ensureSender(header.senderID, header.shareId);
      spdlog::info("MultiPlayer: {} WebRTC for share {} to player {}",
                   header.type == WebRtcSignalType::Reconnect ? "reconnecting"
                                                              : "starting",
                   header.shareId,
                   header.senderID);
      break;
    }
    case WebRtcSignalType::Offer: {
      std::string sdp;
      if (!decodeWebRtcSdp(data, size, header, sdp)) {
        return;
      }
      ensureWebRtc();
      webrtc->ensureReceiver(header.senderID, header.shareId);
      webrtc->handleOffer(header.senderID, header.shareId, sdp);
      break;
    }
    case WebRtcSignalType::Answer: {
      std::string sdp;
      if (!decodeWebRtcSdp(data, size, header, sdp)) {
        return;
      }
      ensureWebRtc();
      webrtc->handleAnswer(header.senderID, header.shareId, sdp);
      break;
    }
    case WebRtcSignalType::Ice: {
      int32_t mlineIndex = 0;
      std::string candidate;
      if (!decodeWebRtcIce(data, size, header, mlineIndex, candidate)) {
        return;
      }
      ensureWebRtc();
      webrtc->handleIce(
        header.senderID, header.shareId, mlineIndex, candidate);
      break;
    }
  }
}

void
Client::tickWebRtc()
{
  if (!webrtc) {
    return;
  }

  const double now = nowSeconds();
  if (peer != nullptr) {
    for (const auto& signal : webrtc->takeSignals()) {
      if (signal.type == WebRtcSignalType::Offer ||
          signal.type == WebRtcSignalType::Answer) {
        sendWebRtcSignal(encodeWebRtcSdp(signal.type,
                                         signal.targetOwner,
                                         signal.shareId,
                                         peer->connectID,
                                         signal.data));
      } else if (signal.type == WebRtcSignalType::Ice) {
        sendWebRtcSignal(encodeWebRtcIce(signal.targetOwner,
                                         signal.shareId,
                                         peer->connectID,
                                         signal.mlineIndex,
                                         signal.data));
      }
    }
  } else {
    webrtc->takeSignals();
  }

  for (uint32_t shareId : webrtc->takeKeyframeRequests()) {
    if (encodeWorker_) {
      encodeWorker_->requestKeyframe(shareId);
      spdlog::info("WebRTC: forcing keyframe for share {}", shareId);
    }
  }

  for (const auto& event : webrtc->takeConnectionEvents()) {
    if (event.type == WebRtcManager::ConnectionEvent::Type::SenderFailed) {
      // Owner side: drop the failed sender so the viewer's Reconnect creates a
      // fresh offer instead of reusing a dead peer connection.
      webrtc->removeSender(event.peerID, event.shareId);
      spdlog::info("WebRTC: sender for share {} viewer {} failed; dropped",
                   event.shareId,
                   event.peerID);
      continue;
    }
    if (event.type == WebRtcManager::ConnectionEvent::Type::SenderConnected) {
      continue;
    }

    const uint64_t key =
      (static_cast<uint64_t>(event.peerID) << 32) | event.shareId;
    if (event.type == WebRtcManager::ConnectionEvent::Type::ReceiverConnected) {
      std::lock_guard<std::mutex> lock(shareMutex);
      auto it = remoteWindowShares.find(key);
      if (it != remoteWindowShares.end()) {
        it->second.webrtcFailed = false;
      }
      continue;
    }

    if (event.type == WebRtcManager::ConnectionEvent::Type::ReceiverFailed) {
      double retryDelay = 1.0;
      {
        std::lock_guard<std::mutex> lock(shareMutex);
        auto it = remoteWindowShares.find(key);
        if (it == remoteWindowShares.end()) {
          continue;
        }
        it->second.webrtcFailed = true;
        it->second.lastWebRtcFrame = 0.0;
        it->second.webrtcRequested = true;
        it->second.webrtcRequestTime = now;
        retryDelay = std::min(
          8.0,
          1.0 * std::pow(2.0, std::min(3, it->second.webrtcRetryCount)));
        it->second.nextWebRtcRetry = now + retryDelay;
        ++it->second.webrtcRetryCount;
      }
      webrtc->removeReceiver(event.peerID, event.shareId);
      spdlog::info(
        "WebRTC: receiver for share {} owner {} failed; retrying in {:.1f}s",
        event.shareId,
        event.peerID,
        retryDelay);
    }
  }

  std::vector<std::pair<uint32_t, uint32_t>> retries;
  {
    std::lock_guard<std::mutex> lock(shareMutex);
    for (auto& [key, share] : remoteWindowShares) {
      if (share.lastWebRtcFrame > 0.0 &&
          now - share.lastWebRtcFrame < 2.0) {
        share.webrtcFailed = false;
        share.nextWebRtcRetry = 0.0;
        share.webrtcRetryCount = 0;
        continue;
      }

      if (share.nextWebRtcRetry > 0.0 && now >= share.nextWebRtcRetry) {
        share.nextWebRtcRetry = 0.0;
        share.webrtcRequested = true;
        share.webrtcRequestTime = now;
        retries.emplace_back(share.ownerID, share.shareId);
        continue;
      }

      // Silent stall: no frame, no failure callback. Schedule a reconnect
      // instead of leaving the viewer on WebRTC forever. ENet fallback keeps
      // the window live in the meantime.
      if (share.nextWebRtcRetry == 0.0 && share.webrtcRequested &&
          now - share.webrtcRequestTime > 3.0 &&
          now - share.lastWebRtcFrame > 3.0) {
        share.webrtcFailed = true;
        share.lastWebRtcFrame = 0.0;
        share.nextWebRtcRetry = now;
        ++share.webrtcRetryCount;
      }
    }
  }

  for (const auto& [ownerID, shareId] : retries) {
    webrtc->ensureReceiver(ownerID, shareId);
    if (peer != nullptr) {
      sendWebRtcSignal(
        encodeWebRtcReconnect(ownerID, shareId, peer->connectID));
    }
    spdlog::info("WebRTC: reconnecting share {} from player {}",
                 shareId,
                 ownerID);
  }

  if (now >= nextWebRtcStatsPoll_) {
    nextWebRtcStatsPoll_ = now + 2.0;
    {
      std::lock_guard<std::mutex> lock(shareMutex);
      for (auto& [id, share] : localWindowShares) {
        (void)id;
        share.webrtcLoss = -1.0;
        share.webrtcRtt = -1.0;
      }
    }
    webrtc->pollSenderStats();
  }
  for (const auto& stats : webrtc->takeSenderStats()) {
    std::lock_guard<std::mutex> lock(shareMutex);
    auto it = localWindowShares.find(stats.shareId);
    if (it == localWindowShares.end()) {
      continue;
    }
    it->second.webrtcLoss =
      std::max(it->second.webrtcLoss, stats.fractionLost);
    it->second.webrtcRtt =
      std::max(it->second.webrtcRtt, stats.roundTripTime);
    it->second.webrtcStatsTime = now;
  }

  for (const auto& frame : webrtc->takeDecodedFrames()) {
    const uint64_t key =
      (static_cast<uint64_t>(frame.ownerID) << 32) | frame.shareId;
    entt::entity entity = entt::null;
    {
      std::lock_guard<std::mutex> lock(shareMutex);
      auto it = remoteWindowShares.find(key);
      if (it == remoteWindowShares.end()) {
        continue;
      }
      it->second.lastWebRtcFrame = now;
      it->second.webrtcFailed = false;
      it->second.nextWebRtcRetry = 0.0;
      it->second.webrtcRetryCount = 0;
      entity = it->second.entity;
    }
    if (auto* component = registry->try_get<RemoteApp::Component>(entity)) {
      if (component->app) {
        component->app->setRgbaFrame(
          frame.rgba.data(), frame.width, frame.height);
      }
    }
  }
}

void
Client::handleControlMessage(const uint8_t* data, std::size_t size)
{
  if (!data || size < kControlHeaderSize || peer == nullptr) {
    return;
  }

  ControlHeader header;
  if (!decodeControlHeader(data, size, header)) {
    return;
  }
  if (header.targetOwner != peer->connectID) {
    return;
  }

  const auto type = static_cast<ControlMessageType>(data[0]);
  if (type == ControlMessageType::KeyframeRequest) {
    ControlHeader request;
    if (!decodeControlKeyframeRequest(data, size, request) ||
        !sceneMatches(request.senderID)) {
      return;
    }
    {
      std::lock_guard<std::mutex> lock(shareMutex);
      if (!localWindowShares.contains(request.shareId)) {
        return;
      }
    }
    if (encodeWorker_) {
      encodeWorker_->requestKeyframe(request.shareId);
      spdlog::info("MultiPlayer: keyframe requested for share {} by player {}",
                   request.shareId,
                   request.senderID);
    }
    return;
  }

  if (type == ControlMessageType::Request ||
      type == ControlMessageType::Release) {
    bool release = false;
    if (!decodeControlRequestOrRelease(data, size, header, release)) {
      return;
    }
    if (!release && !sceneMatches(header.senderID)) {
      return;
    }
    if (release) {
      bool released = false;
      {
        std::lock_guard<std::mutex> lock(shareMutex);
        auto it = localShareControllers.find(header.shareId);
        if (it != localShareControllers.end() &&
            it->second == header.senderID) {
          localShareControllers.erase(it);
          released = true;
        }
      }
      if (released) {
        {
          std::lock_guard<std::mutex> lock(shareMutex);
          auto shareIt = localWindowShares.find(header.shareId);
          if (shareIt != localWindowShares.end()) {
            shareIt->second.controlActive = false;
            shareIt->second.controllerID = 0;
            shareIt->second.remotePointerX = -1;
            shareIt->second.remotePointerY = -1;
          }
        }
        controlNotice_ = "Player #" + std::to_string(header.senderID) +
                         " released control";
        controlNoticeUntil_ = nowSeconds() + 4.0;
        sendControlState(header.shareId, true, 0);
        spdlog::info("MultiPlayer: player {} released control of share {}",
                     header.senderID,
                     header.shareId);
      }
      return;
    }

    std::shared_ptr<WaylandApp> app;
    bool accepted = false;
    uint32_t controllerID = 0;
    {
      std::lock_guard<std::mutex> lock(shareMutex);
      auto shareIt = localWindowShares.find(header.shareId);
      if (shareIt == localWindowShares.end()) {
        return;
      }
      entt::entity entity = shareIt->second.entity;
      auto settingsIt = windowShares.find(entity);
      if (settingsIt == windowShares.end() ||
          !settingsIt->second.allowControl) {
        return;
      }
      auto controllerIt = localShareControllers.find(header.shareId);
      if (controllerIt != localShareControllers.end() &&
          controllerIt->second != header.senderID) {
        return;
      }
      localShareControllers[header.shareId] = header.senderID;
      controllerID = header.senderID;
      accepted = true;
      shareIt->second.controlActive = true;
      shareIt->second.controllerID = header.senderID;
      shareIt->second.remotePointerX = -1;
      shareIt->second.remotePointerY = -1;

      if (registry) {
        if (auto* component =
              registry->try_get<WaylandApp::Component>(entity)) {
          app = component->app;
        }
      }
    }

    if (accepted) {
      sendControlState(header.shareId, true, controllerID);
      controlNotice_ = "Player #" + std::to_string(controllerID) +
                       " took control";
      if (app && !app->getWindowName().empty()) {
        controlNotice_ += " of '" + app->getWindowName() + "'";
      }
      controlNoticeUntil_ = nowSeconds() + 5.0;
      spdlog::info("MultiPlayer: player {} took control of share {}",
                   controllerID,
                   header.shareId);
    }
    return;
  }

  std::shared_ptr<WaylandApp> app;
  {
    std::lock_guard<std::mutex> lock(shareMutex);
    auto controllerIt = localShareControllers.find(header.shareId);
    if (controllerIt == localShareControllers.end() ||
        controllerIt->second != header.senderID) {
      return;
    }
    auto shareIt = localWindowShares.find(header.shareId);
    if (shareIt == localWindowShares.end() || !registry) {
      return;
    }
    if (auto* component =
          registry->try_get<WaylandApp::Component>(shareIt->second.entity)) {
      app = component->app;
    }
  }
  if (!app) {
    return;
  }

  switch (type) {
    case ControlMessageType::PointerMotion: {
      float x = 0.0f;
      float y = 0.0f;
      if (decodeControlPointerMotion(data, size, header, x, y)) {
        if (!std::isfinite(x) || !std::isfinite(y)) {
          break;
        }
        const int pointerX = static_cast<int>(std::lround(x));
        const int pointerY = static_cast<int>(std::lround(y));
        {
          std::lock_guard<std::mutex> lock(shareMutex);
          auto shareIt = localWindowShares.find(header.shareId);
          if (shareIt != localWindowShares.end()) {
            shareIt->second.remotePointerX = pointerX;
            shareIt->second.remotePointerY = pointerY;
          }
        }
        app->injectPointerMotion(x, y);
      }
      break;
    }
    case ControlMessageType::PointerButton: {
      uint32_t button = 0;
      bool pressed = false;
      if (decodeControlPointerButton(data, size, header, button, pressed)) {
        app->injectPointerButton(button, pressed);
      }
      break;
    }
    case ControlMessageType::PointerAxis: {
      float delta = 0.0f;
      int32_t discrete = 0;
      uint32_t source = 0;
      uint32_t orientation = 0;
      if (decodeControlPointerAxis(
            data, size, header, delta, discrete, source, orientation)) {
        app->injectPointerAxis(delta, discrete, source, orientation);
      }
      break;
    }
    case ControlMessageType::Key: {
      uint32_t keycode = 0;
      bool pressed = false;
      if (decodeControlKey(data, size, header, keycode, pressed)) {
        app->injectKey(keycode, pressed);
      }
      break;
    }
    default:
      break;
  }
}

void
Client::handleControllerDisconnect(uint32_t playerID)
{
  std::vector<uint32_t> shares;
  {
    std::lock_guard<std::mutex> lock(shareMutex);
    for (auto it = localShareControllers.begin();
         it != localShareControllers.end();) {
      if (it->second == playerID) {
        shares.push_back(it->first);
        it = localShareControllers.erase(it);
      } else {
        ++it;
      }
    }
  }
  for (uint32_t shareId : shares) {
    sendControlState(shareId, true, 0);
  }
  if (!shares.empty()) {
    spdlog::info("MultiPlayer: controller {} disconnected; released {} share(s)",
                 playerID,
                 shares.size());
  }
}

std::vector<RemoteWindowInfo>
Client::remoteWindows() const
{
  std::lock_guard<std::mutex> lock(shareMutex);
  std::vector<RemoteWindowInfo> result;
  result.reserve(remoteWindowShares.size());
  const double now = nowSeconds();
  const uint32_t localID = peer != nullptr ? peer->connectID : 0;
  for (const auto& [key, share] : remoteWindowShares) {
    (void)key;
    RemoteWindowInfo info;
    info.ownerID = share.ownerID;
    info.shareId = share.shareId;
    info.controllerID = share.controllerID;
    info.allowControl = share.allowControl;
    info.controlPending =
      share.controlPending && now - share.controlRequestTime < 3.0;
    info.name = share.name;

    if (share.controllerID != 0 && share.controllerID == localID) {
      info.controlStatus = "Controlled by you";
    } else if (share.controllerID != 0) {
      info.controlStatus = "Controlled by #" +
                           std::to_string(share.controllerID);
    } else if (info.controlPending) {
      info.controlStatus = "Requesting control...";
    } else if (share.allowControl) {
      info.controlStatus = "Read-only (control available)";
    } else {
      info.controlStatus = "Read-only";
    }

    if (share.lastWebRtcFrame > 0.0 &&
        now - share.lastWebRtcFrame < 1.0) {
      info.transport = "WebRTC";
    } else if (share.webrtcFailed) {
      if (share.nextWebRtcRetry > now) {
        info.transport = "ENet fallback (WebRTC retry pending)";
      } else {
        info.transport = "WebRTC reconnecting";
      }
    } else if (share.webrtcRequested && share.lastWebRtcFrame > 0.0) {
      info.transport = "ENet fallback (WebRTC stalled)";
    } else if (share.webrtcRequested &&
               now - share.webrtcRequestTime < 5.0) {
      info.transport = "WebRTC connecting";
    } else {
      info.transport = "ENet fallback";
    }
    result.push_back(std::move(info));
  }
  return result;
}

std::string
Client::windowStreamTransport(entt::entity entity) const
{
  std::lock_guard<std::mutex> lock(shareMutex);
  auto idIt = localShareIds.find(entity);
  if (idIt == localShareIds.end()) {
    return {};
  }
  auto shareIt = localWindowShares.find(idIt->second);
  if (shareIt == localWindowShares.end()) {
    return {};
  }
  const auto& share = shareIt->second;
  std::string result;
  if (share.h264Unavailable) {
    result = "ENet MJPEG";
  } else if (webrtc && webrtc->hasConnectedSenderForShare(idIt->second)) {
    result = "WebRTC";
  } else {
    result = "ENet H.264";
  }
  const int fps = share.controlActive
                    ? std::max(std::clamp(windowStreamFps(), 1, 30), 15)
                    : std::clamp(windowStreamFps(), 1, 30);
  result += " " + std::to_string(fps) + "fps";
  if (share.adaptiveQuality > 0) {
    result += " q" + std::to_string(share.adaptiveQuality);
  }
  if (share.webrtcRtt >= 0.0) {
    result += " rtt" +
              std::to_string(static_cast<int>(std::lround(share.webrtcRtt *
                                                          1000.0))) +
              "ms";
  }
  if (share.controlActive && share.controllerID != 0) {
    result += " <- #" + std::to_string(share.controllerID);
  }
  return result;
}

std::string
Client::controlNotice() const
{
  if (nowSeconds() < controlNoticeUntil_) {
    return controlNotice_;
  }
  return {};
}

bool
Client::isControllingRemote() const
{
  std::lock_guard<std::mutex> lock(shareMutex);
  return controlledShareId != 0;
}

bool
Client::isControllingRemoteEntity(entt::entity entity) const
{
  std::lock_guard<std::mutex> lock(shareMutex);
  if (controlledShareId == 0) {
    return false;
  }
  const uint64_t key =
    (static_cast<uint64_t>(controlledOwnerID) << 32) | controlledShareId;
  auto it = remoteWindowShares.find(key);
  return it != remoteWindowShares.end() && it->second.entity == entity;
}

void
Client::requestRemoteControl(uint32_t ownerID, uint32_t shareId)
{
  if (!isConnected() || peer == nullptr) {
    return;
  }
  const uint64_t key = (static_cast<uint64_t>(ownerID) << 32) | shareId;
  {
    std::lock_guard<std::mutex> lock(shareMutex);
    auto it = remoteWindowShares.find(key);
    if (it == remoteWindowShares.end()) {
      return;
    }
    it->second.controlPending = true;
    it->second.controlRequestTime = nowSeconds();
  }
  sendControlPacket(
    encodeControlRequest(ownerID, shareId, peer->connectID), true);
}

void
Client::requestRemoteKeyframe(uint32_t ownerID, uint32_t shareId)
{
  if (!isConnected() || peer == nullptr) {
    return;
  }
  sendControlPacket(
    encodeControlKeyframeRequest(ownerID, shareId, peer->connectID), true);
  spdlog::info("MultiPlayer: requested keyframe for share {}", shareId);
}

void
Client::releaseRemoteControl()
{
  uint32_t ownerID = 0;
  uint32_t shareId = 0;
  {
    std::lock_guard<std::mutex> lock(shareMutex);
    ownerID = controlledOwnerID;
    shareId = controlledShareId;
    controlledOwnerID = 0;
    controlledShareId = 0;
    const uint64_t key = (static_cast<uint64_t>(ownerID) << 32) | shareId;
    auto it = remoteWindowShares.find(key);
    if (it != remoteWindowShares.end()) {
      it->second.controlPending = false;
      if (it->second.controllerID ==
          (peer != nullptr ? peer->connectID : 0)) {
        it->second.controllerID = 0;
      }
    }
  }
  if (ownerID != 0 && shareId != 0 && isConnected() && peer != nullptr) {
    sendControlPacket(
      encodeControlRelease(ownerID, shareId, peer->connectID), true);
    spdlog::info("MultiPlayer: released control of share {}", shareId);
  }
}

void
Client::sendRemotePointerMotion(double x, double y)
{
  uint32_t ownerID = 0;
  uint32_t shareId = 0;
  {
    std::lock_guard<std::mutex> lock(shareMutex);
    ownerID = controlledOwnerID;
    shareId = controlledShareId;
  }
  if (ownerID == 0 || shareId == 0 || peer == nullptr) {
    return;
  }
  sendControlPacket(encodeControlPointerMotion(ownerID,
                                               shareId,
                                               peer->connectID,
                                               static_cast<float>(x),
                                               static_cast<float>(y)),
                    false);
}

void
Client::sendRemotePointerButton(uint32_t button, bool pressed)
{
  uint32_t ownerID = 0;
  uint32_t shareId = 0;
  {
    std::lock_guard<std::mutex> lock(shareMutex);
    ownerID = controlledOwnerID;
    shareId = controlledShareId;
  }
  if (ownerID == 0 || shareId == 0 || peer == nullptr) {
    return;
  }
  sendControlPacket(encodeControlPointerButton(
                      ownerID, shareId, peer->connectID, button, pressed),
                    true);
}

void
Client::sendRemotePointerAxis(double delta,
                              int32_t deltaDiscrete,
                              uint32_t source,
                              uint32_t orientation)
{
  uint32_t ownerID = 0;
  uint32_t shareId = 0;
  {
    std::lock_guard<std::mutex> lock(shareMutex);
    ownerID = controlledOwnerID;
    shareId = controlledShareId;
  }
  if (ownerID == 0 || shareId == 0 || peer == nullptr) {
    return;
  }
  sendControlPacket(encodeControlPointerAxis(ownerID,
                                             shareId,
                                             peer->connectID,
                                             static_cast<float>(delta),
                                             deltaDiscrete,
                                             source,
                                             orientation),
                    false);
}

void
Client::sendRemoteKey(uint32_t keycode, bool pressed)
{
  uint32_t ownerID = 0;
  uint32_t shareId = 0;
  {
    std::lock_guard<std::mutex> lock(shareMutex);
    ownerID = controlledOwnerID;
    shareId = controlledShareId;
  }
  if (ownerID == 0 || shareId == 0 || peer == nullptr) {
    return;
  }
  sendControlPacket(encodeControlKey(
                      ownerID, shareId, peer->connectID, keycode, pressed),
                    true);
}

void
Client::removeRemoteWindow(uint32_t ownerID, uint32_t shareId)
{
  entt::entity entity = entt::null;
  {
    std::lock_guard<std::mutex> lock(shareMutex);
    const uint64_t key = (static_cast<uint64_t>(ownerID) << 32) | shareId;
    auto it = remoteWindowShares.find(key);
    if (it == remoteWindowShares.end()) {
      return;
    }
    entity = it->second.entity;
    remoteWindowShares.erase(it);
    if (controlledOwnerID == ownerID && controlledShareId == shareId) {
      controlledOwnerID = 0;
      controlledShareId = 0;
    }
  }
  if (webrtc) {
    webrtc->removeReceiver(ownerID, shareId);
  }
  if (registry && entity != entt::null && registry->valid(entity)) {
    registry->destroy(entity);
  }
}

void
Client::removeRemoteWindowsForOwner(uint32_t ownerID)
{
  std::vector<entt::entity> entities;
  {
    std::lock_guard<std::mutex> lock(shareMutex);
    for (auto it = remoteWindowShares.begin();
         it != remoteWindowShares.end();) {
      if (it->second.ownerID == ownerID) {
        entities.push_back(it->second.entity);
        it = remoteWindowShares.erase(it);
      } else {
        ++it;
      }
    }
    if (controlledOwnerID == ownerID) {
      controlledOwnerID = 0;
      controlledShareId = 0;
    }
  }
  if (webrtc) {
    webrtc->removePeer(ownerID);
  }
  if (registry) {
    for (entt::entity entity : entities) {
      if (entity != entt::null && registry->valid(entity)) {
        registry->destroy(entity);
      }
    }
  }
  if (!entities.empty()) {
    spdlog::info("MultiPlayer: removed {} shared window(s) from player {}",
                 entities.size(),
                 ownerID);
  }
}

void
Client::clearRemoteWindows()
{
  std::vector<entt::entity> entities;
  {
    std::lock_guard<std::mutex> lock(shareMutex);
    entities.reserve(remoteWindowShares.size());
    for (const auto& [key, share] : remoteWindowShares) {
      (void)key;
      entities.push_back(share.entity);
    }
    remoteWindowShares.clear();
    controlledOwnerID = 0;
    controlledShareId = 0;
  }
  if (registry) {
    for (entt::entity entity : entities) {
      if (entity != entt::null && registry->valid(entity)) {
        registry->destroy(entity);
      }
    }
  }
}

bool
Client::isConnected() const
{
  return connectionState_.load() == ConnectionState::Connected;
}

bool
Client::isConnecting() const
{
  return connectionState_.load() == ConnectionState::Connecting;
}

bool
Client::isDisconnecting() const
{
  return connectionState_.load() == ConnectionState::Disconnecting;
}

bool
Client::isDisconnected() const
{
  return connectionState_.load() == ConnectionState::Disconnected;
}

std::string
Client::connectionStateText() const
{
  switch (connectionState_.load()) {
    case ConnectionState::Connecting:
      return "Connecting...";
    case ConnectionState::Connected:
      return "Connected";
    case ConnectionState::Disconnecting:
      return "Disconnecting...";
    case ConnectionState::Disconnected:
    default:
      if (!connectionError_.empty()) {
        return "Disconnected: " + connectionError_;
      }
      return "Disconnected";
  }
}

bool
Client::shouldSendPlayerPacket() {
  return nowSeconds()-lastUpdate >= UPDATE_EVERY;
}

void Client::justSentPlayerPacket() {
  lastUpdate = nowSeconds();
}

bool
Client::sendPlayer(glm::vec3 position, glm::vec3 front)
{
  if (isConnected() && peer != nullptr && client != nullptr &&
      shouldSendPlayerPacket()) {
    ENetPacket* packet = enet_packet_create(
      NULL, sizeof(glm::vec3) * 2, ENET_PACKET_FLAG_UNSEQUENCED);
    if (packet == nullptr) {
      return false;
    }

    // Copy the player's position and front vector into the packet data
    glm::vec3* data = reinterpret_cast<glm::vec3*>(packet->data);
    data[0] = position;
    data[1] = front;

    // Send the packet on the dedicated channel for players
    if (enet_peer_send(peer, 1, packet) != 0) {
      enet_packet_destroy(packet);
      return false;
    }
    enet_host_flush(client);

    justSentPlayerPacket();
    return true;
  }

  return false;
}
}
