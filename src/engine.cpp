#include <future>
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-enum-enum-conversion"
#define ENET_IMPLEMENTATION
#include <enet/enet.h>
#pragma GCC diagnostic pop

#include "engine.h"
#include "imgui_keymap.h"
#include "components/Bootable.h"
#include "components/Key.h"
#include "components/Lock.h"
#include "components/Parent.h"
#include "components/Scriptable.h"
#include "components/Light.h"
#include "entity.h"
#include "logger.h"
#include "model.h"
#include "persister.h"
#include "TypedKeyOverlay.h"
#include "systems/Boot.h"
#include "systems/Derivative.h"
#include "systems/Light.h"
#include "WindowManager/WindowManager.h"
#include "blocks.h"
#include "systems/Door.h"
#include "tracy/Tracy.hpp"
#include "tracy/TracyOpenGL.hpp"
#ifdef MATRIX_ENABLE_WEBGUI
#include "MultiPlayer/WebGui.h"
#endif

#include <memory>
#include <spdlog/common.h>
#include <spdlog/spdlog.h>
#include <cstdlib>
#include <chrono>
#include <fstream>

#include "imgui/imgui.h"
#include "imgui/imgui_impl_opengl3.h"

namespace {

double
currentTimeSeconds()
{
  static const auto start = std::chrono::steady_clock::now();
  const auto now = std::chrono::steady_clock::now();
  std::chrono::duration<double> elapsed = now - start;
  return elapsed.count();
}

} // namespace

void
Engine::setupRegistry()
{
  registry = make_shared<EntityRegistry>();

  // Create all the Persistor
  // declaritively, because C++ type system can't 
  // iterate through the type parameters
  
  shared_ptr<SQLPersister> postionablePersister =
    make_shared<PositionablePersister>(registry);
  registry->addPersister(postionablePersister);

  shared_ptr<SQLPersister> modelPersister =
    make_shared<ModelPersister>(registry);
  registry->addPersister(modelPersister);

  shared_ptr<SQLPersister> lightPersister =
    make_shared<LightPersister>(registry);
  registry->addPersister(lightPersister);

  shared_ptr<SQLPersister> doorPersister =
    make_shared<systems::DoorPersister>(registry);
  registry->addPersister(doorPersister);

  shared_ptr<SQLPersister> keyPersister = make_shared<KeyPersister>(registry);
  registry->addPersister(keyPersister);

  shared_ptr<SQLPersister> lockPersister = make_shared<LockPersister>(registry);
  registry->addPersister(lockPersister);

  shared_ptr<SQLPersister> parentPersister =
    make_shared<ParentPersister>(registry);
  registry->addPersister(parentPersister);

  shared_ptr<SQLPersister> scriptablePersister =
    make_shared<ScriptablePersister>(registry);
  registry->addPersister(scriptablePersister);

  shared_ptr<SQLPersister> bootablePersister =
    make_shared<BootablePersister>(registry);
  registry->addPersister(bootablePersister);

  registry->createTablesIfNeeded();
  registry->loadAll();
}

shared_ptr<LoggerVector> Engine::setupLogger() {
  auto loggerVector = make_shared<LoggerVector>();
  auto imGuiSink = make_shared<ImGuiSink>(loggerVector);
  loggerSink = make_shared<LoggerSink>(fileSink, imGuiSink);
  logger = make_shared<spdlog::logger>("engine", loggerSink);
  logger->set_level(spdlog::level::debug);
  // Flush on every message so file logs survive a crash/forced kill.
  logger->flush_on(spdlog::level::debug);
  // Modules without their own logger use spdlog::info/error, which go through
  // this default logger and therefore the same file + ImGui sinks.
  spdlog::set_default_logger(logger);
  return loggerVector;
}

Engine::Engine(char** envp, EngineOptions options)
  : envp(envp)
  , options(options)
  , frameTimes(20, 0.0)
{
  setupRegistry();

  // this probably doesn't belong here
  // candidate for refactor
  systems::createDerivativeComponents(registry);

  auto loggerVector = setupLogger();
  initializeMemberObjs();

  // The ImGui debug overlay needs a current GL context. In the wlroots path the
  // Engine is constructed from the output render callback, so one is available.
  if (options.enableGui) {
    engineGui = std::make_shared<EngineGui>(this, registry, loggerVector);
  }

  TracyGpuContext;

  wire();
  if (wm) {
    wm->createAndRegisterApps(envp);
  }
}

Engine::~Engine()
{
  // The API poll thread references world/renderer/controls, so stop it first.
  api.reset();
  controls.reset();
  renderer.reset();
  world.reset();
  camera.reset();
  registry->saveAll();
}

void
Engine::action(Action action)
{
  if (world) {
    world->action(action);
  }
}

void
Engine::initializeMemberObjs()
{
  const char* apiAddressEnv = std::getenv("VOXEL_API_BIND");
  std::string apiAddress =
    apiAddressEnv != nullptr ? apiAddressEnv : "tcp://*:4455";
  if (const char* logPath = std::getenv("MATRIX_WLROOTS_OUTPUT")) {
    std::ofstream out(logPath, std::ios::app);
    out << "engine: VOXEL_API_BIND=" << apiAddress << "\n";
  }

  auto texturePack = blocks::initializeBasicPack();
  auto typedKeyOverlay = std::make_shared<TypedKeyOverlay>();
  wm = make_shared<WindowManager::WindowManager>(registry, loggerSink, envp);
  camera = std::make_unique<Camera>();
#ifdef MATRIX_ENABLE_WEBGUI
  webGui = std::make_unique<MultiPlayer::WebGui>(this);
#endif
  world = std::make_unique<World>(
    registry, camera.get(), texturePack, true, loggerSink);
  renderer = std::make_unique<Renderer>(registry,
                                       camera.get(),
                                       world.get(),
                                       typedKeyOverlay,
                                       texturePack,
                                       options.invertYAxis);
  camera->setInvertY(options.invertYAxis);
  if (options.enableControls) {
    controls = std::make_unique<Controls>(wm,
                                          world.get(),
                                          camera.get(),
                                          renderer.get(),
                                          typedKeyOverlay,
                                          registry,
                                          texturePack);
  } else {
    controls = nullptr;
  }
  api = std::make_unique<Api>(
    apiAddress, registry, controls.get(), renderer.get(), world.get(), wm);
  api->setClientProvider([this]() { return client; });
  api->setKeyInjector([this](std::uint32_t sym, bool pressed) {
    if (keyInjector_) {
      keyInjector_(sym, pressed);
    }
  });
  api->setPointerInjector([this](std::uint32_t button, bool pressed) {
    if (pointerInjector_) {
      return pointerInjector_(button, pressed);
    }
    return false;
  });
  if (wm) {
    wm->registerControls(controls.get());
  }
}

void
Engine::wire()
{
  world->attachRenderer(renderer.get());
  if (wm) {
    wm->wire(wm, camera.get(), renderer.get());
  }
}

void
Engine::requestMultiplayerConnect(std::string host, int port)
{
  std::lock_guard<std::mutex> lock(multiplayerCommandMutex_);
  PendingMultiplayerCommand command;
  command.type = PendingMultiplayerCommand::Type::Connect;
  command.host = std::move(host);
  command.port = port;
  multiplayerCommands_.push_back(std::move(command));
}

void
Engine::requestMultiplayerDisconnect()
{
  std::lock_guard<std::mutex> lock(multiplayerCommandMutex_);
  PendingMultiplayerCommand command;
  command.type = PendingMultiplayerCommand::Type::Disconnect;
  multiplayerCommands_.push_back(std::move(command));
}

void
Engine::drainMultiplayerCommands()
{
  std::vector<PendingMultiplayerCommand> commands;
  {
    std::lock_guard<std::mutex> lock(multiplayerCommandMutex_);
    commands.swap(multiplayerCommands_);
  }
  for (auto& command : commands) {
    if (command.type == PendingMultiplayerCommand::Type::Connect) {
      if (client && !client->isDisconnected()) {
        continue;
      }
      if (client) {
        client.reset();
      }
      multiplayerError_.clear();
      auto candidate = std::make_shared<MultiPlayer::Client>(registry);
      candidate->setWorld(world.get());
      if (!candidate->connect(command.host, command.port)) {
        multiplayerError_ = "Could not start connection to " + command.host +
                            ":" + std::to_string(command.port);
        spdlog::warn("Engine: {}", multiplayerError_);
        continue;
      }
      client = std::move(candidate);
    } else if (command.type ==
               PendingMultiplayerCommand::Type::Disconnect) {
      if (client) {
        client->disconnect();
      }
    }
  }
}

void Engine::multiplayerClientIteration(double frameStart) {

  drainMultiplayerCommands();

  static double lastPlayerUpdate = 0;
  if (client) {
    client->poll();
    if (auto remoteFocus = client->takeRemoteFocusRequest();
        remoteFocus != entt::null && controls) {
      controls->focusRemoteApp(remoteFocus);
    }
    if (client->takeRemoteUnfocusRequest() && controls) {
      controls->returnToFreeLook();
    }
    if (client->isDisconnected()) {
      if (!client->connectionError().empty()) {
        multiplayerError_ = client->connectionError();
      }
      client.reset();
    } else {
      // Capture/encode shared windows. This runs after renderer->render(), so
      // the GL context is current and the window textures hold this frame.
      if (controls) {
        client->setWindowPreviewEntity(controls->windowPreviewEntity());
      }
      client->tickWindowStreams();
      client->tickWebRtc();
    }
  }

  if (client && client->isConnected() &&
      frameStart - lastPlayerUpdate > 1.0 / 20.0) {
    client->sendPlayer(camera->position, camera->front);
    lastPlayerUpdate = frameStart;
  }
}

void
Engine::frame()
{
  double frameStart = currentTimeSeconds();
  api->mutateEntities();
  controls->pollPressedKeys();
  if (engineGui) {
    static bool debugExitOpened = false;
    if (!debugExitOpened && std::getenv("HACKMATRIX_DEBUG_EXIT_CONFIRM")) {
      debugExitOpened = true;
      engineGui->openExitConfirm();
    }
    if (controls->takeExitConfirmRequest()) {
      engineGui->openExitConfirm();
    }
  }
  renderer->render();
  if (engineGui) {
    engineGui->render(fps, frameIndex, frameTimes);
  }
  world->tick();
  if (engineGui) {
    disableKeysIfImguiActive();
  }
  controls->poll();
  multiplayerClientIteration(frameStart);

  // Debug aid for headless verification: capture a screenshot on frame N or
  // after a wall-clock delay (useful when a client connects later).
  if (wm) {
    if (const char* shotEnv = std::getenv("HACKMATRIX_SCREENSHOT_FRAME")) {
      static const long shotFrame = std::atol(shotEnv);
      static long frameCount = 0;
      if (frameCount++ == shotFrame) {
        wm->requestScreenshot();
      }
    }
    if (const char* msEnv = std::getenv("HACKMATRIX_SCREENSHOT_AFTER_MS")) {
      static const long afterMs = std::atol(msEnv);
      static const double firstFrameTime = currentTimeSeconds();
      static bool taken = false;
      if (!taken && (currentTimeSeconds() - firstFrameTime) * 1000.0 >= afterMs) {
        taken = true;
        wm->requestScreenshot();
      }
    }
  }

  TracyGpuCollect;
  FrameMark;

  frameTimes[frameIndex] = currentTimeSeconds() - frameStart;
  frameIndex = (frameIndex + 1) % frameTimes.size();
  if (frameIndex == 0 && frameTimes.size() > 0) {
    fps = 0.0;
    for (double ft : frameTimes) {
      fps += ft;
    }
    fps /= static_cast<double>(frameTimes.size());
    if (fps > 0.0) {
      fps = 1.0 / fps;
    }
  }
}

void
Engine::disableKeysIfImguiActive() {
  if (ImGui::IsAnyItemActive() || isChatOpen() || isExitConfirmOpen()) {
    controls->disableKeys();
  } else {
    controls->enableKeys();
  }
}

void
Engine::updateImGuiPointer(float xPixels, float yPixels, const bool buttons[3])
{
  if (!engineGui) {
    return;
  }
  ImGuiIO& io = ImGui::GetIO();
  io.MousePos = ImVec2(xPixels, yPixels);
  io.MouseDown[0] = buttons[0];
  io.MouseDown[1] = buttons[1];
  io.MouseDown[2] = buttons[2];
  setImguiWantsMouse(io.WantCaptureMouse);
}


void
Engine::updateImGuiKey(xkb_keysym_t sym, bool pressed)
{
  if (!engineGui) {
    return;
  }
  ImGuiIO& io = ImGui::GetIO();
  const ImGuiKey key = keysymToImGuiKey(sym);
  if (key != ImGuiKey_None) {
    io.AddKeyEvent(key, pressed);
  }
  if (pressed) {
    const uint32_t codepoint = xkb_keysym_to_utf32(sym);
    if (codepoint > 0) {
      io.AddInputCharacter(codepoint);
    }
  }
}

void
Engine::registerClient(shared_ptr<MultiPlayer::Client> _client)
{
  if (_client) {
    _client->setWorld(world.get());
  }
  client = _client;
}

void
Engine::registerServer(shared_ptr<MultiPlayer::Server> _server)
{
  server = _server;
}

shared_ptr<EntityRegistry>
Engine::getRegistry()
{
  return registry;
}
