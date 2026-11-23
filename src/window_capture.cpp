#include "window_capture.h"

#include <glad/glad.h>
#include <EGL/egl.h>

#include <algorithm>
#include <cstring>

WindowCapture::WindowCapture() = default;

WindowCapture::~WindowCapture()
{
  destroy();
}

void
WindowCapture::destroy()
{
  // If the capture is destroyed from a non-GL thread (for example during an
  // abnormal client teardown), leaking the objects is preferable to touching
  // an invalid context.
  if (eglGetCurrentContext() == EGL_NO_CONTEXT) {
    fbo_ = 0;
    pbo_ = 0;
    fence_ = nullptr;
    pending_ = false;
    return;
  }
  if (fence_ != nullptr) {
    glDeleteSync(static_cast<GLsync>(fence_));
    fence_ = nullptr;
  }
  if (pbo_ != 0) {
    glDeleteBuffers(1, &pbo_);
    pbo_ = 0;
  }
  if (fbo_ != 0) {
    glDeleteFramebuffers(1, &fbo_);
    fbo_ = 0;
  }
  pending_ = false;
}

void
WindowCapture::reset()
{
  destroy();
}

bool
WindowCapture::fenceReady() const
{
  if (fence_ == nullptr) {
    return true;
  }
  const GLenum status = glClientWaitSync(
    static_cast<GLsync>(fence_), GL_SYNC_FLUSH_COMMANDS_BIT, 0);
  return status == GL_ALREADY_SIGNALED || status == GL_CONDITION_SATISFIED;
}

void
WindowCapture::startReadback(int textureId, int width, int height)
{
  if (textureId <= 0 || width <= 0 || height <= 0) {
    return;
  }

  GLint previousRead = 0;
  GLint previousPackAlignment = 4;
  GLint previousPackBuffer = 0;
  glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previousRead);
  glGetIntegerv(GL_PACK_ALIGNMENT, &previousPackAlignment);
  glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &previousPackBuffer);

  if (fbo_ == 0) {
    glGenFramebuffers(1, &fbo_);
  }
  if (fbo_ == 0) {
    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(previousRead));
    return;
  }

  glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo_);
  glFramebufferTexture2D(GL_READ_FRAMEBUFFER,
                         GL_COLOR_ATTACHMENT0,
                         GL_TEXTURE_2D,
                         static_cast<GLuint>(textureId),
                         0);
  const bool complete =
    glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
  if (!complete) {
    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(previousRead));
    glPixelStorei(GL_PACK_ALIGNMENT, previousPackAlignment);
    return;
  }

  if (pbo_ == 0) {
    glGenBuffers(1, &pbo_);
  }
  if (pbo_ == 0) {
    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(previousRead));
    glPixelStorei(GL_PACK_ALIGNMENT, previousPackAlignment);
    return;
  }

  const std::size_t size =
    static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4;
  glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo_);
  // Orphan the previous storage so the driver does not have to wait for an
  // in-flight readback before reusing the buffer.
  glBufferData(GL_PIXEL_PACK_BUFFER,
               static_cast<GLsizeiptr>(size),
               nullptr,
               GL_STREAM_READ);

  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  const GLboolean scissorEnabled = glIsEnabled(GL_SCISSOR_TEST);
  if (scissorEnabled) {
    glDisable(GL_SCISSOR_TEST);
  }
  glReadPixels(0,
               0,
               width,
               height,
               GL_RGBA,
               GL_UNSIGNED_BYTE,
               nullptr);
  if (scissorEnabled) {
    glEnable(GL_SCISSOR_TEST);
  }
  glBindBuffer(GL_PIXEL_PACK_BUFFER, static_cast<GLuint>(previousPackBuffer));
  glPixelStorei(GL_PACK_ALIGNMENT, previousPackAlignment);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(previousRead));

  if (glGetError() != GL_NO_ERROR) {
    return;
  }

  if (fence_ != nullptr) {
    glDeleteSync(static_cast<GLsync>(fence_));
  }
  fence_ = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
  pendingWidth_ = width;
  pendingHeight_ = height;
  pending_ = true;
}

bool
WindowCapture::readback(int textureId,
                        int width,
                        int height,
                        std::vector<std::uint8_t>& rgba,
                        int& outWidth,
                        int& outHeight)
{
  if (textureId <= 0 || width <= 0 || height <= 0) {
    return false;
  }

  if (!pending_) {
    startReadback(textureId, width, height);
    return false;
  }

  if (!fenceReady()) {
    return false;
  }

  const std::size_t size =
    static_cast<std::size_t>(pendingWidth_) * pendingHeight_ * 4;
  GLint previousPackBuffer = 0;
  glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &previousPackBuffer);
  glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo_);
  void* mapped = glMapBufferRange(
    GL_PIXEL_PACK_BUFFER, 0, static_cast<GLsizeiptr>(size), GL_MAP_READ_BIT);
  if (mapped == nullptr) {
    glBindBuffer(GL_PIXEL_PACK_BUFFER, static_cast<GLuint>(previousPackBuffer));
    if (fence_ != nullptr) {
      glDeleteSync(static_cast<GLsync>(fence_));
      fence_ = nullptr;
    }
    pending_ = false;
    startReadback(textureId, width, height);
    return false;
  }

  rgba.resize(size);
  std::memcpy(rgba.data(), mapped, size);
  glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
  glBindBuffer(GL_PIXEL_PACK_BUFFER, static_cast<GLuint>(previousPackBuffer));
  if (fence_ != nullptr) {
    glDeleteSync(static_cast<GLsync>(fence_));
    fence_ = nullptr;
  }
  outWidth = pendingWidth_;
  outHeight = pendingHeight_;
  pending_ = false;

  // Start the next transfer immediately; the next call maps this one.
  startReadback(textureId, width, height);
  // The frame copy above is valid even if starting the next transfer failed.
  return true;
}
