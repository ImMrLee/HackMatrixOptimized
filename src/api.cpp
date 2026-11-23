#include "api.h"
#include "MultiPlayer/Client.h"
#include "components/Light.h"
#include "controls.h"
#include "dynamicObject.h"
#include "glm/fwd.hpp"
#include <glm/glm.hpp>
#include <glm/gtx/hash.hpp>
#include "logger.h"
#include "renderer.h"
#include "model.h"
#include "systems/KeyAndLock.h"
#include "systems/Move.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <map>
#include <spdlog/common.h>
#include <sstream>
#include <string>
#include <thread>
#include <zmq/zmq.hpp>
#include <xkbcommon/xkbcommon.h>
#include <unordered_set>
#include "time_utils.h"
#undef Status
#include "protos/api.pb.h"

extern "C" {
#include <wayland-server-core.h>
}

using namespace std;

namespace {
void
log_to_tmp_api(const std::string& msg)
{
  const char* path = std::getenv("MATRIX_WLROOTS_OUTPUT");
  if (!path) {
    path = "/tmp/matrix-wlroots-output.log";
  }
  FILE* f = std::fopen(path, "a");
  if (!f) {
    return;
  }
  std::fwrite(msg.c_str(), 1, msg.size(), f);
  std::fclose(f);
}

// Serialize `response` and send it as one ZMQ frame.
// SerializeToString() is [[nodiscard]]; ignoring it meant a serialization
// failure sent an empty, undecodable reply with no diagnostic.
void
sendApiResponse(zmq::socket_t& socket,
                const ApiRequestResponse& response,
                const char* what)
{
  std::string payload;
  if (!response.SerializeToString(&payload)) {
    log_to_tmp_api(std::string("api: failed to serialize ") + what +
                   " response\n");
  }
  zmq::message_t reply(payload.size());
  if (!payload.empty()) {
    std::memcpy(reply.data(), payload.c_str(), payload.size());
  }
  socket.send(reply, zmq::send_flags::none);
}

glm::vec3
toVec3(const Vector& v)
{
  return glm::vec3(v.x(), v.y(), v.z());
}

Vector
toProtoVec3(const glm::vec3& v)
{
  Vector out;
  out.set_x(v.x);
  out.set_y(v.y);
  out.set_z(v.z);
  return out;
}

} // namespace

int BatchedRequest::nextId = 0;

EngineStatus
Api::buildStatus() const
{
  EngineStatus status;
  uint32_t totalEntities = 0;
  if (registry) {
    auto view = registry->view<entt::entity>();
    totalEntities = static_cast<uint32_t>(view.size_hint());
    auto wlView = registry->view<WaylandApp::Component>();
    status.set_wayland_apps(static_cast<uint32_t>(wlView.size()));
  }
  status.set_total_entities(totalEntities);
  bool waylandFocus = false;
  if (wm && registry) {
    if (auto focused = wm->getCurrentlyFocusedApp()) {
      if (registry->all_of<WaylandApp::Component>(*focused)) {
        waylandFocus = true;
      }
    }
  }
  status.set_wayland_focus(waylandFocus);
  if (renderer) {
    if (auto* camera = renderer->getCamera()) {
      auto* pos = status.mutable_camera_position();
      pos->set_x(camera->position.x);
      pos->set_y(camera->position.y);
      pos->set_z(camera->position.z);
    }
  }
  return status;
}

void
Api::updateCachedStatus()
{
  auto newStatus = buildStatus();
  std::lock_guard<std::mutex> lk(statusMutex);
  cachedStatus = newStatus;
  // Fulfill any pending STATUS requests.
  for (auto& pending : pendingStatus) {
    if (!pending) {
      continue;
    }
    pending->response.Clear();
    pending->response.set_requestid(pending->requestId);
    pending->response.set_success(true);
    *pending->response.mutable_status() = cachedStatus;
    pending->ready = true;
  }
  statusCv.notify_all();
}

Api::Api(std::string bindAddress,
         shared_ptr<EntityRegistry> registry,
         Controls* controls,
         Renderer* renderer,
         World* world,
         WindowManager::WindowManagerPtr wm)
  : registry(registry)
  , controls(controls)
  , renderer(renderer)
  , world(world)
  , wm(wm)
{
  context = zmq::context_t(2);
  logger = make_shared<spdlog::logger>("Api", fileSink);
  logger->set_level(spdlog::level::debug);
  {
    char buf[256];
    snprintf(buf, sizeof(buf), "api: binding command server at %s\n", bindAddress.c_str());
    log_to_tmp_api(std::string(buf));
  }
  try {
    commandServer = new ProtobufCommandServer(this, bindAddress, context);
  } catch (const std::exception& e) {
    logger->error("Failed to bind API socket on {}: {}", bindAddress, e.what());
    throw;
  }
  offRenderThread = thread(&Api::poll, this);
}

CommandServer::CommandServer(Api* api,
                             std::string bindAddress,
                             zmq::context_t& context)
  : api(api)
{
  logger = make_shared<spdlog::logger>("CommandServer", fileSink);
  logger->set_level(spdlog::level::info);
  socket = zmq::socket_t(context, zmq::socket_type::rep);
  socket.bind(bindAddress);
}

void
Api::ProtobufCommandServer::poll()
{
  try {
    zmq::message_t recv;
    zmq::recv_result_t result = socket.recv(recv);

    if (result >= 0) {
      ApiRequest apiRequest;
      if (!apiRequest.ParseFromArray(recv.data(), recv.size())) {
        // A malformed frame would otherwise be processed as a default-
        // constructed request (type 0), mutating state with garbage. A REP
        // socket requires exactly one send per recv, so still reply - returning
        // without one leaves the socket in an invalid state (EFSM) and wedges
        // the API in a busy loop.
        log_to_tmp_api("api: unparseable request\n");
        ApiRequestResponse errorResponse;
        errorResponse.set_success(false);
        sendApiResponse(socket, errorResponse, "parse-error");
        return;
      }

      api->grabBatched();
      auto batchedRequests = api->getBatchedRequests();
      auto request = BatchedRequest(apiRequest);
      if (apiRequest.type() == QUIT) {
        log_to_tmp_api("api quit requested\n");
        // Process QUIT immediately so the display is terminated even if the
        // main mutate loop isn't ticking (e.g., in headless tests).
        api->processBatchedRequest(request);
      } else if (apiRequest.type() == STATUS) {
        auto pending = std::make_shared<PendingStatusRequest>();
        pending->requestId = request.id;
        {
          std::lock_guard<std::mutex> lk(api->statusMutex);
          api->pendingStatus.push_back(pending);
        }
        api->releaseBatched();
        // Wait for render thread to fill response
        std::unique_lock<std::mutex> lk(api->statusMutex);
        api->statusCv.wait_for(lk, std::chrono::milliseconds(2000), [&pending]() {
          return pending->ready;
        });
        ApiRequestResponse response = pending->response;
        {
          char buf[128];
          snprintf(buf,
                   sizeof(buf),
                   "api status reply id=%ld wayland=%u total=%u ready=%d\n",
                   (long)request.id,
                   response.status().wayland_apps(),
                   response.status().total_entities(),
                   pending->ready ? 1 : 0);
          log_to_tmp_api(std::string(buf));
        }
        sendApiResponse(socket, response, "status");
        return;
      } else if (apiRequest.type() == LIST_ENTITIES ||
                 apiRequest.type() == GET_COMPONENT ||
                 apiRequest.type() == ADD_VOXELS ||
                 apiRequest.type() == CLEAR_VOXELS) {
        auto pending = std::make_shared<PendingApiResponse>();
        pending->requestId = request.id;
        {
          std::lock_guard<std::mutex> lk(api->responseMutex);
          api->pendingResponses[request.id] = pending;
        }
        batchedRequests->push(request);
        api->releaseBatched();
        ApiRequestResponse response;
        {
          std::unique_lock<std::mutex> lk(api->responseMutex);
          api->responseCv.wait_for(lk, std::chrono::milliseconds(2000), [&pending]() {
            return pending->ready;
          });
          response = pending->response;
          api->pendingResponses.erase(request.id);
        }
        if (!pending->ready) {
          response.Clear();
          response.set_requestid(request.id);
          response.set_success(false);
        }
        sendApiResponse(socket, response, "response");
        return;
      } else {
        batchedRequests->push(request);
      }
      api->releaseBatched();

      ApiRequestResponse response;
      response.set_requestid(request.id);
      if (request.actionId.has_value()) {
        response.set_actionid(request.actionId.value());
      }
      response.set_success(true);

      sendApiResponse(socket, response, "request");
    }
  } catch (zmq::error_t& e) {
    // Never spin at 100% CPU if the socket ends up in an unexpected state
    // (e.g. EFSM after a protocol misuse).
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

void
Api::poll()
{
  while (continuePolling) {
    if (commandServer != NULL) {
      commandServer->poll();
    }
  }
}

void
Api::fulfillPendingResponse(int64_t requestId,
                            const ApiRequestResponse& response)
{
  std::lock_guard<std::mutex> lk(responseMutex);
  auto it = pendingResponses.find(requestId);
  if (it == pendingResponses.end()) {
    return;
  }
  it->second->response = response;
  it->second->ready = true;
  responseCv.notify_all();
}

void
Api::processBatchedRequest(BatchedRequest batchedRequest)
{
  using Handler = void (Api::*)(const BatchedRequest&);
  // Dispatch table keeps each request isolated. Unknown types (and STATUS,
  // which poll() handles synchronously) are simply ignored, matching the old
  // switch's `default: break;`.
  static const std::map<MessageType, Handler> handlers = {
    { MOVE, &Api::handleMove },
    { TURN_KEY, &Api::handleTurnKey },
    { PLAYER_MOVE, &Api::handlePlayerMove },
    { UNFOCUS_WINDOW, &Api::handleUnfocusWindow },
    { ADD_VOXELS, &Api::handleAddVoxels },
    { CLEAR_VOXELS, &Api::handleClearVoxels },
    { CONFIRM_ACTION, &Api::handleConfirmAction },
    { QUIT, &Api::handleQuit },
    { POINTER_REPLAY, &Api::handlePointerReplay },
    { KEY_REPLAY, &Api::handleKeyReplay },
    { CREATE_ENTITY, &Api::handleCreateEntity },
    { DELETE_ENTITY, &Api::handleDeleteEntity },
    { LIST_ENTITIES, &Api::handleListEntities },
    { GET_COMPONENT, &Api::handleGetComponent },
    { ADD_COMPONENT, &Api::handleAddComponent },
    { DELETE_COMPONENT, &Api::handleDeleteComponent },
    { EDIT_COMPONENT, &Api::handleEditComponent },
  };

  auto it = handlers.find(batchedRequest.request.type());
  if (it == handlers.end()) {
    return;
  }

  // A single malformed request must not take down the request thread or leave
  // the caller waiting forever for a response.
  try {
    (this->*(it->second))(batchedRequest);
  } catch (const std::exception& error) {
    if (logger) {
      logger->error("Api: request {} failed: {}",
                    static_cast<int>(batchedRequest.request.type()),
                    error.what());
    }
    ApiRequestResponse response;
    response.set_requestid(batchedRequest.id);
    response.set_success(false);
    fulfillPendingResponse(batchedRequest.id, response);
  } catch (...) {
    if (logger) {
      logger->error("Api: request {} failed with an unknown error",
                    static_cast<int>(batchedRequest.request.type()));
    }
    ApiRequestResponse response;
    response.set_requestid(batchedRequest.id);
    response.set_success(false);
    fulfillPendingResponse(batchedRequest.id, response);
  }
}

void
Api::handleMove(const BatchedRequest& batchedRequest)
{
  auto entityId = static_cast<entt::entity>(batchedRequest.request.entityid());
  auto move = batchedRequest.request.move();
  auto client = clientProvider_ ? clientProvider_() : nullptr;
  if (client && client->isConnected() && client->worldAuthorityID() != 0 &&
      !client->isWorldAuthority() &&
      client->isSyncedEntity(
        static_cast<uint32_t>(batchedRequest.request.entityid()))) {
    // F14a v2: only the authority mutates shared scene entities.
    client->sendWorldEntityMove(
      static_cast<uint32_t>(batchedRequest.request.entityid()),
      glm::vec3(move.xdelta(), move.ydelta(), move.zdelta()),
      move.unitspersecond());
    return;
  }
  systems::translate(registry,
                     entityId,
                     glm::vec3(move.xdelta(), move.ydelta(), move.zdelta()),
                     move.unitspersecond());
}

void
Api::handleTurnKey(const BatchedRequest& batchedRequest)
{
  auto entityId = static_cast<entt::entity>(batchedRequest.request.entityid());
  auto turnKey = batchedRequest.request.turnkey();
  if (turnKey.on()) {
    systems::turnKey(registry, entityId);
  } else {
    systems::unturnKey(registry, entityId);
  }
}

void
Api::handlePlayerMove(const BatchedRequest& batchedRequest)
{
  if (!controls) {
    return;
  }
  auto playerMove = batchedRequest.request.playermove();
  glm::vec3 pos(playerMove.position().x(),
                playerMove.position().y(),
                playerMove.position().z());
  glm::vec3 rotation(playerMove.rotation().x(),
                     playerMove.rotation().y(),
                     playerMove.rotation().z());
  controls->moveTo(pos, rotation, playerMove.unitspersecond());
}

void
Api::handleUnfocusWindow(const BatchedRequest&)
{
  if (wm) {
    wm->unfocusApp();
  }
}

void
Api::handleAddVoxels(const BatchedRequest& batchedRequest)
{
  ApiRequestResponse response;
  response.set_requestid(batchedRequest.id);
  auto voxels = batchedRequest.request.addvoxels();
  std::vector<glm::vec3> positions;
  positions.reserve(voxels.voxels_size());
  for (const auto& v : voxels.voxels()) {
    positions.emplace_back(v.x(), v.y(), v.z());
  }
  float size = voxels.size() > 0 ? voxels.size() : 1.0f;
  bool replace = voxels.replace();
  glm::vec3 color(1.0f);
  vector<glm::vec3> colors;
  if (voxels.has_color()) {
    color = glm::vec3(voxels.color().x(), voxels.color().y(), voxels.color().z());
  }
  if (voxels.colors_size() > 0) {
    colors.reserve(voxels.colors_size());
    for (const auto& c : voxels.colors()) {
      colors.emplace_back(c.x(), c.y(), c.z());
    }
  }
  // Allow empty positions when replace is true so callers can clear voxels.
  bool shouldUpdate = replace || !positions.empty();
  if (world == nullptr || !shouldUpdate) {
    if (renderer != nullptr && shouldUpdate) {
      if (logger) {
        logger->info("AddVoxels: count={}, replace={}, size={}",
                     positions.size(),
                     replace,
                     size);
      }
      renderer->setLines(world != nullptr ? world->getLines()
                                          : std::vector<Line>{});
      renderer->addVoxels(positions, replace, size, color);
      response.set_success(true);
    } else {
      response.set_success(false);
    }
    fulfillPendingResponse(batchedRequest.id, response);
    return;
  }

  auto client = clientProvider_ ? clientProvider_() : nullptr;
  const bool synced = client && client->isConnected() &&
                      client->worldAuthorityID() != 0;
  if (synced && !client->isWorldAuthority()) {
    // F14a: the authority owns the world overlay; forward and wait for its
    // authoritative broadcast instead of applying locally.
    std::vector<glm::vec3> syncColors = colors;
    if (syncColors.empty()) {
      syncColors.assign(positions.size(), color);
    }
    client->sendWorldAdd(replace, size, positions, syncColors);
    response.set_success(true);
    fulfillPendingResponse(batchedRequest.id, response);
    return;
  }

  auto ids = colors.empty()
               ? world->addApiVoxels(positions, size, color, replace)
               : world->addApiVoxels(positions, size, colors, replace);
  for (auto id : ids) {
    response.add_voxel_ids(id);
  }
  response.set_success(true);
  if (synced) {
    std::vector<glm::vec3> syncColors = colors;
    if (syncColors.empty()) {
      syncColors.assign(positions.size(), color);
    }
    client->sendWorldAdd(replace, size, positions, syncColors);
  }
  fulfillPendingResponse(batchedRequest.id, response);
}

void
Api::handleClearVoxels(const BatchedRequest& batchedRequest)
{
  auto client = clientProvider_ ? clientProvider_() : nullptr;
  const bool synced = client && client->isConnected() &&
                      client->worldAuthorityID() != 0;
  const bool isAuthority = synced && client->isWorldAuthority();

  auto clear = batchedRequest.request.clearvoxels();
  if (clear.has_ids()) {
    ApiRequestResponse response;
    response.set_requestid(batchedRequest.id);
    vector<int64_t> ids;
    ids.reserve(clear.ids().ids_size());
    for (auto id : clear.ids().ids()) {
      ids.push_back(id);
      response.add_voxel_ids(id);
    }
    if (synced && !isAuthority) {
      // Ids are allocated locally by the authority's world; a non-authority
      // never created these objects, so it cannot resolve them. Ask for a box
      // clear instead.
      if (logger) {
        logger->warn("ClearVoxels by id requires the world authority; "
                     "ignored on this client");
      }
      response.set_success(false);
      fulfillPendingResponse(batchedRequest.id, response);
      return;
    }
    if (world != nullptr) {
      if (isAuthority) {
        const auto positions = world->apiVoxelPositions(ids);
        world->clearApiVoxelsByIds(ids);
        client->sendWorldClearPositions(positions);
      } else {
        world->clearApiVoxelsByIds(ids);
      }
      response.set_success(true);
    } else {
      response.set_success(false);
    }
    fulfillPendingResponse(batchedRequest.id, response);
    return;
  }

  ApiRequestResponse response;
  response.set_requestid(batchedRequest.id);
  if (!clear.has_box()) {
    response.set_success(false);
    fulfillPendingResponse(batchedRequest.id, response);
    return;
  }

  const auto& box = clear.box();
  glm::vec3 min(box.x().min(), box.y().min(), box.z().min());
  glm::vec3 max(box.x().max(), box.y().max(), box.z().max());
  if (logger) {
    logger->info("ClearVoxels request: min=({}, {}, {}), max=({}, {}, {})",
                 min.x,
                 min.y,
                 min.z,
                 max.x,
                 max.y,
                 max.z);
  }
  if (synced && !isAuthority) {
    client->sendWorldClearBox(min, max);
    response.set_success(true);
    fulfillPendingResponse(batchedRequest.id, response);
    return;
  }
  if (world != nullptr) {
    world->clearDynamicObjectsInBox(min, max);
    if (isAuthority) {
      client->sendWorldClearBox(min, max);
    }
    response.set_success(true);
  } else {
    response.set_success(false);
  }
  fulfillPendingResponse(batchedRequest.id, response);
}

void
Api::handleConfirmAction(const BatchedRequest& batchedRequest)
{
  auto confirm = batchedRequest.request.confirmaction();
  confirmClearArea(confirm.actionid());
}

void
Api::handleQuit(const BatchedRequest&)
{
  // Allow external harnesses/tests to terminate the compositor cleanly.
  if (logger) {
    logger->info("Received QUIT request; terminating display");
  }
  log_to_tmp_api("api quit requested\n");
  if (display) {
    wl_display_terminate(display);
  }
}

void
Api::handlePointerReplay(const BatchedRequest& batchedRequest)
{
  if (!controls) {
    return;
  }
  const auto& replay = batchedRequest.request.pointerreplay();
  uint64_t cumulative = 0;
  for (const auto& e : replay.entries()) {
    cumulative += e.delay_ms();
    // Deliver immediately; delay handling could be extended later if needed.
    bool handled = controls->handlePointerButton(e.button(), e.pressed());
    {
      char buf[160];
      snprintf(buf,
               sizeof(buf),
               "api pointer replay button=%u pressed=%d handled=%d cumulative_ms=%llu\n",
               e.button(),
               e.pressed() ? 1 : 0,
               handled ? 1 : 0,
               (unsigned long long)cumulative);
      log_to_tmp_api(std::string(buf));
    }
  }
}

void
Api::handleKeyReplay(const BatchedRequest& batchedRequest)
{
  if (!controls) {
    return;
  }
  // A press and its release must land on different frames so Controls::poll()
  // observes the key while it is held; schedule both on the controls timeline.
  // 150 ms survives a slow frame (two pollPressedKeys() calls per frame).
  constexpr double kHoldSeconds = 0.15;
  const auto& replay = batchedRequest.request.keyreplay();

  // KEY_REPLAY has no explicit modifier-release event. Treat a modifier keysym
  // in the sequence as held for the keys that follow it.
  bool modifierHeld = false;
  bool shiftHeld = false;

  double at = 0.0;
  for (const auto& entry : replay.entries()) {
    const xkb_keysym_t sym = xkb_keysym_from_name(
      entry.sym().c_str(), XKB_KEYSYM_CASE_INSENSITIVE);
    if (sym == XKB_KEY_NoSymbol) {
      log_to_tmp_api("api key replay: unknown keysym '" + entry.sym() + "'\n");
      continue;
    }
    switch (sym) {
      case XKB_KEY_Super_L:
      case XKB_KEY_Super_R:
        modifierHeld = true;
        continue;
      case XKB_KEY_Shift_L:
      case XKB_KEY_Shift_R:
        shiftHeld = true;
        continue;
      default:
        break;
    }

    controls->scheduleKeySym(sym, true, modifierHeld, shiftHeld, at);
    controls->scheduleKeySym(sym, false, modifierHeld, shiftHeld,
                             at + kHoldSeconds);
    if (keyInjector_) {
      // Also deliver to the focused Wayland client (and the IME keyboard grab).
      keyInjector_(static_cast<std::uint32_t>(sym), true);
      keyInjector_(static_cast<std::uint32_t>(sym), false);
    }
    at += kHoldSeconds + static_cast<double>(entry.delay_ms()) / 1000.0;
  }
}

void
Api::handleCreateEntity(const BatchedRequest&)
{
  if (registry) {
    registry->createPersistent();
  }
}

void
Api::handleDeleteEntity(const BatchedRequest& batchedRequest)
{
  if (!registry) {
    return;
  }
  entt::entity target =
    static_cast<entt::entity>(batchedRequest.request.entityid());
  if (registry->valid(target)) {
    registry->depersist(target);
  }
}

void
Api::handleListEntities(const BatchedRequest& batchedRequest)
{
  ApiRequestResponse response;
  response.set_requestid(batchedRequest.id);
  bool success = false;
  if (registry) {
    success = true;
    ComponentType filter = COMPONENT_TYPE_UNSPECIFIED;
    if (batchedRequest.request.has_listentities()) {
      filter = batchedRequest.request.listentities().filter_type();
    }
    auto view = registry->view<Persistable>();
    for (auto [entity, persistable] : view.each()) {
      if (filter == COMPONENT_TYPE_POSITIONABLE &&
          !registry->any_of<Positionable>(entity)) {
        continue;
      }
      if (filter == COMPONENT_TYPE_MODEL &&
          !registry->any_of<Model>(entity)) {
        continue;
      }
      if (filter == COMPONENT_TYPE_LIGHT &&
          !registry->any_of<Light>(entity)) {
        continue;
      }
      const auto entityId = static_cast<int64_t>(entity);
      response.add_entity_ids(entityId);
      auto* info = response.add_entity_components();
      info->set_entity_id(entityId);
      if (registry->any_of<Positionable>(entity)) {
        info->add_component_types(COMPONENT_TYPE_POSITIONABLE);
      }
      if (registry->any_of<Model>(entity)) {
        info->add_component_types(COMPONENT_TYPE_MODEL);
      }
      if (registry->any_of<Light>(entity)) {
        info->add_component_types(COMPONENT_TYPE_LIGHT);
      }
    }
  }
  response.set_success(success);
  fulfillPendingResponse(batchedRequest.id, response);
}

void
Api::handleGetComponent(const BatchedRequest& batchedRequest)
{
  ApiRequestResponse response;
  response.set_requestid(batchedRequest.id);
  bool success = false;
  if (registry && batchedRequest.request.has_getcomponent()) {
    entt::entity target =
      static_cast<entt::entity>(batchedRequest.request.entityid());
    if (registry->valid(target)) {
      ComponentType type =
        batchedRequest.request.getcomponent().component_type();
      auto* component = response.mutable_component();
      if (type == COMPONENT_TYPE_POSITIONABLE &&
          registry->any_of<Positionable>(target)) {
        const auto& positionable = registry->get<Positionable>(target);
        component->set_type(COMPONENT_TYPE_POSITIONABLE);
        auto* data = component->mutable_positionable();
        data->set_entityid(batchedRequest.request.entityid());
        *data->mutable_position() = toProtoVec3(positionable.pos);
        *data->mutable_rotation() = toProtoVec3(positionable.rotate);
        data->set_scale(positionable.scale);
        *data->mutable_origin() = toProtoVec3(positionable.origin);
        success = true;
      } else if (type == COMPONENT_TYPE_MODEL &&
                 registry->any_of<Model>(target)) {
        const auto& model = registry->get<Model>(target);
        component->set_type(COMPONENT_TYPE_MODEL);
        auto* data = component->mutable_model();
        data->set_entityid(batchedRequest.request.entityid());
        data->set_model_path(model.path);
        success = true;
      } else if (type == COMPONENT_TYPE_LIGHT &&
                 registry->any_of<Light>(target)) {
        const auto& light = registry->get<Light>(target);
        component->set_type(COMPONENT_TYPE_LIGHT);
        auto* data = component->mutable_light();
        data->set_entityid(batchedRequest.request.entityid());
        *data->mutable_color() = toProtoVec3(light.color);
        success = true;
      }
    }
  }
  response.set_success(success);
  fulfillPendingResponse(batchedRequest.id, response);
}

void
Api::handleAddComponent(const BatchedRequest& batchedRequest)
{
  const auto& add = batchedRequest.request.addcomponent();
  const auto& component = add.component();
  if (!registry) {
    return;
  }
  entt::entity target =
    static_cast<entt::entity>(batchedRequest.request.entityid());
  if (!registry->valid(target)) {
    return;
  }
  switch (component.type()) {
    case COMPONENT_TYPE_POSITIONABLE: {
      if (!component.has_positionable()) {
        break;
      }
      const auto& data = component.positionable();
      glm::vec3 pos = toVec3(data.position());
      glm::vec3 origin =
        data.has_origin() ? toVec3(data.origin()) : glm::vec3(0.0f);
      glm::vec3 rotation = toVec3(data.rotation());
      float scale = data.scale();
      auto client = clientProvider_ ? clientProvider_() : nullptr;
      if (client && client->isConnected() &&
          client->worldAuthorityID() != 0 && !client->isWorldAuthority() &&
          registry->any_of<Positionable>(target)) {
        client->sendWorldEntityEdit(
          static_cast<uint32_t>(target), pos, origin, rotation, scale);
        break;
      }
      if (registry->any_of<Positionable>(target)) {
        auto& positionable = registry->get<Positionable>(target);
        positionable.pos = pos;
        positionable.origin = origin;
        positionable.rotate = rotation;
        positionable.scale = scale;
        positionable.damage();
      } else {
        registry->emplace<Positionable>(target, pos, origin, rotation, scale);
      }
      break;
    }
    case COMPONENT_TYPE_MODEL: {
      if (!component.has_model()) {
        break;
      }
      const auto& data = component.model();
      if (data.model_path().empty()) {
        break;
      }
      if (registry->any_of<Model>(target)) {
        registry->removePersistent<Model>(target);
      }
      registry->emplace<Model>(target, data.model_path());
      break;
    }
    case COMPONENT_TYPE_LIGHT: {
      if (!component.has_light()) {
        break;
      }
      const auto& data = component.light();
      glm::vec3 color = toVec3(data.color());
      if (registry->any_of<Light>(target)) {
        registry->get<Light>(target).color = color;
      } else {
        registry->emplace<Light>(target, color);
      }
      break;
    }
    default:
      break;
  }
}

void
Api::handleDeleteComponent(const BatchedRequest& batchedRequest)
{
  const auto& del = batchedRequest.request.deletecomponent();
  if (!registry) {
    return;
  }
  entt::entity target =
    static_cast<entt::entity>(batchedRequest.request.entityid());
  if (!registry->valid(target)) {
    return;
  }
  switch (del.component_type()) {
    case COMPONENT_TYPE_POSITIONABLE:
      if (registry->any_of<Positionable>(target)) {
        registry->removePersistent<Positionable>(target);
      }
      break;
    case COMPONENT_TYPE_MODEL:
      if (registry->any_of<Model>(target)) {
        registry->removePersistent<Model>(target);
      }
      break;
    case COMPONENT_TYPE_LIGHT:
      if (registry->any_of<Light>(target)) {
        registry->removePersistent<Light>(target);
      }
      break;
    default:
      break;
  }
}

void
Api::handleEditComponent(const BatchedRequest& batchedRequest)
{
  const auto& edit = batchedRequest.request.editcomponent();
  const auto& component = edit.component();
  if (logger) {
    logger->debug("EditComponent: entity {} type {}",
                  static_cast<long long>(batchedRequest.request.entityid()),
                  static_cast<int>(component.type()));
  }
  if (!registry) {
    return;
  }
  entt::entity target =
    static_cast<entt::entity>(batchedRequest.request.entityid());
  if (!registry->valid(target)) {
    return;
  }
  switch (component.type()) {
    case COMPONENT_TYPE_POSITIONABLE: {
      if (!component.has_positionable() ||
          !registry->any_of<Positionable>(target)) {
        break;
      }
      const auto& data = component.positionable();
      auto& positionable = registry->get<Positionable>(target);
      glm::vec3 pos = toVec3(data.position());
      glm::vec3 origin =
        data.has_origin() ? toVec3(data.origin()) : glm::vec3(0.0f);
      glm::vec3 rotation = toVec3(data.rotation());
      float scale = data.scale();
      auto client = clientProvider_ ? clientProvider_() : nullptr;
      if (client && client->isConnected() &&
          client->worldAuthorityID() != 0 && !client->isWorldAuthority()) {
        client->sendWorldEntityEdit(
          static_cast<uint32_t>(target), pos, origin, rotation, scale);
        break;
      }
      positionable.pos = pos;
      positionable.origin = origin;
      positionable.rotate = rotation;
      positionable.scale = scale;
      positionable.damage();
      break;
    }
    case COMPONENT_TYPE_MODEL: {
      if (!component.has_model() || !registry->any_of<Model>(target)) {
        break;
      }
      const auto& data = component.model();
      if (data.model_path().empty()) {
        break;
      }
      if (registry->any_of<Model>(target)) {
        registry->removePersistent<Model>(target);
      }
      registry->emplace<Model>(target, data.model_path());
      break;
    }
    case COMPONENT_TYPE_LIGHT: {
      if (!component.has_light() || !registry->any_of<Light>(target)) {
        break;
      }
      const auto& data = component.light();
      registry->get<Light>(target).color = toVec3(data.color());
      break;
    }
    default:
      break;
  }
}

std::vector<Line>
Api::buildClearAreaLines(const glm::vec3& min, const glm::vec3& max) const
{
  glm::vec3 lower(std::min(min.x, max.x),
                  std::min(min.y, max.y),
                  std::min(min.z, max.z));
  glm::vec3 upper(std::max(min.x, max.x),
                  std::max(min.y, max.y),
                  std::max(min.z, max.z));
  const glm::vec3 color(1.0f, 0.3f, 0.2f);

  auto makeLine = [&](glm::vec3 a, glm::vec3 b) {
    Line l;
    l.points[0] = a;
    l.points[1] = b;
    l.color = color;
    return l;
  };

  glm::vec3 c000(lower.x, lower.y, lower.z);
  glm::vec3 c100(upper.x, lower.y, lower.z);
  glm::vec3 c010(lower.x, upper.y, lower.z);
  glm::vec3 c110(upper.x, upper.y, lower.z);
  glm::vec3 c001(lower.x, lower.y, upper.z);
  glm::vec3 c101(upper.x, lower.y, upper.z);
  glm::vec3 c011(lower.x, upper.y, upper.z);
  glm::vec3 c111(upper.x, upper.y, upper.z);

  std::vector<Line> lines;
  lines.reserve(12);
  // Bottom
  lines.emplace_back(makeLine(c000, c100));
  lines.emplace_back(makeLine(c100, c110));
  lines.emplace_back(makeLine(c110, c010));
  lines.emplace_back(makeLine(c010, c000));
  // Top
  lines.emplace_back(makeLine(c001, c101));
  lines.emplace_back(makeLine(c101, c111));
  lines.emplace_back(makeLine(c111, c011));
  lines.emplace_back(makeLine(c011, c001));
  // Vertical edges
  lines.emplace_back(makeLine(c000, c001));
  lines.emplace_back(makeLine(c100, c101));
  lines.emplace_back(makeLine(c110, c111));
  lines.emplace_back(makeLine(c010, c011));

  return lines;
}

std::vector<glm::vec3>
Api::buildClearAreaVoxels(const glm::vec3& min, const glm::vec3& max) const
{
  glm::vec3 lower(std::min(min.x, max.x),
                  std::min(min.y, max.y),
                  std::min(min.z, max.z));
  glm::vec3 upper(std::max(min.x, max.x),
                  std::max(min.y, max.y),
                  std::max(min.z, max.z));
  float s = renderer != nullptr ? renderer->getVoxelSize() : 1.0f;
  if (s <= 0.0f) {
    s = 1.0f;
  }
  std::unordered_set<glm::vec3> voxels;
  auto addEdge = [&](glm::vec3 fixedA, glm::vec3 fixedB, bool varyX, bool varyY, bool varyZ) {
    if (varyX) {
      for (float x = lower.x; x <= upper.x + 0.0001f; x += s) {
        voxels.insert(glm::vec3(x, fixedA.y, fixedA.z));
        voxels.insert(glm::vec3(x, fixedB.y, fixedB.z));
      }
    } else if (varyY) {
      for (float y = lower.y; y <= upper.y + 0.0001f; y += s) {
        voxels.insert(glm::vec3(fixedA.x, y, fixedA.z));
        voxels.insert(glm::vec3(fixedB.x, y, fixedB.z));
      }
    } else if (varyZ) {
      for (float z = lower.z; z <= upper.z + 0.0001f; z += s) {
        voxels.insert(glm::vec3(fixedA.x, fixedA.y, z));
        voxels.insert(glm::vec3(fixedB.x, fixedB.y, z));
      }
    }
  };

  // Edges parallel to X
  addEdge(glm::vec3(lower.x, lower.y, lower.z),
          glm::vec3(lower.x, upper.y, lower.z),
          true,
          false,
          false);
  addEdge(glm::vec3(lower.x, lower.y, upper.z),
          glm::vec3(lower.x, upper.y, upper.z),
          true,
          false,
          false);

  // Edges parallel to Y
  addEdge(glm::vec3(lower.x, lower.y, lower.z),
          glm::vec3(upper.x, lower.y, lower.z),
          false,
          true,
          false);
  addEdge(glm::vec3(lower.x, lower.y, upper.z),
          glm::vec3(upper.x, lower.y, upper.z),
          false,
          true,
          false);

  // Edges parallel to Z
  addEdge(glm::vec3(lower.x, lower.y, lower.z),
          glm::vec3(lower.x, lower.y, upper.z),
          false,
          false,
          true);
  addEdge(glm::vec3(upper.x, lower.y, lower.z),
          glm::vec3(upper.x, lower.y, upper.z),
          false,
          false,
          true);

  // Opposite edges on top face for Y/Z and X/Z
  addEdge(glm::vec3(lower.x, upper.y, lower.z),
          glm::vec3(lower.x, upper.y, upper.z),
          false,
          false,
          true);
  addEdge(glm::vec3(upper.x, upper.y, lower.z),
          glm::vec3(upper.x, upper.y, upper.z),
          false,
          false,
          true);

  addEdge(glm::vec3(lower.x, lower.y, lower.z),
          glm::vec3(lower.x, upper.y, lower.z),
          false,
          true,
          false);
  addEdge(glm::vec3(upper.x, lower.y, lower.z),
          glm::vec3(upper.x, upper.y, lower.z),
          false,
          true,
          false);

  addEdge(glm::vec3(lower.x, lower.y, upper.z),
          glm::vec3(lower.x, upper.y, upper.z),
          false,
          true,
          false);
  addEdge(glm::vec3(upper.x, lower.y, upper.z),
          glm::vec3(upper.x, upper.y, upper.z),
          false,
          true,
          false);
  return std::vector<glm::vec3>(voxels.begin(), voxels.end());
}

int64_t
Api::registerClearArea(const glm::vec3& min,
                       const glm::vec3& max,
                       std::optional<int64_t> requestedId)
{
  int64_t actionId = requestedId.has_value() ? requestedId.value()
                                             : allocateActionId();
  glm::vec3 lower(std::min(min.x, max.x),
                  std::min(min.y, max.y),
                  std::min(min.z, max.z));
  glm::vec3 upper(std::max(min.x, max.x),
                  std::max(min.y, max.y),
                  std::max(min.z, max.z));
  pendingClearAreas[actionId] =
    ClearAreaAction{ actionId, lower, upper, {}, {} };
  // Build a voxel outline so it is visible even if line rendering is disabled.
  auto previewVoxels = buildClearAreaVoxels(lower, upper);
  pendingClearAreas[actionId].previewVoxels = previewVoxels;
  if (renderer != nullptr && !previewVoxels.empty()) {
    renderer->addVoxels(
      previewVoxels, false, renderer->getVoxelSize(), glm::vec3(1.0f, 0.2f, 0.2f));
  }
  if (logger) {
    logger->info("Registered clear area {}: min=({}, {}, {}), max=({}, {}, {})",
                 actionId,
                 lower.x,
                 lower.y,
                 lower.z,
                 upper.x,
                 upper.y,
                 upper.z);
  }
  return actionId;
}

bool
Api::confirmClearArea(int64_t actionId)
{
  auto it = pendingClearAreas.find(actionId);
  if (it == pendingClearAreas.end()) {
    return false;
  }
  const auto& action = it->second;
  if (world != nullptr) {
    world->clearDynamicObjectsInBox(action.min, action.max);
  }
  auto client = clientProvider_ ? clientProvider_() : nullptr;
  if (client && client->isConnected() && client->isWorldAuthority()) {
    client->sendWorldClearBox(action.min, action.max);
  }
  if (logger) {
    logger->info("Confirmed clear area {}", actionId);
  }
  pendingClearAreas.erase(it);
  return true;
}

void
Api::mutateEntities()
{
  double time = nowSeconds();
  double target = time + 0.005;
  grabBatched();
  auto batchedRequests = getBatchedRequests();
  for (; time <= target && batchedRequests->size() != 0; time = nowSeconds()) {
    processBatchedRequest(batchedRequests->front());
    batchedRequests->pop();
  }
  releaseBatched();
  updateCachedStatus();
}

void
Api::grabBatched()
{
  renderMutex.lock();
}

void
Api::releaseBatched()
{
  renderMutex.unlock();
}

queue<BatchedRequest>*
Api::getBatchedRequests()
{
  return &batchedRequests;
}

Api::~Api()
{
  continuePolling = false;
  context.shutdown();
  offRenderThread.join();
  delete commandServer;
}
