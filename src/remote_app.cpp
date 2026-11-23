#include "remote_app.h"

#include <algorithm>
#include <utility>

#include <glm/gtx/transform.hpp>
#include <EGL/egl.h>

#include "screen.h"
#include "texture.h"

namespace MultiPlayer {

RemoteApp::RemoteApp(std::shared_ptr<AppSurface> source,
                     std::string name,
                     bool useSourceTexture)
  : source_(std::move(source))
  , useSourceTexture_(useSourceTexture && source_ != nullptr)
{
  if (source_) {
    width_ = source_->getWidth();
    height_ = source_->getHeight();
    name_ = name.empty() ? source_->getWindowName() : std::move(name);
  } else {
    name_ = std::move(name);
  }
  if (name_.empty()) {
    name_ = "remote";
  }
  updateHeightScalar();
}

RemoteApp::~RemoteApp()
{
  // The WebGui can tear a client down from a non-GL thread; deleting GL
  // objects without a current context is undefined. Leaking in that rare
  // case is preferable to crashing on shutdown.
  if (eglGetCurrentContext() != EGL_NO_CONTEXT) {
    if (readFbo_ != 0) {
      glDeleteFramebuffers(1, &readFbo_);
    }
    if (drawFbo_ != 0) {
      glDeleteFramebuffers(1, &drawFbo_);
    }
    if (ownsTexture_ && textureId_ != 0) {
      glDeleteTextures(1, &textureId_);
    }
  }
  textureId_ = 0;
  texture_.reset();
}

int
RemoteApp::createTexture()
{
  if (useSourceTexture_ && source_) {
    textureId_ = static_cast<unsigned int>(source_->getTextureId());
    return static_cast<int>(textureId_);
  }
  if (textureId_ != 0) {
    return static_cast<int>(textureId_);
  }
  texture_ = std::make_unique<Texture>(GL_TEXTURE0);
  textureId_ = texture_->ID;
  ownsTexture_ = true;
  recreateTextureStorage();
  return static_cast<int>(textureId_);
}

void
RemoteApp::resize(int width, int height)
{
  if (width <= 0 || height <= 0) {
    return;
  }
  if (width == width_ && height == height_) {
    return;
  }
  width_ = width;
  height_ = height;
  updateHeightScalar();
  if (!useSourceTexture_) {
    recreateTextureStorage();
  }
}

void
RemoteApp::resizeMove(int width, int height, int x, int y)
{
  x_ = x;
  y_ = y;
  resize(width, height);
}

bool
RemoteApp::setRgbaFrame(const std::uint8_t* pixels, int width, int height)
{
  if (!pixels || width <= 0 || height <= 0 || useSourceTexture_) {
    return false;
  }
  if (textureId_ == 0) {
    createTexture();
  }
  if (textureId_ == 0) {
    return false;
  }
  if (width != width_ || height != height_) {
    resize(width, height);
  }

  glBindTexture(GL_TEXTURE_2D, textureId_);
  GLint previousAlignment = 4;
  glGetIntegerv(GL_UNPACK_ALIGNMENT, &previousAlignment);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glTexSubImage2D(GL_TEXTURE_2D,
                  0,
                  0,
                  0,
                  width,
                  height,
                  GL_RGBA,
                  GL_UNSIGNED_BYTE,
                  pixels);
  glPixelStorei(GL_UNPACK_ALIGNMENT, previousAlignment);
  return glGetError() == GL_NO_ERROR;
}

void
RemoteApp::appTexture()
{
  if (useSourceTexture_) {
    if (source_) {
      textureId_ = static_cast<unsigned int>(source_->getTextureId());
    }
    return;
  }
  if (!source_ || textureId_ == 0) {
    return;
  }

  const int sourceWidth = source_->getWidth();
  const int sourceHeight = source_->getHeight();
  if (sourceWidth <= 0 || sourceHeight <= 0) {
    return;
  }
  if (sourceWidth != width_ || sourceHeight != height_) {
    resize(sourceWidth, sourceHeight);
  }
  copyFromSource();
}

void
RemoteApp::updateHeightScalar()
{
  if (width_ <= 0 || height_ <= 0 || SCREEN_WIDTH <= 0.0f) {
    heightScalar_ = glm::mat4(1.0f);
    return;
  }
  const float standardRatio = SCREEN_HEIGHT / SCREEN_WIDTH;
  const float currentRatio = static_cast<float>(height_) /
                             static_cast<float>(width_);
  const float scaleFactor = currentRatio / standardRatio;
  heightScalar_ = glm::scale(glm::mat4(1.0f), glm::vec3(1.0f, scaleFactor, 1.0f));
}

void
RemoteApp::recreateTextureStorage()
{
  if (!ownsTexture_ || textureId_ == 0) {
    return;
  }
  const int width = std::max(1, width_);
  const int height = std::max(1, height_);
  glBindTexture(GL_TEXTURE_2D, textureId_);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexImage2D(GL_TEXTURE_2D,
               0,
               GL_RGBA,
               width,
               height,
               0,
               GL_RGBA,
               GL_UNSIGNED_BYTE,
               nullptr);
}

bool
RemoteApp::copyFromSource()
{
  if (useSourceTexture_ || !source_ || textureId_ == 0 ||
      glBlitFramebuffer == nullptr) {
    return false;
  }
  const int width = std::max(1, width_);
  const int height = std::max(1, height_);

  if (readFbo_ == 0) {
    glGenFramebuffers(1, &readFbo_);
  }
  if (drawFbo_ == 0) {
    glGenFramebuffers(1, &drawFbo_);
  }
  if (readFbo_ == 0 || drawFbo_ == 0) {
    return false;
  }

  GLint previousRead = 0;
  GLint previousDraw = 0;
  glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previousRead);
  glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previousDraw);

  glBindFramebuffer(GL_READ_FRAMEBUFFER, readFbo_);
  glFramebufferTexture2D(GL_READ_FRAMEBUFFER,
                         GL_COLOR_ATTACHMENT0,
                         GL_TEXTURE_2D,
                         static_cast<GLuint>(source_->getTextureId()),
                         0);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, drawFbo_);
  glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER,
                         GL_COLOR_ATTACHMENT0,
                         GL_TEXTURE_2D,
                         textureId_,
                         0);

  const GLenum readStatus =
    glCheckFramebufferStatus(GL_READ_FRAMEBUFFER);
  const GLenum drawStatus =
    glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
  const bool complete =
    readStatus == GL_FRAMEBUFFER_COMPLETE && drawStatus == GL_FRAMEBUFFER_COMPLETE;

  if (complete) {
    const GLboolean scissorEnabled = glIsEnabled(GL_SCISSOR_TEST);
    if (scissorEnabled) {
      glDisable(GL_SCISSOR_TEST);
    }
    glBlitFramebuffer(0,
                      0,
                      width,
                      height,
                      0,
                      0,
                      width,
                      height,
                      GL_COLOR_BUFFER_BIT,
                      GL_NEAREST);
    if (scissorEnabled) {
      glEnable(GL_SCISSOR_TEST);
    }
  }

  glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(previousRead));
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(previousDraw));
  return complete;
}

} // namespace MultiPlayer
