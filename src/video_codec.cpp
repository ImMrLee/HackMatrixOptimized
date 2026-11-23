#include "video_codec.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <string_view>

#include <spdlog/spdlog.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/dict.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

namespace {

constexpr int kMinBitrate = 300000;
constexpr int kBitratePerQuality = 60000;

// Returns the Annex-B start offset of the next NAL unit at or after `from`, or
// `data.size()` when none is found. `nalOffset` receives the NAL header offset.
std::size_t
findStartCode(const std::vector<std::uint8_t>& data,
              std::size_t from,
              std::size_t& nalOffset)
{
  for (std::size_t i = from; i + 3 < data.size(); ++i) {
    if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
      nalOffset = i + 3;
      return i;
    }
    if (i + 4 < data.size() && data[i] == 0 && data[i + 1] == 0 &&
        data[i + 2] == 0 && data[i + 3] == 1) {
      nalOffset = i + 4;
      return i;
    }
  }
  return data.size();
}

// Copies SPS (7) and PPS (8) NAL units out of an Annex-B access unit.
std::vector<std::uint8_t>
extractParameterSets(const std::vector<std::uint8_t>& data)
{
  std::vector<std::uint8_t> out;
  std::size_t nalOffset = 0;
  std::size_t start = findStartCode(data, 0, nalOffset);
  while (start < data.size()) {
    std::size_t nextNalOffset = 0;
    const std::size_t next = findStartCode(data, nalOffset, nextNalOffset);
    const int type = data[nalOffset] & 0x1f;
    if (type == 7 || type == 8) {
      out.insert(out.end(), data.begin() + start, data.begin() + next);
    }
    start = next;
    nalOffset = nextNalOffset;
  }
  return out;
}

bool
hasParameterSets(const std::vector<std::uint8_t>& data)
{
  bool sps = false;
  bool pps = false;
  std::size_t nalOffset = 0;
  std::size_t start = findStartCode(data, 0, nalOffset);
  while (start < data.size()) {
    std::size_t nextNalOffset = 0;
    const std::size_t next = findStartCode(data, nalOffset, nextNalOffset);
    const int type = data[nalOffset] & 0x1f;
    sps = sps || type == 7;
    pps = pps || type == 8;
    start = next;
    nalOffset = nextNalOffset;
  }
  return sps && pps;
}

} // namespace

H264Encoder::H264Encoder(int fps, int quality)
  : fps_(std::max(1, fps))
  , quality_(std::clamp(quality, 1, 100))
{
}

H264Encoder::~H264Encoder()
{
  close();
}

void
H264Encoder::close()
{
  if (scaler_ != nullptr) {
    sws_freeContext(scaler_);
    scaler_ = nullptr;
  }
  if (frame_ != nullptr) {
    av_frame_free(&frame_);
  }
  if (context_ != nullptr) {
    avcodec_free_context(&context_);
  }
  width_ = 0;
  height_ = 0;
  codecName_.clear();
  parameterSets_.clear();
}

bool
H264Encoder::ensureContext(int width, int height)
{
  width = std::max(2, width & ~1);
  height = std::max(2, height & ~1);
  if (context_ != nullptr && width == width_ && height == height_) {
    return true;
  }

  close();

  // Prefer a hardware encoder when available. NVENC is the hardware path on
  // NVIDIA systems; VAAPI encode requires a VA-API encode-capable device and
  // is not available on the NVIDIA driver used here.
  const char* candidates[] = { "h264_nvenc", "libx264" };
  const int bitrate = kMinBitrate + quality_ * kBitratePerQuality;
  for (const char* name : candidates) {
    const AVCodec* codec = avcodec_find_encoder_by_name(name);
    if (codec == nullptr) {
      continue;
    }
    context_ = avcodec_alloc_context3(codec);
    if (context_ == nullptr) {
      continue;
    }

    context_->width = width;
    context_->height = height;
    context_->time_base = AVRational{ 1, fps_ };
    context_->framerate = AVRational{ fps_, 1 };
    context_->pix_fmt = AV_PIX_FMT_YUV420P;
    context_->bit_rate = bitrate;
    context_->rc_max_rate = bitrate * 2;
    context_->rc_buffer_size = bitrate * 2;
    context_->gop_size = std::max(1, fps_);
    context_->max_b_frames = 0;
    context_->thread_count = 1;
    context_->flags |= AV_CODEC_FLAG_LOW_DELAY;

    AVDictionary* options = nullptr;
    if (std::string_view(name) == "h264_nvenc") {
      av_dict_set(&options, "preset", "p1", 0);
      av_dict_set(&options, "tune", "ull", 0);
      av_dict_set(&options, "rc", "vbr", 0);
      av_dict_set(&options, "zerolatency", "1", 0);
      av_dict_set(&options, "delay", "0", 0);
      av_dict_set(&options, "bf", "0", 0);
    } else {
      av_dict_set(&options, "preset", "ultrafast", 0);
      av_dict_set(&options, "tune", "zerolatency", 0);
    }
    const int result = avcodec_open2(context_, codec, &options);
    av_dict_free(&options);
    if (result >= 0) {
      codecName_ = name;
      spdlog::info("H264Encoder: using {}", codecName_);
      break;
    }
    avcodec_free_context(&context_);
  }

  if (context_ == nullptr) {
    spdlog::warn("H264Encoder: no usable H.264 encoder available");
    return false;
  }

  frame_ = av_frame_alloc();
  if (frame_ == nullptr) {
    close();
    return false;
  }
  frame_->format = AV_PIX_FMT_YUV420P;
  frame_->width = width;
  frame_->height = height;
  if (av_frame_get_buffer(frame_, 32) < 0) {
    close();
    return false;
  }

  scaler_ = sws_getContext(width,
                           height,
                           AV_PIX_FMT_RGBA,
                           width,
                           height,
                           AV_PIX_FMT_YUV420P,
                           SWS_BILINEAR,
                           nullptr,
                           nullptr,
                           nullptr);
  if (scaler_ == nullptr) {
    close();
    return false;
  }

  width_ = width;
  height_ = height;
  pts_ = 0;
  return true;
}

bool
H264Encoder::encode(const std::uint8_t* rgba,
                    int width,
                    int height,
                    std::vector<std::vector<std::uint8_t>>& packets,
                    bool& keyframe)
{
  packets.clear();
  keyframe = false;
  if (rgba == nullptr || width <= 0 || height <= 0 ||
      !ensureContext(width, height)) {
    return false;
  }
  if (av_frame_make_writable(frame_) < 0) {
    return false;
  }

  const std::uint8_t* source[4] = { rgba, nullptr, nullptr, nullptr };
  const int sourceStride[4] = { width * 4, 0, 0, 0 };
  // Use the encoder's even-sized dimensions; the source buffer may have one
  // extra row/column when the window size is odd.
  sws_scale(scaler_,
            source,
            sourceStride,
            0,
            height_,
            frame_->data,
            frame_->linesize);

  frame_->pts = pts_++;
  const bool wantsKeyframe =
    forceKeyframe_.exchange(false) || frame_->pts == 0 ||
    (frame_->pts % std::max(1, fps_)) == 0;
  frame_->pict_type =
    wantsKeyframe ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
  if (wantsKeyframe) {
    frame_->flags |= AV_FRAME_FLAG_KEY;
  } else {
    frame_->flags &= ~AV_FRAME_FLAG_KEY;
  }

  const int sendResult = avcodec_send_frame(context_, frame_);
  if (sendResult < 0) {
    return false;
  }

  AVPacket* packet = av_packet_alloc();
  if (packet == nullptr) {
    return false;
  }
  int result = 0;
  while ((result = avcodec_receive_packet(context_, packet)) == 0) {
    packets.emplace_back(packet->data, packet->data + packet->size);
    if ((packet->flags & AV_PKT_FLAG_KEY) != 0) {
      keyframe = true;
    }
    av_packet_unref(packet);
  }
  av_packet_free(&packet);

  // Repeat SPS/PPS in every keyframe. Some encoders only emit them once, which
  // leaves late decoders (the WebRTC viewer, or a peer that joins later)
  // unable to start until the next stream restart.
  if (keyframe && !packets.empty()) {
    auto& first = packets.front();
    auto sets = extractParameterSets(first);
    if (!sets.empty()) {
      parameterSets_ = std::move(sets);
    }
    if (!parameterSets_.empty() && !hasParameterSets(first)) {
      std::vector<std::uint8_t> withHeaders;
      withHeaders.reserve(parameterSets_.size() + first.size());
      withHeaders.insert(
        withHeaders.end(), parameterSets_.begin(), parameterSets_.end());
      withHeaders.insert(withHeaders.end(), first.begin(), first.end());
      first.swap(withHeaders);
    }
  }
  if (result != AVERROR(EAGAIN) && result != AVERROR_EOF) {
    return false;
  }
  return !packets.empty();
}

H264Decoder::H264Decoder() = default;

H264Decoder::~H264Decoder()
{
  close();
}

void
H264Decoder::close()
{
  if (scaler_ != nullptr) {
    sws_freeContext(scaler_);
    scaler_ = nullptr;
  }
  if (frame_ != nullptr) {
    av_frame_free(&frame_);
  }
  if (context_ != nullptr) {
    avcodec_free_context(&context_);
  }
  scalerWidth_ = 0;
  scalerHeight_ = 0;
  receivedKeyframe_ = false;
}

void
H264Decoder::reset()
{
  close();
}

bool
H264Decoder::ensureContext()
{
  if (context_ != nullptr) {
    return true;
  }
  const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
  if (codec == nullptr) {
    return false;
  }
  context_ = avcodec_alloc_context3(codec);
  if (context_ == nullptr) {
    return false;
  }
  context_->thread_count = 1;
  if (avcodec_open2(context_, codec, nullptr) < 0) {
    close();
    return false;
  }
  frame_ = av_frame_alloc();
  if (frame_ == nullptr) {
    close();
    return false;
  }
  return true;
}

bool
H264Decoder::decode(const std::uint8_t* data,
                    std::size_t size,
                    bool keyframe,
                    std::vector<std::uint8_t>& rgba,
                    int& width,
                    int& height)
{
  if (data == nullptr || size == 0 ||
      size > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
      !ensureContext()) {
    return false;
  }
  if (!keyframe && !receivedKeyframe_) {
    return false;
  }

  AVPacket* packet = av_packet_alloc();
  if (packet == nullptr) {
    return false;
  }
  if (av_new_packet(packet, static_cast<int>(size)) < 0) {
    av_packet_free(&packet);
    return false;
  }
  std::memcpy(packet->data, data, size);

  const int sendResult = avcodec_send_packet(context_, packet);
  av_packet_free(&packet);
  if (sendResult < 0) {
    reset();
    return false;
  }
  if (keyframe) {
    receivedKeyframe_ = true;
  }

  const int receiveResult = avcodec_receive_frame(context_, frame_);
  if (receiveResult < 0) {
    if (receiveResult != AVERROR(EAGAIN) && receiveResult != AVERROR_EOF) {
      reset();
    }
    return false;
  }

  const int frameWidth = frame_->width;
  const int frameHeight = frame_->height;
  if (frameWidth <= 0 || frameHeight <= 0) {
    av_frame_unref(frame_);
    return false;
  }


  scaler_ = sws_getCachedContext(scaler_,
                                 frameWidth,
                                 frameHeight,
                                 static_cast<AVPixelFormat>(frame_->format),
                                 frameWidth,
                                 frameHeight,
                                 AV_PIX_FMT_RGBA,
                                 SWS_BILINEAR,
                                 nullptr,
                                 nullptr,
                                 nullptr);
  if (scaler_ == nullptr) {
    av_frame_unref(frame_);
    reset();
    return false;
  }

  scalerWidth_ = frameWidth;
  scalerHeight_ = frameHeight;
  rgba.resize(static_cast<std::size_t>(frameWidth) * frameHeight * 4);
  std::uint8_t* destination[4] = { rgba.data(), nullptr, nullptr, nullptr };
  const int destinationStride[4] = { frameWidth * 4, 0, 0, 0 };
  sws_scale(scaler_,
            frame_->data,
            frame_->linesize,
            0,
            frameHeight,
            destination,
            destinationStride);

  width = frameWidth;
  height = frameHeight;
  av_frame_unref(frame_);
  return true;
}
