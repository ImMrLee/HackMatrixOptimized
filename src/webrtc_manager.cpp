#define GST_USE_UNSTABLE_API 1

#include "webrtc_manager.h"

#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>
#include <gst/sdp/sdp.h>
#include <gst/webrtc/webrtc.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <functional>
#include <mutex>
#include <thread>
#include <unordered_map>

#include <spdlog/spdlog.h>

namespace {

void
ensureGstInitialized()
{
  static std::once_flag once;
  std::call_once(once, []() { gst_init(nullptr, nullptr); });
}

uint64_t
sessionKey(uint32_t peer, uint32_t share)
{
  return (static_cast<uint64_t>(peer) << 32) | share;
}

} // namespace

struct WebRtcManager::Impl
{
  struct SenderSession;
  struct ReceiverSession;

  GMainContext* context = nullptr;
  GMainLoop* loop = nullptr;
  std::thread thread;
  std::thread::id gstThreadId;
  std::atomic<bool> shuttingDown{ false };

  mutable std::mutex mutex;
  std::unordered_map<uint64_t, std::shared_ptr<SenderSession>> senders;
  std::unordered_map<uint64_t, std::shared_ptr<ReceiverSession>> receivers;
  // Sessions are retained after teardown because queued GLib tasks and
  // promise change callbacks may still reference them. They are released when
  // the manager is destroyed (a handful per share/unshare cycle, not per
  // frame).
  std::vector<std::shared_ptr<SenderSession>> retiredSenders;
  std::vector<std::shared_ptr<ReceiverSession>> retiredReceivers;
  std::vector<Signal> signals;
  std::vector<DecodedFrame> frames;
  std::vector<SenderStats> senderStats;
  std::vector<ConnectionEvent> connectionEvents;
  std::vector<uint32_t> keyframeRequests;

  Impl()
  {
    ensureGstInitialized();
    context = g_main_context_new();
    loop = g_main_loop_new(context, FALSE);
    thread = std::thread([this]() {
      gstThreadId = std::this_thread::get_id();
      g_main_context_push_thread_default(context);
      g_main_loop_run(loop);
      g_main_context_pop_thread_default(context);
    });
  }

  ~Impl();

  void post(std::function<void()> fn)
  {
    struct Task
    {
      std::function<void()> fn;
    };
    auto* task = new Task{ std::move(fn) };
    if (shuttingDown.load()) {
      delete task;
      return;
    }
    // Always queue an idle source (even when called from the GLib thread) so
    // the destroy notify reliably frees the task whether it runs or is
    // discarded when the context is torn down.
    GSource* source = g_idle_source_new();
    g_source_set_priority(source, G_PRIORITY_DEFAULT);
    g_source_set_callback(
      source,
      [](gpointer data) -> gboolean {
        static_cast<Task*>(data)->fn();
        return G_SOURCE_REMOVE;
      },
      task,
      [](gpointer data) { delete static_cast<Task*>(data); });
    g_source_attach(source, context);
    g_source_unref(source);
  }

  void queueSignal(Signal signal)
  {
    std::lock_guard<std::mutex> lock(mutex);
    signals.push_back(std::move(signal));
  }

  void queueKeyframeRequest(uint32_t shareId)
  {
    std::lock_guard<std::mutex> lock(mutex);
    keyframeRequests.push_back(shareId);
  }

  void queueSenderStats(SenderStats stats)
  {
    std::lock_guard<std::mutex> lock(mutex);
    senderStats.push_back(stats);
  }

  void queueConnectionEvent(ConnectionEvent event)
  {
    std::lock_guard<std::mutex> lock(mutex);
    connectionEvents.push_back(event);
  }

  void queueFrame(DecodedFrame frame)
  {
    std::lock_guard<std::mutex> lock(mutex);
    // Keep latency bounded if the render loop stalls: only the newest frames
    // matter for a live window preview.
    frames.push_back(std::move(frame));
    while (frames.size() > 4) {
      frames.erase(frames.begin());
    }
  }
};

struct WebRtcManager::Impl::SenderSession
{
  Impl* impl = nullptr;
  uint32_t viewerID = 0;
  uint32_t shareId = 0;
  GstElement* pipeline = nullptr;
  GstElement* webrtc = nullptr;
  GstElement* appsrc = nullptr;
  GstPad* sinkPad = nullptr;
  std::atomic<bool> stopping{ false };
  std::atomic<bool> connected{ false };
  std::atomic<bool> loggedFirstFrame{ false };
  std::atomic<bool> loggedStats{ false };

  static void
  onNegotiationNeeded(GstElement* webrtc, gpointer data)
  {
    (void)webrtc;
    auto* self = static_cast<SenderSession*>(data);
    if (!self || self->stopping.load()) {
      return;
    }
    self->impl->post([self]() {
      if (self->stopping.load()) {
        return;
      }
      GstPromise* promise = gst_promise_new_with_change_func(
        &SenderSession::onOfferCreated, self, nullptr);
      g_signal_emit_by_name(self->webrtc, "create-offer", nullptr, promise);
    });
  }

  static void
  onOfferCreated(GstPromise* promise, gpointer data)
  {
    auto* self = static_cast<SenderSession*>(data);
    if (!self || self->stopping.load()) {
      gst_promise_unref(promise);
      return;
    }
    const GstStructure* reply = gst_promise_get_reply(promise);
    GstWebRTCSessionDescription* offer = nullptr;
    if (reply != nullptr) {
      gst_structure_get(reply,
                        "offer",
                        GST_TYPE_WEBRTC_SESSION_DESCRIPTION,
                        &offer,
                        nullptr);
    }
    if (offer == nullptr) {
      gst_promise_unref(promise);
      return;
    }
    g_signal_emit_by_name(self->webrtc, "set-local-description", offer, nullptr);
    gchar* text = gst_sdp_message_as_text(offer->sdp);
    if (text != nullptr) {
      spdlog::info("WebRTC sender: offer ready for share {} to viewer {}",
                   self->shareId,
                   self->viewerID);
      Signal signal;
      signal.type = MultiPlayer::WebRtcSignalType::Offer;
      signal.targetOwner = self->viewerID;
      signal.shareId = self->shareId;
      signal.data = text;
      self->impl->queueSignal(std::move(signal));
      g_free(text);
    }
    gst_webrtc_session_description_free(offer);
    gst_promise_unref(promise);
  }

  static void
  onConnectionState(GObject* object, GParamSpec* pspec, gpointer data)
  {
    (void)pspec;
    auto* self = static_cast<SenderSession*>(data);
    if (!self || self->stopping.load()) {
      return;
    }
    GstWebRTCPeerConnectionState state = GST_WEBRTC_PEER_CONNECTION_STATE_NEW;
    g_object_get(object, "connection-state", &state, nullptr);
    spdlog::info("WebRTC sender: connection state {} for share {} viewer {}",
                 static_cast<int>(state),
                 self->shareId,
                 self->viewerID);
    if (state == GST_WEBRTC_PEER_CONNECTION_STATE_CONNECTED) {
      self->connected.store(true);
      self->impl->queueConnectionEvent(
        ConnectionEvent{ ConnectionEvent::Type::SenderConnected,
                         self->shareId,
                         self->viewerID });
      // The transport is ready now. Force a fresh IDR instead of waiting for
      // the encoder's GOP boundary.
      self->impl->queueKeyframeRequest(self->shareId);
    } else if (state == GST_WEBRTC_PEER_CONNECTION_STATE_FAILED ||
               state == GST_WEBRTC_PEER_CONNECTION_STATE_DISCONNECTED ||
               state == GST_WEBRTC_PEER_CONNECTION_STATE_CLOSED) {
      self->connected.store(false);
      self->impl->queueConnectionEvent(
        ConnectionEvent{ ConnectionEvent::Type::SenderFailed,
                         self->shareId,
                         self->viewerID });
    }
  }

  static void
  onIceConnectionState(GObject* object, GParamSpec* pspec, gpointer data)
  {
    (void)pspec;
    auto* self = static_cast<SenderSession*>(data);
    if (!self || self->stopping.load()) {
      return;
    }
    GstWebRTCICEConnectionState state =
      GST_WEBRTC_ICE_CONNECTION_STATE_NEW;
    g_object_get(object, "ice-connection-state", &state, nullptr);
    spdlog::info("WebRTC sender: ICE state {} for share {} viewer {}",
                 static_cast<int>(state),
                 self->shareId,
                 self->viewerID);
  }

  static void
  onIceCandidate(GstElement* webrtc,
                 guint mlineIndex,
                 gchar* candidate,
                 gpointer data)
  {
    (void)webrtc;
    auto* self = static_cast<SenderSession*>(data);
    if (!self || self->stopping.load() || candidate == nullptr) {
      return;
    }
    Signal signal;
    signal.type = MultiPlayer::WebRtcSignalType::Ice;
    signal.targetOwner = self->viewerID;
    signal.shareId = self->shareId;
    signal.mlineIndex = static_cast<int32_t>(mlineIndex);
    signal.data = candidate;
    self->impl->queueSignal(std::move(signal));
  }

  static gboolean
  onBusMessage(GstBus* bus, GstMessage* message, gpointer data)
  {
    (void)bus;
    (void)data;
    if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
      GError* error = nullptr;
      gchar* debug = nullptr;
      gst_message_parse_error(message, &error, &debug);
      spdlog::warn("WebRTC sender error: {} ({})",
                   error ? error->message : "unknown",
                   debug ? debug : "");
      g_clear_error(&error);
      g_free(debug);
    } else if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_WARNING) {
      GError* error = nullptr;
      gchar* debug = nullptr;
      gst_message_parse_warning(message, &error, &debug);
      spdlog::warn("WebRTC sender warning: {} ({})",
                   error ? error->message : "unknown",
                   debug ? debug : "");
      g_clear_error(&error);
      g_free(debug);
    }
    return G_SOURCE_CONTINUE;
  }

  void start()
  {
    if (stopping.load()) {
      return;
    }
    pipeline = gst_pipeline_new(nullptr);
    webrtc = gst_element_factory_make("webrtcbin", nullptr);
    appsrc = gst_element_factory_make("appsrc", nullptr);
    GstElement* parse = gst_element_factory_make("h264parse", nullptr);
    GstElement* pay = gst_element_factory_make("rtph264pay", nullptr);
    GstElement* capsFilter = gst_element_factory_make("capsfilter", nullptr);
    if (!pipeline || !webrtc || !appsrc || !parse || !pay || !capsFilter) {
      spdlog::warn("WebRTC sender: missing GStreamer elements");
      stop();
      return;
    }

    GstCaps* h264Caps = gst_caps_from_string(
      "video/x-h264,stream-format=byte-stream,alignment=au");
    // do-timestamp stamps each pushed buffer with the pipeline clock, which
    // keeps the RTP timeline aligned with the receiving jitterbuffer.
    g_object_set(appsrc,
                 "is-live",
                 TRUE,
                 "do-timestamp",
                 TRUE,
                 "format",
                 GST_FORMAT_TIME,
                 "block",
                 FALSE,
                 "caps",
                 h264Caps,
                 nullptr);
    gst_caps_unref(h264Caps);
    g_object_set(pay, "config-interval", 1, "pt", 96, nullptr);

    GstCaps* rtpCaps = gst_caps_from_string(
      "application/x-rtp,media=video,encoding-name=H264,payload=96,"
      "clock-rate=90000");
    g_object_set(capsFilter, "caps", rtpCaps, nullptr);
    gst_caps_unref(rtpCaps);

    gst_bin_add_many(
      GST_BIN(pipeline), appsrc, parse, pay, capsFilter, webrtc, nullptr);
    if (!gst_element_link_many(appsrc, parse, pay, capsFilter, nullptr)) {
      spdlog::warn("WebRTC sender: failed to link encoder pipeline");
      stop();
      return;
    }

    GstPad* requestedPad =
      gst_element_request_pad_simple(webrtc, "sink_%u");
    GstPad* capsSrc = gst_element_get_static_pad(capsFilter, "src");
    if (requestedPad == nullptr || capsSrc == nullptr ||
        gst_pad_link(capsSrc, requestedPad) != GST_PAD_LINK_OK) {
      spdlog::warn("WebRTC sender: failed to link RTP to webrtcbin");
      if (requestedPad) gst_object_unref(requestedPad);
      if (capsSrc) gst_object_unref(capsSrc);
      stop();
      return;
    }
    sinkPad = requestedPad;
    gst_object_unref(capsSrc);

    g_signal_connect(
      webrtc, "on-negotiation-needed", G_CALLBACK(onNegotiationNeeded), this);
    g_signal_connect(
      webrtc, "on-ice-candidate", G_CALLBACK(onIceCandidate), this);
    g_signal_connect(
      webrtc, "notify::connection-state", G_CALLBACK(onConnectionState), this);
    g_signal_connect(webrtc,
                     "notify::ice-connection-state",
                     G_CALLBACK(onIceConnectionState),
                     this);

    GstBus* bus = gst_element_get_bus(pipeline);
    if (bus != nullptr) {
      gst_bus_add_watch(bus, onBusMessage, this);
      gst_object_unref(bus);
    }

    if (gst_element_set_state(pipeline, GST_STATE_PLAYING) ==
        GST_STATE_CHANGE_FAILURE) {
      spdlog::warn("WebRTC sender: failed to start pipeline");
      stop();
    } else {
      spdlog::info("WebRTC sender: pipeline started for share {} viewer {}",
                   shareId,
                   viewerID);
    }
  }

  void setRemoteAnswer(const std::string& text)
  {
    if (stopping.load() || webrtc == nullptr) {
      return;
    }
    GstSDPMessage* sdp = nullptr;
    if (gst_sdp_message_new_from_text(text.c_str(), &sdp) != GST_SDP_OK) {
      return;
    }
    auto* description =
      gst_webrtc_session_description_new(GST_WEBRTC_SDP_TYPE_ANSWER, sdp);
    g_signal_emit_by_name(
      webrtc, "set-remote-description", description, nullptr);
    gst_webrtc_session_description_free(description);
  }

  void addIceCandidate(int32_t mlineIndex, const std::string& candidate)
  {
    if (stopping.load() || webrtc == nullptr) {
      return;
    }
    g_signal_emit_by_name(
      webrtc, "add-ice-candidate", mlineIndex, candidate.c_str());
  }

  static void
  onStats(GstPromise* promise, gpointer data)
  {
    auto* self = static_cast<SenderSession*>(data);
    if (self == nullptr || self->stopping.load()) {
      // The producer owns this reference; requestStats() already released the
      // consumer reference.
      return;
    }
    const GstStructure* stats = gst_promise_get_reply(promise);
    double loss = -1.0;
    double rtt = -1.0;
    if (stats != nullptr) {
      const int fields = gst_structure_n_fields(stats);
      for (int i = 0; i < fields; ++i) {
        const char* name = gst_structure_nth_field_name(stats, i);
        if (name == nullptr ||
            !g_str_has_prefix(name, "rtp-remote-inbound-stream-stats_")) {
          continue;
        }
        const GValue* value = gst_structure_get_value(stats, name);
        if (value == nullptr || !GST_VALUE_HOLDS_STRUCTURE(value)) {
          continue;
        }
        const GstStructure* entry = gst_value_get_structure(value);
        double entryLoss = 0.0;
        double entryRtt = 0.0;
        if (gst_structure_get_double(entry, "fraction-lost", &entryLoss)) {
          loss = std::max(loss, entryLoss);
        }
        if (gst_structure_get_double(entry, "round-trip-time", &entryRtt)) {
          rtt = std::max(rtt, entryRtt);
        }
      }
    }
    if (loss >= 0.0 || rtt >= 0.0) {
      SenderStats result;
      result.shareId = self->shareId;
      result.viewerID = self->viewerID;
      result.fractionLost = std::max(0.0, loss);
      result.roundTripTime = std::max(0.0, rtt);
      if (!self->loggedStats.exchange(true)) {
        spdlog::info("WebRTC sender stats: share {} viewer {} loss={:.2f}% "
                     "rtt={:.1f}ms",
                     result.shareId,
                     result.viewerID,
                     result.fractionLost * 100.0,
                     result.roundTripTime * 1000.0);
      }
      self->impl->queueSenderStats(result);
    }
    // requestStats() already released the consumer reference; webrtcbin owns
    // the remaining reference and will release it with the stats task.
  }

  void requestStats()
  {
    if (stopping.load() || webrtc == nullptr || sinkPad == nullptr) {
      return;
    }
    GstPromise* promise =
      gst_promise_new_with_change_func(&SenderSession::onStats, this, nullptr);
    g_signal_emit_by_name(webrtc, "get-stats", sinkPad, promise);
    gst_promise_unref(promise);
  }

  void pushFrame(const uint8_t* data, size_t size)
  {
    if (stopping.load() || !data || size == 0) {
      return;
    }
    // Copy on the engine thread and hand the buffer to the GLib thread so all
    // appsrc/pipeline access stays on one thread.
    std::vector<std::uint8_t> payload(data, data + size);
    impl->post([this, payload = std::move(payload)]() {
      if (stopping.load() || appsrc == nullptr) {
        return;
      }
      GstBuffer* buffer =
        gst_buffer_new_allocate(nullptr, payload.size(), nullptr);
      if (buffer == nullptr) {
        return;
      }
      GstMapInfo map{};
      if (!gst_buffer_map(buffer, &map, GST_MAP_WRITE)) {
        gst_buffer_unref(buffer);
        return;
      }
      std::memcpy(map.data, payload.data(), payload.size());
      gst_buffer_unmap(buffer, &map);
      // Let appsrc stamp the buffer with the pipeline clock. Using a
      // program-relative timestamp here makes webrtcbin hold the first frames
      // until the pipeline clock catches up (multi-second start-up latency).
      GST_BUFFER_PTS(buffer) = GST_CLOCK_TIME_NONE;
      GST_BUFFER_DTS(buffer) = GST_CLOCK_TIME_NONE;
      if (!loggedFirstFrame.exchange(true)) {
        spdlog::info(
          "WebRTC sender: first encoded frame for share {} ({} bytes)",
          shareId,
          payload.size());
      }
      // appsrc takes ownership of the buffer, including when the push fails.
      if (gst_app_src_push_buffer(GST_APP_SRC(appsrc), buffer) != GST_FLOW_OK) {
        spdlog::warn("WebRTC sender: appsrc rejected a frame for share {}",
                     shareId);
      }
    });
  }

  void teardown()
  {
    connected.store(false);
    if (pipeline != nullptr) {
      gst_element_set_state(pipeline, GST_STATE_NULL);
    }
    appsrc = nullptr;
    webrtc = nullptr;
    if (sinkPad != nullptr) {
      gst_object_unref(sinkPad);
      sinkPad = nullptr;
    }
    if (pipeline != nullptr) {
      gst_object_unref(pipeline);
      pipeline = nullptr;
    }
  }

  void stop()
  {
    if (stopping.exchange(true)) {
      return;
    }
    if (impl->shuttingDown.load() ||
        std::this_thread::get_id() == impl->gstThreadId) {
      teardown();
      return;
    }
    impl->post([this]() { teardown(); });
  }
};

struct WebRtcManager::Impl::ReceiverSession
{
  Impl* impl = nullptr;
  uint32_t ownerID = 0;
  uint32_t shareId = 0;
  GstElement* pipeline = nullptr;
  GstElement* webrtc = nullptr;
  GstElement* appsink = nullptr;
  std::atomic<bool> stopping{ false };
  std::atomic<bool> answerCreated{ false };

  static void
  onConnectionState(GObject* object, GParamSpec* pspec, gpointer data)
  {
    (void)pspec;
    auto* self = static_cast<ReceiverSession*>(data);
    if (!self || self->stopping.load()) {
      return;
    }
    GstWebRTCPeerConnectionState state = GST_WEBRTC_PEER_CONNECTION_STATE_NEW;
    g_object_get(object, "connection-state", &state, nullptr);
    spdlog::info("WebRTC receiver: connection state {} for share {} owner {}",
                 static_cast<int>(state),
                 self->shareId,
                 self->ownerID);
    if (state == GST_WEBRTC_PEER_CONNECTION_STATE_CONNECTED) {
      self->impl->queueConnectionEvent(
        ConnectionEvent{ ConnectionEvent::Type::ReceiverConnected,
                         self->shareId,
                         self->ownerID });
    } else if (state == GST_WEBRTC_PEER_CONNECTION_STATE_FAILED ||
               state == GST_WEBRTC_PEER_CONNECTION_STATE_DISCONNECTED ||
               state == GST_WEBRTC_PEER_CONNECTION_STATE_CLOSED) {
      self->impl->queueConnectionEvent(
        ConnectionEvent{ ConnectionEvent::Type::ReceiverFailed,
                         self->shareId,
                         self->ownerID });
    }
  }

  static void
  onIceConnectionState(GObject* object, GParamSpec* pspec, gpointer data)
  {
    (void)pspec;
    auto* self = static_cast<ReceiverSession*>(data);
    if (!self || self->stopping.load()) {
      return;
    }
    GstWebRTCICEConnectionState state =
      GST_WEBRTC_ICE_CONNECTION_STATE_NEW;
    g_object_get(object, "ice-connection-state", &state, nullptr);
    spdlog::info("WebRTC receiver: ICE state {} for share {} owner {}",
                 static_cast<int>(state),
                 self->shareId,
                 self->ownerID);
  }

  static void
  onIceCandidate(GstElement* webrtc,
                 guint mlineIndex,
                 gchar* candidate,
                 gpointer data)
  {
    (void)webrtc;
    auto* self = static_cast<ReceiverSession*>(data);
    if (!self || self->stopping.load() || candidate == nullptr) {
      return;
    }
    Signal signal;
    signal.type = MultiPlayer::WebRtcSignalType::Ice;
    signal.targetOwner = self->ownerID;
    signal.shareId = self->shareId;
    signal.mlineIndex = static_cast<int32_t>(mlineIndex);
    signal.data = candidate;
    self->impl->queueSignal(std::move(signal));
  }

  static gboolean
  onBusMessage(GstBus* bus, GstMessage* message, gpointer data)
  {
    (void)bus;
    (void)data;
    if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
      GError* error = nullptr;
      gchar* debug = nullptr;
      gst_message_parse_error(message, &error, &debug);
      spdlog::warn("WebRTC receiver error: {} ({})",
                   error ? error->message : "unknown",
                   debug ? debug : "");
      g_clear_error(&error);
      g_free(debug);
    } else if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_WARNING) {
      GError* error = nullptr;
      gchar* debug = nullptr;
      gst_message_parse_warning(message, &error, &debug);
      spdlog::warn("WebRTC receiver warning: {} ({})",
                   error ? error->message : "unknown",
                   debug ? debug : "");
      g_clear_error(&error);
      g_free(debug);
    }
    return G_SOURCE_CONTINUE;
  }

  static void
  onPadAdded(GstElement* webrtc, GstPad* pad, gpointer data)
  {
    (void)webrtc;
    auto* self = static_cast<ReceiverSession*>(data);
    if (!self || self->stopping.load()) {
      return;
    }
    GstCaps* caps = gst_pad_get_current_caps(pad);
    if (caps == nullptr) {
      caps = gst_pad_query_caps(pad, nullptr);
    }
    if (caps == nullptr) {
      return;
    }
    const GstStructure* structure = gst_caps_get_structure(caps, 0);
    const gchar* media = structure ? gst_structure_get_string(structure, "media")
                                   : nullptr;
    const gchar* encoding =
      structure ? gst_structure_get_string(structure, "encoding-name")
                : nullptr;
    const bool isH264 =
      media != nullptr && encoding != nullptr &&
      std::strcmp(media, "video") == 0 &&
      std::strcmp(encoding, "H264") == 0;
    gst_caps_unref(caps);
    if (!isH264) {
      return;
    }
    spdlog::info("WebRTC receiver: media pad added for share {}",
                 self->shareId);

    GstElement* depay = gst_element_factory_make("rtph264depay", nullptr);
    GstElement* parse = gst_element_factory_make("h264parse", nullptr);
    GstElement* decoder = gst_element_factory_make("avdec_h264", nullptr);
    GstElement* convert = gst_element_factory_make("videoconvert", nullptr);
    GstElement* capsFilter = gst_element_factory_make("capsfilter", nullptr);
    GstElement* appsink = gst_element_factory_make("appsink", nullptr);
    if (!depay || !parse || !decoder || !convert || !capsFilter || !appsink) {
      spdlog::warn("WebRTC receiver: missing decode elements");
      return;
    }
    GstCaps* rgbaCaps = gst_caps_from_string("video/x-raw,format=RGBA");
    g_object_set(capsFilter, "caps", rgbaCaps, nullptr);
    gst_caps_unref(rgbaCaps);
    g_object_set(appsink,
                 "emit-signals",
                 TRUE,
                 "sync",
                 FALSE,
                 "max-buffers",
                 1,
                 "drop",
                 TRUE,
                 nullptr);
    gst_bin_add_many(GST_BIN(self->pipeline),
                     depay,
                     parse,
                     decoder,
                     convert,
                     capsFilter,
                     appsink,
                     nullptr);
    if (!gst_element_link_many(
          depay, parse, decoder, convert, capsFilter, appsink, nullptr)) {
      spdlog::warn("WebRTC receiver: failed to link decode pipeline");
      return;
    }
    gst_element_sync_state_with_parent(depay);
    gst_element_sync_state_with_parent(parse);
    gst_element_sync_state_with_parent(decoder);
    gst_element_sync_state_with_parent(convert);
    gst_element_sync_state_with_parent(capsFilter);
    gst_element_sync_state_with_parent(appsink);
    GstPad* sinkPad = gst_element_get_static_pad(depay, "sink");
    if (sinkPad != nullptr) {
      gst_pad_link(pad, sinkPad);
      gst_object_unref(sinkPad);
    }
    self->appsink = appsink;
    g_signal_connect(
      appsink, "new-sample", G_CALLBACK(onNewSample), self);
  }

  static GstFlowReturn
  onNewSample(GstAppSink* sink, gpointer data)
  {
    auto* self = static_cast<ReceiverSession*>(data);
    if (!self || self->stopping.load()) {
      return GST_FLOW_OK;
    }
    GstSample* sample = gst_app_sink_pull_sample(sink);
    if (sample == nullptr) {
      return GST_FLOW_OK;
    }
    GstCaps* caps = gst_sample_get_caps(sample);
    GstBuffer* buffer = gst_sample_get_buffer(sample);
    if (caps == nullptr || buffer == nullptr) {
      gst_sample_unref(sample);
      return GST_FLOW_OK;
    }
    const GstStructure* structure = gst_caps_get_structure(caps, 0);
    int width = 0;
    int height = 0;
    if (structure == nullptr ||
        !gst_structure_get_int(structure, "width", &width) ||
        !gst_structure_get_int(structure, "height", &height) ||
        width <= 0 || height <= 0) {
      gst_sample_unref(sample);
      return GST_FLOW_OK;
    }
    GstMapInfo map{};
    if (!gst_buffer_map(buffer, &map, GST_MAP_READ)) {
      gst_sample_unref(sample);
      return GST_FLOW_OK;
    }
    const std::size_t expected =
      static_cast<std::size_t>(width) * height * 4;
    if (map.size >= expected) {
      DecodedFrame frame;
      frame.ownerID = self->ownerID;
      frame.shareId = self->shareId;
      frame.width = width;
      frame.height = height;
      frame.rgba.assign(
        static_cast<const uint8_t*>(map.data),
        static_cast<const uint8_t*>(map.data) + expected);
      static std::atomic<int> decodedCount{ 0 };
      const int count = decodedCount.fetch_add(1) + 1;
      if (count == 1 || count % 60 == 0) {
        spdlog::info("WebRTC receiver: decoded {} frames ({}x{}, share {})",
                     count,
                     width,
                     height,
                     self->shareId);
      }
      self->impl->queueFrame(std::move(frame));
    }
    gst_buffer_unmap(buffer, &map);
    gst_sample_unref(sample);
    return GST_FLOW_OK;
  }

  void start()
  {
    if (stopping.load()) {
      return;
    }
    pipeline = gst_pipeline_new(nullptr);
    webrtc = gst_element_factory_make("webrtcbin", nullptr);
    if (!pipeline || !webrtc) {
      spdlog::warn("WebRTC receiver: missing webrtcbin");
      stop();
      return;
    }
    gst_bin_add(GST_BIN(pipeline), webrtc);
    g_signal_connect(
      webrtc, "on-ice-candidate", G_CALLBACK(onIceCandidate), this);
    g_signal_connect(
      webrtc, "notify::connection-state", G_CALLBACK(onConnectionState), this);
    g_signal_connect(webrtc,
                     "notify::ice-connection-state",
                     G_CALLBACK(onIceConnectionState),
                     this);
    g_signal_connect(webrtc, "pad-added", G_CALLBACK(onPadAdded), this);
    GstBus* bus = gst_element_get_bus(pipeline);
    if (bus != nullptr) {
      gst_bus_add_watch(bus, onBusMessage, this);
      gst_object_unref(bus);
    }
    if (gst_element_set_state(pipeline, GST_STATE_PLAYING) ==
        GST_STATE_CHANGE_FAILURE) {
      spdlog::warn("WebRTC receiver: failed to start pipeline");
      stop();
    } else {
      spdlog::info("WebRTC receiver: pipeline started for share {} owner {}",
                   shareId,
                   ownerID);
    }
  }

  static void
  onAnswerCreated(GstPromise* promise, gpointer data)
  {
    auto* self = static_cast<ReceiverSession*>(data);
    if (!self || self->stopping.load()) {
      gst_promise_unref(promise);
      return;
    }
    const GstStructure* reply = gst_promise_get_reply(promise);
    GstWebRTCSessionDescription* answer = nullptr;
    if (reply != nullptr) {
      gst_structure_get(reply,
                        "answer",
                        GST_TYPE_WEBRTC_SESSION_DESCRIPTION,
                        &answer,
                        nullptr);
    }
    if (answer == nullptr) {
      gst_promise_unref(promise);
      return;
    }
    g_signal_emit_by_name(self->webrtc, "set-local-description", answer, nullptr);
    gchar* text = gst_sdp_message_as_text(answer->sdp);
    if (text != nullptr) {
      spdlog::info("WebRTC receiver: answer ready for share {} from owner {}",
                   self->shareId,
                   self->ownerID);
      Signal signal;
      signal.type = MultiPlayer::WebRtcSignalType::Answer;
      signal.targetOwner = self->ownerID;
      signal.shareId = self->shareId;
      signal.data = text;
      self->impl->queueSignal(std::move(signal));
      g_free(text);
    }
    gst_webrtc_session_description_free(answer);
    gst_promise_unref(promise);
    self->answerCreated.store(true);
  }

  void setRemoteOffer(const std::string& text)
  {
    if (stopping.load() || webrtc == nullptr) {
      return;
    }
    GstSDPMessage* sdp = nullptr;
    if (gst_sdp_message_new_from_text(text.c_str(), &sdp) != GST_SDP_OK) {
      return;
    }
    auto* description =
      gst_webrtc_session_description_new(GST_WEBRTC_SDP_TYPE_OFFER, sdp);
    g_signal_emit_by_name(
      webrtc, "set-remote-description", description, nullptr);
    gst_webrtc_session_description_free(description);

    GstPromise* promise = gst_promise_new_with_change_func(
      &ReceiverSession::onAnswerCreated, this, nullptr);
    g_signal_emit_by_name(webrtc, "create-answer", nullptr, promise);
  }

  void addIceCandidate(int32_t mlineIndex, const std::string& candidate)
  {
    if (stopping.load() || webrtc == nullptr) {
      return;
    }
    g_signal_emit_by_name(
      webrtc, "add-ice-candidate", mlineIndex, candidate.c_str());
  }

  void teardown()
  {
    if (pipeline != nullptr) {
      gst_element_set_state(pipeline, GST_STATE_NULL);
    }
    appsink = nullptr;
    webrtc = nullptr;
    if (pipeline != nullptr) {
      gst_object_unref(pipeline);
      pipeline = nullptr;
    }
  }

  void stop()
  {
    if (stopping.exchange(true)) {
      return;
    }
    if (impl->shuttingDown.load() ||
        std::this_thread::get_id() == impl->gstThreadId) {
      teardown();
      return;
    }
    impl->post([this]() { teardown(); });
  }
};

WebRtcManager::Impl::~Impl()
{
  shuttingDown.store(true);
  std::vector<std::shared_ptr<SenderSession>> senderCopy;
  std::vector<std::shared_ptr<ReceiverSession>> receiverCopy;
  {
    std::lock_guard<std::mutex> lock(mutex);
    for (auto& [key, session] : senders) {
      (void)key;
      senderCopy.push_back(session);
    }
    for (auto& [key, session] : receivers) {
      (void)key;
      receiverCopy.push_back(session);
    }
    for (auto& session : retiredSenders) {
      senderCopy.push_back(session);
    }
    for (auto& session : retiredReceivers) {
      receiverCopy.push_back(session);
    }
    senders.clear();
    receivers.clear();
    retiredSenders.clear();
    retiredReceivers.clear();
  }
  if (loop != nullptr) {
    g_main_loop_quit(loop);
  }
  if (thread.joinable()) {
    thread.join();
  }
  // The GLib thread is gone, so tear the pipelines down directly. This also
  // covers sessions whose asynchronous stop() was queued but never ran.
  for (auto& session : senderCopy) {
    session->stopping.store(true);
    session->teardown();
  }
  for (auto& session : receiverCopy) {
    session->stopping.store(true);
    session->teardown();
  }
  if (loop != nullptr) {
    g_main_loop_unref(loop);
    loop = nullptr;
  }
  if (context != nullptr) {
    g_main_context_unref(context);
    context = nullptr;
  }
}

WebRtcManager::WebRtcManager()
  : impl_(std::make_unique<Impl>())
{
}

WebRtcManager::~WebRtcManager() = default;

void
WebRtcManager::ensureSender(uint32_t viewerID, uint32_t shareId)
{
  const auto key = sessionKey(viewerID, shareId);
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (impl_->senders.contains(key)) {
    return;
  }
  auto session = std::make_shared<Impl::SenderSession>();
  session->impl = impl_.get();
  session->viewerID = viewerID;
  session->shareId = shareId;
  impl_->senders[key] = session;
  impl_->post([session]() { session->start(); });
}

bool
WebRtcManager::hasSenderForShare(uint32_t shareId) const
{
  std::lock_guard<std::mutex> lock(impl_->mutex);
  for (const auto& [key, session] : impl_->senders) {
    if (session->shareId == shareId) {
      return true;
    }
  }
  return false;
}

bool
WebRtcManager::hasConnectedSenderForShare(uint32_t shareId) const
{
  std::lock_guard<std::mutex> lock(impl_->mutex);
  for (const auto& [key, session] : impl_->senders) {
    if (session->shareId == shareId && session->connected.load()) {
      return true;
    }
  }
  return false;
}

void
WebRtcManager::removeSender(uint32_t viewerID, uint32_t shareId)
{
  std::shared_ptr<Impl::SenderSession> removed;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto key = sessionKey(viewerID, shareId);
    auto it = impl_->senders.find(key);
    if (it != impl_->senders.end()) {
      removed = it->second;
      impl_->retiredSenders.push_back(removed);
      impl_->senders.erase(it);
    }
  }
  if (removed) {
    removed->stop();
  }
}

void
WebRtcManager::removeViewer(uint32_t viewerID)
{
  std::vector<std::shared_ptr<Impl::SenderSession>> removed;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    for (auto it = impl_->senders.begin(); it != impl_->senders.end();) {
      if (it->second->viewerID == viewerID) {
        removed.push_back(it->second);
        impl_->retiredSenders.push_back(it->second);
        it = impl_->senders.erase(it);
      } else {
        ++it;
      }
    }
  }
  for (auto& session : removed) {
    session->stop();
  }
}

void
WebRtcManager::removeSenderShare(uint32_t shareId)
{
  std::vector<std::shared_ptr<Impl::SenderSession>> removed;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    for (auto it = impl_->senders.begin(); it != impl_->senders.end();) {
      if (it->second->shareId == shareId) {
        removed.push_back(it->second);
        impl_->retiredSenders.push_back(it->second);
        it = impl_->senders.erase(it);
      } else {
        ++it;
      }
    }
  }
  for (auto& session : removed) {
    session->stop();
  }
}

void
WebRtcManager::removePeer(uint32_t peerID)
{
  removeViewer(peerID);

  std::vector<std::shared_ptr<Impl::ReceiverSession>> removed;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    for (auto it = impl_->receivers.begin(); it != impl_->receivers.end();) {
      if (it->second->ownerID == peerID) {
        removed.push_back(it->second);
        impl_->retiredReceivers.push_back(it->second);
        it = impl_->receivers.erase(it);
      } else {
        ++it;
      }
    }
  }
  for (auto& session : removed) {
    session->stop();
  }
}

void
WebRtcManager::ensureReceiver(uint32_t ownerID, uint32_t shareId)
{
  const auto key = sessionKey(ownerID, shareId);
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (impl_->receivers.contains(key)) {
    return;
  }
  auto session = std::make_shared<Impl::ReceiverSession>();
  session->impl = impl_.get();
  session->ownerID = ownerID;
  session->shareId = shareId;
  impl_->receivers[key] = session;
  impl_->post([session]() { session->start(); });
}

void
WebRtcManager::removeReceiver(uint32_t ownerID, uint32_t shareId)
{
  const auto key = sessionKey(ownerID, shareId);
  std::shared_ptr<Impl::ReceiverSession> removed;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto it = impl_->receivers.find(key);
    if (it != impl_->receivers.end()) {
      removed = it->second;
      impl_->retiredReceivers.push_back(removed);
      impl_->receivers.erase(it);
    }
  }
  if (removed) {
    removed->stop();
  }
}

void
WebRtcManager::removeShare(uint32_t shareId)
{
  std::vector<std::shared_ptr<Impl::SenderSession>> senderRemoved;
  std::vector<std::shared_ptr<Impl::ReceiverSession>> receiverRemoved;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    for (auto it = impl_->senders.begin(); it != impl_->senders.end();) {
      if (it->second->shareId == shareId) {
        senderRemoved.push_back(it->second);
        impl_->retiredSenders.push_back(it->second);
        it = impl_->senders.erase(it);
      } else {
        ++it;
      }
    }
    for (auto it = impl_->receivers.begin(); it != impl_->receivers.end();) {
      if (it->second->shareId == shareId) {
        receiverRemoved.push_back(it->second);
        impl_->retiredReceivers.push_back(it->second);
        it = impl_->receivers.erase(it);
      } else {
        ++it;
      }
    }
  }
  for (auto& session : senderRemoved) {
    session->stop();
  }
  for (auto& session : receiverRemoved) {
    session->stop();
  }
}

void
WebRtcManager::pushEncodedFrame(uint32_t shareId,
                                const uint8_t* data,
                                std::size_t size)
{
  std::vector<std::shared_ptr<Impl::SenderSession>> targets;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    for (const auto& [key, session] : impl_->senders) {
      (void)key;
      if (session->shareId == shareId) {
        targets.push_back(session);
      }
    }
  }
  for (auto& session : targets) {
    session->pushFrame(data, size);
  }
}

void
WebRtcManager::pollSenderStats()
{
  std::vector<std::shared_ptr<Impl::SenderSession>> targets;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    for (const auto& [key, session] : impl_->senders) {
      (void)key;
      targets.push_back(session);
    }
  }
  for (auto& session : targets) {
    session->impl->post([session]() { session->requestStats(); });
  }
}

std::vector<WebRtcManager::SenderStats>
WebRtcManager::takeSenderStats()
{
  std::lock_guard<std::mutex> lock(impl_->mutex);
  std::vector<SenderStats> result = std::move(impl_->senderStats);
  impl_->senderStats.clear();
  return result;
}

void
WebRtcManager::handleOffer(uint32_t ownerID,
                           uint32_t shareId,
                           const std::string& sdp)
{
  std::shared_ptr<Impl::ReceiverSession> session;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto it = impl_->receivers.find(sessionKey(ownerID, shareId));
    if (it != impl_->receivers.end()) {
      session = it->second;
    }
  }
  if (!session) {
    ensureReceiver(ownerID, shareId);
    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto it = impl_->receivers.find(sessionKey(ownerID, shareId));
    if (it != impl_->receivers.end()) {
      session = it->second;
    }
  }
  if (session) {
    session->impl->post([session, sdp]() {
      session->setRemoteOffer(sdp);
    });
  }
}

void
WebRtcManager::handleAnswer(uint32_t ownerID,
                            uint32_t shareId,
                            const std::string& sdp)
{
  std::shared_ptr<Impl::SenderSession> session;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto it = impl_->senders.find(sessionKey(ownerID, shareId));
    if (it != impl_->senders.end()) {
      session = it->second;
    }
  }
  if (session) {
    session->impl->post([session, sdp]() {
      session->setRemoteAnswer(sdp);
    });
  }
}

void
WebRtcManager::handleIce(uint32_t peerID,
                         uint32_t shareId,
                         int32_t mlineIndex,
                         const std::string& candidate)
{
  std::shared_ptr<Impl::SenderSession> sender;
  std::shared_ptr<Impl::ReceiverSession> receiver;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto sit = impl_->senders.find(sessionKey(peerID, shareId));
    if (sit != impl_->senders.end()) {
      sender = sit->second;
    }
    auto rit = impl_->receivers.find(sessionKey(peerID, shareId));
    if (rit != impl_->receivers.end()) {
      receiver = rit->second;
    }
  }
  if (sender) {
    sender->impl->post([sender, mlineIndex, candidate]() {
      sender->addIceCandidate(mlineIndex, candidate);
    });
  }
  if (receiver) {
    receiver->impl->post([receiver, mlineIndex, candidate]() {
      receiver->addIceCandidate(mlineIndex, candidate);
    });
  }
}

std::vector<WebRtcManager::Signal>
WebRtcManager::takeSignals()
{
  std::lock_guard<std::mutex> lock(impl_->mutex);
  std::vector<Signal> result = std::move(impl_->signals);
  impl_->signals.clear();
  return result;
}

std::vector<WebRtcManager::DecodedFrame>
WebRtcManager::takeDecodedFrames()
{
  std::lock_guard<std::mutex> lock(impl_->mutex);
  std::vector<DecodedFrame> result = std::move(impl_->frames);
  impl_->frames.clear();
  return result;
}

std::vector<uint32_t>
WebRtcManager::takeKeyframeRequests()
{
  std::lock_guard<std::mutex> lock(impl_->mutex);
  std::vector<uint32_t> result = std::move(impl_->keyframeRequests);
  impl_->keyframeRequests.clear();
  return result;
}

std::vector<WebRtcManager::ConnectionEvent>
WebRtcManager::takeConnectionEvents()
{
  std::lock_guard<std::mutex> lock(impl_->mutex);
  std::vector<ConnectionEvent> result = std::move(impl_->connectionEvents);
  impl_->connectionEvents.clear();
  return result;
}
