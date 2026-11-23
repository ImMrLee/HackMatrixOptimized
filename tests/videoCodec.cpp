// Smoke tests for the libavcodec H.264 wrappers used by window streaming.

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "video_codec.h"

namespace {

std::vector<std::uint8_t> makeRgba(int width, int height, int offset)
{
  std::vector<std::uint8_t> pixels(
    static_cast<std::size_t>(width) * height * 4);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const std::size_t index =
        (static_cast<std::size_t>(y) * width + x) * 4;
      pixels[index + 0] = static_cast<std::uint8_t>((x + offset) & 0xff);
      pixels[index + 1] = static_cast<std::uint8_t>((y + offset) & 0xff);
      pixels[index + 2] = static_cast<std::uint8_t>((x + y) & 0xff);
      pixels[index + 3] = 255;
    }
  }
  return pixels;
}

bool
containsNalType(const std::vector<std::uint8_t>& data, int wanted)
{
  for (std::size_t i = 0; i + 3 < data.size(); ++i) {
    if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
      if ((data[i + 3] & 0x1f) == wanted) {
        return true;
      }
      i += 3;
    }
  }
  return false;
}

} // namespace

TEST(VideoCodecTest, EncoderProducesKeyframeAndDecoderConvertsBack)
{
  constexpr int width = 64;
  constexpr int height = 48;
  H264Encoder encoder(15, 70);
  H264Decoder decoder;

  const auto rgba = makeRgba(width, height, 0);
  std::vector<std::vector<std::uint8_t>> packets;
  bool keyframe = false;
  ASSERT_TRUE(
    encoder.encode(rgba.data(), width, height, packets, keyframe));
  ASSERT_FALSE(packets.empty());
  EXPECT_TRUE(keyframe);
  EXPECT_EQ(encoder.encodedWidth(), width);
  EXPECT_EQ(encoder.encodedHeight(), height);

  std::vector<std::uint8_t> decoded;
  int decodedWidth = 0;
  int decodedHeight = 0;
  ASSERT_TRUE(decoder.decode(packets.front().data(),
                             packets.front().size(),
                             keyframe,
                             decoded,
                             decodedWidth,
                             decodedHeight));
  EXPECT_EQ(decodedWidth, width);
  EXPECT_EQ(decodedHeight, height);
  EXPECT_EQ(decoded.size(),
            static_cast<std::size_t>(width) * height * 4);
}

TEST(VideoCodecTest, KeyframesRepeatParameterSets)
{
  constexpr int width = 64;
  constexpr int height = 48;
  H264Encoder encoder(15, 70);
  const auto rgba = makeRgba(width, height, 0);
  std::vector<std::vector<std::uint8_t>> packets;
  bool keyframe = false;

  ASSERT_TRUE(
    encoder.encode(rgba.data(), width, height, packets, keyframe));
  ASSERT_TRUE(keyframe);
  ASSERT_FALSE(packets.empty());

  // Advance past the first GOP, then force a fresh IDR. A late decoder must be
  // able to start from this keyframe alone.
  const auto shifted = makeRgba(width, height, 40);
  for (int i = 0; i < 5; ++i) {
    ASSERT_TRUE(
      encoder.encode(shifted.data(), width, height, packets, keyframe));
  }
  encoder.requestKeyframe();
  ASSERT_TRUE(
    encoder.encode(shifted.data(), width, height, packets, keyframe));
  ASSERT_TRUE(keyframe);
  ASSERT_FALSE(packets.empty());

  EXPECT_TRUE(containsNalType(packets.front(), 7)) << "missing SPS";
  EXPECT_TRUE(containsNalType(packets.front(), 8)) << "missing PPS";
}

TEST(VideoCodecTest, DecoderWaitsForKeyframe)
{
  constexpr int width = 32;
  constexpr int height = 32;
  H264Encoder encoder(15, 70);
  H264Decoder decoder;

  const auto rgba = makeRgba(width, height, 0);
  std::vector<std::vector<std::uint8_t>> packets;
  bool keyframe = false;
  ASSERT_TRUE(encoder.encode(rgba.data(),
                             width,
                             height,
                             packets,
                             keyframe));
  ASSERT_TRUE(keyframe);
  ASSERT_FALSE(packets.empty());

  std::vector<std::uint8_t> decoded;
  int decodedWidth = 0;
  int decodedHeight = 0;
  EXPECT_FALSE(decoder.decode(packets.front().data(),
                              packets.front().size(),
                              false,
                              decoded,
                              decodedWidth,
                              decodedHeight));
}
