#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

class H264Encoder;

struct WindowEncodeJob
{
  uint32_t shareId = 0;
  int width = 0;
  int height = 0;
  int fps = 5;
  int quality = 65;
  int cursorX = -1;
  int cursorY = -1;
  std::vector<uint8_t> rgba;
};

struct WindowEncodeResult
{
  uint32_t shareId = 0;
  int width = 0;
  int height = 0;
  bool keyframe = false;
  bool h264 = false;
  bool h264Unavailable = false;
  std::vector<std::vector<uint8_t>> packets;
  std::vector<uint8_t> jpeg;
};

// Encodes captured RGBA frames without touching the render thread's GL
// context. One worker is owned by a Client. `isBusy(shareId)` lets the render
// thread drop captures instead of building a backlog.
class WindowEncodeWorker
{
public:
  WindowEncodeWorker();
  ~WindowEncodeWorker();

  WindowEncodeWorker(const WindowEncodeWorker&) = delete;
  WindowEncodeWorker& operator=(const WindowEncodeWorker&) = delete;

  void enqueue(WindowEncodeJob job);
  void removeShare(uint32_t shareId);
  void requestKeyframe(uint32_t shareId);
  bool isBusy(uint32_t shareId) const;
  std::vector<WindowEncodeResult> takeResults();
  void stop();

private:
  struct ShareState
  {
    std::unique_ptr<H264Encoder> encoder;
    int fps = 0;
    int quality = -1;
    bool h264Unavailable = false;
    bool keyframeRequested = false;
    bool removed = false;
  };

  void run();
  void encode(WindowEncodeJob& job,
              const std::shared_ptr<ShareState>& state,
              WindowEncodeResult& result);
  static bool encodeJpeg(const WindowEncodeJob& job,
                         WindowEncodeResult& result);

  std::thread thread_;
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<WindowEncodeJob> queue_;
  std::unordered_map<uint32_t, std::shared_ptr<ShareState>> shares_;
  std::unordered_map<uint32_t, bool> busy_;
  std::vector<WindowEncodeResult> results_;
  bool stopping_ = false;
};
