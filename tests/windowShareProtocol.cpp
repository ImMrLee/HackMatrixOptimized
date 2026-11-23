// Unit tests for the ENet window-share wire format.

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include "window_share_protocol.h"

using namespace MultiPlayer;

TEST(WindowShareProtocolTest, StartRoundTrips)
{
  WindowShareStartData start;
  start.shareId = 42;
  start.pos = { 1.0f, 2.0f, 3.0f };
  start.origin = { 0.1f, 0.2f, 0.3f };
  start.rotate = { 10.0f, 20.0f, 30.0f };
  start.scale = 1.5f;
  start.width = 1280;
  start.height = 720;
  start.name = "Wayland Terminal";

  auto bytes = encodeWindowShareStart(7, start);
  uint32_t owner = 0;
  ASSERT_TRUE(windowShareOwner(bytes.data(), bytes.size(), owner));
  EXPECT_EQ(owner, 7u);

  WindowShareStartData decoded;
  ASSERT_TRUE(decodeWindowShareStart(bytes.data(), bytes.size(), decoded));
  EXPECT_EQ(decoded.shareId, start.shareId);
  EXPECT_FLOAT_EQ(decoded.pos.x, start.pos.x);
  EXPECT_FLOAT_EQ(decoded.origin.y, start.origin.y);
  EXPECT_FLOAT_EQ(decoded.rotate.z, start.rotate.z);
  EXPECT_FLOAT_EQ(decoded.scale, start.scale);
  EXPECT_EQ(decoded.width, start.width);
  EXPECT_EQ(decoded.height, start.height);
  EXPECT_EQ(decoded.name, start.name);
}

TEST(WindowShareProtocolTest, StartRejectsTruncatedPacket)
{
  WindowShareStartData start;
  start.name = "test";
  auto bytes = encodeWindowShareStart(1, start);

  WindowShareStartData decoded;
  EXPECT_FALSE(decodeWindowShareStart(bytes.data(), bytes.size() - 1, decoded));
  EXPECT_FALSE(decodeWindowShareStart(nullptr, 0, decoded));
}

TEST(WindowShareProtocolTest, StopRoundTrips)
{
  const auto bytes = encodeWindowShareStop(11, 22);
  uint32_t owner = 0;
  uint32_t shareId = 0;
  ASSERT_TRUE(decodeWindowShareStop(bytes.data(), bytes.size(), owner, shareId));
  EXPECT_EQ(owner, 11u);
  EXPECT_EQ(shareId, 22u);
}

TEST(WindowShareProtocolTest, OwnerLeftRoundTrips)
{
  const auto bytes = encodeWindowShareOwnerLeft(99);
  uint32_t owner = 0;
  ASSERT_TRUE(decodeWindowShareOwnerLeft(bytes.data(), bytes.size(), owner));
  EXPECT_EQ(owner, 99u);
}

TEST(WindowShareProtocolTest, TransformRoundTrips)
{
  WindowShareTransformData transform;
  transform.shareId = 4;
  transform.pos = { 1.0f, 2.0f, 3.0f };
  transform.origin = { 0.1f, 0.2f, 0.3f };
  transform.rotate = { 4.0f, 5.0f, 6.0f };
  transform.scale = 0.75f;

  const auto bytes = encodeWindowShareTransform(8, transform);
  WindowShareTransformData decoded;
  ASSERT_TRUE(
    decodeWindowShareTransform(bytes.data(), bytes.size(), decoded));
  EXPECT_EQ(decoded.shareId, 4u);
  EXPECT_FLOAT_EQ(decoded.pos.y, 2.0f);
  EXPECT_FLOAT_EQ(decoded.origin.z, 0.3f);
  EXPECT_FLOAT_EQ(decoded.rotate.x, 4.0f);
  EXPECT_FLOAT_EQ(decoded.scale, 0.75f);

  uint32_t owner = 0;
  ASSERT_TRUE(windowShareOwner(bytes.data(), bytes.size(), owner));
  EXPECT_EQ(owner, 8u);
}

TEST(WindowShareProtocolTest, FrameRoundTrips)
{
  const std::vector<uint8_t> jpeg{ 1, 2, 3, 4, 5 };
  const auto bytes = encodeWindowShareFrame(3, 9, 640, 480,
                                            jpeg.data(), jpeg.size());

  WindowShareFrameView frame;
  ASSERT_TRUE(decodeWindowShareFrame(bytes.data(), bytes.size(), frame));
  EXPECT_EQ(frame.shareId, 9u);
  EXPECT_EQ(frame.width, 640);
  EXPECT_EQ(frame.height, 480);
  ASSERT_EQ(frame.size, jpeg.size());
  EXPECT_EQ(0, std::memcmp(frame.data, jpeg.data(), jpeg.size()));
}

TEST(WindowShareProtocolTest, ServerCanRewriteOwner)
{
  WindowShareStartData start;
  start.shareId = 5;
  auto bytes = encodeWindowShareStart(1, start);

  setWindowShareOwner(bytes.data(), bytes.size(), 1234);
  uint32_t owner = 0;
  ASSERT_TRUE(windowShareOwner(bytes.data(), bytes.size(), owner));
  EXPECT_EQ(owner, 1234u);

  WindowShareStartData decoded;
  ASSERT_TRUE(decodeWindowShareStart(bytes.data(), bytes.size(), decoded));
  EXPECT_EQ(decoded.shareId, 5u);
}

TEST(WindowShareProtocolTest, H264FrameRoundTrips)
{
  const std::vector<uint8_t> nal{ 0, 0, 0, 1, 0x67, 0x42, 0x00, 0x1e };
  const auto bytes = encodeWindowShareH264Frame(
    4, 12, 1150, 666, true, nal.data(), nal.size());

  uint32_t owner = 0;
  ASSERT_TRUE(windowShareOwner(bytes.data(), bytes.size(), owner));
  EXPECT_EQ(owner, 4u);

  WindowShareH264FrameView frame;
  ASSERT_TRUE(
    decodeWindowShareH264Frame(bytes.data(), bytes.size(), frame));
  EXPECT_EQ(frame.shareId, 12u);
  EXPECT_EQ(frame.width, 1150);
  EXPECT_EQ(frame.height, 666);
  EXPECT_TRUE(frame.keyframe);
  ASSERT_EQ(frame.size, nal.size());
  EXPECT_EQ(0, std::memcmp(frame.data, nal.data(), nal.size()));
}

TEST(WindowShareProtocolTest, ControlStateRoundTrips)
{
  const auto bytes =
    encodeWindowShareControlState(5, 7, true, 1234);
  uint32_t owner = 0;
  uint32_t shareId = 0;
  uint32_t controllerID = 0;
  bool allowControl = false;
  ASSERT_TRUE(windowShareOwner(bytes.data(), bytes.size(), owner));
  ASSERT_TRUE(decodeWindowShareControlState(
    bytes.data(), bytes.size(), shareId, allowControl, controllerID));
  EXPECT_EQ(owner, 5u);
  EXPECT_EQ(shareId, 7u);
  EXPECT_TRUE(allowControl);
  EXPECT_EQ(controllerID, 1234u);
}

TEST(WindowShareProtocolTest, ControlRequestAndServerStamp)
{
  auto bytes = encodeControlRequest(5, 7, 1);
  uint32_t targetOwner = 0;
  ASSERT_TRUE(
    controlTargetOwner(bytes.data(), bytes.size(), targetOwner));
  EXPECT_EQ(targetOwner, 5u);

  ControlHeader header;
  bool release = false;
  ASSERT_TRUE(
    decodeControlRequestOrRelease(bytes.data(), bytes.size(), header, release));
  EXPECT_FALSE(release);
  EXPECT_EQ(header.targetOwner, 5u);
  EXPECT_EQ(header.shareId, 7u);

  setControlSender(bytes.data(), bytes.size(), 99);
  ASSERT_TRUE(
    decodeControlRequestOrRelease(bytes.data(), bytes.size(), header, release));
  EXPECT_EQ(header.senderID, 99u);
}

TEST(WindowShareProtocolTest, ControlPointerAndKeyRoundTrip)
{
  ControlHeader header;
  float x = 0.0f;
  float y = 0.0f;
  auto motion = encodeControlPointerMotion(1, 2, 3, 12.5f, 34.5f);
  ASSERT_TRUE(
    decodeControlPointerMotion(motion.data(), motion.size(), header, x, y));
  EXPECT_EQ(header.shareId, 2u);
  EXPECT_FLOAT_EQ(x, 12.5f);
  EXPECT_FLOAT_EQ(y, 34.5f);

  uint32_t button = 0;
  bool pressed = false;
  auto buttonBytes = encodeControlPointerButton(1, 2, 3, 272, true);
  ASSERT_TRUE(decodeControlPointerButton(
    buttonBytes.data(), buttonBytes.size(), header, button, pressed));
  EXPECT_EQ(button, 272u);
  EXPECT_TRUE(pressed);

  float delta = 0.0f;
  int32_t discrete = 0;
  uint32_t source = 0;
  uint32_t orientation = 0;
  auto axis = encodeControlPointerAxis(1, 2, 3, -1.5f, -2, 4, 0);
  ASSERT_TRUE(decodeControlPointerAxis(
    axis.data(), axis.size(), header, delta, discrete, source, orientation));
  EXPECT_FLOAT_EQ(delta, -1.5f);
  EXPECT_EQ(discrete, -2);
  EXPECT_EQ(source, 4u);

  uint32_t keycode = 0;
  auto key = encodeControlKey(1, 2, 3, 30, true);
  ASSERT_TRUE(
    decodeControlKey(key.data(), key.size(), header, keycode, pressed));
  EXPECT_EQ(keycode, 30u);
  EXPECT_TRUE(pressed);
}

TEST(WindowShareProtocolTest, ReadsShareId)
{
  const auto bytes = encodeWindowShareStop(1, 0xAABBCCDD);
  uint32_t shareId = 0;
  ASSERT_TRUE(windowShareId(bytes.data(), bytes.size(), shareId));
  EXPECT_EQ(shareId, 0xAABBCCDDu);
}
