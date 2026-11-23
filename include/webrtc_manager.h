#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "webrtc_protocol.h"

// GStreamer webrtcbin transport for shared windows. All public methods are
// called from the engine/render thread; GStreamer work is marshalled onto an
// internal GLib main context.
class WebRtcManager
{
public:
  struct Signal
  {
    MultiPlayer::WebRtcSignalType type =
      MultiPlayer::WebRtcSignalType::Request;
    uint32_t targetOwner = 0;
    uint32_t shareId = 0;
    std::string data;
    int32_t mlineIndex = 0;
  };

  struct DecodedFrame
  {
    uint32_t ownerID = 0;
    uint32_t shareId = 0;
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba;
  };

  // RTCP receiver-report feedback for one sender/viewer pair. `fractionLost`
  // is in the range [0, 1]; `roundTripTime` is in seconds.
  struct SenderStats
  {
    uint32_t shareId = 0;
    uint32_t viewerID = 0;
    double fractionLost = 0.0;
    double roundTripTime = 0.0;
  };

  struct ConnectionEvent
  {
    enum class Type
    {
      SenderConnected,
      SenderFailed,
      ReceiverConnected,
      ReceiverFailed,
    };
    Type type = Type::ReceiverFailed;
    uint32_t shareId = 0;
    // Viewer ID for sender events, owner ID for receiver events.
    uint32_t peerID = 0;
  };

  WebRtcManager();
  ~WebRtcManager();

  WebRtcManager(const WebRtcManager&) = delete;
  WebRtcManager& operator=(const WebRtcManager&) = delete;

  // Owner side: one peer connection per viewer/share.
  void ensureSender(uint32_t viewerID, uint32_t shareId);
  void removeViewer(uint32_t viewerID);
  void removePeer(uint32_t peerID);
  void removeSender(uint32_t viewerID, uint32_t shareId);
  void removeSenderShare(uint32_t shareId);
  bool hasSenderForShare(uint32_t shareId) const;
  bool hasConnectedSenderForShare(uint32_t shareId) const;

  // Viewer side: one peer connection per owner/share.
  void ensureReceiver(uint32_t ownerID, uint32_t shareId);
  void removeReceiver(uint32_t ownerID, uint32_t shareId);

  // Remove all sessions belonging to a share (owner or viewer side).
  void removeShare(uint32_t shareId);

  // Owner side: push an encoded H.264 access unit to every sender for a share.
  // The pipeline timestamps the buffer on arrival (appsrc do-timestamp).
  void pushEncodedFrame(uint32_t shareId,
                        const uint8_t* data,
                        std::size_t size);

  // Ask every sender for a fresh RTCP stats snapshot. Responses are drained
  // with takeSenderStats() on the engine thread.
  void pollSenderStats();

  // Incoming signaling from ENet.
  void handleOffer(uint32_t ownerID,
                   uint32_t shareId,
                   const std::string& sdp);
  void handleAnswer(uint32_t ownerID,
                    uint32_t shareId,
                    const std::string& sdp);
  void handleIce(uint32_t peerID,
                 uint32_t shareId,
                 int32_t mlineIndex,
                 const std::string& candidate);

  // Engine-thread drains.
  std::vector<Signal> takeSignals();
  std::vector<DecodedFrame> takeDecodedFrames();
  std::vector<SenderStats> takeSenderStats();
  std::vector<ConnectionEvent> takeConnectionEvents();
  // Shares whose WebRTC transport just became ready. The owner should force an
  // IDR so the viewer does not wait for the next GOP.
  std::vector<uint32_t> takeKeyframeRequests();

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
