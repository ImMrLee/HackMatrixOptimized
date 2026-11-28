#include "world.h"
#include <glad/glad.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <optional>

#include <iostream>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <cctype>
#include <cstdint>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
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
#include "screen.h"

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
    // A window gained focus (e.g. via a click); stop any free-view manipulation
    // so a stale drag cannot keep owning the mouse.
    if (windowMoveEntity != entt::null) {
      endWindowMove();
    }
    if (windowResizeEntity != entt::null) {
      endWindowResize();
    }
    handleUnfocusApp();
    handleCloseFocusedApp();
    handleMoveFocusedApp();
    handleResizeFocusedApp();
    return;
  }

  // Finally, handle standard wm keys
  handleMovement();
  handleToggleApp();
  handleWindowMoveKey();
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
  bool shouldDebug = isPressedEither(XKB_KEY_y, XKB_KEY_Y);
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

bool
Controls::cameraScreenAxes(glm::vec3& right, glm::vec3& up) const
{
  if (!camera) {
    return false;
  }
  glm::vec3 front = glm::normalize(camera->front);
  const glm::vec3 worldUp(0.0f, 1.0f, 0.0f);
  right = glm::cross(front, worldUp);
  if (glm::length(right) < 1e-4f) {
    right = glm::vec3(1.0f, 0.0f, 0.0f);
  } else {
    right = glm::normalize(right);
  }
  up = glm::cross(right, front);
  if (glm::length(up) < 1e-4f) {
    up = glm::vec3(0.0f, 1.0f, 0.0f);
  } else {
    up = glm::normalize(up);
  }
  return true;
}

float
Controls::worldPerPixelAt(float distance) const
{
  if (!camera || SCREEN_HEIGHT <= 0.0f) {
    return 0.0f;
  }
  const glm::mat4 proj = camera->getProjectionMatrix(true);
  // invertY flips projection[1][1], so use its magnitude: world units per
  // screen pixel at `distance` is 2 * distance / (|p11| * screenHeight).
  const float p11 = std::fabs(proj[1][1]);
  if (p11 < 1e-6f) {
    return 0.0f;
  }
  return (2.0f * distance) / (p11 * SCREEN_HEIGHT);
}

std::optional<WindowManager::AppRayHit>
Controls::windowAimHit()
{
  if (!windowManagerSpace) {
    return std::nullopt;
  }
  // Free view (cursor grabbed) aims with the screen-centre crosshair; a visible
  // cursor aims with the real pointer position.
  if (grabbedCursor) {
    return windowManagerSpace->getLookedAtAppHit();
  }
  return windowManagerSpace->raycastAppFromScreen(
    pointerScreenX, pointerScreenY, SCREEN_WIDTH, SCREEN_HEIGHT, 10000.0f);
}

void
Controls::handleWindowMoveKey()
{
  const bool held = isPressedEither(XKB_KEY_g, XKB_KEY_G);
  if (!held) {
    windowMoveNoHitLogged = false;
    debugResizeFired = false;
    if (windowMoveEntity != entt::null) {
      endWindowMove();
    }
    return;
  }
  if (std::getenv("HACKMATRIX_DEBUG_WINDOW_RESIZE")) {
    if (!debugResizeFired) {
      debugResizeFired = true;
      debugResizeOnce();
    }
    return;
  }
  if (windowMoveEntity != entt::null) {
    return;
  }
  if (!grabbedCursor && isPressedEither(XKB_KEY_g, XKB_KEY_G)) {
    // Cursor mode without a focused app can still move a window, but stay out
    // of the way when an app owns keyboard focus (handleKeys returns earlier).
  }
  auto hit = windowAimHit();
  if (!hit.has_value()) {
    if (!windowMoveNoHitLogged) {
      windowMoveNoHitLogged = true;
      log_controls(
        "controls: window move: G held but no window under aim "
        "(grabbed=%d space=%d pointer=%.1f,%.1f)\n",
        grabbedCursor ? 1 : 0,
        windowManagerSpace ? 1 : 0,
        static_cast<double>(pointerScreenX),
        static_cast<double>(pointerScreenY));
    }
    return;
  }
  beginWindowMove(hit->entity, hit->distance);
}

bool
Controls::beginWindowMove(entt::entity entity, float distance)
{
  if (!registry || !camera || entity == entt::null ||
      !registry->valid(entity)) {
    return false;
  }
  auto* comp = registry->try_get<WaylandApp::Component>(entity);
  if (!comp || !comp->app || comp->accessory || comp->layer_shell) {
    log_controls(
      "controls: window move rejected ent=%d comp=%d acc=%d layer=%d\n",
      (int)entt::to_integral(entity),
      comp ? 1 : 0,
      comp && comp->accessory ? 1 : 0,
      comp && comp->layer_shell ? 1 : 0);
    return false;
  }
  if (!registry->all_of<Positionable>(entity)) {
    log_controls("controls: window move rejected ent=%d (no Positionable)\n",
                 (int)entt::to_integral(entity));
    return false;
  }
  auto& positionable = registry->get<Positionable>(entity);
  windowMoveEntity = entity;
  windowMoveDistance =
    distance > 0.0f ? distance
                    : glm::length(positionable.pos - camera->position);
  log_controls("controls: window move begin ent=%d dist=%.3f pos=(%.3f,%.3f,%.3f) scale=%.3f\n",
               (int)entt::to_integral(entity),
               static_cast<double>(windowMoveDistance),
               static_cast<double>(positionable.pos.x),
               static_cast<double>(positionable.pos.y),
               static_cast<double>(positionable.pos.z),
               static_cast<double>(positionable.scale));
  return true;
}

void
Controls::updateHeldWindowPose()
{
  if (windowMoveEntity == entt::null || !registry || !camera) {
    return;
  }
  if (!registry->valid(windowMoveEntity)) {
    windowMoveEntity = entt::null;
    return;
  }
  auto* positionable = registry->try_get<Positionable>(windowMoveEntity);
  if (!positionable) {
    windowMoveEntity = entt::null;
    return;
  }
  // Reuse the spawn orientation: the window's local -Z maps to the camera's
  // look direction, so it always faces the camera. Recompute every frame so it
  // follows mouse-look and WASD player movement.
  const float yaw = camera->getYaw();
  const float pitch = camera->getPitch();
  const glm::quat yawRotation =
    glm::angleAxis(glm::radians(90.0f + yaw), glm::vec3(0.0f, -1.0f, 0.0f));
  const glm::quat pitchRotation =
    glm::angleAxis(glm::radians(pitch), glm::vec3(1.0f, 0.0f, 0.0f));
  const glm::quat finalRotation = yawRotation * pitchRotation;
  positionable->rotate = glm::degrees(glm::eulerAngles(finalRotation));
  positionable->pos =
    camera->position + finalRotation * glm::vec3(0.0f, 0.0f, -windowMoveDistance);
  // Apply immediately so this frame renders the carried pose.
  positionable->update();
}

void
Controls::endWindowMove()
{
  if (windowMoveEntity == entt::null) {
    return;
  }
  if (registry && registry->valid(windowMoveEntity)) {
    if (auto* positionable = registry->try_get<Positionable>(windowMoveEntity)) {
      log_controls(
        "controls: window move commit ent=%d pos=(%.3f,%.3f,%.3f) "
        "rot=(%.1f,%.1f,%.1f)\n",
        (int)entt::to_integral(windowMoveEntity),
        static_cast<double>(positionable->pos.x),
        static_cast<double>(positionable->pos.y),
        static_cast<double>(positionable->pos.z),
        static_cast<double>(positionable->rotate.x),
        static_cast<double>(positionable->rotate.y),
        static_cast<double>(positionable->rotate.z));
    }
  }
  windowMoveEntity = entt::null;
}

bool
Controls::beginWindowResize(const WindowManager::AppRayHit& hit)
{
  if (!registry || !camera || hit.entity == entt::null ||
      !registry->valid(hit.entity)) {
    return false;
  }
  entt::entity entity = hit.entity;
  auto* comp = registry->try_get<WaylandApp::Component>(entity);
  if (!comp || !comp->app || comp->accessory || comp->layer_shell) {
    return false;
  }
  auto* positionable = registry->try_get<Positionable>(entity);
  if (!positionable) {
    return false;
  }
  const int width = comp->app->getWidth();
  const int height = comp->app->getHeight();
  if (width <= 0 || height <= 0) {
    return false;
  }

  const float px = hit.surfacePixels.x;
  const float py = hit.surfacePixels.y;
  const float margin =
    std::max(6.0f, std::min(24.0f, 0.04f * std::min(width, height)));
  bool left = px <= margin;
  bool right = px >= static_cast<float>(width) - margin;
  bool top = py <= margin;
  bool bottom = py >= static_cast<float>(height) - margin;
  // A very small window can be within margin of both sides; pick the nearer.
  if (left && right) {
    if (px < static_cast<float>(width) * 0.5f) {
      right = false;
    } else {
      left = false;
    }
  }
  if (top && bottom) {
    if (py < static_cast<float>(height) * 0.5f) {
      bottom = false;
    } else {
      top = false;
    }
  }
  if (!left && !right && !top && !bottom) {
    return false;
  }

  const glm::quat rotation = glm::quat(glm::radians(positionable->rotate));
  resizeRightAxis = glm::normalize(rotation * glm::vec3(1.0f, 0.0f, 0.0f));
  resizeUpAxis = glm::normalize(rotation * glm::vec3(0.0f, 1.0f, 0.0f));
  // The in-world quad is 1.0 wide and (SCREEN_HEIGHT/SCREEN_WIDTH) tall before
  // heightScalar, so world width = scale and world height =
  // (SCREEN_HEIGHT/SCREEN_WIDTH) * scale * heightScalar.y.
  const float standardRatio = SCREEN_HEIGHT / SCREEN_WIDTH;
  resizeW0 = positionable->scale;
  resizeH0 = standardRatio * positionable->scale *
             comp->app->heightScalar[1][1];
  if (resizeH0 <= 0.0f || !std::isfinite(resizeH0)) {
    resizeH0 = resizeW0 * standardRatio;
  }
  resizePos0 = positionable->pos;
  resizePixelW0 = width;
  resizeAccumW = 0.0f;
  resizeAccumH = 0.0f;
  lastResizeRequestTime = 0.0;
  lastResizePixelW = -1;
  lastResizePixelH = -1;
  resizeDistance = glm::length(positionable->pos - camera->position);
  resizeLeft = left;
  resizeRight = right;
  resizeTop = top;
  resizeBottom = bottom;
  windowResizeEntity = entity;
  windowResizeDeltaLogged = false;
  log_controls(
    "controls: window resize begin ent=%d edges=%s%s%s%s %.0fx%.0f\n",
    (int)entt::to_integral(entity),
    left ? "L" : "",
    right ? "R" : "",
    top ? "T" : "",
    bottom ? "B" : "",
    static_cast<double>(width),
    static_cast<double>(height));
  return true;
}

bool
Controls::handleWindowResizeDelta(double dx, double dy)
{
  if (windowResizeEntity == entt::null || !registry || !camera) {
    return false;
  }
  if (!registry->valid(windowResizeEntity)) {
    windowResizeEntity = entt::null;
    return false;
  }
  auto* positionable = registry->try_get<Positionable>(windowResizeEntity);
  auto* comp = registry->try_get<WaylandApp::Component>(windowResizeEntity);
  if (!positionable || !comp || !comp->app) {
    windowResizeEntity = entt::null;
    return false;
  }
  glm::vec3 camRight;
  glm::vec3 camUp;
  if (!cameraScreenAxes(camRight, camUp)) {
    return false;
  }
  const float ppu = worldPerPixelAt(resizeDistance);
  if (!std::isfinite(ppu) || ppu <= 0.0f) {
    return false;
  }
  const glm::vec3 worldDelta = camRight * (static_cast<float>(dx) * ppu) +
                               camUp * (static_cast<float>(-dy) * ppu);
  const float dr = glm::dot(worldDelta, resizeRightAxis);
  const float du = glm::dot(worldDelta, resizeUpAxis);

  // Integrate the per-frame mouse delta. Recomputing the target from this
  // frame's delta alone would snap back to the original size every frame.
  if (resizeRight) {
    resizeAccumW += dr;
  }
  if (resizeLeft) {
    resizeAccumW -= dr;
  }
  if (resizeTop) {
    resizeAccumH += du;
  }
  if (resizeBottom) {
    resizeAccumH -= du;
  }
  constexpr float kMinWorld = 0.12f;
  constexpr float kMaxWorld = 60.0f;
  float newW = std::clamp(resizeW0 + resizeAccumW, kMinWorld, kMaxWorld);
  float newH = std::clamp(resizeH0 + resizeAccumH, kMinWorld, kMaxWorld);

  // Anchor the opposite edge: shift the centre by half the size delta.
  const float dW = newW - resizeW0;
  const float dH = newH - resizeH0;
  glm::vec3 shift(0.0f);
  if (resizeRight) {
    shift += resizeRightAxis * (dW * 0.5f);
  }
  if (resizeLeft) {
    shift -= resizeRightAxis * (dW * 0.5f);
  }
  if (resizeTop) {
    shift += resizeUpAxis * (dH * 0.5f);
  }
  if (resizeBottom) {
    shift -= resizeUpAxis * (dH * 0.5f);
  }
  positionable->pos = resizePos0 + shift;
  positionable->scale = newW;
  // World width is `scale`; world height is
  // (SCREEN_HEIGHT/SCREEN_WIDTH) * scale * heightScalar.y. Solve heightScalar.y
  // so the previewed world aspect equals newH/newW.
  const float standardRatio = SCREEN_HEIGHT / SCREEN_WIDTH;
  comp->app->heightScalar = glm::scale(
    glm::mat4(1.0f),
    glm::vec3(1.0f, newH / (standardRatio * newW), 1.0f));
  positionable->update();

  // Ask the client for the real surface size while dragging, so this is an
  // actual resize instead of only a stretched texture. Throttled to ~20 Hz and
  // only when the target size changed.
  const double now = nowSeconds();
  if (now - lastResizeRequestTime >= 0.05) {
    const float worldScaleX = newW / std::max(1e-3f, resizeW0);
    int pixelW = static_cast<int>(
      std::lround(static_cast<float>(resizePixelW0) * worldScaleX));
    int pixelH = static_cast<int>(
      std::lround(static_cast<float>(pixelW) * (newH / newW)));
    pixelW = std::clamp(pixelW, 64, 8192);
    pixelH = std::clamp(pixelH, 64, 8192);
    if (pixelW != lastResizePixelW || pixelH != lastResizePixelH) {
      lastResizeRequestTime = now;
      lastResizePixelW = pixelW;
      lastResizePixelH = pixelH;
      comp->app->requestSize(pixelW, pixelH);
    }
  }

  if (!windowResizeDeltaLogged) {
    windowResizeDeltaLogged = true;
    log_controls(
      "controls: window resize delta dx=%.3f dy=%.3f W0=%.3f H0=%.3f "
      "newW=%.3f newH=%.3f\n",
      dx,
      dy,
      static_cast<double>(resizeW0),
      static_cast<double>(resizeH0),
      static_cast<double>(newW),
      static_cast<double>(newH));
  }
  return true;
}

void
Controls::endWindowResize()
{
  if (windowResizeEntity == entt::null) {
    return;
  }
  entt::entity entity = windowResizeEntity;
  windowResizeEntity = entt::null;
  if (!registry || !registry->valid(entity)) {
    return;
  }
  auto* positionable = registry->try_get<Positionable>(entity);
  auto* comp = registry->try_get<WaylandApp::Component>(entity);
  if (!positionable || !comp || !comp->app) {
    return;
  }
  const float standardRatio = SCREEN_HEIGHT / SCREEN_WIDTH;
  const float worldW = std::max(1e-3f, positionable->scale);
  // World aspect (height / width) = standardRatio * heightScalar.y, which is
  // also the pixel aspect the client should be configured to.
  const float worldAspect = std::clamp(
    standardRatio * comp->app->heightScalar[1][1], 0.05f, 20.0f);
  const float worldScaleX = worldW / std::max(1e-3f, resizeW0);
  int pixelW = static_cast<int>(
    std::lround(static_cast<float>(resizePixelW0) * worldScaleX));
  int pixelH = static_cast<int>(
    std::lround(static_cast<float>(pixelW) * worldAspect));
  pixelW = std::clamp(pixelW, 64, 8192);
  pixelH = std::clamp(pixelH, 64, 8192);
  // Update local geometry now; ask the client for the real configure. The
  // committed surface size refreshes heightScalar again on the next commit.
  comp->app->resize(pixelW, pixelH);
  comp->app->requestSize(pixelW, pixelH);
  positionable->damage();
  log_controls("controls: window resize commit ent=%d -> %dx%d world=%.3f\n",
               (int)entt::to_integral(entity),
               pixelW,
               pixelH,
               static_cast<double>(worldW));
}

bool
Controls::debugResizeOnce()
{
  if (!registry || !windowManagerSpace) {
    return false;
  }
  const char* spec = std::getenv("HACKMATRIX_DEBUG_WINDOW_RESIZE");
  if (!spec) {
    return false;
  }
  auto base = windowAimHit();
  if (!base.has_value()) {
    log_controls("controls: debug resize: no window under aim\n");
    return false;
  }
  auto* comp = registry->try_get<WaylandApp::Component>(base->entity);
  if (!comp || !comp->app) {
    return false;
  }
  const int width = comp->app->getWidth();
  const int height = comp->app->getHeight();
  if (width <= 0 || height <= 0) {
    return false;
  }
  char edges[8] = { 0 };
  double dx = 0.0;
  double dy = 0.0;
  if (std::sscanf(spec, "%7[^,],%lf,%lf", edges, &dx, &dy) < 1) {
    return false;
  }
  bool left = false;
  bool right = false;
  bool top = false;
  bool bottom = false;
  for (const char* c = edges; *c; ++c) {
    switch (std::toupper(static_cast<unsigned char>(*c))) {
      case 'L':
        left = true;
        break;
      case 'R':
        right = true;
        break;
      case 'T':
        top = true;
        break;
      case 'B':
        bottom = true;
        break;
      default:
        break;
    }
  }
  WindowManager::AppRayHit hit = *base;
  const float margin =
    std::max(6.0f, std::min(24.0f, 0.04f * std::min(width, height))) * 0.5f;
  hit.surfacePixels.x =
    right ? static_cast<float>(width) - margin
          : (left ? margin : static_cast<float>(width) * 0.5f);
  hit.surfacePixels.y =
    bottom ? static_cast<float>(height) - margin
           : (top ? margin : static_cast<float>(height) * 0.5f);
  if (!beginWindowResize(hit)) {
    return false;
  }
  handleWindowResizeDelta(dx, dy);
  endWindowResize();
  return true;
}

entt::entity
Controls::windowPreviewEntity() const
{
  return windowResizeEntity != entt::null ? windowResizeEntity
                                          : windowMoveEntity;
}

unsigned
Controls::windowResizeHoverMask()
{
  if (!grabbedCursor || !windowManagerSpace || !registry) {
    return 0;
  }
  auto hit = windowManagerSpace->getLookedAtAppHit();
  if (!hit.has_value()) {
    return 0;
  }
  auto* comp = registry->try_get<WaylandApp::Component>(hit->entity);
  if (!comp || !comp->app || comp->accessory || comp->layer_shell) {
    return 0;
  }
  const int width = comp->app->getWidth();
  const int height = comp->app->getHeight();
  if (width <= 0 || height <= 0) {
    return 0;
  }
  const float px = hit->surfacePixels.x;
  const float py = hit->surfacePixels.y;
  const float margin =
    std::max(6.0f, std::min(24.0f, 0.04f * std::min(width, height)));
  unsigned mask = 0;
  if (px <= margin) {
    mask |= 1u;
  }
  if (px >= static_cast<float>(width) - margin) {
    mask |= 2u;
  }
  if (py <= margin) {
    mask |= 4u;
  }
  if (py >= static_cast<float>(height) - margin) {
    mask |= 8u;
  }
  if ((mask & 3u) == 3u) {
    mask &= (px < static_cast<float>(width) * 0.5f) ? ~2u : ~1u;
  }
  if ((mask & 12u) == 12u) {
    mask &= (py < static_cast<float>(height) * 0.5f) ? ~8u : ~4u;
  }
  return mask;
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
    // Quit only on the initial press: the release must be consumed too, but it
    // must not quit after the chat closed on the press (or double-trigger).
    if (is_pressed) {
      if (keysEnabled && typedKeyOverlay) {
        typedKeyOverlay->recordKeysym(sym);
      }
      if (keysEnabled) {
        exitConfirmPending_ = true;
      }
    }
    return key_handled_by_wm();
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
Controls::takeExitConfirmRequest()
{
  const bool pending = exitConfirmPending_;
  exitConfirmPending_ = false;
  return pending;
}

void
Controls::beginPointerMode(bool centerPointer)
{
  if (pointerModeRefs_++ == 0) {
    pointerModeRestoreFreeLook_ = grabbedCursor;
    log_controls("controls: pointer mode begin (restoreFreeLook=%d)\n",
                 pointerModeRestoreFreeLook_ ? 1 : 0);
    if (pointerModeRestoreFreeLook_) {
      grabbedCursor = false;
      resetMouse = true;
      if (wm) {
        wm->setCursorVisible(true);
        wm->clearCursorInputFocus();
      }
    }
  }
  if (centerPointer && pointerCenterCallback) {
    pointerCenterCallback();
  }
}

void
Controls::endPointerMode()
{
  if (pointerModeRefs_ <= 0) {
    return;
  }
  if (--pointerModeRefs_ > 0) {
    return;
  }
  log_controls("controls: pointer mode end (restoreFreeLook=%d)\n",
               pointerModeRestoreFreeLook_ ? 1 : 0);
  if (!pointerModeRestoreFreeLook_) {
    return;
  }
  pointerModeRestoreFreeLook_ = false;
  grabbedCursor = true;
  resetMouse = true;
  if (wm) {
    wm->setCursorVisible(false);
    wm->clearCursorInputFocus();
  }
}

void
Controls::beginExitDialog()
{
  // Put the cursor over the centred dialog instead of wherever it was hiding.
  beginPointerMode(true);
  log_controls("controls: exit dialog begin\n");
}

void
Controls::endExitDialog()
{
  log_controls("controls: exit dialog end\n");
  endPointerMode();
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
