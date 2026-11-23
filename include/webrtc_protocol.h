#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace MultiPlayer {

// WebRTC signaling messages travel on channel 7 and are unicast by the relay.
// Layout matches the control header: type, target owner, share, sender.
constexpr std::size_t kWebRtcHeaderSize = 13;
constexpr std::uint32_t kWebRtcTargetOffset = 1;
constexpr std::uint32_t kWebRtcShareOffset = 5;
constexpr std::uint32_t kWebRtcSenderOffset = 9;

enum class WebRtcSignalType : std::uint8_t
{
  Request = 1,
  Offer = 2,
  Answer = 3,
  Ice = 4,
  // Full renegotiation after a failed WebRTC session.
  Reconnect = 5,
};

struct WebRtcHeader
{
  WebRtcSignalType type = WebRtcSignalType::Request;
  std::uint32_t targetOwner = 0;
  std::uint32_t shareId = 0;
  std::uint32_t senderID = 0;
};

namespace webrtc_detail {
inline void appendU8(std::vector<std::uint8_t>& out, std::uint8_t v)
{
  out.push_back(v);
}
inline void appendU32(std::vector<std::uint8_t>& out, std::uint32_t v)
{
  for (int i = 0; i < 4; ++i) {
    out.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xff));
  }
}
inline void appendI32(std::vector<std::uint8_t>& out, std::int32_t v)
{
  appendU32(out, static_cast<std::uint32_t>(v));
}
inline void appendString(std::vector<std::uint8_t>& out,
                         const std::string& text)
{
  appendU32(out, static_cast<std::uint32_t>(text.size()));
  out.insert(out.end(), text.begin(), text.end());
}
struct Reader
{
  const std::uint8_t* data = nullptr;
  std::size_t size = 0;
  std::size_t offset = 0;
  bool u8(std::uint8_t& v)
  {
    if (offset + 1 > size) return false;
    v = data[offset++];
    return true;
  }
  bool u32(std::uint32_t& v)
  {
    if (offset + 4 > size) return false;
    v = 0;
    for (int i = 0; i < 4; ++i) {
      v |= static_cast<std::uint32_t>(data[offset + i]) << (8 * i);
    }
    offset += 4;
    return true;
  }
  bool i32(std::int32_t& v)
  {
    std::uint32_t raw = 0;
    if (!u32(raw)) return false;
    v = static_cast<std::int32_t>(raw);
    return true;
  }
  bool string(std::string& text)
  {
    std::uint32_t length = 0;
    if (!u32(length) || offset + length > size) return false;
    text.assign(reinterpret_cast<const char*>(data + offset), length);
    offset += length;
    return true;
  }
};
} // namespace webrtc_detail

inline std::vector<std::uint8_t> encodeWebRtcRequest(std::uint32_t targetOwner,
                                                     std::uint32_t shareId,
                                                     std::uint32_t senderID)
{
  using namespace webrtc_detail;
  std::vector<std::uint8_t> out;
  out.reserve(kWebRtcHeaderSize);
  appendU8(out, static_cast<std::uint8_t>(WebRtcSignalType::Request));
  appendU32(out, targetOwner);
  appendU32(out, shareId);
  appendU32(out, senderID);
  return out;
}

inline std::vector<std::uint8_t> encodeWebRtcReconnect(
  std::uint32_t targetOwner,
  std::uint32_t shareId,
  std::uint32_t senderID)
{
  using namespace webrtc_detail;
  std::vector<std::uint8_t> out;
  out.reserve(kWebRtcHeaderSize);
  appendU8(out, static_cast<std::uint8_t>(WebRtcSignalType::Reconnect));
  appendU32(out, targetOwner);
  appendU32(out, shareId);
  appendU32(out, senderID);
  return out;
}

inline std::vector<std::uint8_t> encodeWebRtcSdp(
  WebRtcSignalType type,
  std::uint32_t targetOwner,
  std::uint32_t shareId,
  std::uint32_t senderID,
  const std::string& sdp)
{
  using namespace webrtc_detail;
  std::vector<std::uint8_t> out;
  out.reserve(kWebRtcHeaderSize + 4 + sdp.size());
  appendU8(out, static_cast<std::uint8_t>(type));
  appendU32(out, targetOwner);
  appendU32(out, shareId);
  appendU32(out, senderID);
  appendString(out, sdp);
  return out;
}

inline std::vector<std::uint8_t> encodeWebRtcIce(std::uint32_t targetOwner,
                                                 std::uint32_t shareId,
                                                 std::uint32_t senderID,
                                                 std::int32_t mlineIndex,
                                                 const std::string& candidate)
{
  using namespace webrtc_detail;
  std::vector<std::uint8_t> out;
  out.reserve(kWebRtcHeaderSize + 8 + candidate.size());
  appendU8(out, static_cast<std::uint8_t>(WebRtcSignalType::Ice));
  appendU32(out, targetOwner);
  appendU32(out, shareId);
  appendU32(out, senderID);
  appendI32(out, mlineIndex);
  appendString(out, candidate);
  return out;
}

inline bool decodeWebRtcHeader(const std::uint8_t* data,
                               std::size_t size,
                               WebRtcHeader& header)
{
  using namespace webrtc_detail;
  Reader reader{ data, size, 0 };
  std::uint8_t type = 0;
  if (!reader.u8(type) || !reader.u32(header.targetOwner) ||
      !reader.u32(header.shareId) || !reader.u32(header.senderID)) {
    return false;
  }
  header.type = static_cast<WebRtcSignalType>(type);
  return true;
}

inline bool decodeWebRtcSdp(const std::uint8_t* data,
                            std::size_t size,
                            WebRtcHeader& header,
                            std::string& sdp)
{
  using namespace webrtc_detail;
  Reader reader{ data, size, 0 };
  std::uint8_t type = 0;
  if (!reader.u8(type) || !reader.u32(header.targetOwner) ||
      !reader.u32(header.shareId) || !reader.u32(header.senderID)) {
    return false;
  }
  header.type = static_cast<WebRtcSignalType>(type);
  if (header.type != WebRtcSignalType::Offer &&
      header.type != WebRtcSignalType::Answer) {
    return false;
  }
  return reader.string(sdp);
}

inline bool decodeWebRtcIce(const std::uint8_t* data,
                            std::size_t size,
                            WebRtcHeader& header,
                            std::int32_t& mlineIndex,
                            std::string& candidate)
{
  using namespace webrtc_detail;
  Reader reader{ data, size, 0 };
  std::uint8_t type = 0;
  if (!reader.u8(type) || !reader.u32(header.targetOwner) ||
      !reader.u32(header.shareId) || !reader.u32(header.senderID) ||
      !reader.i32(mlineIndex) || !reader.string(candidate)) {
    return false;
  }
  header.type = static_cast<WebRtcSignalType>(type);
  return header.type == WebRtcSignalType::Ice;
}

inline void setWebRtcSender(std::uint8_t* data,
                            std::size_t size,
                            std::uint32_t sender)
{
  if (!data || size < kWebRtcHeaderSize) return;
  for (int i = 0; i < 4; ++i) {
    data[kWebRtcSenderOffset + i] =
      static_cast<std::uint8_t>((sender >> (8 * i)) & 0xff);
  }
}

inline bool webRtcTargetOwner(const std::uint8_t* data,
                              std::size_t size,
                              std::uint32_t& owner)
{
  if (!data || size < kWebRtcHeaderSize) return false;
  owner = 0;
  for (int i = 0; i < 4; ++i) {
    owner |= static_cast<std::uint32_t>(data[kWebRtcTargetOffset + i])
             << (8 * i);
  }
  return true;
}

} // namespace MultiPlayer
