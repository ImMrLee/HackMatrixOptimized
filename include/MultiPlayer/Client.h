#pragma once

#include <enet/enet.h>
#include <string>
#include <glm/glm.hpp>
#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>
#include "entity.h"
#include "scene_fingerprint.h"

class H264Decoder;
class WebRtcManager;
class WindowCapture;
class WindowEncodeWorker;
class World;

namespace MultiPlayer {

struct WorldSyncEntityTransform;

struct WindowShareSettings
{
  bool shared = false;
  bool allowControl = false;
};

struct RemoteWindowInfo
{
  uint32_t ownerID = 0;
  uint32_t shareId = 0;
  uint32_t controllerID = 0;
  bool allowControl = false;
  bool controlPending = false;
  std::string name;
  // "WebRTC", "WebRTC connecting", "ENet fallback", ...
  std::string transport;
  // Human-readable control state for the table.
  std::string controlStatus;
};

class Client
{
public:
  Client(std::shared_ptr<EntityRegistry>, bool persistSettings = true);
  ~Client();

  // Starts a non-blocking connection. The state machine is driven by poll().
  bool connect(const std::string& address, int port);
  bool isConnected() const;
  bool isConnecting() const;
  bool isDisconnecting() const;
  bool isDisconnected() const;
  const std::string& connectionError() const { return connectionError_; }
  // "Disconnected", "Connecting...", "Connected", "Disconnecting..."
  std::string connectionStateText() const;
  void disconnect();
  bool sendPlayer(glm::vec3, glm::vec3);
  void poll();
  bool shouldSendPlayerPacket();
  void justSentPlayerPacket();

  SceneFingerprint localSceneFingerprint() const;
  bool hasLocalScene() const;
  bool isComputingScene() const;
  bool hasSceneMismatch() const;
  bool hasPeerSceneInfo() const;
  bool hasSceneInfoFor(uint32_t playerID) const;
  bool sceneMatches(uint32_t playerID) const;
  std::vector<uint32_t> mismatchedPeers() const;
  std::size_t peerSceneCount() const;

  // Video capture/transport tick. Must run on the render thread with the GL
  // context current (Engine::multiplayerClientIteration calls it after render).
  void tickWindowStreams();
  void tickWebRtc();
  void clearRemoteWindows();

  // Target capture settings for windows this client shares.
  int windowStreamFps() const { return windowStreamFps_.load(); }
  void setWindowStreamFps(int fps);
  int windowStreamQuality() const { return windowStreamQuality_.load(); }
  void setWindowStreamQuality(int quality);

  // Local per-window sharing state used by the Multiplayer panel. These flags
  // are the source of truth for "which windows are published" and "who may
  // control them".
  WindowShareSettings windowShare(entt::entity entity) const;
  void setWindowShared(entt::entity entity, bool shared);
  void setWindowAllowControl(entt::entity entity, bool allowControl);
  std::vector<entt::entity> sharedWindows() const;
  void clearWindowShares();

  std::vector<RemoteWindowInfo> remoteWindows() const;
  // "WebRTC (N viewers)" / "ENet fallback" / "Control boost 15 FPS", ...
  std::string windowStreamTransport(entt::entity entity) const;
  std::string controlNotice() const;
  uint32_t localPlayerID() const { return peer ? peer->connectID : 0; }
  bool isControllingRemote() const;
  bool isControllingRemoteEntity(entt::entity entity) const;
  void requestRemoteControl(uint32_t ownerID, uint32_t shareId);
  void releaseRemoteControl();
  void requestRemoteKeyframe(uint32_t ownerID, uint32_t shareId);
  void sendRemotePointerMotion(double x, double y);
  void sendRemotePointerButton(uint32_t button, bool pressed);
  void sendRemotePointerAxis(double delta,
                             int32_t deltaDiscrete,
                             uint32_t source,
                             uint32_t orientation);
  void sendRemoteKey(uint32_t keycode, bool pressed);

  // F14a authoritative world sync (voxel overlay). The relay elects the first
  // client as authority; the authority broadcasts sequenced ops, other clients
  // forward edits and apply the authoritative broadcast.
  void setWorld(World* world) { world_ = world; }
  uint32_t worldAuthorityID() const { return authorityID_.load(); }
  bool isWorldAuthority() const;
  // Send an edit. Authority broadcasts with a fresh seq; others forward to the
  // authority. Local application is the caller's (Api) responsibility.
  void sendWorldAdd(bool replace,
                    float size,
                    const std::vector<glm::vec3>& positions,
                    const std::vector<glm::vec3>& colors);
  void sendWorldClearBox(const glm::vec3& min, const glm::vec3& max);
  void sendWorldClearPositions(const std::vector<glm::vec3>& positions);
  // v2: DB-persisted scene entities (stable ids across peers).
  bool isSyncedEntity(uint32_t entityID) const;
  void sendWorldEntityMove(uint32_t entityID,
                           const glm::vec3& delta,
                           float unitsPerSecond);
  void sendWorldEntityEdit(uint32_t entityID,
                           const glm::vec3& position,
                           const glm::vec3& origin,
                           const glm::vec3& rotate,
                           float scale);

private:
  enum class ConnectionState : std::uint8_t
  {
    Disconnected,
    Connecting,
    Connected,
    Disconnecting,
  };

  struct LocalWindowShare
  {
    entt::entity entity = entt::null;
    double lastFrame = 0.0;
    glm::vec3 pos{ 0.0f };
    glm::vec3 origin{ 0.0f };
    glm::vec3 rotate{ 0.0f };
    float scale = 1.0f;
    int configuredQuality = -1;
    int adaptiveQuality = -1;
    double lastAdaptation = 0.0;
    // Worst RTCP feedback seen for this share since the last stats poll.
    double webrtcLoss = -1.0;
    double webrtcRtt = -1.0;
    double webrtcStatsTime = 0.0;
    bool h264Unavailable = false;
    bool controlActive = false;
    uint32_t controllerID = 0;
    // Last remote pointer position in surface coordinates (cursor overlay).
    int remotePointerX = -1;
    int remotePointerY = -1;
    // GL readback state is only touched on the render thread.
    std::shared_ptr<WindowCapture> capture;
  };

  struct RemoteWindowShare
  {
    entt::entity entity = entt::null;
    uint32_t ownerID = 0;
    uint32_t shareId = 0;
    uint32_t controllerID = 0;
    bool allowControl = false;
    bool controlPending = false;
    double controlRequestTime = 0.0;
    bool webrtcRequested = false;
    double webrtcRequestTime = 0.0;
    bool webrtcFailed = false;
    double nextWebRtcRetry = 0.0;
    int webrtcRetryCount = 0;
    std::string name;
    std::shared_ptr<H264Decoder> decoder;
    double lastKeyframeRequest = 0.0;
    // Timestamp of the last frame decoded from the WebRTC path. While it is
    // fresh, the ENet H.264 fallback is ignored to avoid double decoding.
    double lastWebRtcFrame = 0.0;
  };

  bool sendSceneInfo();
  void handleSceneInfo(const uint8_t* data, std::size_t size);
  void startSceneHash();
  void pollSceneHash();
  void handleEnetEvent(ENetEvent& event);
  void failConnection(const std::string& reason);
  void finishConnection();
  void finishDisconnect();
  void cleanupConnectionState();

  bool startWindowShare(entt::entity entity);
  void syncWindowShares();
  void stopWindowShare(entt::entity entity);
  void stopAllWindowShares();
  void handleWindowStream(const uint8_t* data, std::size_t size);
  void handleVideoFrame(const uint8_t* data, std::size_t size);
  void handleWebRtcSignal(const uint8_t* data, std::size_t size);
  void sendWebRtcSignal(const std::vector<uint8_t>& bytes);
  void ensureWebRtc();
  void handleControlMessage(const uint8_t* data, std::size_t size);
  void handleControlState(const uint8_t* data, std::size_t size);
  void flushPendingWindowMessages(uint32_t ownerID);
  void sendControlState(uint32_t shareId,
                        bool allowControl,
                        uint32_t controllerID);
  void sendControlPacket(const std::vector<uint8_t>& bytes, bool reliable);
  void handleControllerDisconnect(uint32_t playerID);
  void removeRemoteWindow(uint32_t ownerID, uint32_t shareId);
  void removeRemoteWindowsForOwner(uint32_t ownerID);
  void sendWindowShareStop(uint32_t shareId);
  void sendWindowShareTransform(uint32_t shareId,
                                const glm::vec3& pos,
                                const glm::vec3& origin,
                                const glm::vec3& rotate,
                                float scale);
  void loadWindowStreamSettings();
  void persistWindowStreamFps(int fps);
  void persistWindowStreamQuality(int quality);
  WindowEncodeWorker* ensureEncodeWorker();

  void handleWorldSync(const uint8_t* data, std::size_t size);
  void tickWorldSync();
  void queueWorldMessage(const std::vector<uint8_t>& bytes);
  void requestWorldSnapshot();
  void sendWorldSnapshot(uint32_t requester);
  void applyWorldAdd(bool replace,
                     float size,
                     const std::vector<glm::vec3>& positions,
                     const std::vector<glm::vec3>& colors);
  void applyWorldClearBox(const glm::vec3& min, const glm::vec3& max);
  void applyWorldClearPositions(const std::vector<glm::vec3>& positions);
  void tickEntitySync();
  void sendWorldEntitySnapshot(uint32_t targetOwner);
  void applyWorldEntityTransform(const WorldSyncEntityTransform& transform);

  ENetHost* client;
  ENetPeer* peer;
  std::atomic<ConnectionState> connectionState_{
    ConnectionState::Disconnected
  };
  double connectDeadline_ = 0.0;
  double disconnectDeadline_ = 0.0;
  std::string connectionError_;
  double lastUpdate = 0;
  double UPDATE_EVERY = 1.0 / 20.0;
  std::shared_ptr<EntityRegistry> registry;

  mutable std::mutex sceneMutex;
  SceneFingerprint localScene;
  std::unordered_map<uint32_t, SceneFingerprint> peerScenes;

  mutable std::mutex shareMutex;
  std::unordered_map<entt::entity, WindowShareSettings> windowShares;
  std::unordered_map<entt::entity, uint32_t> localShareIds;
  std::unordered_map<uint32_t, LocalWindowShare> localWindowShares;
  std::unordered_map<uint32_t, uint32_t> localShareControllers;
  std::unordered_map<uint64_t, RemoteWindowShare> remoteWindowShares;
  std::unordered_map<uint32_t, std::vector<std::vector<uint8_t>>>
    pendingWindowMessages;
  uint32_t controlledOwnerID = 0;
  uint32_t controlledShareId = 0;
  uint32_t nextWindowShareId = 1;
  std::atomic<int> windowStreamFps_{ 5 };
  std::atomic<int> windowStreamQuality_{ 65 };
  bool persistSettings_ = true;
  std::unique_ptr<WebRtcManager> webrtc;
  double nextWebRtcStatsPoll_ = 0.0;

  struct SceneHashJob;
  std::shared_ptr<SceneHashJob> sceneHashJob_;
  bool sceneInfoSent_ = false;

  std::unique_ptr<WindowEncodeWorker> encodeWorker_;
  std::string controlNotice_;
  double controlNoticeUntil_ = 0.0;

  // F14a world sync state. world_ is owned by Engine and set each frame.
  World* world_ = nullptr;
  std::atomic<uint32_t> authorityID_{ 0 };
  uint64_t worldSeq_ = 0;          // authority's outgoing sequence
  uint64_t lastAppliedWorldSeq_ = 0; // last authoritative seq applied
  std::mutex worldOutMutex_;
  std::vector<std::vector<uint8_t>> worldOutbox_;
  // Authority-side cache of DB-persisted entity transforms, scanned at ~20 Hz.
  struct SyncedTransform
  {
    glm::vec3 position{ 0.0f };
    glm::vec3 origin{ 0.0f };
    glm::vec3 rotate{ 0.0f };
    float scale = 1.0f;
  };
  std::unordered_map<uint32_t, SyncedTransform> syncedTransforms_;
  bool entitySyncPrimed_ = false;
  double nextEntityScan_ = 0.0;
};

}
