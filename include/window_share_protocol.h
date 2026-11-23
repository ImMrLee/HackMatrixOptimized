#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <glm/glm.hpp>

namespace MultiPlayer {

// Wire messages on the ENet window-stream channel. The relay server rewrites
// the owner field (offset 1) with the sender's ENet connect ID, so a peer
// cannot impersonate another sharer.
//
// Layout:
//   [0]      message type
//   [1..4]   owner player ID (little endian)
//   [5..8]   share ID (little endian)
//   [...]    type-specific payload
constexpr std::size_t kWindowShareHeaderSize = 9;
constexpr std::uint32_t kWindowShareOwnerOffset = 1;
constexpr std::uint32_t kWindowShareIdOffset = 5;
constexpr std::size_t kMaxWindowShareName = 256;
constexpr std::size_t kMaxWindowFrameBytes = 8 * 1024 * 1024;

enum class WindowShareMessageType : std::uint8_t
{
  Start = 1,
  Stop = 2,
  Frame = 3,
  OwnerLeft = 4,
  Transform = 5,
  ControlState = 6,
  H264Frame = 7,
};

// Control messages travel on channel 5 and are unicast by the relay to the
// window owner. The server overwrites senderID with the real ENet peer ID.
constexpr std::size_t kControlHeaderSize = 13;
constexpr std::uint32_t kControlTargetOffset = 1;
constexpr std::uint32_t kControlShareOffset = 5;
constexpr std::uint32_t kControlSenderOffset = 9;

enum class ControlMessageType : std::uint8_t
{
  Request = 1,
  Release = 2,
  PointerMotion = 3,
  PointerButton = 4,
  PointerAxis = 5,
  Key = 6,
  KeyframeRequest = 7,
};

struct ControlHeader
{
  std::uint32_t targetOwner = 0;
  std::uint32_t shareId = 0;
  std::uint32_t senderID = 0;
};

struct WindowShareStartData
{
  std::uint32_t shareId = 0;
  glm::vec3 pos{ 0.0f };
  glm::vec3 origin{ 0.0f };
  glm::vec3 rotate{ 0.0f };
  float scale = 1.0f;
  std::int32_t width = 0;
  std::int32_t height = 0;
  std::string name;
};

struct WindowShareTransformData
{
  std::uint32_t shareId = 0;
  glm::vec3 pos{ 0.0f };
  glm::vec3 origin{ 0.0f };
  glm::vec3 rotate{ 0.0f };
  float scale = 1.0f;
};

struct WindowShareFrameView
{
  std::uint32_t shareId = 0;
  std::int32_t width = 0;
  std::int32_t height = 0;
  const std::uint8_t* data = nullptr;
  std::size_t size = 0;
};

struct WindowShareH264FrameView
{
  std::uint32_t shareId = 0;
  std::int32_t width = 0;
  std::int32_t height = 0;
  bool keyframe = false;
  const std::uint8_t* data = nullptr;
  std::size_t size = 0;
};

inline void
setWindowShareOwner(std::uint8_t* data, std::size_t size, std::uint32_t owner)
{
  if (!data || size < kWindowShareHeaderSize) {
    return;
  }
  for (int i = 0; i < 4; ++i) {
    data[kWindowShareOwnerOffset + i] =
      static_cast<std::uint8_t>((owner >> (8 * i)) & 0xff);
  }
}

namespace window_share_detail {

inline void
appendU8(std::vector<std::uint8_t>& out, std::uint8_t value)
{
  out.push_back(value);
}

inline void
appendU32(std::vector<std::uint8_t>& out, std::uint32_t value)
{
  for (int i = 0; i < 4; ++i) {
    out.push_back(static_cast<std::uint8_t>((value >> (8 * i)) & 0xff));
  }
}

inline void
appendI32(std::vector<std::uint8_t>& out, std::int32_t value)
{
  appendU32(out, static_cast<std::uint32_t>(value));
}

inline void
appendF32(std::vector<std::uint8_t>& out, float value)
{
  std::uint32_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  appendU32(out, bits);
}

inline void
appendVec3(std::vector<std::uint8_t>& out, const glm::vec3& value)
{
  appendF32(out, value.x);
  appendF32(out, value.y);
  appendF32(out, value.z);
}

struct Reader
{
  const std::uint8_t* data = nullptr;
  std::size_t size = 0;
  std::size_t offset = 0;

  bool skip(std::size_t count)
  {
    if (offset + count > size) {
      return false;
    }
    offset += count;
    return true;
  }

  bool u8(std::uint8_t& value)
  {
    if (offset + 1 > size) {
      return false;
    }
    value = data[offset++];
    return true;
  }

  bool u32(std::uint32_t& value)
  {
    if (offset + 4 > size) {
      return false;
    }
    value = 0;
    for (int i = 0; i < 4; ++i) {
      value |= static_cast<std::uint32_t>(data[offset + i]) << (8 * i);
    }
    offset += 4;
    return true;
  }

  bool i32(std::int32_t& value)
  {
    std::uint32_t raw = 0;
    if (!u32(raw)) {
      return false;
    }
    value = static_cast<std::int32_t>(raw);
    return true;
  }

  bool f32(float& value)
  {
    std::uint32_t raw = 0;
    if (!u32(raw)) {
      return false;
    }
    std::memcpy(&value, &raw, sizeof(value));
    return true;
  }

  bool vec3(glm::vec3& value)
  {
    return f32(value.x) && f32(value.y) && f32(value.z);
  }
};

} // namespace window_share_detail

inline std::vector<std::uint8_t>
encodeWindowShareStart(std::uint32_t owner,
                       const WindowShareStartData& start)
{
  using namespace window_share_detail;
  std::vector<std::uint8_t> out;
  out.reserve(kWindowShareHeaderSize + 64 + start.name.size());
  appendU8(out, static_cast<std::uint8_t>(WindowShareMessageType::Start));
  appendU32(out, owner);
  appendU32(out, start.shareId);
  appendVec3(out, start.pos);
  appendVec3(out, start.origin);
  appendVec3(out, start.rotate);
  appendF32(out, start.scale);
  appendI32(out, start.width);
  appendI32(out, start.height);
  const auto nameSize = static_cast<std::uint16_t>(
    std::min<std::size_t>(start.name.size(), kMaxWindowShareName));
  out.push_back(static_cast<std::uint8_t>(nameSize & 0xff));
  out.push_back(static_cast<std::uint8_t>((nameSize >> 8) & 0xff));
  out.insert(out.end(), start.name.begin(), start.name.begin() + nameSize);
  return out;
}

inline bool
decodeWindowShareStart(const std::uint8_t* data,
                       std::size_t size,
                       WindowShareStartData& start)
{
  using namespace window_share_detail;
  Reader reader{ data, size, 0 };
  std::uint8_t type = 0;
  std::uint32_t owner = 0;
  std::uint32_t shareId = 0;
  if (!reader.u8(type) || type != static_cast<std::uint8_t>(
                                   WindowShareMessageType::Start) ||
      !reader.u32(owner) || !reader.u32(shareId) ||
      !reader.vec3(start.pos) || !reader.vec3(start.origin) ||
      !reader.vec3(start.rotate) || !reader.f32(start.scale) ||
      !reader.i32(start.width) || !reader.i32(start.height)) {
    return false;
  }

  std::uint8_t nameLow = 0;
  std::uint8_t nameHigh = 0;
  if (!reader.u8(nameLow) || !reader.u8(nameHigh)) {
    return false;
  }
  const std::size_t nameSize =
    static_cast<std::size_t>(nameLow) |
    (static_cast<std::size_t>(nameHigh) << 8);
  if (nameSize > kMaxWindowShareName || reader.offset + nameSize > size) {
    return false;
  }
  start.shareId = shareId;
  start.name.assign(reinterpret_cast<const char*>(data + reader.offset),
                    nameSize);
  return true;
}

inline std::vector<std::uint8_t>
encodeWindowShareTransform(std::uint32_t owner,
                           const WindowShareTransformData& transform)
{
  using namespace window_share_detail;
  std::vector<std::uint8_t> out;
  out.reserve(kWindowShareHeaderSize + 40);
  appendU8(out,
           static_cast<std::uint8_t>(WindowShareMessageType::Transform));
  appendU32(out, owner);
  appendU32(out, transform.shareId);
  appendVec3(out, transform.pos);
  appendVec3(out, transform.origin);
  appendVec3(out, transform.rotate);
  appendF32(out, transform.scale);
  return out;
}

inline bool
decodeWindowShareTransform(const std::uint8_t* data,
                           std::size_t size,
                           WindowShareTransformData& transform)
{
  using namespace window_share_detail;
  Reader reader{ data, size, 0 };
  std::uint8_t type = 0;
  std::uint32_t owner = 0;
  std::uint32_t shareId = 0;
  if (!reader.u8(type) ||
      type != static_cast<std::uint8_t>(
                WindowShareMessageType::Transform) ||
      !reader.u32(owner) || !reader.u32(shareId) ||
      !reader.vec3(transform.pos) || !reader.vec3(transform.origin) ||
      !reader.vec3(transform.rotate) || !reader.f32(transform.scale)) {
    return false;
  }
  transform.shareId = shareId;
  return true;
}

inline std::vector<std::uint8_t>
encodeWindowShareStop(std::uint32_t owner, std::uint32_t shareId)
{
  using namespace window_share_detail;
  std::vector<std::uint8_t> out;
  out.reserve(kWindowShareHeaderSize);
  appendU8(out, static_cast<std::uint8_t>(WindowShareMessageType::Stop));
  appendU32(out, owner);
  appendU32(out, shareId);
  return out;
}

inline bool
decodeWindowShareStop(const std::uint8_t* data,
                      std::size_t size,
                      std::uint32_t& owner,
                      std::uint32_t& shareId)
{
  using namespace window_share_detail;
  Reader reader{ data, size, 0 };
  std::uint8_t type = 0;
  if (!reader.u8(type) ||
      type != static_cast<std::uint8_t>(WindowShareMessageType::Stop) ||
      !reader.u32(owner) || !reader.u32(shareId)) {
    return false;
  }
  return true;
}

inline std::vector<std::uint8_t>
encodeWindowShareOwnerLeft(std::uint32_t owner)
{
  using namespace window_share_detail;
  std::vector<std::uint8_t> out;
  out.reserve(kWindowShareHeaderSize);
  appendU8(out,
           static_cast<std::uint8_t>(WindowShareMessageType::OwnerLeft));
  appendU32(out, owner);
  appendU32(out, 0);
  return out;
}

inline bool
decodeWindowShareOwnerLeft(const std::uint8_t* data,
                           std::size_t size,
                           std::uint32_t& owner)
{
  using namespace window_share_detail;
  Reader reader{ data, size, 0 };
  std::uint8_t type = 0;
  std::uint32_t ignoredShareId = 0;
  if (!reader.u8(type) ||
      type != static_cast<std::uint8_t>(
                WindowShareMessageType::OwnerLeft) ||
      !reader.u32(owner) || !reader.u32(ignoredShareId)) {
    return false;
  }
  return true;
}

inline std::vector<std::uint8_t>
encodeWindowShareFrame(std::uint32_t owner,
                       std::uint32_t shareId,
                       std::int32_t width,
                       std::int32_t height,
                       const std::uint8_t* pixels,
                       std::size_t pixelSize)
{
  using namespace window_share_detail;
  std::vector<std::uint8_t> out;
  out.reserve(kWindowShareHeaderSize + 8 + pixelSize);
  appendU8(out, static_cast<std::uint8_t>(WindowShareMessageType::Frame));
  appendU32(out, owner);
  appendU32(out, shareId);
  appendI32(out, width);
  appendI32(out, height);
  if (pixels && pixelSize > 0) {
    out.insert(out.end(), pixels, pixels + pixelSize);
  }
  return out;
}

inline bool
decodeWindowShareFrame(const std::uint8_t* data,
                       std::size_t size,
                       WindowShareFrameView& frame)
{
  using namespace window_share_detail;
  Reader reader{ data, size, 0 };
  std::uint8_t type = 0;
  std::uint32_t owner = 0;
  std::uint32_t shareId = 0;
  if (!reader.u8(type) ||
      type != static_cast<std::uint8_t>(WindowShareMessageType::Frame) ||
      !reader.u32(owner) || !reader.u32(shareId) ||
      !reader.i32(frame.width) || !reader.i32(frame.height)) {
    return false;
  }
  frame.shareId = shareId;
  frame.data = data + reader.offset;
  frame.size = size - reader.offset;
  return frame.size <= kMaxWindowFrameBytes;
}

inline std::vector<std::uint8_t>
encodeWindowShareControlState(std::uint32_t owner,
                              std::uint32_t shareId,
                              bool allowControl,
                              std::uint32_t controllerID)
{
  using namespace window_share_detail;
  std::vector<std::uint8_t> out;
  out.reserve(kWindowShareHeaderSize + 5);
  appendU8(out,
           static_cast<std::uint8_t>(WindowShareMessageType::ControlState));
  appendU32(out, owner);
  appendU32(out, shareId);
  appendU8(out, allowControl ? 1 : 0);
  appendU32(out, controllerID);
  return out;
}

inline bool
decodeWindowShareControlState(const std::uint8_t* data,
                              std::size_t size,
                              std::uint32_t& shareId,
                              bool& allowControl,
                              std::uint32_t& controllerID)
{
  using namespace window_share_detail;
  Reader reader{ data, size, 0 };
  std::uint8_t type = 0;
  std::uint32_t owner = 0;
  std::uint8_t allow = 0;
  if (!reader.u8(type) ||
      type != static_cast<std::uint8_t>(
                WindowShareMessageType::ControlState) ||
      !reader.u32(owner) || !reader.u32(shareId) || !reader.u8(allow) ||
      !reader.u32(controllerID)) {
    return false;
  }
  allowControl = allow != 0;
  return true;
}

inline void
setControlSender(std::uint8_t* data, std::size_t size, std::uint32_t sender)
{
  if (!data || size < kControlHeaderSize) {
    return;
  }
  for (int i = 0; i < 4; ++i) {
    data[kControlSenderOffset + i] =
      static_cast<std::uint8_t>((sender >> (8 * i)) & 0xff);
  }
}

inline bool
controlTargetOwner(const std::uint8_t* data,
                   std::size_t size,
                   std::uint32_t& owner)
{
  if (!data || size < kControlHeaderSize) {
    return false;
  }
  owner = 0;
  for (int i = 0; i < 4; ++i) {
    owner |= static_cast<std::uint32_t>(data[kControlTargetOffset + i])
             << (8 * i);
  }
  return true;
}

inline bool
decodeControlHeader(const std::uint8_t* data,
                    std::size_t size,
                    ControlHeader& header)
{
  using namespace window_share_detail;
  Reader reader{ data, size, 0 };
  std::uint8_t type = 0;
  if (!reader.u8(type) || !reader.u32(header.targetOwner) ||
      !reader.u32(header.shareId) || !reader.u32(header.senderID)) {
    return false;
  }
  (void)type;
  return true;
}

inline std::vector<std::uint8_t>
encodeControlRequest(std::uint32_t targetOwner,
                     std::uint32_t shareId,
                     std::uint32_t senderID)
{
  using namespace window_share_detail;
  std::vector<std::uint8_t> out;
  out.reserve(kControlHeaderSize);
  appendU8(out, static_cast<std::uint8_t>(ControlMessageType::Request));
  appendU32(out, targetOwner);
  appendU32(out, shareId);
  appendU32(out, senderID);
  return out;
}

inline std::vector<std::uint8_t>
encodeControlRelease(std::uint32_t targetOwner,
                     std::uint32_t shareId,
                     std::uint32_t senderID)
{
  using namespace window_share_detail;
  std::vector<std::uint8_t> out;
  out.reserve(kControlHeaderSize);
  appendU8(out, static_cast<std::uint8_t>(ControlMessageType::Release));
  appendU32(out, targetOwner);
  appendU32(out, shareId);
  appendU32(out, senderID);
  return out;
}

inline std::vector<std::uint8_t>
encodeControlKeyframeRequest(std::uint32_t targetOwner,
                             std::uint32_t shareId,
                             std::uint32_t senderID)
{
  using namespace window_share_detail;
  std::vector<std::uint8_t> out;
  out.reserve(kControlHeaderSize);
  appendU8(out,
           static_cast<std::uint8_t>(ControlMessageType::KeyframeRequest));
  appendU32(out, targetOwner);
  appendU32(out, shareId);
  appendU32(out, senderID);
  return out;
}

inline std::vector<std::uint8_t>
encodeControlPointerMotion(std::uint32_t targetOwner,
                           std::uint32_t shareId,
                           std::uint32_t senderID,
                           float x,
                           float y)
{
  using namespace window_share_detail;
  std::vector<std::uint8_t> out;
  out.reserve(kControlHeaderSize + 8);
  appendU8(out, static_cast<std::uint8_t>(ControlMessageType::PointerMotion));
  appendU32(out, targetOwner);
  appendU32(out, shareId);
  appendU32(out, senderID);
  appendF32(out, x);
  appendF32(out, y);
  return out;
}

inline std::vector<std::uint8_t>
encodeControlPointerButton(std::uint32_t targetOwner,
                           std::uint32_t shareId,
                           std::uint32_t senderID,
                           std::uint32_t button,
                           bool pressed)
{
  using namespace window_share_detail;
  std::vector<std::uint8_t> out;
  out.reserve(kControlHeaderSize + 5);
  appendU8(out, static_cast<std::uint8_t>(ControlMessageType::PointerButton));
  appendU32(out, targetOwner);
  appendU32(out, shareId);
  appendU32(out, senderID);
  appendU32(out, button);
  appendU8(out, pressed ? 1 : 0);
  return out;
}

inline std::vector<std::uint8_t>
encodeControlPointerAxis(std::uint32_t targetOwner,
                         std::uint32_t shareId,
                         std::uint32_t senderID,
                         float delta,
                         std::int32_t deltaDiscrete,
                         std::uint32_t source,
                         std::uint32_t orientation)
{
  using namespace window_share_detail;
  std::vector<std::uint8_t> out;
  out.reserve(kControlHeaderSize + 16);
  appendU8(out, static_cast<std::uint8_t>(ControlMessageType::PointerAxis));
  appendU32(out, targetOwner);
  appendU32(out, shareId);
  appendU32(out, senderID);
  appendF32(out, delta);
  appendI32(out, deltaDiscrete);
  appendU32(out, source);
  appendU32(out, orientation);
  return out;
}

inline std::vector<std::uint8_t>
encodeControlKey(std::uint32_t targetOwner,
                 std::uint32_t shareId,
                 std::uint32_t senderID,
                 std::uint32_t keycode,
                 bool pressed)
{
  using namespace window_share_detail;
  std::vector<std::uint8_t> out;
  out.reserve(kControlHeaderSize + 5);
  appendU8(out, static_cast<std::uint8_t>(ControlMessageType::Key));
  appendU32(out, targetOwner);
  appendU32(out, shareId);
  appendU32(out, senderID);
  appendU32(out, keycode);
  appendU8(out, pressed ? 1 : 0);
  return out;
}

inline bool
decodeControlPointerMotion(const std::uint8_t* data,
                           std::size_t size,
                           ControlHeader& header,
                           float& x,
                           float& y)
{
  using namespace window_share_detail;
  Reader reader{ data, size, 0 };
  std::uint8_t type = 0;
  if (!reader.u8(type) ||
      type != static_cast<std::uint8_t>(
                ControlMessageType::PointerMotion) ||
      !reader.u32(header.targetOwner) || !reader.u32(header.shareId) ||
      !reader.u32(header.senderID) || !reader.f32(x) || !reader.f32(y)) {
    return false;
  }
  return true;
}

inline bool
decodeControlPointerButton(const std::uint8_t* data,
                           std::size_t size,
                           ControlHeader& header,
                           std::uint32_t& button,
                           bool& pressed)
{
  using namespace window_share_detail;
  Reader reader{ data, size, 0 };
  std::uint8_t type = 0;
  std::uint8_t pressedByte = 0;
  if (!reader.u8(type) ||
      type != static_cast<std::uint8_t>(
                ControlMessageType::PointerButton) ||
      !reader.u32(header.targetOwner) || !reader.u32(header.shareId) ||
      !reader.u32(header.senderID) || !reader.u32(button) ||
      !reader.u8(pressedByte)) {
    return false;
  }
  pressed = pressedByte != 0;
  return true;
}

inline bool
decodeControlPointerAxis(const std::uint8_t* data,
                         std::size_t size,
                         ControlHeader& header,
                         float& delta,
                         std::int32_t& deltaDiscrete,
                         std::uint32_t& source,
                         std::uint32_t& orientation)
{
  using namespace window_share_detail;
  Reader reader{ data, size, 0 };
  std::uint8_t type = 0;
  if (!reader.u8(type) ||
      type != static_cast<std::uint8_t>(
                ControlMessageType::PointerAxis) ||
      !reader.u32(header.targetOwner) || !reader.u32(header.shareId) ||
      !reader.u32(header.senderID) || !reader.f32(delta) ||
      !reader.i32(deltaDiscrete) || !reader.u32(source) ||
      !reader.u32(orientation)) {
    return false;
  }
  return true;
}

inline bool
decodeControlKey(const std::uint8_t* data,
                 std::size_t size,
                 ControlHeader& header,
                 std::uint32_t& keycode,
                 bool& pressed)
{
  using namespace window_share_detail;
  Reader reader{ data, size, 0 };
  std::uint8_t type = 0;
  std::uint8_t pressedByte = 0;
  if (!reader.u8(type) ||
      type != static_cast<std::uint8_t>(ControlMessageType::Key) ||
      !reader.u32(header.targetOwner) || !reader.u32(header.shareId) ||
      !reader.u32(header.senderID) || !reader.u32(keycode) ||
      !reader.u8(pressedByte)) {
    return false;
  }
  pressed = pressedByte != 0;
  return true;
}

inline std::vector<std::uint8_t>
encodeWindowShareH264Frame(std::uint32_t owner,
                           std::uint32_t shareId,
                           std::int32_t width,
                           std::int32_t height,
                           bool keyframe,
                           const std::uint8_t* data,
                           std::size_t size)
{
  using namespace window_share_detail;
  std::vector<std::uint8_t> out;
  out.reserve(kWindowShareHeaderSize + 9 + size);
  appendU8(out, static_cast<std::uint8_t>(WindowShareMessageType::H264Frame));
  appendU32(out, owner);
  appendU32(out, shareId);
  appendI32(out, width);
  appendI32(out, height);
  appendU8(out, keyframe ? 1 : 0);
  if (data && size > 0) {
    out.insert(out.end(), data, data + size);
  }
  return out;
}

inline bool
decodeWindowShareH264Frame(const std::uint8_t* data,
                           std::size_t size,
                           WindowShareH264FrameView& frame)
{
  using namespace window_share_detail;
  Reader reader{ data, size, 0 };
  std::uint8_t type = 0;
  std::uint8_t keyframe = 0;
  std::uint32_t owner = 0;
  std::uint32_t shareId = 0;
  if (!reader.u8(type) ||
      type != static_cast<std::uint8_t>(WindowShareMessageType::H264Frame) ||
      !reader.u32(owner) || !reader.u32(shareId) ||
      !reader.i32(frame.width) || !reader.i32(frame.height) ||
      !reader.u8(keyframe)) {
    return false;
  }
  frame.shareId = shareId;
  frame.keyframe = keyframe != 0;
  frame.data = data + reader.offset;
  frame.size = size - reader.offset;
  return frame.size <= kMaxWindowFrameBytes;
}

inline bool
decodeControlKeyframeRequest(const std::uint8_t* data,
                             std::size_t size,
                             ControlHeader& header)
{
  if (!decodeControlHeader(data, size, header)) {
    return false;
  }
  return data[0] ==
         static_cast<std::uint8_t>(ControlMessageType::KeyframeRequest);
}

inline bool
decodeControlRequestOrRelease(const std::uint8_t* data,
                              std::size_t size,
                              ControlHeader& header,
                              bool& release)
{
  using namespace window_share_detail;
  Reader reader{ data, size, 0 };
  std::uint8_t type = 0;
  if (!reader.u8(type) || !reader.u32(header.targetOwner) ||
      !reader.u32(header.shareId) || !reader.u32(header.senderID)) {
    return false;
  }
  if (type == static_cast<std::uint8_t>(ControlMessageType::Request)) {
    release = false;
    return true;
  }
  if (type == static_cast<std::uint8_t>(ControlMessageType::Release)) {
    release = true;
    return true;
  }
  return false;
}

inline bool
windowShareOwner(const std::uint8_t* data,
                 std::size_t size,
                 std::uint32_t& owner)
{
  if (!data || size < kWindowShareHeaderSize) {
    return false;
  }
  owner = 0;
  for (int i = 0; i < 4; ++i) {
    owner |= static_cast<std::uint32_t>(data[kWindowShareOwnerOffset + i])
             << (8 * i);
  }
  return true;
}

inline bool
windowShareId(const std::uint8_t* data,
              std::size_t size,
              std::uint32_t& shareId)
{
  if (!data || size < kWindowShareHeaderSize) {
    return false;
  }
  shareId = 0;
  for (int i = 0; i < 4; ++i) {
    shareId |= static_cast<std::uint32_t>(data[kWindowShareIdOffset + i])
               << (8 * i);
  }
  return true;
}

} // namespace MultiPlayer
