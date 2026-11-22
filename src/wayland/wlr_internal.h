#pragma once

#include <memory>
#include <utility>

#include "wayland/wlr_compositor.h"

struct wlr_xwayland_surface;
struct wlr_xdg_surface;

// Helpers shared between wlr_compositor.cpp, wlr_shells.cpp and wlr_xwayland.cpp.
void safe_remove_listener(wl_listener* listener);
void ensure_wayland_apps_registered(WlrServer* server);
std::pair<int, int> get_initial_toplevel_size(WlrServer* server);

// Output frame/commit/state + backend fullscreen (defined in wlr_output.cpp).
void handle_new_output(wl_listener* listener, void* data);

// xdg_shell + layer_shell handling (defined in wlr_shells.cpp).
void create_wayland_app(WlrServer* server, wlr_xdg_surface* xdg_surface);
void handle_new_layer_surface(wl_listener* listener, void* data);
void handle_new_xdg_surface(wl_listener* listener, void* data);

// XWayland surface handling (defined in wlr_xwayland.cpp).
void handle_xwayland_ready(wl_listener* listener, void* data);
void handle_new_xwayland_surface(wl_listener* listener, void* data);
