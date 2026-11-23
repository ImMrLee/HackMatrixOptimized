// Unit tests for the WebRTC signaling wire format relayed over ENet.

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "webrtc_protocol.h"

using namespace MultiPlayer;

TEST(WebRtcProtocolTest, RequestRoundTrips)
{
  const auto bytes = encodeWebRtcRequest(11, 22, 33);
  ASSERT_EQ(bytes.size(), kWebRtcHeaderSize);

  WebRtcHeader header;
  ASSERT_TRUE(decodeWebRtcHeader(bytes.data(), bytes.size(), header));
  EXPECT_EQ(header.type, WebRtcSignalType::Request);
  EXPECT_EQ(header.targetOwner, 11u);
  EXPECT_EQ(header.shareId, 22u);
  EXPECT_EQ(header.senderID, 33u);
}

TEST(WebRtcProtocolTest, SdpRoundTrips)
{
  const std::string sdp = "v=0\r\no=- 1 2 IN IP4 127.0.0.1\r\n";
  for (auto type : { WebRtcSignalType::Offer, WebRtcSignalType::Answer }) {
    const auto bytes = encodeWebRtcSdp(type, 1, 2, 3, sdp);
    WebRtcHeader header;
    std::string decoded;
    ASSERT_TRUE(decodeWebRtcSdp(bytes.data(), bytes.size(), header, decoded));
    EXPECT_EQ(header.type, type);
    EXPECT_EQ(header.targetOwner, 1u);
    EXPECT_EQ(header.shareId, 2u);
    EXPECT_EQ(header.senderID, 3u);
    EXPECT_EQ(decoded, sdp);
  }
}

TEST(WebRtcProtocolTest, SdpRejectsRequestType)
{
  const auto bytes =
    encodeWebRtcSdp(WebRtcSignalType::Request, 1, 2, 3, "not-sdp");
  WebRtcHeader header;
  std::string decoded;
  EXPECT_FALSE(decodeWebRtcSdp(bytes.data(), bytes.size(), header, decoded));
}

TEST(WebRtcProtocolTest, IceRoundTrips)
{
  const std::string candidate =
    "candidate:1 1 UDP 2122252543 192.168.1.2 50000 typ host";
  const auto bytes = encodeWebRtcIce(7, 8, 9, 1, candidate);

  WebRtcHeader header;
  int32_t mlineIndex = -1;
  std::string decoded;
  ASSERT_TRUE(
    decodeWebRtcIce(bytes.data(), bytes.size(), header, mlineIndex, decoded));
  EXPECT_EQ(header.type, WebRtcSignalType::Ice);
  EXPECT_EQ(header.targetOwner, 7u);
  EXPECT_EQ(header.shareId, 8u);
  EXPECT_EQ(header.senderID, 9u);
  EXPECT_EQ(mlineIndex, 1);
  EXPECT_EQ(decoded, candidate);
}

TEST(WebRtcProtocolTest, SenderAndTargetFields)
{
  auto bytes = encodeWebRtcRequest(100, 200, 0);
  uint32_t owner = 0;
  ASSERT_TRUE(webRtcTargetOwner(bytes.data(), bytes.size(), owner));
  EXPECT_EQ(owner, 100u);

  setWebRtcSender(bytes.data(), bytes.size(), 4242);
  WebRtcHeader header;
  ASSERT_TRUE(decodeWebRtcHeader(bytes.data(), bytes.size(), header));
  EXPECT_EQ(header.senderID, 4242u);
  // The relay must not touch the requested destination.
  EXPECT_EQ(header.targetOwner, 100u);
  EXPECT_EQ(header.shareId, 200u);
}

TEST(WebRtcProtocolTest, RejectsTruncatedPackets)
{
  const auto bytes = encodeWebRtcSdp(WebRtcSignalType::Offer, 1, 2, 3, "abc");
  WebRtcHeader header;
  std::string decoded;
  EXPECT_FALSE(
    decodeWebRtcSdp(bytes.data(), kWebRtcHeaderSize - 1, header, decoded));
  EXPECT_FALSE(decodeWebRtcHeader(nullptr, 0, header));

  const auto ice = encodeWebRtcIce(1, 2, 3, 0, "candidate");
  int32_t mlineIndex = 0;
  EXPECT_FALSE(decodeWebRtcIce(
    ice.data(), ice.size() - 1, header, mlineIndex, decoded));
  EXPECT_FALSE(decodeWebRtcIce(
    ice.data(), kWebRtcHeaderSize, header, mlineIndex, decoded));
}
