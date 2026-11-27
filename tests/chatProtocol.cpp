// Unit tests for the ENet chat wire format.

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "chat_protocol.h"

using namespace MultiPlayer;

TEST(ChatProtocolTest, RoundTrips)
{
  const auto bytes = encodeChat(1234, "alice", "hello world", 987654321);
  ASSERT_FALSE(bytes.empty());

  ChatMessage decoded;
  ASSERT_TRUE(decodeChat(bytes.data(), bytes.size(), decoded));
  EXPECT_EQ(decoded.senderID, 1234u);
  EXPECT_EQ(decoded.name, "alice");
  EXPECT_EQ(decoded.text, "hello world");
  EXPECT_EQ(decoded.timestampMs, 987654321u);
}

TEST(ChatProtocolTest, EmptyTextIsRejected)
{
  EXPECT_TRUE(encodeChat(1, "alice", "", 0).empty());
  EXPECT_TRUE(encodeChat(1, "alice", std::string(), 0).empty());
}

TEST(ChatProtocolTest, SanitizesControlCharacters)
{
  std::string text = "a\nb\tc\r";
  text.push_back(static_cast<char>(0x01));
  text += "d";
  const auto bytes = encodeChat(1, "bob", text, 0);
  ASSERT_FALSE(bytes.empty());

  ChatMessage decoded;
  ASSERT_TRUE(decodeChat(bytes.data(), bytes.size(), decoded));
  EXPECT_EQ(decoded.text, "a b c d");
}

TEST(ChatProtocolTest, TruncatesLongText)
{
  const std::string longText(kMaxChatTextBytes + 256, 'x');
  const auto bytes = encodeChat(1, "bob", longText, 0);
  ASSERT_FALSE(bytes.empty());

  ChatMessage decoded;
  ASSERT_TRUE(decodeChat(bytes.data(), bytes.size(), decoded));
  EXPECT_EQ(decoded.text.size(), kMaxChatTextBytes);
}

TEST(ChatProtocolTest, TruncatesLongName)
{
  const std::string longName(kMaxChatNameBytes + 64, 'n');
  const auto bytes = encodeChat(1, longName, "hi", 0);
  ASSERT_FALSE(bytes.empty());

  ChatMessage decoded;
  ASSERT_TRUE(decodeChat(bytes.data(), bytes.size(), decoded));
  EXPECT_EQ(decoded.name.size(), kMaxChatNameBytes);
}

TEST(ChatProtocolTest, RewritesSender)
{
  auto bytes = encodeChat(1, "alice", "hi", 0);
  ASSERT_FALSE(bytes.empty());
  setChatSender(bytes.data(), bytes.size(), 4242);

  ChatMessage decoded;
  ASSERT_TRUE(decodeChat(bytes.data(), bytes.size(), decoded));
  EXPECT_EQ(decoded.senderID, 4242u);
}

TEST(ChatProtocolTest, RejectsMalformedPackets)
{
  ChatMessage decoded;
  EXPECT_FALSE(decodeChat(nullptr, 0, decoded));

  const auto bytes = encodeChat(1, "alice", "hello", 0);
  ASSERT_FALSE(bytes.empty());
  for (std::size_t cut = 0; cut < bytes.size(); ++cut) {
    EXPECT_FALSE(decodeChat(bytes.data(), cut, decoded))
      << "accepted truncated packet of " << cut << " bytes";
  }
}

TEST(ChatProtocolTest, RejectsOversizedNameLength)
{
  std::vector<std::uint8_t> bytes;
  chat_detail::appendU32(bytes, 7);
  chat_detail::appendU16(
    bytes, static_cast<std::uint16_t>(kMaxChatNameBytes + 1));
  bytes.push_back('x');

  ChatMessage decoded;
  EXPECT_FALSE(decodeChat(bytes.data(), bytes.size(), decoded));
}

TEST(ChatProtocolTest, RejectsZeroTextLength)
{
  std::vector<std::uint8_t> bytes;
  chat_detail::appendU32(bytes, 7);
  chat_detail::appendU16(bytes, 1);
  bytes.push_back('n');
  chat_detail::appendU16(bytes, 0);

  ChatMessage decoded;
  EXPECT_FALSE(decodeChat(bytes.data(), bytes.size(), decoded));
}
