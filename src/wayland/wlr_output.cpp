#define WLR_USE_UNSTABLE 1

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <utility>
#include <EGL/egl.h>
#include <xkbcommon/xkbcommon.h>
#include <wayland-server-core.h>

// wlroots is a C library: include its headers with C linkage.
extern "C" {
#include <wlr/backend.h>
#include <wlr/backend/wayland.h>
#include <wlr/backend/x11.h>
#include <wlr/render/allocator.h>
#include <wlr/render/gles2.h>
#include <wlr/render/swapchain.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_keyboard.h>
#define namespace namespace_
#include <wlr/types/wlr_layer_shell_v1.h>
#undef namespace
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/util/log.h>
#include <xdg-shell-client-protocol.h>
// wlroots does not install its private backend headers, but output fullscreen
// requests inspect struct wlr_wl_output / wlr_x11_output.
#include "backend/wayland.h"
#include "backend/x11.h"
}

#include <glad/glad.h>

#include "components/Bootable.h"
#include "controls.h"
#include "engine.h"
#include "screen.h"
#include "time_utils.h"
#include "wayland/pointer.h"
#include "wayland/wlr_compositor.h"
#include "wayland/ime.h"
#include "wlr_internal.h"

namespace {

static xcb_atom_t
intern_x11_atom(xcb_connection_t* xcb, const char* name)
{
  xcb_intern_atom_cookie_t cookie =
    xcb_intern_atom(xcb, false, std::strlen(name), name);
  xcb_intern_atom_reply_t* reply = xcb_intern_atom_reply(xcb, cookie, nullptr);
  if (!reply) {
    return XCB_ATOM_NONE;
  }
  xcb_atom_t atom = reply->atom;
  std::free(reply);
  return atom;
}

static void
request_wayland_output_fullscreen(wlr_output* output)
{
  if (!output || !wlr_output_is_wl(output)) {
    return;
  }

  auto* wl_output =
    wl_container_of(output, static_cast<wlr_wl_output*>(nullptr), wlr_output);
  if (!wl_output || !wl_output->xdg_toplevel || !wl_output->backend ||
      !wl_output->backend->remote_display) {
    return;
  }

  xdg_toplevel_set_fullscreen(wl_output->xdg_toplevel, nullptr);
  wl_display_flush(wl_output->backend->remote_display);
}

static void
request_x11_output_fullscreen(wlr_output* output)
{
  if (!output || !wlr_output_is_x11(output)) {
    return;
  }

  auto* x11_output = wl_container_of(output, static_cast<wlr_x11_output*>(nullptr), wlr_output);
  if (!x11_output || !x11_output->x11 || !x11_output->x11->xcb) {
    return;
  }

  xcb_connection_t* xcb = x11_output->x11->xcb;
  xcb_atom_t wm_state = intern_x11_atom(xcb, "_NET_WM_STATE");
  xcb_atom_t fullscreen = intern_x11_atom(xcb, "_NET_WM_STATE_FULLSCREEN");
  if (wm_state == XCB_ATOM_NONE || fullscreen == XCB_ATOM_NONE) {
    return;
  }

  xcb_change_property(xcb,
                      XCB_PROP_MODE_REPLACE,
                      x11_output->win,
                      wm_state,
                      XCB_ATOM_ATOM,
                      32,
                      1,
                      &fullscreen);

  xcb_client_message_event_t event = {};
  event.response_type = XCB_CLIENT_MESSAGE;
  event.window = x11_output->win;
  event.type = wm_state;
  event.format = 32;
  event.data.data32[0] = 1; // _NET_WM_STATE_ADD
  event.data.data32[1] = fullscreen;
  event.data.data32[2] = XCB_ATOM_NONE;
  event.data.data32[3] = 1; // normal application source indication
  event.data.data32[4] = 0;

  xcb_send_event(xcb,
                 false,
                 x11_output->x11->screen->root,
                 XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT |
                   XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY,
                 reinterpret_cast<const char*>(&event));
  xcb_flush(xcb);
}

static void
sync_default_window_size_to_output(int width, int height)
{
  width = std::max(width, 1);
  height = std::max(height, 1);
  Bootable::DEFAULT_WIDTH =
    std::max(1, static_cast<int>(std::lround(width * 0.85f)));
  Bootable::DEFAULT_HEIGHT =
    std::max(1, static_cast<int>(std::lround(height * 0.85f)));
}

void
handle_output_destroy(wl_listener* listener, void* data)
{
  (void)data;
  auto* handle =
    wl_container_of(listener, static_cast<WlrOutputHandle*>(nullptr), destroy);
  if (handle->server && handle->server->output_layout && handle->output) {
    wlr_output_layout_remove(handle->server->output_layout, handle->output);
  }
  wl_list_remove(&handle->frame.link);
  wl_list_remove(&handle->destroy.link);
  wl_list_remove(&handle->request_state.link);
  wl_list_remove(&handle->commit.link);
  if (handle->swapchain) {
    wlr_swapchain_destroy(handle->swapchain);
  }
  if (handle->depth_rbo) {
    glDeleteRenderbuffers(1, &handle->depth_rbo);
    handle->depth_rbo = 0;
  }
  delete handle;
}

void
ensure_glad(WlrServer* server)
{
  if (server->gladLoaded) {
    return;
  }
  if (!gladLoadGLLoader((GLADloadproc)eglGetProcAddress)) {
    wlr_log(WLR_ERROR, "Failed to load GL functions via EGL");
    wl_display_terminate(server->display);
    return;
  }
  const char* glVersion = reinterpret_cast<const char*>(glGetString(GL_VERSION));
  const char* glslVersion =
    reinterpret_cast<const char*>(glGetString(GL_SHADING_LANGUAGE_VERSION));
  wlr_log(WLR_INFO, "wlroots compositor GL version: %s, GLSL: %s",
               glVersion ? glVersion : "(null)",
               glslVersion ? glslVersion : "(null)");
  server->gladLoaded = true;
}

void
ensure_depth_buffer(WlrOutputHandle* handle, int width, int height, GLuint fbo)
{
  if (handle->depth_rbo != 0 &&
      handle->depth_width == width &&
      handle->depth_height == height) {
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER,
                              GL_DEPTH_ATTACHMENT,
                              GL_RENDERBUFFER,
                              handle->depth_rbo);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER,
                              GL_STENCIL_ATTACHMENT,
                              GL_RENDERBUFFER,
                              handle->depth_rbo);
    return;
  }

  if (handle->depth_rbo != 0) {
    glDeleteRenderbuffers(1, &handle->depth_rbo);
  }
  glGenRenderbuffers(1, &handle->depth_rbo);
  glBindRenderbuffer(GL_RENDERBUFFER, handle->depth_rbo);
  glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, width, height);
  glBindFramebuffer(GL_FRAMEBUFFER, fbo);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER,
                            GL_DEPTH_STENCIL_ATTACHMENT,
                            GL_RENDERBUFFER,
                            handle->depth_rbo);
  handle->depth_width = width;
  handle->depth_height = height;
  GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
  if (status != GL_FRAMEBUFFER_COMPLETE) {
    wlr_log(WLR_ERROR,
                 "FBO incomplete after attaching depth: 0x%x (size %dx%d)",
                 status,
                 width,
                 height);
  } else {
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
  }
}

static void
applyPendingOutputState(WlrServer* server)
{
  if (server == nullptr || server->primary_output == nullptr) {
    return;
  }
  wlr_output* output = server->primary_output;

  if (server->pendingOutputMode) {
    const int index = server->pendingOutputModeIndex;
    server->pendingOutputMode = false;
    wlr_output_mode* selected = nullptr;
    wlr_output_mode* mode = nullptr;
    int current = 0;
    wl_list_for_each(mode, &output->modes, link) {
      if (current++ == index) {
        selected = mode;
        break;
      }
    }
    if (selected != nullptr) {
      wlr_output_state state;
      wlr_output_state_init(&state);
      wlr_output_state_set_enabled(&state, true);
      wlr_output_state_set_mode(&state, selected);
      if (!wlr_output_commit_state(output, &state)) {
        wlr_log(WLR_ERROR, "Failed to apply output mode index %d", index);
      }
      wlr_output_state_finish(&state);
    }
  }

  if (server->pendingOutputScale) {
    const float scale = server->pendingOutputScaleValue;
    server->pendingOutputScale = false;
    wlr_output_state state;
    wlr_output_state_init(&state);
    wlr_output_state_set_scale(&state, scale);
    if (!wlr_output_commit_state(output, &state)) {
      wlr_log(WLR_ERROR, "Failed to apply output scale %.2f", scale);
    }
    wlr_output_state_finish(&state);
  }
}

void
handle_output_frame(wl_listener* listener, void* data)
{
  (void)data;
  auto* handle =
    wl_container_of(listener, static_cast<WlrOutputHandle*>(nullptr), frame);
  auto* server = handle->server;

  // Apply queued mode/scale changes before acquiring this frame's buffer so we
  // never commit the output in the middle of a render pass.
  applyPendingOutputState(server);

  struct wlr_output_state output_state;
  wlr_output_state_init(&output_state);

  // no-op if the swapchain has already been configured
  // only errors if old swapchain isn't configured properly
  // and a new one couldn't be created & configured
  if (!wlr_output_configure_primary_swapchain(
        handle->output, &output_state, &handle->swapchain)) {
    wlr_log(WLR_ERROR, "Failed to configure primary swapchain");
    wlr_output_state_finish(&output_state);
    return;
  }

  wlr_buffer* buffer = wlr_swapchain_acquire(handle->swapchain);
  if (!buffer) {
    wlr_log(WLR_ERROR, "Failed to acquire buffer from swapchain");
    wlr_output_state_finish(&output_state);
    return;
  }

  const wlr_buffer_pass_options pass_options = {};
  wlr_render_pass* pass =
    wlr_renderer_begin_buffer_pass(server->renderer, buffer, &pass_options);
  if (!pass) {
    wlr_log(WLR_ERROR, "Failed to begin buffer pass");
    wlr_buffer_unlock(buffer);
    wlr_output_state_finish(&output_state);
    return;
  }

  int width = handle->swapchain ? handle->swapchain->width : 1;
  int height = handle->swapchain ? handle->swapchain->height : 1;
  // Reduce log spam in hot path; enable by setting WLROOTS_FRAME_LOG=1.
  ensure_glad(server);

  GLuint fbo = wlr_gles2_renderer_get_buffer_fbo(server->renderer, buffer);
  glBindFramebuffer(GL_FRAMEBUFFER, fbo);
  ensure_depth_buffer(handle, width, height, fbo);
  auto submit_output_frame = [&](bool send_frame_done) {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    wlr_render_pass_submit(pass);

    if (send_frame_done) {
      struct timespec now;
      clock_gettime(CLOCK_MONOTONIC, &now);
      for (auto& entry : server->surface_map) {
        wlr_surface* root = entry.first;
        wlr_surface_for_each_surface(
          root,
          [](wlr_surface* surface, int, int, void* data) {
            auto* ts = static_cast<timespec*>(data);
            wlr_surface_send_frame_done(surface, ts);
          },
          &now);
      }
    }

    wlr_output_state_set_buffer(&output_state, buffer);
    if (!wlr_output_commit_state(handle->output, &output_state)) {
      wlr_log(WLR_ERROR, "Failed to commit output frame");
    }
    wlr_output_state_finish(&output_state);
    wlr_buffer_unlock(buffer);
  };

  if (!server->engine) {
    EngineOptions options;
    options.enableControls = true; // wlroots path feeds input through Controls
    options.enableGui = true; // draw HackMatrix ImGui overlay inside compositor
    // Allow disabling the debug overlay (e.g. for benchmarking or CI).
    if (const char* guiEnv = std::getenv("HACKMATRIX_ENABLE_GUI")) {
      options.enableGui = std::atoi(guiEnv) != 0;
    }
    options.invertYAxis = true;
    server->engine = std::make_unique<Engine>(server->envp, options);
    if (server->engine) {
      server->engine->setImguiOverlay([server]() {
        if (server->ime) {
          server->ime->renderOverlay();
        }
      });
      if (auto* controls = server->engine->getControls()) {
        controls->setPointerCenterCallback([server]() {
          if (!server->cursor || !server->cursor_mgr) {
            return;
          }
          wlr_cursor_warp_absolute(
            server->cursor, server->last_pointer_device, 0.5, 0.5);
          server->pointer_x = server->cursor->x;
          server->pointer_y = server->cursor->y;
        });
      }
      // Expose the primary output's modes/scale to the System Config tab.
      if (server->primary_output) {
        wlr_output* output = server->primary_output;
        server->engine->setOutputControl(
          [output]() -> std::optional<Engine::OutputInfo> {
            if (output == nullptr) {
              return std::nullopt;
            }
            Engine::OutputInfo info;
            info.name = output->name ? output->name : "";
            info.description =
              output->description ? output->description : "";
            info.width = output->width;
            info.height = output->height;
            info.refresh = output->refresh;
            info.scale = output->scale;
            info.enabled = output->enabled;
            int index = 0;
            wlr_output_mode* mode = nullptr;
            wl_list_for_each(mode, &output->modes, link) {
              info.modes.push_back(
                Engine::OutputModeInfo{ mode->width,
                                        mode->height,
                                        mode->refresh,
                                        mode->preferred });
              if (mode == output->current_mode) {
                info.currentMode = index;
              }
              ++index;
            }
            return info;
          },
          [server](int index) -> bool {
            if (server->primary_output == nullptr || index < 0) {
              return false;
            }
            // Applied at the start of the next output frame.
            server->pendingOutputMode = true;
            server->pendingOutputModeIndex = index;
            return true;
          },
          [server](float scale) -> bool {
            if (server->primary_output == nullptr || scale <= 0.0f) {
              return false;
            }
            server->pendingOutputScale = true;
            server->pendingOutputScaleValue = scale;
            return true;
          });
      }
      // KEY_REPLAY / remote control typing: map a keysym back to a keycode on
      // the active keymap and deliver it through the seat (and IME grab).
      server->engine->setPointerInjector(
        [server](std::uint32_t button, bool pressed) {
          return inject_pointer_button(server, button, pressed);
        });
      server->engine->setKeyInjector(
        [server](std::uint32_t sym, bool pressed) {
          if (server->seat == nullptr) {
            return;
          }
          auto* keyboard = wlr_seat_get_keyboard(server->seat);
          if (keyboard == nullptr || keyboard->keymap == nullptr) {
            return;
          }
          std::uint32_t keycode = 0;
          for (std::uint32_t k = 8; k < 256; ++k) {
            const xkb_keysym_t* syms = nullptr;
            if (xkb_keymap_key_get_syms_by_level(
                  keyboard->keymap, k, 0, 0, &syms) > 0 &&
                syms[0] == static_cast<xkb_keysym_t>(sym)) {
              keycode = k;
              break;
            }
            if (xkb_keymap_key_get_syms_by_level(
                  keyboard->keymap, k, 1, 0, &syms) > 0 &&
                syms[0] == static_cast<xkb_keysym_t>(sym)) {
              keycode = k;
              break;
            }
          }
          if (keycode == 0) {
            return;
          }
          // xkb keycodes are evdev + 8; the seat expects the raw evdev code.
          wlr_seat_keyboard_notify_key(
            server->seat,
            static_cast<uint32_t>(
              std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count()),
            keycode - 8,
            pressed ? WL_KEYBOARD_KEY_STATE_PRESSED
                    : WL_KEYBOARD_KEY_STATE_RELEASED);
        });
    }
    // Surface API quit handling through wl_display so tests can terminate
    // cleanly.
    if (server->engine) {
      auto api = server->engine->getApi();
      if (api) {
        api->setDisplay(server->display);
      }
    }
  }
  if (server->engine && !server->registry) {
    server->registry = server->engine->getRegistry();
  }
  // Ensure Wayland apps have textures attached once the renderer exists.
  ensure_wayland_apps_registered(server);

  if (server->engine) {
    Controls* controls = server->engine->getControls();
    Camera* camera = server->engine->getCamera();
    if (camera) {
      if (controls) {
        auto local = output_local_pointer(server, handle->output);
        controls->setPointerScreenPosition(static_cast<float>(local.first),
                                          static_cast<float>(local.second));
        //controls->applyMovementInput(
         // server->input.forward, server->input.back, server->input.left, server->input.right);
      }
      bool cursorOverrideVisible = false;
      bool cursorOverrideSet = false;
      if (server->engine) {
        if (auto wm = server->engine->getWindowManager()) {
          if (auto ov = wm->getCursorVisibleOverride()) {
            cursorOverrideSet = true;
            cursorOverrideVisible = *ov;
          }
        }
      }
      // Free-look crosshair focus is pointer-only and must survive across frames
      // between press and release. The cursor-mode cleanup below would
      // otherwise clear it every frame and clients would never see a click.
      const bool freeViewPointerActive =
        free_view_pointer_active(server) ||
        !server->free_view_buttons.empty();
      if (cursorOverrideSet && cursorOverrideVisible) {
        sync_cursor_mode_pointer_focus(server);
      } else if (server->engine && !freeViewPointerActive) {
        if (auto wm = server->engine->getWindowManager()) {
          if (!wm->getCurrentlyFocusedApp().has_value()) {
            clear_cursor_mode_input_focus(server);
          }
        }
      }
      bool pointerFocusRequested = wayland_pointer_focus_requested(server);
      bool pointerVisible = pointerFocusRequested || (cursorOverrideSet && cursorOverrideVisible);
      // A window resize owns the mouse delta so the camera stays put while the
      // edge is being dragged. A carried window (G) does NOT block look: the
      // camera rotates normally and the window follows it after the look below.
      bool windowManipulated = false;
      if (controls && controls->isWindowResizeActive()) {
        controls->handleWindowResizeDelta(server->input.delta_x,
                                           server->input.delta_y);
        windowManipulated = true;
      }
      if (!windowManipulated && !pointerVisible &&
          !server->engine->isExitConfirmOpen() &&
          !server->engine->isDisplayConfirmOpen() &&
          (server->input.delta_x != 0.0 || server->input.delta_y != 0.0)) {
        if (controls) {
          controls->applyLookDelta(server->input.delta_x, -server->input.delta_y);
        } else {
          camera->handleRotateForce(server->input.delta_x, -server->input.delta_y);
        }
      }
      // Carry the grabbed window in front of the camera after the look/position
      // update so it tracks the view and player movement.
      if (controls && controls->isWindowMoveActive()) {
        controls->updateHeldWindowPose();
      }
      // Hide cursor and clear pointer focus when game mode is active so only deltas matter.
      // Keep free-look crosshair focus alive while it is aiming at a surface.
      if (!pointerVisible && !freeViewPointerActive && server->seat &&
          server->seat->pointer_state.focused_surface) {
        wlr_seat_pointer_notify_clear_focus(server->seat);
        update_pointer_constraint(server, nullptr);
      }
      set_cursor_visible(server, pointerVisible, handle->output);
      // Always clear accumulated deltas so they don't apply after focus changes.
      server->input.delta_x = 0.0;
      server->input.delta_y = 0.0;
    }
  }

  if (server->engine && server->registry && !server->pending_surfaces.empty()) {
    auto pending = std::move(server->pending_surfaces);
    server->pending_surfaces.clear();
    for (auto* xdg : pending) {
      create_wayland_app(server, xdg);
    }
  }

  glViewport(0, 0, width, height);
  server->engine->frame();
  if (server->engine->consumeQuitRequest()) {
    wl_display_terminate(server->display);
  }

  bool pointerLocked = wayland_pointer_locked(server);
  // Render software cursors (clients set them via wl_pointer.set_cursor).
  if (!pointerLocked) {
    pixman_region32_t cursor_damage;
    pixman_region32_init_rect(&cursor_damage, 0, 0, width, height);
    wlr_output_add_software_cursors_to_render_pass(handle->output, pass, &cursor_damage);
    pixman_region32_fini(&cursor_damage);
  }
  // Draw a compositor-owned software cursor when either a Wayland surface has focus
  // or the WM explicitly requested visibility (e.g., toggle_cursor hotkey).
  bool cursorVisibleOverride = false;
  if (server->engine) {
    if (auto wm = server->engine->getWindowManager()) {
      if (auto ov = wm->getCursorVisibleOverride()) {
        cursorVisibleOverride = *ov;
      }
    }
  }
  if (!pointerLocked &&
      server->engine &&
      (wayland_pointer_focus_requested(server) || cursorVisibleOverride)) {
    if (auto* renderer = server->engine->getRenderer()) {
      float sizePx = 24.0f * (handle->output ? handle->output->scale : 1.0f);
      auto pointer = output_local_pointer(server, handle->output);
      renderer->renderSoftwareCursor(pointer.first, pointer.second, sizePx);
    }
  }

  // Run screenshot capture after everything (including the software cursor)
  // has been drawn so the image matches what is displayed.
  if (server->engine && server->engine->getWindowManager() &&
      server->engine->getRenderer()) {
    auto wm = server->engine->getWindowManager();
    if (wm && wm->consumeScreenshotRequest()) {
      if (auto* renderer = server->engine->getRenderer()) {
        renderer->screenshotFromCurrentFramebuffer(width, height, fbo);
      }
    }
  }

  submit_output_frame(true);
}

static void
handle_output_commit(wl_listener* listener, void* data)
{
  auto* handle =
    wl_container_of(listener, static_cast<WlrOutputHandle*>(nullptr), commit);
  auto* event = static_cast<wlr_output_event_commit*>(data);
  auto* server = handle->server;
  auto* output = event->output;

  if (!server || handle->output != output) {
    return;
  }

  if (handle->width == output->width && handle->height == output->height) {
    return;
  }

  handle->width = output->width;
  handle->height = output->height;

  if (server->primary_output == output) {
    SCREEN_WIDTH = static_cast<float>(output->width);
    SCREEN_HEIGHT = static_cast<float>(output->height);
    sync_default_window_size_to_output(output->width, output->height);
    wlr_box box = {};
    if (server->output_layout) {
      wlr_output_layout_get_box(server->output_layout, output, &box);
    }
    double originX = box.x;
    double originY = box.y;
    server->pointer_x = std::clamp(server->pointer_x,
                                   originX,
                                   originX + output->width);
    server->pointer_y = std::clamp(server->pointer_y,
                                   originY,
                                   originY + output->height);
  }

  wlr_log(WLR_DEBUG, "output resized: %dx%d", handle->width, handle->height);
}

static void
handle_output_request_state(wl_listener* listener, void* data)
{
  auto* handle =
    wl_container_of(listener, static_cast<WlrOutputHandle*>(nullptr), request_state);
  auto* event = static_cast<wlr_output_event_request_state*>(data);
  if (!handle || !handle->output || !event || event->output != handle->output) {
    return;
  }

  if (!wlr_output_commit_state(handle->output, event->state)) {
    wlr_log(WLR_ERROR,
            "Failed to commit requested state for output %s",
            handle->output->name ? handle->output->name : "(unknown)");
    return;
  }

  if (handle->width != handle->output->width ||
      handle->height != handle->output->height) {
    handle->width = handle->output->width;
    handle->height = handle->output->height;
    if (handle->server && handle->server->primary_output == handle->output) {
      SCREEN_WIDTH = static_cast<float>(handle->output->width);
      SCREEN_HEIGHT = static_cast<float>(handle->output->height);
      sync_default_window_size_to_output(handle->output->width,
                                         handle->output->height);
      wlr_box box = {};
      if (handle->server->output_layout) {
        wlr_output_layout_get_box(
          handle->server->output_layout, handle->output, &box);
      }
      double originX = box.x;
      double originY = box.y;
      handle->server->pointer_x =
        std::clamp(handle->server->pointer_x,
                   originX,
                   originX + handle->output->width);
      handle->server->pointer_y =
        std::clamp(handle->server->pointer_y,
                   originY,
                   originY + handle->output->height);
    }
    wlr_log(WLR_DEBUG,
                 "output request_state applied: %dx%d",
                 handle->width,
                 handle->height);
  }

  wlr_output_schedule_frame(handle->output);
}

// This is called when wayland detects a new output such as a monitor

} // namespace

void
handle_new_output(wl_listener* listener, void* data)
{
  auto* server =
    wl_container_of(listener, static_cast<WlrServer*>(nullptr), new_output);
  auto* output = static_cast<wlr_output*>(data);

  if (!wlr_output_init_render(output, server->allocator, server->renderer)) {
    wlr_log(WLR_ERROR, "Failed to init render for output %s", output->name);
    return;
  }

  if (server->output_layout) {
    wlr_output_layout_add_auto(server->output_layout, output);
  }

  struct wlr_output_state state;
  wlr_output_state_init(&state);
  wlr_output_state_set_enabled(&state, true);
  if (wlr_output_mode* mode = wlr_output_preferred_mode(output)) {
    wlr_output_state_set_mode(&state, mode);
  }
  if (wlr_output_is_wl(output)) {
    // In nested Wayland mode, inherit a neutral scale so parent compositor
    // HiDPI settings don't shrink client geometry inside HackMatrix.
    wlr_output_state_set_scale(&state, 1.0f);
  }
  if (!wlr_output_commit_state(output, &state)) {
    wlr_log(WLR_ERROR, "Failed to commit output %s", output->name);
    wlr_output_state_finish(&state);
    return;
  }
  wlr_output_state_finish(&state);

  // Keep global screen dimensions in sync with the wlroots output so renderer
  // projection and texture sizing match the real buffer size.
  SCREEN_WIDTH = static_cast<float>(output->width);
  SCREEN_HEIGHT = static_cast<float>(output->height);
  sync_default_window_size_to_output(output->width, output->height);

  auto* handle = new WlrOutputHandle();
  handle->server = server;
  handle->output = output;
  handle->width = output->width;
  handle->height = output->height;
  bool isPrimaryOutput = !server->primary_output;
  if (isPrimaryOutput) {
    server->primary_output = output;
    wlr_box box = {};
    if (server->output_layout) {
      wlr_output_layout_get_box(server->output_layout, output, &box);
    }
    server->pointer_x = box.x + output->width / 2.0;
    server->pointer_y = box.y + output->height / 2.0;
  }
  if (isPrimaryOutput && server->cursor) {
    wlr_cursor_map_to_output(server->cursor, output);
    wlr_cursor_warp(server->cursor, nullptr, server->pointer_x, server->pointer_y);
    if (server->cursor_mgr) {
      wlr_xcursor_manager_load(server->cursor_mgr, output->scale);
      set_cursor_visible(server, wayland_pointer_focus_requested(server), output);
    }
  }
  request_wayland_output_fullscreen(output);
  request_x11_output_fullscreen(output);
  for (auto& entry : server->surface_map) {
    wlr_surface_send_enter(entry.first, output);
  }
  wlr_log(WLR_DEBUG, "new output %s %dx%d", output->name, output->width, output->height);

  handle->frame.notify = handle_output_frame;
  wl_signal_add(&output->events.frame, &handle->frame);
  handle->destroy.notify = handle_output_destroy;
  wl_signal_add(&output->events.destroy, &handle->destroy);
  handle->request_state.notify = handle_output_request_state;
  wl_signal_add(&output->events.request_state, &handle->request_state);
  handle->commit.notify = handle_output_commit;
  wl_signal_add(&output->events.commit, &handle->commit);

  wlr_output_schedule_frame(output);
}
