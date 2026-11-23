#pragma once

#include <cstdint>
#include <vector>

// Asynchronous GL readback for a shared window. Only touch this from the
// render thread with the GL context current. The first readback() call starts
// a PBO transfer; a later call maps the completed PBO, so frames are one
// capture behind while the GPU transfer happens off the render thread's
// critical path.
class WindowCapture
{
public:
  WindowCapture();
  ~WindowCapture();

  WindowCapture(const WindowCapture&) = delete;
  WindowCapture& operator=(const WindowCapture&) = delete;

  // Returns true when `rgba` contains a completed frame. `outWidth` and
  // `outHeight` describe that frame (they may differ from the current window
  // size if a resize happened while the readback was in flight).
  bool readback(int textureId,
                int width,
                int height,
                std::vector<std::uint8_t>& rgba,
                int& outWidth,
                int& outHeight);

  void reset();

private:
  void startReadback(int textureId, int width, int height);
  bool fenceReady() const;
  void destroy();

  unsigned int fbo_ = 0;
  unsigned int pbo_ = 0;
  void* fence_ = nullptr;
  int pendingWidth_ = 0;
  int pendingHeight_ = 0;
  bool pending_ = false;
};
