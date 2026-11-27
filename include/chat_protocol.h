#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace MultiPlayer {

// Chat travels on its own reliable ENet channel. The relay rewrites the sender
// field with the real ENet connect ID so a peer cannot impersonate another
// player.
//
// Layout (little endian):
//   [0..3]   sender player ID
//   [4..5]   name length (u16)
//   [...]    name bytes
//   [..]     text length (u16)
//   [...]    text bytes
//   [..]     timestamp milliseconds (u64)
constexpr std::uint8_t kChatChannel = 10;
constexpr std::size_t kChatSenderOffset = 0;
constexpr std::size_t kMaxChatNameBytes = 64;
constexpr std::size_t kMaxChatTextBytes = 1024;

struct ChatMessage
{
  std::uint32_t senderID = 0;
  std::string name;
  std::string text;
  std::uint64_t timestampMs = 0;
};

namespace chat_detail {

inline void
appendU16(std::vector<std::uint8_t>& out, std::uint16_t value)
{
  out.push_back(static_cast<std::uint8_t>(value & 0xff));
  out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
}

inline void
appendU32(std::vector<std::uint8_t>& out, std::uint32_t value)
{
  for (int i = 0; i < 4; ++i) {
    out.push_back(static_cast<std::uint8_t>((value >> (8 * i)) & 0xff));
  }
}

inline void
appendU64(std::vector<std::uint8_t>& out, std::uint64_t value)
{
  for (int i = 0; i < 8; ++i) {
    out.push_back(static_cast<std::uint8_t>((value >> (8 * i)) & 0xff));
  }
}

inline bool
readU16(const std::uint8_t* data,
        std::size_t size,
        std::size_t& offset,
        std::uint16_t& value)
{
  if (!data || offset + 2 > size) {
    return false;
  }
  value = static_cast<std::uint16_t>(data[offset]) |
          static_cast<std::uint16_t>(data[offset + 1] << 8);
  offset += 2;
  return true;
}

inline bool
readU32(const std::uint8_t* data,
        std::size_t size,
        std::size_t& offset,
        std::uint32_t& value)
{
  if (!data || offset + 4 > size) {
    return false;
  }
  value = 0;
  for (int i = 0; i < 4; ++i) {
    value |= static_cast<std::uint32_t>(data[offset + i]) << (8 * i);
  }
  offset += 4;
  return true;
}

inline bool
readU64(const std::uint8_t* data,
        std::size_t size,
        std::size_t& offset,
        std::uint64_t& value)
{
  if (!data || offset + 8 > size) {
    return false;
  }
  value = 0;
  for (int i = 0; i < 8; ++i) {
    value |= static_cast<std::uint64_t>(data[offset + i]) << (8 * i);
  }
  offset += 8;
  return true;
}

// Strip control characters, turn newlines/tabs into spaces, and truncate to
// maxBytes without splitting a UTF-8 sequence.
inline std::string
sanitize(const std::string& input, std::size_t maxBytes)
{
  std::string out;
  out.reserve(std::min(input.size(), maxBytes));
  for (unsigned char c : input) {
    if (c == '\n' || c == '\r' || c == '\t') {
      c = ' ';
    }
    if (c < 0x20 || c == 0x7f) {
      continue;
    }
    out.push_back(static_cast<char>(c));
  }
  if (out.size() <= maxBytes) {
    return out;
  }
  std::size_t cut = maxBytes;
  // Walk back off continuation bytes so the final character stays valid.
  while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80) {
    --cut;
  }
  out.resize(cut);
  return out;
}

} // namespace chat_detail

inline std::vector<std::uint8_t>
encodeChat(std::uint32_t senderID,
           const std::string& name,
           const std::string& text,
           std::uint64_t timestampMs)
{
  const std::string safeName =
    chat_detail::sanitize(name, kMaxChatNameBytes);
  const std::string safeText =
    chat_detail::sanitize(text, kMaxChatTextBytes);
  if (safeText.empty()) {
    return {};
  }
  std::vector<std::uint8_t> out;
  out.reserve(4 + 2 + safeName.size() + 2 + safeText.size() + 8);
  chat_detail::appendU32(out, senderID);
  chat_detail::appendU16(out, static_cast<std::uint16_t>(safeName.size()));
  out.insert(out.end(), safeName.begin(), safeName.end());
  chat_detail::appendU16(out, static_cast<std::uint16_t>(safeText.size()));
  out.insert(out.end(), safeText.begin(), safeText.end());
  chat_detail::appendU64(out, timestampMs);
  return out;
}

inline bool
decodeChat(const std::uint8_t* data, std::size_t size, ChatMessage& out)
{
  if (!data || size < 4 + 2 + 2 + 8) {
    return false;
  }
  std::size_t offset = 0;
  std::uint32_t senderID = 0;
  std::uint16_t nameLen = 0;
  std::uint16_t textLen = 0;
  if (!chat_detail::readU32(data, size, offset, senderID) ||
      !chat_detail::readU16(data, size, offset, nameLen)) {
    return false;
  }
  if (nameLen > kMaxChatNameBytes || offset + nameLen > size) {
    return false;
  }
  std::string name(reinterpret_cast<const char*>(data + offset), nameLen);
  offset += nameLen;
  if (!chat_detail::readU16(data, size, offset, textLen)) {
    return false;
  }
  if (textLen == 0 || textLen > kMaxChatTextBytes ||
      offset + textLen > size) {
    return false;
  }
  std::string text(reinterpret_cast<const char*>(data + offset), textLen);
  offset += textLen;
  std::uint64_t timestampMs = 0;
  if (!chat_detail::readU64(data, size, offset, timestampMs)) {
    return false;
  }
  out.senderID = senderID;
  out.name = std::move(name);
  out.text = std::move(text);
  out.timestampMs = timestampMs;
  return true;
}

inline void
setChatSender(std::uint8_t* data, std::size_t size, std::uint32_t senderID)
{
  if (!data || size < 4) {
    return;
  }
  for (int i = 0; i < 4; ++i) {
    data[i] = static_cast<std::uint8_t>((senderID >> (8 * i)) & 0xff);
  }
}

} // namespace MultiPlayer
