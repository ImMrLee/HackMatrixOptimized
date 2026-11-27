// ENet-level test for chat relay: a dedicated Server must stamp the real
// sender ID and forward the message to other peers on the chat channel.

#include <gtest/gtest.h>

#include <chrono>
#include <thread>

#include <enet/enet.h>

#include "MultiPlayer/Server.h"
#include "chat_protocol.h"

using namespace MultiPlayer;

namespace {
bool
waitForConnect(ENetHost* host, bool& connected, int timeoutMs)
{
  const auto deadline =
    std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  while (std::chrono::steady_clock::now() < deadline) {
    ENetEvent event;
    while (enet_host_service(host, &event, 5) > 0) {
      if (event.type == ENET_EVENT_TYPE_CONNECT) {
        connected = true;
      } else if (event.type == ENET_EVENT_TYPE_RECEIVE) {
        enet_packet_destroy(event.packet);
      }
    }
    if (connected) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return connected;
}
} // namespace

TEST(ChatRelayTest, ForwardsAndStampsSender)
{
  constexpr int kPort = 23987;
  ASSERT_EQ(enet_initialize(), 0);

  Server server(true);
  ASSERT_TRUE(server.Start(kPort));

  ENetHost* sender = enet_host_create(nullptr, 1, 16, 0, 0);
  ENetHost* receiver = enet_host_create(nullptr, 1, 16, 0, 0);
  ASSERT_NE(sender, nullptr);
  ASSERT_NE(receiver, nullptr);

  ENetAddress address;
  address.host = ENET_HOST_ANY;
  address.port = kPort;
  enet_address_set_host(&address, "127.0.0.1");

  ENetPeer* senderPeer = enet_host_connect(sender, &address, 16, 0);
  ENetPeer* receiverPeer = enet_host_connect(receiver, &address, 16, 0);
  ASSERT_NE(senderPeer, nullptr);
  ASSERT_NE(receiverPeer, nullptr);

  bool senderConnected = false;
  bool receiverConnected = false;
  EXPECT_TRUE(waitForConnect(sender, senderConnected, 3000));
  EXPECT_TRUE(waitForConnect(receiver, receiverConnected, 3000));
  ASSERT_TRUE(senderConnected);
  ASSERT_TRUE(receiverConnected);

  // Spoof a different sender ID; the relay must overwrite it.
  const auto bytes = encodeChat(999999, "alice", "hello relay", 42);
  ASSERT_FALSE(bytes.empty());
  ENetPacket* packet = enet_packet_create(
    bytes.data(), bytes.size(), ENET_PACKET_FLAG_RELIABLE);
  ASSERT_NE(packet, nullptr);
  ASSERT_EQ(enet_peer_send(senderPeer, kChatChannel, packet), 0);
  enet_host_flush(sender);

  ChatMessage message;
  bool received = false;
  const auto deadline =
    std::chrono::steady_clock::now() + std::chrono::milliseconds(3000);
  while (!received && std::chrono::steady_clock::now() < deadline) {
    ENetEvent event;
    while (enet_host_service(receiver, &event, 5) > 0) {
      if (event.type == ENET_EVENT_TYPE_RECEIVE) {
        if (event.channelID == kChatChannel) {
          ASSERT_TRUE(
            decodeChat(event.packet->data, event.packet->dataLength, message));
          received = true;
        }
        enet_packet_destroy(event.packet);
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }

  EXPECT_TRUE(received);
  if (received) {
    EXPECT_EQ(message.senderID, senderPeer->connectID);
    EXPECT_EQ(message.name, "alice");
    EXPECT_EQ(message.text, "hello relay");
    EXPECT_EQ(message.timestampMs, 42u);
  }

  enet_peer_disconnect(senderPeer, 0);
  enet_peer_disconnect(receiverPeer, 0);
  enet_host_flush(sender);
  enet_host_flush(receiver);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  server.Stop();
  enet_host_destroy(sender);
  enet_host_destroy(receiver);
  enet_deinitialize();
}
