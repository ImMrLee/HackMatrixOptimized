#include "api.h"
#include "camera.h"
#include "controls.h"
#include "renderer.h"
#include "TypedKeyOverlay.h"
#include "WindowManager/WindowManager.h"
#include "world.h"
#include "entity.h"
#include "engineGui.h"
#include "MultiPlayer/Client.h"
#include "MultiPlayer/Server.h"
#ifdef MATRIX_ENABLE_WEBGUI
#include "MultiPlayer/WebGui.h"
#endif

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <spdlog/common.h>
#include <string>
#include <vector>

struct EngineOptions {
  bool enableGui = true;
  bool enableControls = true;
  bool invertYAxis = false; // flip rendering for wlroots FBO orientation
};

class Engine
{
  std::unique_ptr<World> world;
  std::unique_ptr<Api> api;
  std::unique_ptr<Renderer> renderer;
  std::unique_ptr<Controls> controls;
  std::unique_ptr<Camera> camera;
  WindowManager::WindowManagerPtr wm;
  char** envp = nullptr;
  EngineOptions options;
  std::vector<double> frameTimes;
  int frameIndex = 0;
  double fps = 0.0;
  std::shared_ptr<spdlog::logger> logger;
  std::shared_ptr<EntityRegistry> registry;
  std::shared_ptr<EngineGui> engineGui;

  std::shared_ptr<MultiPlayer::Client> client;
  std::shared_ptr<MultiPlayer::Server> server;
#ifdef MATRIX_ENABLE_WEBGUI
  std::unique_ptr<MultiPlayer::WebGui> webGui;
#endif

  spdlog::sink_ptr loggerSink;
  std::shared_ptr<LoggerVector> setupLogger();
  void disableKeysIfImguiActive();
  bool imguiMouseCapture = false;

  void setupRegistry();
  void initializeMemberObjs();
  void multiplayerClientIteration(double frameStart);

  struct PendingMultiplayerCommand
  {
    enum class Type
    {
      Connect,
      Disconnect,
    };
    Type type = Type::Connect;
    std::string host;
    int port = 0;
  };
  std::mutex multiplayerCommandMutex_;
  std::vector<PendingMultiplayerCommand> multiplayerCommands_;
  std::string multiplayerError_;
  void drainMultiplayerCommands();
  // Drawn inside EngineGui's active ImGui frame (IME candidate window, ...).
  std::function<void()> imguiOverlay_;
  std::function<void(std::uint32_t, bool)> keyInjector_;
  std::function<bool(std::uint32_t, bool)> pointerInjector_;

public:
  Engine(char** envp, EngineOptions options = {});
  ~Engine();
  shared_ptr<EntityRegistry> getRegistry();
  shared_ptr<MultiPlayer::Client> getClient() { return client; }
  shared_ptr<MultiPlayer::Server> getServer() { return server; }
  bool isChatOpen() const { return engineGui && engineGui->isChatOpen(); }
  Camera* getCamera() { return camera.get(); }
  Renderer* getRenderer() { return renderer.get(); }
  Api* getApi() { return api.get(); }
  WindowManager::WindowManagerPtr getWindowManager() { return wm; }
  Controls* getControls() { return controls.get(); }
  void action(Action action);
  void wire();
  void frame();
  void registerServer(shared_ptr<MultiPlayer::Server>);
  void registerClient(shared_ptr<MultiPlayer::Client>);
  // Thread-safe requests used by the web UI. They are drained on the render
  // thread in multiplayerClientIteration().
  void requestMultiplayerConnect(std::string host, int port);
  void requestMultiplayerDisconnect();
  const std::string& multiplayerError() const { return multiplayerError_; }
  void updateImGuiPointer(float xPixels, float yPixels, const bool buttons[3]);
  // Forward a real keyboard event to the ImGui debug overlay (text input).
  void updateImGuiKey(xkb_keysym_t sym, bool pressed);
  void setImguiWantsMouse(bool wants) { imguiMouseCapture = wants; }
  bool imguiWantsMouse() const { return imguiMouseCapture; }
  void setImguiOverlay(std::function<void()> overlay)
  {
    imguiOverlay_ = std::move(overlay);
  }
  void setKeyInjector(std::function<void(std::uint32_t, bool)> injector)
  {
    keyInjector_ = std::move(injector);
  }
  void setPointerInjector(std::function<bool(std::uint32_t, bool)> injector)
  {
    pointerInjector_ = std::move(injector);
  }
  void renderImguiOverlays()
  {
    if (imguiOverlay_) {
      imguiOverlay_();
    }
  }
};
