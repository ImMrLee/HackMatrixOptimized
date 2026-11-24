#include "world.h"
#include <glad/glad.h>
#include <array>
#include <optional>

#include <iostream>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <cctype>
#include <cstdint>
#include <cstdarg>
#include <fstream>
#include <mutex>
#include <xkbcommon/xkbcommon.h>
#include <glm/gtc/quaternion.hpp>
#include <linux/input-event-codes.h>

#include "Config.h"
#include "controls.h"
#include "camera.h"
#include "renderer.h"
#include "TypedKeyOverlay.h"
#include "time_utils.h"
#include "remote_app.h"

using namespace std;

namespace {
struct KeyBinding
{
  const char* fn = nullptr;
  xkb_keysym_t fallbackLower = XKB_KEY_NoSymbol;
  xkb_keysym_t fallbackUpper = XKB_KEY_NoSymbol;
};

bool
is_configured_or_fallback_pressed(ControlMappings& controlMappings,
                                  const std::unordered_set<xkb_keysym_t>& pressed,
                                  const std::string& fn,
                                  xkb_keysym_t fallbackLower,
                                  xkb_keysym_t fallbackUpper)
{
  int mapped = controlMappings.getKey(fn);
  if (mapped != -1) {
    return pressed.count(static_cast<xkb_keysym_t>(mapped)) > 0;
  }
  return pressed.count(fallbackLower) > 0 || pressed.count(fallbackUpper) > 0;
}

bool
matches_configured_or_fallback_key(ControlMappings& controlMappings,
                                   xkb_keysym_t sym,
                                   const std::string& fn,
                                   xkb_keysym_t fallbackLower,
                                   xkb_keysym_t fallbackUpper)
{
  int mapped = controlMappings.getKey(fn);
  if (mapped != -1) {
    return sym == static_cast<xkb_keysym_t>(mapped);
  }
  return sym == fallbackLower || sym == fallbackUpper;
}

ControlResponse
key_handled_by_wm()
{
  ControlResponse resp;
  resp.handledByWM = true;
  resp.shouldExit = false;
  return resp;
}

ControlResponse
quit_key_handled_by_wm()
{
  ControlResponse resp;
  resp.handledByWM = true;
  resp.shouldExit = true;
  return resp;
}


bool
is_super_keysym(xkb_keysym_t sym)
{
  return sym == XKB_KEY_Super_L || sym == XKB_KEY_Super_R ||
         sym == XKB_KEY_Meta_L || sym == XKB_KEY_Meta_R;
}

xkb_keysym_t
normalize_hotkey_sym(xkb_keysym_t sym)
{
  switch (sym) {
    case XKB_KEY_exclam:
      return XKB_KEY_1;
    case XKB_KEY_at:
      return XKB_KEY_2;
    case XKB_KEY_numbersign:
      return XKB_KEY_3;
    case XKB_KEY_dollar:
      return XKB_KEY_4;
    case XKB_KEY_percent:
      return XKB_KEY_5;
    case XKB_KEY_asciicircum:
      return XKB_KEY_6;
    case XKB_KEY_ampersand:
      return XKB_KEY_7;
    case XKB_KEY_asterisk:
      return XKB_KEY_8;
    case XKB_KEY_parenleft:
      return XKB_KEY_9;
    default:
      return sym;
  }
}

const std::array<KeyBinding, 3> kUnfocusedGlobalBindings = {{
  {.fallbackLower = XKB_KEY_v, .fallbackUpper = XKB_KEY_V},
  {.fn = "screenshot", .fallbackLower = XKB_KEY_p, .fallbackUpper = XKB_KEY_P},
  {.fn = "toggle_cursor", .fallbackLower = XKB_KEY_f, .fallbackUpper = XKB_KEY_F},
}};

const std::array<KeyBinding, 11> kRegularWorldControlBindings = {{
  {.fallbackLower = XKB_KEY_b, .fallbackUpper = XKB_KEY_B},
  {.fallbackLower = XKB_KEY_t, .fallbackUpper = XKB_KEY_T},
  {.fn = "debug_mesh",
   .fallbackLower = XKB_KEY_comma,
   .fallbackUpper = XKB_KEY_comma},
  {.fallbackLower = XKB_KEY_m, .fallbackUpper = XKB_KEY_M},
  {.fallbackLower = XKB_KEY_slash, .fallbackUpper = XKB_KEY_question},
  {.fn = "select", .fallbackLower = XKB_KEY_e, .fallbackUpper = XKB_KEY_E},
  {.fallbackLower = XKB_KEY_period, .fallbackUpper = XKB_KEY_greater},
  {.fallbackLower = XKB_KEY_l, .fallbackUpper = XKB_KEY_L},
  {.fallbackLower = XKB_KEY_equal, .fallbackUpper = XKB_KEY_plus},
  {.fallbackLower = XKB_KEY_minus, .fallbackUpper = XKB_KEY_underscore},
  {.fallbackLower = XKB_KEY_Delete, .fallbackUpper = XKB_KEY_Delete},
}};

void
log_controls(const char* fmt, ...)
{
  std::ofstream out("/tmp/matrix-wlroots-output.log", std::ios::app);
  if (!out.is_open()) {
    return;
  }
  va_list args;
  va_start(args, fmt);
  char buf[256];
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  out << buf;
}
} // namespace

void
Controls::poll() {
  runQueuedActions();
  pollPressedKeys();
  handleKeys();
  doDeferedActions();
}

// wayland version
void Controls::handleKeys() {
  auto appFocused = wm->hasCurrentOrPendingFocus();
  if (appFocused != lastWaylandFocusActive) {
    resetWaylandInputState();
    lastWaylandFocusActive = appFocused;
  }

  // Order is important here.

  // First don't handle keys if they are disabled
  // (ie for some in game ui feature that isn't app, but has a textbox)
  if (!keysEnabled) {
    return;
  }

  // Second, handle 0-9 hotkeys, regardless of focus or not
  handleWaylandHotkeys();

  // Third, handle keys when the app is focused and return,
  // because no other keys should be handled when focused
  if (appFocused) {
    handleUnfocusApp();
    handleCloseFocusedApp();
    handleMoveFocusedApp();
    handleResizeFocusedApp();
    return;
  }

  // Finally, handle standard wm keys
  handleMovement();
  handleToggleApp();
  handleSpawnTerminal();
  handleReloadConfig();
  handleDMenu();
  handleToggleCursor();
  handleScreenshot();
  handleModEscape();
  handleSave();
  handleSelection();
  handleCodeBlock();
  handleDebug();
  handleToggleMeshing();
  handleToggleWireframe();
  handleLogBlockCounts();
  handleLogBlockType();
  handleWindowFlop();
  handleChangePlayerSpeed();
}

void
Controls::disableKeys()
{
  keysEnabled = false;
}
void
Controls::enableKeys()
{
  keysEnabled = true;
}

double DEBOUNCE_TIME = 0.1;
bool
debounce(double& lastTime)
{
  double curTime = nowSeconds();
  double interval = curTime - lastTime;
  lastTime = curTime;
  return interval > DEBOUNCE_TIME;
}

void
Controls::handleDMenu()
{
  bool dMenuActive = isPressedEither(XKB_KEY_v, XKB_KEY_V);
  if (dMenuActive && debounce(lastKeyPressTime)) {
    if (wm) {
      log_controls("controls: menu\n");
      wm->menu();
    }
  }
}

void
Controls::handleLogBlockType()
{
  bool shouldDebug = isPressedEither(XKB_KEY_t, XKB_KEY_T);
  if (shouldDebug && debounce(lastKeyPressTime)) {
    world->action(LOG_BLOCK_TYPE);
    log_controls("controls: logBlockType\n");
  }
}

void
Controls::handleLogBlockCounts()
{
  bool shouldDebug = isPressedEither(XKB_KEY_b, XKB_KEY_B);
  if (shouldDebug && debounce(lastKeyPressTime)) {
    texturePack->logCounts();
    log_controls("controls: logCounts\n");
  }
}

void
Controls::handleDebug()
{
  bool shouldDebug = is_configured_or_fallback_pressed(
    controlMappings, pressed, "debug_mesh", XKB_KEY_comma, XKB_KEY_comma);
  if (shouldDebug && debounce(lastKeyPressTime)) {
    world->mesh();
    log_controls("controls: mesh\n");
  }
}

void
Controls::handleToggleMeshing()
{
  bool shouldToggleMeshing = isPressedEither(XKB_KEY_m, XKB_KEY_M);
  if (shouldToggleMeshing && debounce(lastKeyPressTime)) {
    world->mesh(false);
    log_controls("controls: mesh=false\n");
  }
}

void
Controls::handleToggleWireframe()
{
  bool shouldToggleWireframe =
    isPressed(XKB_KEY_slash) || isPressed(XKB_KEY_question);
  if (shouldToggleWireframe && debounce(lastKeyPressTime)) {
    renderer->toggleWireframe();
    log_controls("controls: wireframe toggle\n");
  }
}

void
Controls::handleSelection()
{
  bool shouldSelect = is_configured_or_fallback_pressed(
    controlMappings, pressed, "select", XKB_KEY_e, XKB_KEY_E);
  if (shouldSelect && debounce(lastKeyPressTime)) {
    // world->action(SELECT_CUBE);
  }
}

void
Controls::handleCodeBlock()
{
  bool shouldOpenCodeBlock = isPressed(XKB_KEY_period) || isPressed(XKB_KEY_greater);
  if (shouldOpenCodeBlock && debounce(lastKeyPressTime)) {
    auto newPos = camera->position + camera->front * -3.0f;
    log_controls("controls: code block move distance=3.0\n");
    moveTo(newPos, std::nullopt, 0.5, [this]() -> void {
      world->action(OPEN_SELECTION_CODE);
    });
  }
}

void
Controls::handleSave()
{
  bool shouldSave = isPressedEither(XKB_KEY_l, XKB_KEY_L);
  if (shouldSave && debounce(lastKeyPressTime)) {
    auto t = std::time(nullptr);
    auto tm = *std::localtime(&t);
    stringstream filenameSS;
    filenameSS << "saves/" << std::put_time(&tm, "%Y-%m-%d:%H-%M-%S.save");
    world->save(filenameSS.str());
    log_controls("controls: save %s\n", filenameSS.str().c_str());
  }
}

void
Controls::handleScreenshot()
{
  int screenshotKey = controlMappings.getKey("screenshot");
  bool shouldCapture = screenshotKey != -1 && isPressed(static_cast<xkb_keysym_t>(screenshotKey));
  if (shouldCapture && debounce(lastKeyPressTime)) {
    log_controls("controls: screenshot\n");
    triggerScreenshot();
  }
}

void
Controls::triggerScreenshot()
{
  wm->requestScreenshot();
}

void
Controls::handleFocus()
{
  if (pressed.count(XKB_KEY_r) > 0 && debounce(lastKeyPressTime)) {
    if (auto looked = windowManagerSpace->getLookedAtApp()) {
      goToApp(*looked);
    }
  }
}

void
Controls::handleMovement()
{
  bool zPlus = pressed.count(XKB_KEY_e) > 0;
  bool zNegative = pressed.count(XKB_KEY_e) > 0;
  bool up = is_configured_or_fallback_pressed(
    controlMappings, pressed, "move_forward", XKB_KEY_w, XKB_KEY_W);
  bool down = is_configured_or_fallback_pressed(
    controlMappings, pressed, "move_back", XKB_KEY_s, XKB_KEY_S);
  bool left = is_configured_or_fallback_pressed(
    controlMappings, pressed, "move_left", XKB_KEY_a, XKB_KEY_A);
  bool right = is_configured_or_fallback_pressed(
    controlMappings, pressed, "move_right", XKB_KEY_d, XKB_KEY_D);
  // I need to change this to get zPlus and zNegative working
  camera->handleTranslateForce(up, down, left, right, zPlus, zNegative);
}

void
Controls::handleModEscape()
{
  if (isPressed(XKB_KEY_Delete)) {
    throw "errorEscape";
  }
}

void
Controls::focusRemoteApp(entt::entity app)
{
  if (!wm || !windowManagerSpace || !registry || !registry->valid(app) ||
      !registry->all_of<Positionable>(app) ||
      !registry->all_of<MultiPlayer::RemoteApp::Component>(app) || !camera) {
    return;
  }
  log_controls("controls: focusRemoteApp ent=%d\n",
               (int)entt::to_integral(app));
  const float deltaZ = windowManagerSpace->getViewDistanceForWindowSize(app);
  const glm::vec3 rotationDegrees = windowManagerSpace->getAppRotation(app);
  const glm::quat rotationQuat = glm::quat(glm::radians(rotationDegrees));
  glm::vec3 targetPosition = windowManagerSpace->getAppPosition(app);
  targetPosition =
    targetPosition + rotationQuat * glm::vec3(0, 0, deltaZ);

  grabbedCursor = false;
  resetMouse = true;
  if (wm) {
    wm->setCursorVisible(true);
    wm->clearCursorInputFocus();
  }
  const glm::vec3 front = rotationQuat * glm::vec3(0, 0, -1);
  auto isDone = camera->moveTo(targetPosition, front, windowFlop);
  doAfter(isDone, [this]() {
    // Keep cursor mode: the view stays locked on the shared window and mouse
    // motion is translated to remote pointer motion.
    grabbedCursor = false;
    resetMouse = true;
    if (wm) {
      wm->setCursorVisible(true);
      wm->clearCursorInputFocus();
    }
    if (pointerCenterCallback) {
      pointerCenterCallback();
    }
  });
}

void
Controls::returnToFreeLook()
{
  log_controls("controls: returnToFreeLook\n");
  grabbedCursor = true;
  resetMouse = true;
  if (wm) {
    wm->setCursorVisible(false);
    wm->clearCursorInputFocus();
  }
}

void
Controls::moveTo(glm::vec3 pos,
                 optional<glm::vec3> rotation,
                 float secs,
                 optional<function<void()>> callback)
{
  grabbedCursor = false;
  resetMouse = true;

  glm::vec3 front;
  // convert front to a rotated vector
  if (rotation.has_value()) {
    glm::quat rotationQuat = glm::quat(glm::radians(rotation.value()));
    front = rotationQuat * glm::vec3(0, 0, -1);
  }

  auto frontOrCamera = rotation.has_value() ? front : camera->front;
  shared_ptr<bool> isDone = camera->moveTo(pos, frontOrCamera, secs);
  doAfter(isDone, [this, callback]() -> void {
    if (callback.has_value()) {
      callback.value()();
    }
    grabbedCursor = true;
  });
}

//TODO: remove the shift detect fallback
void
Controls::handleWindowFlop()
{
  if (isPressed(XKB_KEY_0) &&
      !(waylandShiftHeld || (nowSeconds() - lastShiftPressTime) < 0.25)) {
    windowFlop += windowFlop_dt;
  }
  if (isPressed(XKB_KEY_9)) {
    windowFlop -= windowFlop_dt;
  }
  if (windowFlop <= 0.01) {
    windowFlop = 0.01;
  }
}

void
Controls::handleChangePlayerSpeed()
{
  auto delta = 0.05f;
  if ((isPressed(XKB_KEY_equal) || isPressed(XKB_KEY_plus)) &&
      debounce(lastKeyPressTime)) {
    camera->changeSpeed(delta);
    log_controls("controls: camera speed delta=+0.05\n");
  }
  if ((isPressed(XKB_KEY_minus) || isPressed(XKB_KEY_underscore)) &&
      debounce(lastKeyPressTime)) {
    camera->changeSpeed(-delta);
    log_controls("controls: camera speed delta=-0.05\n");
  }

  bool zeroPressed = isPressed(XKB_KEY_0);
  if (zeroPressed &&
      (waylandShiftHeld || (nowSeconds() - lastShiftPressTime) < 0.25) &&
      debounce(lastKeyPressTime)) {
    camera->resetSpeed();
    log_controls("controls: camera speed reset\n");
  }
}

void
Controls::goToApp(entt::entity app)
{
  if (!wm || !windowManagerSpace || !registry || !registry->valid(app) ||
      !registry->all_of<Positionable>(app)) {
    return;
  }
  float deltaZ = windowManagerSpace->getViewDistanceForWindowSize(app);
  glm::vec3 rotationDegrees = windowManagerSpace->getAppRotation(app);
  glm::quat rotationQuat = glm::quat(glm::radians(rotationDegrees));

  glm::vec3 targetPosition = windowManagerSpace->getAppPosition(app);
  targetPosition = targetPosition + rotationQuat * glm::vec3(0, 0, deltaZ);
  moveTo(targetPosition, rotationDegrees, windowFlop, [app, this]() {
    wm->focusApp(app);
  });
}

void
Controls::doAfter(shared_ptr<bool> isDone, function<void()> actionFn)
{
  DeferedAction action;
  action.isDone = isDone;
  action.fn = actionFn;
  deferedActions.push_back(action);
}

void
Controls::doDeferedActions()
{
  vector<vector<DeferedAction>::iterator> toDelete;
  for (auto it = deferedActions.begin(); it != deferedActions.end(); it++) {
    if (*it->isDone) {
      it->fn();
      toDelete.push_back(it);
    }
  }
  for (auto it : toDelete) {
    deferedActions.erase(it);
  }
}

void
Controls::handleToggleCursor()
{
  auto toggleCursorKey = controlMappings.getKey("toggle_cursor");
  bool toggleCursorPressed =
    (toggleCursorKey != -1 &&
     isPressed(static_cast<xkb_keysym_t>(toggleCursorKey))) ||
    isPressedEither(XKB_KEY_f, XKB_KEY_F);
  bool toggleCursorJustPressed = toggleCursorPressed && !toggleCursorHeld;
  toggleCursorHeld = toggleCursorPressed;
  if (toggleCursorJustPressed && debounce(lastKeyPressTime)) {
    if (grabbedCursor) {
      grabbedCursor = false;
      resetMouse = true;
      if (wm) {
        wm->setCursorVisible(true);
      }
      log_controls("controls: toggle_cursor=0\n");
    } else {
      grabbedCursor = true;
      resetMouse = true;
      if (wm) {
        wm->setCursorVisible(false);
        // Drop any hover-only focus from cursor mode so the crosshair controls
        // input immediately and the window does not keep keyboard focus.
        wm->clearCursorInputFocus();
      }
      log_controls("controls: toggle_cursor=1\n");
    }
  }
}

void
Controls::wireWindowManager(std::shared_ptr<WindowManager::Space> space)
{
  windowManagerSpace = space;
}

bool
Controls::handlePointerButton(uint32_t button, bool pressed)
{
  if (!pressed) {
    return false;
  }
  if (!debounce(lastClickTime)) {
    return false;
  }
  if (!grabbedCursor) {
    return false;
  }
  // In free look the crosshair delivers clicks straight to the window under
  // it; only the world (no window hit) uses the voxel action. Heading towards a
  // window must not grab/lock it: press R for that.
  if (windowManagerSpace && windowManagerSpace->getLookedAtApp().has_value()) {
    return false;
  }
  if (button == static_cast<uint32_t>(BTN_LEFT)) {
    world->action(PLACE_VOXEL);
    return true;
  }
  // Right click is reserved for window interaction; it has no world action.
  return false;
}

void Controls::pollPressedKeys() {
  // Dispatch due synthetic keys first. handleKeySym() locks keysymMutex, so it
  // must run outside the lock we hold while draining the queue.
  std::vector<ScheduledKeyEvent> due;
  {
    std::lock_guard<std::mutex> lock(keysymMutex);
    const double now = nowSeconds();
    for (auto it = scheduledKeyEvents.begin(); it != scheduledKeyEvents.end();) {
      if (it->whenSeconds <= now) {
        due.push_back(*it);
        it = scheduledKeyEvents.erase(it);
      } else {
        ++it;
      }
    }
  }
  for (const auto& scheduled : due) {
    handleKeySym(scheduled.sym,
                 scheduled.pressed,
                 scheduled.modifierHeld,
                 scheduled.shiftHeld);
  }

  std::lock_guard<std::mutex> lock(keysymMutex);
  KeysymEvent keysymEvent;
  while(keysymQueue.size() > 0) {
    keysymEvent = keysymQueue.front();
    waylandModifierHeld = keysymEvent.modifierHeld;
    waylandShiftHeld = keysymEvent.shiftHeld;
    if (waylandShiftHeld) {
      lastShiftPressTime = nowSeconds();
    }
    if(keysymEvent.pressed) {
      pressed.insert(keysymEvent.sym); 
    } else {
      pressed.erase(keysymEvent.sym);
    }
    keysymQueue.pop();
  }
}

void Controls::scheduleKeySym(xkb_keysym_t sym,
                              bool pressed,
                              bool modifierHeld,
                              bool shiftHeld,
                              double delaySeconds)
{
  std::lock_guard<std::mutex> lock(keysymMutex);
  scheduledKeyEvents.push_back(ScheduledKeyEvent{
    nowSeconds() + delaySeconds, sym, pressed, modifierHeld, shiftHeld });
}


// called by the wayland server when input is detected
ControlResponse
Controls::handleKeySym(xkb_keysym_t sym,
                       bool is_pressed,
                       bool modifierHeld,
                       bool shiftHeld)
{
  {
    std::lock_guard<std::mutex> lock(keysymMutex);
    keysymQueue.push(KeysymEvent{sym, is_pressed, modifierHeld, shiftHeld});
  }

  if (sym == XKB_KEY_Shift_L || sym == XKB_KEY_Shift_R || shiftHeld) {
    lastShiftPressTime = nowSeconds();
  }

  ControlResponse key_not_handled_by_wm;
  const bool waylandFocusActive = wm->hasCurrentOrPendingFocus();
  const bool superKey = is_super_keysym(sym);
  const bool unfocusShortcut =
    modifierHeld &&
    matches_configured_or_fallback_key(
      controlMappings, sym, "unfocus_app", XKB_KEY_e, XKB_KEY_E);

  if (waylandFocusActive && unfocusShortcut) {
    if (is_pressed && debounce(lastKeyPressTime) && wm) {
      const bool hadPerpendicularFocus = wm->getCurrentlyFocusedApp().has_value();
      wm->unfocusApp();
      if (!hadPerpendicularFocus) {
        grabbedCursor = true;
        resetMouse = true;
        wm->setCursorVisible(false);
      }
    }
    if (is_pressed && typedKeyOverlay) {
      typedKeyOverlay->recordKeysym(sym);
    }
    return key_handled_by_wm();
  }

  if (waylandFocusActive) {
    if (superKey) {
      if (is_pressed && typedKeyOverlay) {
        typedKeyOverlay->recordKeysym(sym);
      }
      return key_handled_by_wm();
    }
    if (!modifierHeld) {
      return key_not_handled_by_wm;
    }
  }

  if (matches_configured_or_fallback_key(
        controlMappings, sym, "quit", XKB_KEY_Escape, XKB_KEY_Escape)) {
    if (is_pressed && typedKeyOverlay) {
      typedKeyOverlay->recordKeysym(sym);
    }
    // quit key gets its own special consume function
    // because the caller must be notified of quit flag
    return quit_key_handled_by_wm();
  }

  if (is_pressed && typedKeyOverlay) {
    typedKeyOverlay->recordKeysym(sym);
  }
  return key_handled_by_wm();
}

bool
Controls::matchesConfiguredKey(xkb_keysym_t sym, const std::string fn)
{
  int mapped = controlMappings.getKey(fn);
  return mapped != -1 && sym == static_cast<xkb_keysym_t>(mapped);
}

void
Controls::enqueueAction(std::function<void()> fn)
{
  std::lock_guard<std::mutex> lock(queuedActionsMutex);
  queuedActions.push_back(std::move(fn));
}

void
Controls::runQueuedActions()
{
  std::vector<std::function<void()>> actions;
  {
    std::lock_guard<std::mutex> lock(queuedActionsMutex);
    actions.swap(queuedActions);
  }
  for (auto& fn : actions) {
    if (fn) {
      fn();
    }
  }
}

void
Controls::applyMovementInput(bool forward, bool back, bool left, bool right)
{
  if (camera) {
    camera->handleTranslateForce(forward, back, left, right, false, false);
  }
}

void
Controls::clearMovementInput()
{
  applyMovementInput(false, false, false, false);
}

void
Controls::resetWaylandInputState()
{
  std::lock_guard<std::mutex> lock(keysymMutex);
  pressed.clear();
  scheduledKeyEvents.clear();
  while (!keysymQueue.empty()) {
    keysymQueue.pop();
  }
  waylandModifierHeld = false;
  waylandShiftHeld = false;
}

void
Controls::applyLookDelta(double dx, double dy)
{
  if (renderer && renderer->getCamera()) {
    renderer->getCamera()->handleRotateForce(dx, dy);
  }
}

bool
Controls::isPressed(xkb_keysym_t sym) const
{
  return pressed.count(sym) > 0;
}

bool
Controls::isPressedEither(xkb_keysym_t a, xkb_keysym_t b) const
{
  return isPressed(a) || isPressed(b);
}

void
Controls::handleToggleApp()
{
  if (!isPressedEither(XKB_KEY_r, XKB_KEY_R)) {
    return;
  }
  auto app = windowManagerSpace ? windowManagerSpace->getLookedAtApp()
                                : std::optional<entt::entity>();
  auto remote = !app.has_value() && windowManagerSpace
                  ? windowManagerSpace->getLookedAtRemoteApp()
                  : std::optional<entt::entity>();
  if ((!app.has_value() && !remote.has_value()) ||
      !debounce(lastKeyPressTime)) {
    return;
  }
  if (app.has_value()) {
    goToApp(app.value());
  } else {
    focusRemoteApp(remote.value());
  }
}

void
Controls::handleSpawnTerminal()
{
  if (!waylandModifierHeld || !wm) {
    return;
  }
  bool shouldSpawn =
    isPressed(XKB_KEY_Return) || isPressed(XKB_KEY_KP_Enter);
  if (!shouldSpawn || !debounce(lastKeyPressTime)) {
    return;
  }
  wm->launchTerminal();
}

void
Controls::handleReloadConfig()
{
  bool shouldReload = is_configured_or_fallback_pressed(
    controlMappings, pressed, "reload_config", XKB_KEY_y, XKB_KEY_Y);
  if (!shouldReload || !debounce(lastKeyPressTime)) {
    return;
  }

  Config::singleton()->reload();
  controlMappings = ControlMappings();
}

bool
Controls::handleUnfocusApp()
{
  if (!waylandModifierHeld || !wm) {
    return false;
  }
  if (!is_configured_or_fallback_pressed(
        controlMappings, pressed, "unfocus_app", XKB_KEY_e, XKB_KEY_E)) {
    return false;
  }
  if (!debounce(lastKeyPressTime)) {
    return true;
  }
  wm->unfocusApp();
  return true;
}

bool
Controls::handleCloseFocusedApp()
{
  if (!waylandModifierHeld || !wm) {
    return false;
  }
  bool shouldClose = is_configured_or_fallback_pressed(
    controlMappings, pressed, "close_app", XKB_KEY_q, XKB_KEY_Q);
  if (!shouldClose) {
    return false;
  }
  if (!debounce(lastKeyPressTime)) {
    return true;
  }

  auto focusedAppCandidate = [&]() -> std::optional<entt::entity> {
    if (auto pending = wm->getPendingFocusedApp()) {
      return pending;
    }
    return wm->getCurrentlyFocusedApp();
  };

  if (auto ent = focusedAppCandidate()) {
    if (auto* comp = registry->try_get<WaylandApp::Component>(*ent)) {
      if (comp->app) {
        comp->app->close();
      }
    }
  }
  wm->unfocusApp();
  return true;
}

bool
Controls::handleMoveFocusedApp()
{
  if (!waylandModifierHeld || !wm || !camera) {
    return false;
  }

  glm::vec3 direction(0.0f);
  if (pressed.count(XKB_KEY_Left) > 0) {
    direction.x -= 1.0f;
  }
  if (pressed.count(XKB_KEY_Right) > 0) {
    direction.x += 1.0f;
  }
  if (pressed.count(XKB_KEY_Up) > 0) {
    direction.y += 1.0f;
  }
  if (pressed.count(XKB_KEY_Down) > 0) {
    direction.y -= 1.0f;
  }
  if (direction == glm::vec3(0.0f)) {
    return false;
  }
  if (!debounce(lastKeyPressTime)) {
    return true;
  }

  // Move in the camera's screen plane so the arrows feel directional.
  constexpr float kStep = 0.1f;
  glm::vec3 front = glm::normalize(camera->front);
  glm::vec3 right = glm::cross(front, glm::vec3(0.0f, 1.0f, 0.0f));
  if (glm::length(right) < 1e-4f) {
    right = glm::vec3(1.0f, 0.0f, 0.0f);
  }
  right = glm::normalize(right);
  const glm::vec3 up = glm::normalize(glm::cross(right, front));
  wm->moveFocusedApp((right * direction.x + up * direction.y) * kStep);
  return true;
}

bool
Controls::handleResizeFocusedApp()
{
  if (!waylandModifierHeld || !wm) {
    return false;
  }

  float factor = 1.0f;
  if (pressed.count(XKB_KEY_bracketright) > 0 ||
      pressed.count(XKB_KEY_equal) > 0) {
    factor = 1.05f;
  } else if (pressed.count(XKB_KEY_bracketleft) > 0 ||
             pressed.count(XKB_KEY_minus) > 0) {
    factor = 0.95f;
  }
  if (factor == 1.0f) {
    return false;
  }
  if (!debounce(lastKeyPressTime)) {
    return true;
  }
  wm->resizeFocusedApp(factor);
  return true;
}

bool
Controls::handleWaylandHotkeys()
{
  if (!waylandModifierHeld || !wm) {
    return false;
  }

  for (int i = 0; i < 9; ++i) {
    xkb_keysym_t digit = static_cast<xkb_keysym_t>(XKB_KEY_1 + i);
    xkb_keysym_t shifted = normalize_hotkey_sym(digit);
    switch (digit) {
      case XKB_KEY_1:
        shifted = XKB_KEY_exclam;
        break;
      case XKB_KEY_2:
        shifted = XKB_KEY_at;
        break;
      case XKB_KEY_3:
        shifted = XKB_KEY_numbersign;
        break;
      case XKB_KEY_4:
        shifted = XKB_KEY_dollar;
        break;
      case XKB_KEY_5:
        shifted = XKB_KEY_percent;
        break;
      case XKB_KEY_6:
        shifted = XKB_KEY_asciicircum;
        break;
      case XKB_KEY_7:
        shifted = XKB_KEY_ampersand;
        break;
      case XKB_KEY_8:
        shifted = XKB_KEY_asterisk;
        break;
      case XKB_KEY_9:
        shifted = XKB_KEY_parenleft;
        break;
      default:
        break;
    }
    if (!isPressed(digit) && !isPressed(shifted)) {
      continue;
    }
    if (!debounce(lastKeyPressTime)) {
      return true;
    }
    log_controls("number hotkey: %d", i);
    if (waylandShiftHeld) {
      if (wm->getCurrentlyFocusedApp().has_value()) {
        int source = wm->findAppsHotKey(wm->getCurrentlyFocusedApp().value());
        wm->swapHotKeys(source, i);
      }
    } else if (auto ent = wm->getHotkeyTarget(i)) {
      wm->unfocusApp();
      goToApp(*ent);
    }
    return true;
  }

  return false;
}
