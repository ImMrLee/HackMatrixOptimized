// Protocol contract tests for the ZMQ API.
//
// The public API is protobuf-over-ZMQ, so external clients (the Python client
// in client_libs/, scripts, the test harness) depend on the wire format. These
// tests pin the parts of the schema that break silently: field round-trips,
// oneof semantics and enum values.
//
// This file used to test an Api/DynamicObjectSpace design that has since been
// replaced by the ECS-based API, which is why it no longer compiled. It is now
// rewritten against the current schema.

#include <gtest/gtest.h>

#include "protos/api.pb.h"

namespace {

ApiRequest
parseRequest(const ApiRequest& in)
{
  std::string bytes;
  EXPECT_TRUE(in.SerializeToString(&bytes));

  ApiRequest out;
  EXPECT_TRUE(out.ParseFromArray(bytes.data(), static_cast<int>(bytes.size())));
  return out;
}

ApiRequestResponse
parseResponse(const ApiRequestResponse& in)
{
  std::string bytes;
  EXPECT_TRUE(in.SerializeToString(&bytes));

  ApiRequestResponse out;
  EXPECT_TRUE(out.ParseFromArray(bytes.data(), static_cast<int>(bytes.size())));
  return out;
}

} // namespace

TEST(ApiProtocol, MoveRoundTrip)
{
  ApiRequest request;
  request.set_entityid(42);
  request.set_type(MessageType::MOVE);
  auto* move = request.mutable_move();
  move->set_xdelta(1.5f);
  move->set_ydelta(-2.0f);
  move->set_zdelta(0.25f);
  move->set_unitspersecond(3.0f);

  const ApiRequest parsed = parseRequest(request);

  EXPECT_EQ(parsed.entityid(), 42);
  EXPECT_EQ(parsed.type(), MessageType::MOVE);
  ASSERT_TRUE(parsed.has_move());
  EXPECT_FLOAT_EQ(parsed.move().xdelta(), 1.5f);
  EXPECT_FLOAT_EQ(parsed.move().ydelta(), -2.0f);
  EXPECT_FLOAT_EQ(parsed.move().zdelta(), 0.25f);
  EXPECT_FLOAT_EQ(parsed.move().unitspersecond(), 3.0f);
}

TEST(ApiProtocol, PlayerMoveRoundTrip)
{
  ApiRequest request;
  request.set_type(MessageType::PLAYER_MOVE);
  auto* pm = request.mutable_playermove();
  pm->mutable_position()->set_x(10.0f);
  pm->mutable_position()->set_y(20.0f);
  pm->mutable_position()->set_z(30.0f);
  pm->mutable_rotation()->set_y(90.0f);
  pm->set_unitspersecond(7.5f);

  const ApiRequest parsed = parseRequest(request);

  ASSERT_TRUE(parsed.has_playermove());
  EXPECT_FLOAT_EQ(parsed.playermove().position().x(), 10.0f);
  EXPECT_FLOAT_EQ(parsed.playermove().position().z(), 30.0f);
  EXPECT_FLOAT_EQ(parsed.playermove().rotation().y(), 90.0f);
  EXPECT_FLOAT_EQ(parsed.playermove().unitspersecond(), 7.5f);
}

TEST(ApiProtocol, KeyReplayRoundTrip)
{
  ApiRequest request;
  request.set_type(MessageType::KEY_REPLAY);
  auto* replay = request.mutable_keyreplay();
  auto* first = replay->add_entries();
  first->set_sym("v");
  first->set_delay_ms(50);
  auto* second = replay->add_entries();
  second->set_sym("Super_L");
  second->set_delay_ms(10);

  const ApiRequest parsed = parseRequest(request);

  ASSERT_TRUE(parsed.has_keyreplay());
  ASSERT_EQ(parsed.keyreplay().entries_size(), 2);
  EXPECT_EQ(parsed.keyreplay().entries(0).sym(), "v");
  EXPECT_EQ(parsed.keyreplay().entries(0).delay_ms(), 50u);
  EXPECT_EQ(parsed.keyreplay().entries(1).sym(), "Super_L");
}

TEST(ApiProtocol, PayloadFieldIsAOneof)
{
  ApiRequest request;
  request.mutable_move()->set_xdelta(1.0f);
  EXPECT_EQ(request.payload_case(), ApiRequest::kMove);

  // Setting another member of the oneof must clear the previous one.
  request.mutable_keyreplay();
  EXPECT_EQ(request.payload_case(), ApiRequest::kKeyReplay);
  EXPECT_FALSE(request.has_move());
}

TEST(ApiProtocol, StatusResponseRoundTrip)
{
  ApiRequestResponse response;
  response.set_requestid(99);
  response.set_success(true);
  response.mutable_status()->set_total_entities(12);
  response.mutable_status()->set_wayland_apps(3);
  response.mutable_status()->set_wayland_focus(true);
  response.mutable_status()->mutable_camera_position()->set_x(1.0f);
  response.add_entity_ids(7);
  response.add_entity_ids(8);

  const ApiRequestResponse parsed = parseResponse(response);

  EXPECT_EQ(parsed.requestid(), 99);
  EXPECT_TRUE(parsed.success());
  EXPECT_EQ(parsed.status().total_entities(), 12u);
  EXPECT_EQ(parsed.status().wayland_apps(), 3u);
  EXPECT_TRUE(parsed.status().wayland_focus());
  EXPECT_FLOAT_EQ(parsed.status().camera_position().x(), 1.0f);
  ASSERT_EQ(parsed.entity_ids_size(), 2);
  EXPECT_EQ(parsed.entity_ids(0), 7);
  EXPECT_EQ(parsed.entity_ids(1), 8);
}

TEST(ApiProtocol, MessageTypeValuesAreStable)
{
  // Clients switch on these numeric values; changing one is a breaking change.
  EXPECT_EQ(static_cast<int>(MessageType::MOVE), 0);
  EXPECT_EQ(static_cast<int>(MessageType::QUIT), 7);
  EXPECT_EQ(static_cast<int>(MessageType::STATUS), 9);
  EXPECT_EQ(static_cast<int>(MessageType::LIST_ENTITIES), 16);
  EXPECT_EQ(static_cast<int>(MessageType::GET_COMPONENT), 17);
}
