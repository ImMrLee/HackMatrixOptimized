#define WLR_USE_UNSTABLE 1

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <algorithm>
#include <pixman-1/pixman.h>
#include <memory>
#include <drm_fourcc.h>
#include <ctime>
#include <unordered_map>
#include <EGL/egl.h>
#include <wayland-server-core.h>
#include <xkbcommon/xkbcommon.h>
#include <linux/input-event-codes.h>

extern "C" {
#include <wlr/backend.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/render/allocator.h>
#include <wlr/render/drm_format_set.h>
#include <wlr/render/gles2.h>
#include <wlr/render/swapchain.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_xcursor_manager.h>
#define namespace namespace_
#include <wlr/types/wlr_layer_shell_v1.h>
#undef namespace
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_output.h>
#include <wlr/backend/wayland.h>
#include <wlr/backend/x11.h>
#include <xdg-shell-client-protocol.h>
#include <wlr/types/wlr_pointer_constraints_v1.h>
#include <wlr/types/wlr_relative_pointer_v1.h>
#include <wlr/types/wlr_text_input_v3.h>
#include <wlr/types/wlr_virtual_keyboard_v1.h>
#include <wlr/util/log.h>
#define class class_
#include <wlr/xwayland.h>
#undef class
};

#include <glad/glad.h>
#include <cstdarg>
#include <algorithm>
#include <string>
#include <cctype>
#include <thread>

#include "engine.h"
#include "wayland_app.h"
#include "AppSurface.h"
#include "components/Bootable.h"
#include "entity.h"
#include "screen.h"
#include "Config.h"
#include "controls.h"
#include "wayland/wlr_compositor.h"
#include "wayland/pointer.h"
#include "wayland/keyboard.h"
#include "wayland/ime.h"
#include "wlr_internal.h"

#ifdef WLROOTS_DEBUG_LOGS
constexpr bool kWlrootsDebugLogs = true;
#else
constexpr bool kWlrootsDebugLogs = false;
#endif

// Initialize the externed variables found in screen.h
// Use null values until the server is created.
float SCREEN_WIDTH = 0;
float SCREEN_HEIGHT = 0;

namespace {

static std::pair<int, int>
get_current_output_size(WlrServer* server)
{
  if (server && server->primary_output && server->primary_output->width > 0 &&
      server->primary_output->height > 0) {
    return { server->primary_output->width, server->primary_output->height };
  }

  int width = SCREEN_WIDTH > 0 ? static_cast<int>(SCREEN_WIDTH) : 0;
  int height = SCREEN_HEIGHT > 0 ? static_cast<int>(SCREEN_HEIGHT) : 0;

  if (width <= 0 || height <= 0) {
    if (const char* w_env = std::getenv("WLR_X11_OUTPUT_WIDTH")) {
      width = std::max(width, std::atoi(w_env));
    }
    if (const char* h_env = std::getenv("WLR_X11_OUTPUT_HEIGHT")) {
      height = std::max(height, std::atoi(h_env));
    }
  }
  if (width <= 0 || height <= 0) {
    if (const char* w_env = std::getenv("SCREEN_WIDTH")) {
      width = std::max(width, std::atoi(w_env));
    }
    if (const char* h_env = std::getenv("SCREEN_HEIGHT")) {
      height = std::max(height, std::atoi(h_env));
    }
  }

  width = std::max(width, 1280);
  height = std::max(height, 720);
  return { width, height };
}

static void
add_action(WlrServer* server, PendingWlAction action)
{
  if (!action.menu_surface && server->engine) {
    // Drop duplicate adds for the same surface if it's already registered.
    if (server->surface_map.find(action.surface) != server->surface_map.end()) {
      return;
    }
    entt::entity parentEnt = entt::null;
    if (action.parent_surface) {
      auto pit = server->surface_map.find(action.parent_surface);
      if (pit != server->surface_map.end()) {
        parentEnt = pit->second;
      }
    }
    entt::entity entity = entt::null;
    if (auto wm = server->engine->getWindowManager()) {
      bool spawnAtCamera = !action.accessory;
      entity = wm->registerWaylandApp(action.app,
                                      spawnAtCamera,
                                      action.accessory,
                                      parentEnt,
                                      action.offset_x,
                                      action.offset_y,
                                      action.layer_shell,
                                      action.screen_x,
                                      action.screen_y,
                                      action.screen_w,
                                      action.screen_h);
    }
    if (entity != entt::null) {
      server->surface_map[action.surface] = entity;
    }
  }
}

static void
remove_action(WlrServer* server, PendingWlAction action)
{
  auto it = server->surface_map.find(action.surface);
  if (it != server->surface_map.end()) {
    entt::entity e = it->second;
    if (server->registry && server->registry->valid(e)) {
      if (auto wm =
            server->engine ? server->engine->getWindowManager() : nullptr) {
        if (auto focused = wm->getCurrentlyFocusedApp();
            focused && *focused == e) {
          wm->unfocusApp();
        }
        wm->releaseHotkeySlot(e);
      }
      if (auto* renderer = server->engine ? server->engine->getRenderer()
                                          : nullptr) {
        if (auto* comp = server->registry->try_get<WaylandApp::Component>(e)) {
          if (comp->app) {
            renderer->deregisterApp(comp->app.get());
          }
        }
      }
      server->registry->destroy(e);
    }
    server->surface_map.erase(it);
  }
}

void
handle_new_input(wl_listener* listener, void* data)
{
  auto* server =
    wl_container_of(listener, static_cast<WlrServer*>(nullptr), new_input);
  auto* device = static_cast<wlr_input_device*>(data);
  switch(device->type) {
    case WLR_INPUT_DEVICE_POINTER:
      handle_new_pointer(server, device);
      break;
    case WLR_INPUT_DEVICE_KEYBOARD:
      handle_new_keyboard(server, device);
      break;
  }
}

} // namespace

// Shared with wlr_xwayland.cpp (declared in wlr_internal.h).
std::pair<int, int>
get_initial_toplevel_size(WlrServer* server)
{
  auto [output_w, output_h] = get_current_output_size(server);
  int width = Bootable::DEFAULT_WIDTH;
  int height = Bootable::DEFAULT_HEIGHT;
  if (width <= 0 || height <= 0) {
    width = std::max(1, output_w * 85 / 100);
    height = std::max(1, output_h * 85 / 100);
  }
  return { width, height };
}

void
ensure_wayland_apps_registered(WlrServer* server)
{
  auto actions = std::move(server->pending_wl_actions);
  server->pending_wl_actions.clear();
  for (auto& action : actions) {
    if (action.type == PendingWlAction::Add) {
      add_action(server, action);
    } else if (action.type == PendingWlAction::Remove) {
      remove_action(server, action);
    }
  }
}

// Removes a listener from the linked list of listeners
void
safe_remove_listener(wl_listener* listener)
{
  if (!listener) {
    return;
  }
  if (listener->link.prev || listener->link.next) {
    wl_list_remove(&listener->link);
    listener->link.prev = nullptr;
    listener->link.next = nullptr;
  }
}


const char*
hotkey_modifier_label(HotkeyModifier mod)
{
  return mod == HotkeyModifier::Alt ? "alt" : "super";
}

HotkeyModifier
parse_hotkey_modifier()
{
  std::string mod = "alt";
  try {
    mod = Config::singleton()->get<std::string>("key_mappings.super_modifier");
  } catch (...) {
    // Default to alt when the key is missing.
  }
  std::transform(mod.begin(), mod.end(), mod.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  if (mod == "super" || mod == "logo" || mod == "mod4" || mod == "win") {
    return HotkeyModifier::Super;
  }
  return HotkeyModifier::Alt;
}

uint32_t
hotkey_modifier_mask(HotkeyModifier mod)
{
  return mod == HotkeyModifier::Alt ? WLR_MODIFIER_ALT : WLR_MODIFIER_LOGO;
}

void initialize_wlr_logging() { wlr_log_init(WLR_DEBUG, nullptr); }

// --- Main bootstrap helpers -------------------------------------------------

void
apply_backend_env_defaults()
{
  // TODO: Investigate backends

  // If WLR_BACKENDS not set, prefer wayland when under Wayland, otherwise try
  // X11 so running from an X session or tty can still work.
  if (!std::getenv("WLR_BACKENDS")) {
    if (std::getenv("WAYLAND_DISPLAY")) {
      setenv("WLR_BACKENDS", "wayland", 1);
    } else if (std::getenv("DISPLAY")) {
      setenv("WLR_BACKENDS", "x11", 1);
    }
  }

  // For the X11 backend, wlroots expects an explicit output size. Provide one
  // (default 1920x1080) unless the user already set WLR_X11_OUTPUT_*.
  const char* backends_env = std::getenv("WLR_BACKENDS");
  bool likely_x11 =
    (backends_env && std::string(backends_env).find("x11") != std::string::npos) ||
    (!std::getenv("WAYLAND_DISPLAY") && std::getenv("DISPLAY"));
  if (likely_x11) {
    if (!std::getenv("WLR_X11_OUTPUT_WIDTH") || !std::getenv("WLR_X11_OUTPUT_HEIGHT")) {
      int width = 1920;
      int height = 1080;
      if (const char* w_env = std::getenv("SCREEN_WIDTH")) {
        width = std::max(1, std::atoi(w_env));
      }
      if (const char* h_env = std::getenv("SCREEN_HEIGHT")) {
        height = std::max(1, std::atoi(h_env));
      }
      char buf[32];
      std::snprintf(buf, sizeof(buf), "%d", width);
      setenv("WLR_X11_OUTPUT_WIDTH", buf, 1);
      std::snprintf(buf, sizeof(buf), "%d", height);
      setenv("WLR_X11_OUTPUT_HEIGHT", buf, 1);
    }
  }
}

bool
WlrServer::create_display()
{
  display = wl_display_create();
  if (!display) {
    wlr_log(WLR_ERROR, "Failed to create Wayland display");
    return false;
  }
  return true;
}

bool
WlrServer::create_backend() {
  backend =
    wlr_backend_autocreate(wl_display_get_event_loop(display), nullptr);
  if (!backend) {
    wlr_log(WLR_ERROR, "Failed to create wlroots backend");
    return false;
  }
  return true;
}

bool
WlrServer::create_renderer()
{
  renderer = wlr_renderer_autocreate(backend);
  if (!renderer) {
    wlr_log(WLR_ERROR, "Failed to create renderer");
    return false;
  }
  if (!wlr_renderer_init_wl_display(renderer, display)) {
    wlr_log(WLR_ERROR, "Failed to init renderer for wl_display");
    return false;
  }
  return true;
}

bool
WlrServer::create_allocator() {

  allocator = wlr_allocator_autocreate(backend, renderer);
  if (!allocator) {
    wlr_log(WLR_ERROR, "Failed to create allocator");
    return false;
  }

  return true;
}

// event handler for copy/paste
void handle_set_selection(wl_listener* listener, void* data) {
  auto* server =
        wl_container_of(listener, static_cast<WlrServer*>(nullptr), request_set_selection);
  wlr_seat_request_set_selection_event *event = static_cast<wlr_seat_request_set_selection_event*>(data);
  wlr_seat_set_selection(server->seat, event->source, event->serial);
}

bool
WlrServer::create_seat() {
  seat = wlr_seat_create(display, "seat0");
  if (seat) {
    seat->data = this;
    wlr_seat_set_capabilities(seat, WL_SEAT_CAPABILITY_POINTER | WL_SEAT_CAPABILITY_KEYBOARD);
    // Headless backends have no physical keyboard; synthetic injection and IME
    // grabs still need a keymap on the seat.
    create_virtual_keyboard(this);

    // copy/paste
    request_set_selection.notify = handle_set_selection;
    wl_signal_add(&seat->events.request_set_selection, &request_set_selection);
    if (xwayland) {
      wlr_xwayland_set_seat(xwayland, seat);
    }

    create_cursor(this);
    return true;
  } else {
    wlr_log(WLR_ERROR, "Failed to create seat");
    return false;
  }
}

bool
WlrServer::init_protocols()
{
  output_layout = wlr_output_layout_create(display);

  compositor = wlr_compositor_create(display, 5, renderer);
  if (!compositor) {
    wlr_log(WLR_ERROR, "Failed to create compositor");
    return false;
  }
  // Some clients (e.g., foot) require wl_subcompositor to be advertised.
  if (!wlr_subcompositor_create(display)) {
    wlr_log(WLR_ERROR, "Failed to create subcompositor");
    return false;
  }
  layer_shell = wlr_layer_shell_v1_create(display, 4);
  if (!layer_shell) {
    wlr_log(WLR_ERROR, "Failed to create layer-shell");
    return false;
  }
  xdg_shell = wlr_xdg_shell_create(display, 3);
  if (!xdg_shell) {
    wlr_log(WLR_ERROR, "Failed to create xdg-shell");
    return false;
  }
  xwayland = wlr_xwayland_create(display, compositor, false);
  if (!xwayland) {
    wlr_log(WLR_ERROR, "Failed to create XWayland");
    return false;
  }
  relative_pointer_manager = wlr_relative_pointer_manager_v1_create(display);
  if (!relative_pointer_manager) {
    wlr_log(WLR_ERROR, "Failed to create relative-pointer manager");
    return false;
  }
  pointer_constraints = wlr_pointer_constraints_v1_create(display);
  if (!pointer_constraints) {
    wlr_log(WLR_ERROR, "Failed to create pointer-constraints manager");
    return false;
  }

  // Input method support: GTK/Qt/terminal clients bind text-input-v3, fcitx5
  // binds input-method-v2. Without these globals, no IME can attach to the
  // compositor (even a manually started fcitx5 has nothing to talk to).
  text_input_manager = wlr_text_input_manager_v3_create(display);
  input_method_manager = wlr_input_method_manager_v2_create(display);
  virtual_keyboard_manager = wlr_virtual_keyboard_manager_v1_create(display);
  if (!text_input_manager || !input_method_manager) {
    wlr_log(WLR_ERROR, "Failed to create input method managers");
    return false;
  }
  
  data_device_manager = wlr_data_device_manager_create(display);
  // Advertise the screencopy protocol so tools like grim can capture frames.
  screencopy_manager = wlr_screencopy_manager_v1_create(display);
  if (output_layout) {
    xdg_output_manager =
      wlr_xdg_output_manager_v1_create(display, output_layout);
  }
  return true;
}

void
WlrServer::register_listeners()
{
  new_layer_surface.notify = handle_new_layer_surface;
  wl_signal_add(&layer_shell->events.new_surface, &new_layer_surface);

  new_xdg_surface.notify = handle_new_xdg_surface;
  wl_signal_add(&xdg_shell->events.new_surface, &new_xdg_surface);

  xwayland_ready.notify = handle_xwayland_ready;
  wl_signal_add(&xwayland->events.ready, &xwayland_ready);

  new_xwayland_surface.notify = handle_new_xwayland_surface;
  wl_signal_add(&xwayland->events.new_surface, &new_xwayland_surface);

  new_output.notify = handle_new_output;
  wl_signal_add(&backend->events.new_output, &new_output);

  new_input.notify = handle_new_input;
  wl_signal_add(&backend->events.new_input, &new_input);
}

bool
WlrServer::start_backend_and_socket()
{
  if (!wlr_backend_start(backend)) {
    wlr_log(WLR_ERROR, "Failed to start wlroots backend");
    return false;
  }
  const char* socket = wl_display_add_socket_auto(display);
  if (!socket) {
    wlr_log(WLR_ERROR, "Failed to create Wayland socket");
    return false;
  }
  setenv("HACKMATRIX_WAYLAND_DISPLAY", socket, true);
  setenv("WAYLAND_DISPLAY", socket, true);

  wlr_log(WLR_DEBUG,
          "wlroots compositor ready; WAYLAND_DISPLAY=%s",
          socket ? socket : "(null)");

  wlr_log(WLR_DEBUG,
          "wlroots compositor ready; WAYLAND_DISPLAY=%s",
          socket ? socket : "(null)");
  return true;
}

bool WlrServer::init_resources()
{
  if (!create_display() || !create_backend() || !create_renderer() ||
      !create_allocator() || !init_protocols() || !create_seat()) {
    return false;
  }
  if (seat != nullptr && text_input_manager != nullptr &&
      input_method_manager != nullptr) {
    ime = std::make_unique<ImeRelay>(
      this, seat, text_input_manager, input_method_manager);
  }
  register_listeners();
  return true;
}

bool 
WlrServer::start() {
  if (!start_backend_and_socket()) {
    return false;
  }
  wl_display_run(display);
  return true;
}

WlrServer::WlrServer(char** envp): envp(envp) {
  hotkeyModifier = parse_hotkey_modifier();
  hotkeyModifierMask = hotkey_modifier_mask(hotkeyModifier);

  initialize_wlr_logging();
  apply_backend_env_defaults();
}

WlrServer::~WlrServer()
{
  auto remove_listener = [](wl_listener& listener) {
    if (listener.link.prev || listener.link.next) {
      wl_list_remove(&listener.link);
    }
  };
  if (engine) {
    engine.reset();
  }
  ime.reset();
  if (seat) {
    remove_listener(request_set_cursor);
    remove_listener(request_set_selection);
  }
  if (virtual_keyboard) {
    wlr_keyboard_finish(virtual_keyboard);
    delete virtual_keyboard;
    virtual_keyboard = nullptr;
  }
  remove_listener(new_input);
  remove_listener(new_output);
  remove_listener(new_layer_surface);
  remove_listener(new_xdg_surface);
  remove_listener(xwayland_ready);
  remove_listener(new_xwayland_surface);
  if (active_pointer_constraint) {
    wlr_pointer_constraint_v1_send_deactivated(active_pointer_constraint);
    active_pointer_constraint = nullptr;
  }
  if (active_pointer_constraint_destroy_linked) {
    wl_list_remove(&active_pointer_constraint_destroy.link);
    active_pointer_constraint_destroy_linked = false;
  }
  active_pointer_constraint_destroy.link.prev = nullptr;
  active_pointer_constraint_destroy.link.next = nullptr;
  if (xwayland) {
    wlr_xwayland_destroy(xwayland);
    xwayland = nullptr;
  }
  if (allocator) {
    wlr_allocator_destroy(allocator);
  }
  if (renderer) {
    wlr_renderer_destroy(renderer);
  }
  if (backend) {
    wlr_backend_destroy(backend);
  }
  if (display) {
    wl_display_destroy(display);
  }
}
