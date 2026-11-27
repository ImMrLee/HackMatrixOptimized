#pragma once

#include <functional>
#include <mutex>
#include <queue>
#include <string>
#include <unordered_set>
#include <vector>
#include <glad/glad.h>
#include <xkbcommon/xkbcommon.h>
#include <memory>
#include "blocks.h"
#include "camera.h"
#include "world.h"
#include "WindowManager/WindowManager.h"
#include "ControlMappings.h"

class TypedKeyOverlay;

struct DeferedAction
{
  shared_ptr<bool> isDone;
  function<void()> fn;
};

struct ControlResponse
{
  bool handledByWM = false;
  bool shouldExit = false;
};

class Controls
{
  ControlMappings controlMappings;
  shared_ptr<blocks::TexturePack> texturePack;
  WindowManager::WindowManagerPtr wm;
  shared_ptr<EntityRegistry> registry;
  float windowFlop = 0.25;
  float windowFlop_dt = 0.002;
  World* world;
  Camera* camera;
  Renderer* renderer;
  bool grabbedCursor = true;
  bool appFocused = false;
  double lastClickTime = 0;
  double lastKeyPressTime = 0;
  double lastShiftPressTime = 0;
  bool resetMouse = true;
  bool lastWaylandFocusActive = false;
  bool waylandModifierHeld = false;
  bool waylandShiftHeld = false;
  bool toggleCursorHeld = false;
  float lastX;
  float lastY;
  int clickY = 100;
  vector<DeferedAction> deferedActions;
  std::vector<std::function<void()>> queuedActions;
  std::mutex queuedActionsMutex;
  shared_ptr<WindowManager::Space> windowManagerSpace;
  bool keysEnabled = true;
  std::function<void()> pointerCenterCallback;

  // Free-view window move (G) / resize (edge drag) preview state.
  entt::entity windowMoveEntity = entt::null;
  float windowMoveDistance = 0.0f;
  entt::entity windowResizeEntity = entt::null;
  bool resizeLeft = false;
  bool resizeRight = false;
  bool resizeTop = false;
  bool resizeBottom = false;
  float resizeW0 = 0.0f;
  float resizeH0 = 0.0f;
  // Accumulated mouse movement since the resize began (world units). Each
  // frame's delta is small, so the target size must be integrated, not
  // recomputed from the frame delta alone.
  float resizeAccumW = 0.0f;
  float resizeAccumH = 0.0f;
  int resizePixelW0 = 0;
  int lastResizePixelW = -1;
  int lastResizePixelH = -1;
  glm::vec3 resizePos0{ 0.0f };
  glm::vec3 resizeRightAxis{ 1.0f, 0.0f, 0.0f };
  glm::vec3 resizeUpAxis{ 0.0f, 1.0f, 0.0f };
  float resizeDistance = 0.0f;

  void handleWindowMoveKey();
  bool cameraScreenAxes(glm::vec3& right, glm::vec3& up) const;
  float worldPerPixelAt(float distance) const;
  std::optional<WindowManager::AppRayHit> windowAimHit();
  float pointerScreenX = 0.0f;
  float pointerScreenY = 0.0f;
  bool windowMoveNoHitLogged = false;
  bool windowResizeDeltaLogged = false;
  bool exitConfirmPending_ = false;
  bool exitDialogRestoreFreeLook_ = false;
  double lastResizeRequestTime = 0.0;
  bool debugResizeFired = false;

  std::unordered_set<xkb_keysym_t> pressed;

  struct KeysymEvent {
    xkb_keysym_t sym;
    bool pressed;
    bool modifierHeld;
    bool shiftHeld;
  };
  std::queue<KeysymEvent> keysymQueue;
  std::mutex keysymMutex;

  // Synthetic key events (KEY_REPLAY), dispatched by pollPressedKeys() when due
  // so a press and its release land on different frames.
  struct ScheduledKeyEvent
  {
    double whenSeconds = 0.0;
    xkb_keysym_t sym = XKB_KEY_NoSymbol;
    bool pressed = false;
    bool modifierHeld = false;
    bool shiftHeld = false;
  };
  std::vector<ScheduledKeyEvent> scheduledKeyEvents;
  std::shared_ptr<TypedKeyOverlay> typedKeyOverlay;

  void handleMovement();
  void handleModEscape();
  void handleToggleCursor();
  void handleToggleApp();
  void handleSpawnTerminal();
  void handleReloadConfig();
  void handleFocus();
  bool handleUnfocusApp();
  bool handleCloseFocusedApp();
  bool handleMoveFocusedApp();
  bool handleResizeFocusedApp();
  void handleDMenu();
  void handleScreenshot();
  void handleSave();
  void handleSelection();
  void handleCodeBlock();
  void handleDebug();
  void handleToggleMeshing();
  void handleToggleWireframe();
  void handleLogBlockCounts();
  void handleLogBlockType();
  void handleChangePlayerSpeed();
  void handleWindowFlop();
  bool handleWaylandHotkeys();
  bool isPressed(xkb_keysym_t sym) const;
  bool isPressedEither(xkb_keysym_t a, xkb_keysym_t b) const;

  void handleKeys();
  void doAfter(shared_ptr<bool> isDone, function<void()> actionFn);
  void doDeferedActions();
  void enqueueAction(std::function<void()> fn);

public:
  Controls(WindowManager::WindowManagerPtr wm,
           World* world,
           Camera* camera,
           Renderer* renderer,
           std::shared_ptr<TypedKeyOverlay> typedKeyOverlay,
           shared_ptr<EntityRegistry> registry,
           shared_ptr<blocks::TexturePack> texturePack)
    : wm(wm)
    , world(world)
    , camera(camera)
    , renderer(renderer)
    , typedKeyOverlay(std::move(typedKeyOverlay))
    , texturePack(texturePack)
    , registry(registry)
  {
    lastWaylandFocusActive = false;
  }
  void poll();
  void pollPressedKeys();
  void goToApp(entt::entity);
  // Move the camera to a shared/remote window and switch to cursor mode so
  // mouse input goes to the remote app instead of rotating the camera.
  void focusRemoteApp(entt::entity);
  void returnToFreeLook();
  void setPointerCenterCallback(std::function<void()> callback)
  {
    pointerCenterCallback = std::move(callback);
  }

  // Free-view window manipulation. Move: hold G and move the mouse.
  // Resize: aim the crosshair at a window edge/corner and drag with the left
  // button. Both only preview locally; the shared-window transform is
  // committed (broadcast once) when the interaction ends.
  // "Carry" a window: while G is held it sits in front of the camera at a
  // fixed distance, always facing the camera, so it follows mouse-look and
  // WASD player movement.
  bool beginWindowMove(entt::entity entity, float distance);
  void updateHeldWindowPose();
  void endWindowMove();
  bool isWindowMoveActive() const { return windowMoveEntity != entt::null; }

  bool beginWindowResize(const WindowManager::AppRayHit& hit);
  bool handleWindowResizeDelta(double dx, double dy);
  void endWindowResize();
  bool isWindowResizeActive() const { return windowResizeEntity != entt::null; }

  // Headless test hook: HACKMATRIX_DEBUG_WINDOW_RESIZE="R,300,0" forces one
  // resize on the crosshair target when G is pressed.
  bool debugResizeOnce();

  // Entity currently being previewed (move or resize); entt::null otherwise.
  entt::entity windowPreviewEntity() const;

  // Esc in free view asks for confirmation instead of quitting immediately.
  // EngineGui owns the dialog; it polls this once per frame.
  bool takeExitConfirmRequest();
  // While the exit dialog is open, reveal the cursor so its buttons can be
  // clicked. On cancel, restore free look if the cursor was hidden before.
  void beginExitDialog();
  void endExitDialog();

  // Edge/corner mask under the free-view crosshair for cursor feedback:
  // bit0 left, bit1 right, bit2 top, bit3 bottom.
  unsigned windowResizeHoverMask();

  // Screen-space pointer position in output-local pixels. Used for window
  // move/resize when the cursor is visible (cursor mode) instead of the
  // crosshair.
  void setPointerScreenPosition(float x, float y)
  {
    pointerScreenX = x;
    pointerScreenY = y;
  }
  void moveTo(glm::vec3 pos,
              optional<glm::vec3> rotation,
              float secs,
              optional<function<void()>> = nullopt);
  bool matchesConfiguredKey(xkb_keysym_t sym, const std::string fn);
  void runQueuedActions();
  ControlResponse handleKeySym(xkb_keysym_t sym,
                               bool pressed,
                               bool modifierHeld,
                               bool shiftHeld);
  // Queue a synthetic key (e.g. from the KEY_REPLAY API) `delaySeconds` from now.
  void scheduleKeySym(xkb_keysym_t sym,
                      bool pressed,
                      bool modifierHeld,
                      bool shiftHeld,
                      double delaySeconds);
  void resetWaylandInputState();
  void applyMovementInput(bool forward, bool back, bool left, bool right);
  void clearMovementInput();
  void applyLookDelta(double dx, double dy);
  bool handlePointerButton(uint32_t button, bool pressed);
  // True in first-person/free-look mode: the cursor is hidden and the
  // crosshair at the screen centre aims at windows and the world.
  bool isCursorGrabbed() const { return grabbedCursor; }
  void disableKeys();
  void enableKeys();
  void triggerScreenshot();
  void wireWindowManager(shared_ptr<WindowManager::Space>);
};
