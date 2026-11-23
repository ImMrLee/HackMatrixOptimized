#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct AVCodecContext;
struct AVFrame;
struct SwsContext;

// Thin libavcodec wrappers used by the multiplayer window stream. Both classes
// are single-threaded and must be used on the render thread.
class H264Encoder
{
public:
  H264Encoder(int fps, int quality);
  ~H264Encoder();

  H264Encoder(const H264Encoder&) = delete;
  H264Encoder& operator=(const H264Encoder&) = delete;

  // Encodes one RGBA frame. `packets` receives Annex-B H.264 access units.
  bool encode(const std::uint8_t* rgba,
              int width,
              int height,
              std::vector<std::vector<std::uint8_t>>& packets,
              bool& keyframe);

  int encodedWidth() const { return width_; }
  int encodedHeight() const { return height_; }
  const std::string& codecName() const { return codecName_; }

  // Ask the next encoded frame to be an IDR/keyframe.
  void requestKeyframe() { forceKeyframe_.store(true); }

private:
  bool ensureContext(int width, int height);
  void close();

  AVCodecContext* context_ = nullptr;
  AVFrame* frame_ = nullptr;
  SwsContext* scaler_ = nullptr;
  int fps_ = 5;
  int quality_ = 65;
  int width_ = 0;
  int height_ = 0;
  std::int64_t pts_ = 0;
  std::atomic<bool> forceKeyframe_{ false };
  std::string codecName_;
  // Annex-B SPS/PPS cached from the first keyframe. Repeated in every later
  // IDR so a late decoder (WebRTC or a newly connected peer) can start without
  // waiting for the encoder to emit fresh parameter sets.
  std::vector<std::uint8_t> parameterSets_;
};

class H264Decoder
{
public:
  H264Decoder();
  ~H264Decoder();

  H264Decoder(const H264Decoder&) = delete;
  H264Decoder& operator=(const H264Decoder&) = delete;

  void reset();

  // Decodes one Annex-B access unit into RGBA. Returns false while waiting for
  // a keyframe or when libavcodec has no output yet.
  bool decode(const std::uint8_t* data,
              std::size_t size,
              bool keyframe,
              std::vector<std::uint8_t>& rgba,
              int& width,
              int& height);

private:
  bool ensureContext();
  void close();

  AVCodecContext* context_ = nullptr;
  AVFrame* frame_ = nullptr;
  SwsContext* scaler_ = nullptr;
  int scalerWidth_ = 0;
  int scalerHeight_ = 0;
  bool receivedKeyframe_ = false;
};
