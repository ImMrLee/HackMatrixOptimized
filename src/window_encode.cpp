#include "window_encode.h"

#include <stb/stb_image_write.h>

#include <algorithm>
#include <cstring>

#include <spdlog/spdlog.h>

#include "video_codec.h"

namespace {

void
appendJpeg(void* context, void* data, int size)
{
  auto* output = static_cast<std::vector<uint8_t>*>(context);
  if (!output || !data || size <= 0) {
    return;
  }
  const auto* bytes = static_cast<const uint8_t*>(data);
  output->insert(output->end(), bytes, bytes + size);
}

void
putPixel(std::vector<uint8_t>& rgba, int width, int height, int x, int y,
         uint8_t r, uint8_t g, uint8_t b)
{
  if (x < 0 || y < 0 || x >= width || y >= height) {
    return;
  }
  const std::size_t index =
    (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
     static_cast<std::size_t>(x)) *
    4;
  if (index + 3 >= rgba.size()) {
    return;
  }
  rgba[index + 0] = r;
  rgba[index + 1] = g;
  rgba[index + 2] = b;
  rgba[index + 3] = 255;
}

void
drawRemoteCursor(std::vector<uint8_t>& rgba,
                 int width,
                 int height,
                 int x,
                 int y)
{
  if (x < 0 || y < 0 || width <= 0 || height <= 0) {
    return;
  }
  // A small high-contrast crosshair so a remote controller can see where the
  // injected pointer is. The window texture itself never contains the
  // compositor cursor.
  const int radius = 9;
  for (int i = -radius; i <= radius; ++i) {
    for (int t = -2; t <= 2; ++t) {
      putPixel(rgba, width, height, x + i, y + t, 20, 20, 20);
      putPixel(rgba, width, height, x + t, y + i, 20, 20, 20);
    }
  }
  for (int i = -radius + 1; i <= radius - 1; ++i) {
    for (int t = -1; t <= 1; ++t) {
      putPixel(rgba, width, height, x + i, y + t, 40, 255, 70);
      putPixel(rgba, width, height, x + t, y + i, 40, 255, 70);
    }
  }
  for (int t = -2; t <= 2; ++t) {
    for (int u = -2; u <= 2; ++u) {
      putPixel(rgba, width, height, x + t, y + u, 255, 255, 255);
    }
  }
}

} // namespace

WindowEncodeWorker::WindowEncodeWorker()
  : thread_([this]() { run(); })
{
}

WindowEncodeWorker::~WindowEncodeWorker()
{
  stop();
}

void
WindowEncodeWorker::stop()
{
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopping_ = true;
  }
  cv_.notify_all();
  if (thread_.joinable()) {
    thread_.join();
  }
}

void
WindowEncodeWorker::enqueue(WindowEncodeJob job)
{
  std::lock_guard<std::mutex> lock(mutex_);
  if (stopping_ || job.rgba.empty() || job.width <= 0 || job.height <= 0) {
    return;
  }
  const uint32_t shareId = job.shareId;
  auto& state = shares_[shareId];
  if (!state) {
    state = std::make_shared<ShareState>();
  }
  if (busy_[shareId]) {
    return;
  }
  busy_[shareId] = true;
  queue_.push_back(std::move(job));
  cv_.notify_one();
}

void
WindowEncodeWorker::removeShare(uint32_t shareId)
{
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = shares_.find(shareId);
  if (it != shares_.end()) {
    it->second->removed = true;
    shares_.erase(it);
  }
  busy_.erase(shareId);
}

void
WindowEncodeWorker::requestKeyframe(uint32_t shareId)
{
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = shares_.find(shareId);
  if (it != shares_.end()) {
    it->second->keyframeRequested = true;
  }
}

bool
WindowEncodeWorker::isBusy(uint32_t shareId) const
{
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = busy_.find(shareId);
  return it != busy_.end() && it->second;
}

std::vector<WindowEncodeResult>
WindowEncodeWorker::takeResults()
{
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<WindowEncodeResult> result = std::move(results_);
  results_.clear();
  return result;
}

void
WindowEncodeWorker::run()
{
  while (true) {
    WindowEncodeJob job;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      cv_.wait(lock, [this]() { return stopping_ || !queue_.empty(); });
      if (stopping_ && queue_.empty()) {
        return;
      }
      job = std::move(queue_.front());
      queue_.pop_front();
    }

    std::shared_ptr<ShareState> state;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      auto it = shares_.find(job.shareId);
      if (it != shares_.end()) {
        state = it->second;
      }
    }

    if (!state || state->removed) {
      std::lock_guard<std::mutex> lock(mutex_);
      busy_[job.shareId] = false;
      continue;
    }

    WindowEncodeResult result;
    result.shareId = job.shareId;
    encode(job, state, result);

    {
      std::lock_guard<std::mutex> lock(mutex_);
      busy_[job.shareId] = false;
      if (state->removed) {
        continue;
      }
      if (!result.packets.empty() || !result.jpeg.empty()) {
        results_.push_back(std::move(result));
      }
    }
  }
}

void
WindowEncodeWorker::encode(WindowEncodeJob& job,
                           const std::shared_ptr<ShareState>& state,
                           WindowEncodeResult& result)
{
  if (job.cursorX >= 0 && job.cursorY >= 0) {
    drawRemoteCursor(job.rgba, job.width, job.height, job.cursorX, job.cursorY);
  }

  const int fps = std::max(1, job.fps);
  const int quality = std::clamp(job.quality, 1, 100);
  const bool settingsChanged = state->fps != fps || state->quality != quality;
  if (settingsChanged) {
    state->encoder = std::make_unique<H264Encoder>(fps, quality);
    state->fps = fps;
    state->quality = quality;
    state->h264Unavailable = false;
  }

  bool requested = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state->keyframeRequested) {
      state->keyframeRequested = false;
      requested = true;
    }
  }

  if (!state->h264Unavailable) {
    if (!state->encoder) {
      state->encoder = std::make_unique<H264Encoder>(fps, quality);
      state->fps = fps;
      state->quality = quality;
    }
    if (requested) {
      state->encoder->requestKeyframe();
    }

    std::vector<std::vector<uint8_t>> packets;
    bool keyframe = false;
    if (state->encoder->encode(
          job.rgba.data(), job.width, job.height, packets, keyframe) &&
        !packets.empty()) {
      result.h264 = true;
      result.keyframe = keyframe;
      result.width = state->encoder->encodedWidth();
      result.height = state->encoder->encodedHeight();
      result.packets = std::move(packets);
      result.h264Unavailable = false;
      return;
    }

    state->h264Unavailable = true;
    spdlog::warn("MultiPlayer: H.264 encode failed for share {}; using MJPEG",
                 job.shareId);
  }

  if (encodeJpeg(job, result)) {
    result.h264 = false;
    result.h264Unavailable = true;
    result.width = job.width;
    result.height = job.height;
  }
}

bool
WindowEncodeWorker::encodeJpeg(const WindowEncodeJob& job,
                               WindowEncodeResult& result)
{
  std::vector<uint8_t> jpeg;
  jpeg.reserve(static_cast<std::size_t>(job.width) *
               static_cast<std::size_t>(job.height) / 4);
  if (!stbi_write_jpg_to_func(appendJpeg,
                              &jpeg,
                              job.width,
                              job.height,
                              4,
                              job.rgba.data(),
                              std::clamp(job.quality, 1, 100))) {
    return false;
  }
  result.jpeg = std::move(jpeg);
  return !result.jpeg.empty();
}
