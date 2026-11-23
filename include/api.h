#ifndef __API_H__
#define __API_H__
#include <atomic>
#include <functional>
#include <mutex>
#include <optional>
#include <queue>
#include <thread>
#include <unordered_map>
#include <vector>
#include <zmq/zmq.hpp>
#include <string>
#include "entity.h"
#include "protos/api.pb.h"
#include "world.h"
#include "logger.h"
#include "WindowManager/WindowManager.h"

using namespace std;

// Forward-declare Wayland display to avoid pulling Wayland headers here.
struct wl_display;
class Controls;
class Renderer;
namespace MultiPlayer {
class Client;
}
class Api;
class CommandServer
{
protected:
  Api* api;
  shared_ptr<spdlog::logger> logger;
  zmq::socket_t socket;

public:
  CommandServer(Api* api, std::string bindAddress, zmq::context_t& context);
  virtual void poll() = 0;
  // Deleted through a CommandServer* in Api::~Api(); without this the
  // ProtobufCommandServer destructor is never run (undefined behaviour).
  virtual ~CommandServer() = default;
};

struct ApiCube
{
  float x;
  float y;
  float z;
  int blockType;
};

struct BatchedRequest
{
  static int nextId;
  BatchedRequest(ApiRequest request)
    : request(request)
  {
    id = nextId++;
  }
  int64_t id;
  ApiRequest request;
  std::optional<int64_t> actionId;
};

struct ClearAreaAction
{
  int64_t id;
  glm::vec3 min;
  glm::vec3 max;
  std::vector<Line> lines;
  std::vector<glm::vec3> previewVoxels;
};

struct PendingStatusRequest
{
  int64_t requestId = 0;
  ApiRequestResponse response;
  bool ready = false;
};

struct PendingApiResponse
{
  int64_t requestId = 0;
  ApiRequestResponse response;
  bool ready = false;
};

class Api
{

  class ProtobufCommandServer : public CommandServer
  {
    using CommandServer::CommandServer;
    void poll() override;
  };

  Controls* controls;
  WindowManager::WindowManagerPtr wm;

  shared_ptr<spdlog::logger> logger;
  shared_ptr<EntityRegistry> registry;
  Renderer* renderer = nullptr;
  World* world = nullptr;
  // F14a: provider returning the current multiplayer client (render thread).
  std::function<std::shared_ptr<MultiPlayer::Client>()> clientProvider_;

  zmq::context_t context;
  CommandServer* commandServer;
  // Wayland display (set by wlroots path) so QUIT requests can terminate cleanly.
  wl_display* display = nullptr;

  queue<BatchedRequest> batchedRequests;

  mutex renderMutex;
  thread offRenderThread;

  std::atomic_bool continuePolling = true;
  std::atomic<int64_t> nextActionId = 1;
  std::unordered_map<int64_t, ClearAreaAction> pendingClearAreas;
  mutable std::mutex statusMutex;
  EngineStatus cachedStatus; // updated on render thread, read by API thread
  std::deque<std::shared_ptr<PendingStatusRequest>> pendingStatus;
  std::condition_variable statusCv;
  std::mutex responseMutex;
  std::condition_variable responseCv;
  std::unordered_map<int64_t, std::shared_ptr<PendingApiResponse>> pendingResponses;
  int64_t registerClearArea(const glm::vec3& min,
                            const glm::vec3& max,
                            std::optional<int64_t> requestedId);
  bool confirmClearArea(int64_t actionId);
  std::vector<Line> buildClearAreaLines(const glm::vec3& min,
                                        const glm::vec3& max) const;
  std::vector<glm::vec3> buildClearAreaVoxels(const glm::vec3& min,
                                              const glm::vec3& max) const;
  void fulfillPendingResponse(int64_t requestId,
                              const ApiRequestResponse& response);

protected:
  void grabBatched();
  queue<BatchedRequest>* getBatchedRequests();
  void releaseBatched();
  void processBatchedRequest(BatchedRequest);
  // One handler per MessageType (see processBatchedRequest's dispatch table).
  void handleMove(const BatchedRequest&);
  void handleTurnKey(const BatchedRequest&);
  void handlePlayerMove(const BatchedRequest&);
  void handleUnfocusWindow(const BatchedRequest&);
  void handleAddVoxels(const BatchedRequest&);
  void handleClearVoxels(const BatchedRequest&);
  void handleConfirmAction(const BatchedRequest&);
  void handleQuit(const BatchedRequest&);
  void handlePointerReplay(const BatchedRequest&);
  void handleKeyReplay(const BatchedRequest&);
  void handleCreateEntity(const BatchedRequest&);
  void handleDeleteEntity(const BatchedRequest&);
  void handleListEntities(const BatchedRequest&);
  void handleGetComponent(const BatchedRequest&);
  void handleAddComponent(const BatchedRequest&);
  void handleDeleteComponent(const BatchedRequest&);
  void handleEditComponent(const BatchedRequest&);
  void updateCachedStatus();

public:
  Api(std::string bindAddress,
      shared_ptr<EntityRegistry>,
      Controls* controls,
      Renderer* renderer,
      World* world,
      WindowManager::WindowManagerPtr);
  void setDisplay(wl_display* d) { display = d; }
  void setClientProvider(
    std::function<std::shared_ptr<MultiPlayer::Client>()> provider)
  {
    clientProvider_ = std::move(provider);
  }
  ~Api();
  void poll();
  void mutateEntities();
  int64_t allocateActionId() { return nextActionId++; }
  EngineStatus buildStatus() const;
};

#endif
