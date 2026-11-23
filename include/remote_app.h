#pragma once

#include "AppSurface.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>

class Texture;

namespace MultiPlayer {

// A window whose pixels come from another AppSurface (or, later, a decoded
// network stream). F14c-1 uses the source pointer for a local loopback preview;
// the transport layer will simply leave `source` empty and feed pixels instead.
class RemoteApp : public AppSurface
{
public:
  struct Component
  {
    std::shared_ptr<RemoteApp> app;
  };

  explicit RemoteApp(std::shared_ptr<AppSurface> source,
                     std::string name = {},
                     bool useSourceTexture = false);
  ~RemoteApp() override;

  int createTexture() override;
  void appTexture() override;

  // Upload a decoded RGBA frame into the remote window's own texture.
  bool setRgbaFrame(const std::uint8_t* pixels, int width, int height);

  void focus() override {}
  void takeInputFocus() override {}
  void unfocus() override {}
  bool isFocused() override { return false; }

  void select() override { selected_ = true; }
  void deselect() override { selected_ = false; }
  bool isSelected() override { return selected_; }

  void resize(int width, int height) override;
  void resizeMove(int width, int height, int x, int y) override;

  int getWidth() const override { return width_; }
  int getHeight() const override { return height_; }
  std::array<int, 2> getPosition() const override { return { x_, y_ }; }
  glm::mat4 getHeightScalar() const override { return heightScalar_; }
  int getTextureId() const override { return static_cast<int>(textureId_); }

  std::string getWindowName() override { return name_; }
  int getPID() override { return 0; }

  bool needsTextureImport() const { return source_ != nullptr; }
  const std::shared_ptr<AppSurface>& source() const { return source_; }

private:
  void updateHeightScalar();
  void recreateTextureStorage();
  bool copyFromSource();

  std::shared_ptr<AppSurface> source_;
  std::unique_ptr<Texture> texture_;
  bool useSourceTexture_ = false;
  bool ownsTexture_ = false;
  unsigned int textureId_ = 0;
  unsigned int readFbo_ = 0;
  unsigned int drawFbo_ = 0;
  int width_ = 0;
  int height_ = 0;
  int x_ = 0;
  int y_ = 0;
  bool selected_ = false;
  glm::mat4 heightScalar_{ 1.0f };
  std::string name_;
};

} // namespace MultiPlayer
