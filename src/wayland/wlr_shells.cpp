#include <algorithm>
#include <memory>
#include <string>
#include <utility>

// wlroots is a C library: include its headers with C linkage.
extern "C" {
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_keyboard.h>
#define namespace namespace_
#include <wlr/types/wlr_layer_shell_v1.h>
#undef namespace
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
}

#include "screen.h"
#include "wayland/wlr_compositor.h"
#include "wayland_app.h"
#include "wlr_internal.h"

void create_wayland_app(WlrServer* server, wlr_xdg_surface* xdg_surface)
{
  bool is_popup =
    xdg_surface && xdg_surface->role == WLR_XDG_SURFACE_ROLE_POPUP;
  wlr_surface* parent_surface = nullptr;
  int offset_x = 0;
  int offset_y = 0;
  if (is_popup && xdg_surface && xdg_surface->popup) {
    parent_surface = xdg_surface->popup->parent;
    if (parent_surface) {
      double sx = 0.0;
      double sy = 0.0;
      wlr_xdg_popup_get_position(xdg_surface->popup, &sx, &sy);
      offset_x = static_cast<int>(sx);
      offset_y = static_cast<int>(sy);
    }
  }
  auto app = std::make_shared<WaylandApp>(server->renderer,
                                          server->allocator,
                                          xdg_surface,
                                          0,
                                          /*request_initial_size=*/!is_popup);
  app->setSeat(server->seat, xdg_surface->surface);

  auto* handle = new WaylandAppHandle();
  handle->server = server;
  handle->app = app;
  handle->xdg_surface = xdg_surface;
  handle->surface = xdg_surface->surface;
  handle->accessory = is_popup;
  handle->parent_surface = parent_surface;
  handle->offset_x = offset_x;
  handle->offset_y = offset_y;
  handle->entity = entt::null;
  handle->registered = false;
  handle->commit.notify = [](wl_listener* listener, void* data) {
    auto* handle = wl_container_of(listener, static_cast<WaylandAppHandle*>(nullptr), commit);
    auto* surf = static_cast<wlr_surface*>(data);
    if (!handle->server || !surf || handle->registered) {
      return;
    }
    int w = surf->current.width;
    int h = surf->current.height;
    if (w <= 0 || h <= 0) {
      return;
    }
    // Defer registration to the render loop to avoid GL/context races.
    handle->server->pending_wl_actions.push_back(
      PendingWlAction{ PendingWlAction::Add,
                       handle->app,
                       surf,
                       handle->accessory,
                       false,
                       false,
                       handle->parent_surface,
                       handle->offset_x,
                       handle->offset_y,
                       0,
                       0 });
    handle->registered = true;
  };
  wl_signal_add(&xdg_surface->surface->events.commit, &handle->commit);
  handle->unmap.notify = [](wl_listener* listener, void* /*data*/) {
    auto* handle = wl_container_of(listener, static_cast<WaylandAppHandle*>(nullptr), unmap);
    if (handle->server && handle->surface) {
      handle->server->pending_wl_actions.push_back(
        PendingWlAction{ PendingWlAction::Remove, handle->app, handle->surface });
      handle->registered = false;
      ensure_wayland_apps_registered(handle->server);
    }
    safe_remove_listener(&handle->unmap);
    handle->unmapLinked = false;
  };
  wl_signal_add(&xdg_surface->surface->events.unmap, &handle->unmap);
  handle->unmapLinked = true;
  handle->destroy.notify = [](wl_listener* listener, void* data) {
    auto* handle = wl_container_of(listener, static_cast<WaylandAppHandle*>(nullptr), destroy);
    auto* surf = static_cast<wlr_surface*>(data);
    if (handle->server) {
      handle->server->pending_wl_actions.push_back(
        PendingWlAction{ PendingWlAction::Remove, handle->app, surf });
      handle->registered = false;
      ensure_wayland_apps_registered(handle->server);
    }
    safe_remove_listener(&handle->unmap);
    handle->unmapLinked = false;
    safe_remove_listener(&handle->destroy);
    safe_remove_listener(&handle->commit);
    delete handle;
  };
  wl_signal_add(&xdg_surface->surface->events.destroy, &handle->destroy);

  if (server->primary_output) {
    wlr_surface_send_enter(xdg_surface->surface, server->primary_output);
  }
}

// Primarily positions the Wofi menu (and other overlay style desktop
// notifications/apps). Honour the client's anchors and margins so that e.g. a
// bottom-anchored panel actually lands at the bottom instead of at (0, 0).
static std::pair<int, int>
compute_layer_shell_position(WlrServer* server,
                             wlr_layer_surface_v1* layer,
                             int width,
                             int height)
{
  int out_w = static_cast<int>(SCREEN_WIDTH);
  int out_h = static_cast<int>(SCREEN_HEIGHT);
  if (server && server->primary_output) {
    out_w = server->primary_output->width;
    out_h = server->primary_output->height;
  }
  auto state = layer ? layer->current : wlr_layer_surface_v1_state{};
  bool anchor_left = state.anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT;
  bool anchor_right = state.anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
  bool anchor_top = state.anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP;
  bool anchor_bottom = state.anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM;

  int x = 0;
  int y = 0;
  if (anchor_left && !anchor_right) {
    x = state.margin.left;
  } else if (anchor_right && !anchor_left) {
    x = out_w - width - state.margin.right;
  } else {
    x = std::max(0, (out_w - width) / 2);
  }

  if (anchor_top && !anchor_bottom) {
    y = state.margin.top;
  } else if (anchor_bottom && !anchor_top) {
    y = out_h - height - state.margin.bottom;
  } else {
    y = std::max(0, (out_h - height) / 2);
  }
  return { x, y };
}


// This handles desktop level UI, specifically the wofi menu which does not render as a normal app, but as an overlay
// wlr-layer-shell is a Wayland extension that defines how desktop components such as panels, notifications, and backgrounds
// can be displayed as “layers” above or below application windows.
// These should bypass window management
void
handle_new_layer_surface(wl_listener* listener, void* data)
{
  auto* server =
    wl_container_of(listener, static_cast<WlrServer*>(nullptr), new_layer_surface);
  auto* layer = static_cast<wlr_layer_surface_v1*>(data);
  if (!server || !layer) {
    return;
  }
  if (!layer->output && server->primary_output) {
    layer->output = server->primary_output;
  }

  auto* handle = new LayerSurfaceHandle();
  handle->server = server;
  handle->layer = layer;

  handle->commit.notify = [](wl_listener* listener, void* data) {
    auto* handle =
      wl_container_of(listener, static_cast<LayerSurfaceHandle*>(nullptr), commit);
    auto* surf = static_cast<wlr_surface*>(data);
    if (!handle || !handle->server || !surf) {
      return;
    }
    if (!handle->configured) {
      uint32_t out_w = handle->server->primary_output->width;
      uint32_t out_h = handle->server->primary_output->height;
      uint32_t desired_w =
        handle->layer && handle->layer->current.desired_width > 0
          ? handle->layer->current.desired_width
          : out_w;
      uint32_t desired_h =
        handle->layer && handle->layer->current.desired_height > 0
          ? handle->layer->current.desired_height
          : out_h;
      wlr_layer_surface_v1_configure(handle->layer, desired_w, desired_h);
      handle->configured = true;
      return;
    }
    if (handle->registered) {
      return;
    }
    int w = surf->current.width;
    int h = surf->current.height;
    if (w <= 0 || h <= 0) {
      return;
    }
    auto pos = compute_layer_shell_position(handle->server, handle->layer, w, h);
    auto app = std::make_shared<WaylandApp>(handle->server->renderer,
                                            handle->server->allocator,
                                            surf,
                                            "layer-shell",
                                            0);
    if (handle->server->seat) {
      app->setSeat(handle->server->seat, surf);
    }
    handle->app = app;
    handle->server->pending_wl_actions.push_back(
      PendingWlAction{ PendingWlAction::Add,
                       app,
                       surf,
                       true,
                       true,
                       false,
                       nullptr,
                       0,
                       0,
                       pos.first,
                       pos.second,
                       handle->server->primary_output ? handle->server->primary_output->width
                                                      : static_cast<int>(SCREEN_WIDTH),
                       handle->server->primary_output ? handle->server->primary_output->height
                                                      : static_cast<int>(SCREEN_HEIGHT) });
    handle->registered = true;
  };
  wl_signal_add(&layer->surface->events.commit, &handle->commit);

  handle->unmap.notify = [](wl_listener* listener, void* /*data*/) {
    auto* handle =
      wl_container_of(listener, static_cast<LayerSurfaceHandle*>(nullptr), unmap);
    if (handle && handle->server && handle->app && handle->layer) {
      handle->server->pending_wl_actions.push_back(
        PendingWlAction{ PendingWlAction::Remove, handle->app, handle->layer->surface });
      handle->registered = false;
    }
    if (handle) {
      handle->configured = false;
    }
  };
  wl_signal_add(&layer->surface->events.unmap, &handle->unmap);

  handle->destroy.notify = [](wl_listener* listener, void* data) {
    auto* handle =
      wl_container_of(listener, static_cast<LayerSurfaceHandle*>(nullptr), destroy);
    auto* surf = static_cast<wlr_surface*>(data);
    if (handle && handle->server) {
      handle->server->pending_wl_actions.push_back(
        PendingWlAction{ PendingWlAction::Remove,
                         handle->app,
                         surf,
                         true,
                         true,
                         false,
                         nullptr,
                         0,
                         0,
                         0,
                         0 });
      handle->registered = false;
      ensure_wayland_apps_registered(handle->server);
    }
    safe_remove_listener(&handle->unmap);
    safe_remove_listener(&handle->destroy);
    safe_remove_listener(&handle->commit);
    delete handle;
  };
  wl_signal_add(&layer->surface->events.destroy, &handle->destroy);
}


// xdg-shell is a Wayland extension that defines how desktop-style applications create and manage their windows
// these are "normal" applications that are to be rendered using window management
void
handle_new_xdg_surface(wl_listener* listener, void* data)
{
  auto* server = wl_container_of(listener, static_cast<WlrServer*>(nullptr), new_xdg_surface);
  auto* xdg_surface = static_cast<wlr_xdg_surface*>(data);
  wlr_log(WLR_DEBUG, "new xdg_surface %p", (void*)xdg_surface);
  if (xdg_surface->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL && xdg_surface->toplevel) {
    auto [width, height] = get_initial_toplevel_size(server);
    wlr_xdg_toplevel_set_size(xdg_surface->toplevel, width, height);
    wlr_xdg_surface_schedule_configure(xdg_surface);
  } else if (xdg_surface->role == WLR_XDG_SURFACE_ROLE_POPUP) {
    // Popups need an initial configure to define their anchor/geometry.
    wlr_xdg_surface_schedule_configure(xdg_surface);
  }

  auto* handle = new XdgSurfaceHandle();
  handle->server = server;
  handle->xdg = xdg_surface;

  handle->commit.notify = [](wl_listener* listener, void* data) {
    auto* handle = wl_container_of(listener, static_cast<XdgSurfaceHandle*>(nullptr), commit);
    auto* surface = static_cast<wlr_surface*>(data);
    if (!handle->server || !handle->xdg) {
      return;
    }
    wlr_log(WLR_DEBUG,
            "xdg_surface %p commit role=%d mapped=%d size=%dx%d",
            (void*)handle->xdg,
            handle->xdg->role,
            surface->mapped ? 1 : 0,
            surface->current.width,
            surface->current.height);
    if (handle->xdg->role != WLR_XDG_SURFACE_ROLE_TOPLEVEL &&
        handle->xdg->role != WLR_XDG_SURFACE_ROLE_POPUP) {
      return;
    }
    if (!handle->configured_sent) {
      if (handle->xdg->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL && handle->xdg->toplevel) {
        auto [width, height] = get_initial_toplevel_size(handle->server);
        wlr_xdg_toplevel_set_size(handle->xdg->toplevel, width, height);
      }
      wlr_log(WLR_DEBUG, "xdg_surface %p initial configure", (void*)handle->xdg);
      wlr_xdg_surface_schedule_configure(handle->xdg);
      handle->configured_sent = true;
    }
    if (surface->mapped && !handle->created) {
      handle->created = true;
      create_wayland_app(handle->server, handle->xdg);
    }
    (void)surface;
  };
  wl_signal_add(&xdg_surface->surface->events.commit, &handle->commit);

  handle->map.notify = [](wl_listener* listener, void* /*data*/) {
    auto* handle = wl_container_of(listener, static_cast<XdgSurfaceHandle*>(nullptr), map);
    if (!handle || !handle->server || !handle->xdg) {
      return;
    }
    wlr_log(WLR_DEBUG, "xdg_surface %p mapped", (void*)handle->xdg);
    if (!handle->created) {
      handle->created = true;
      create_wayland_app(handle->server, handle->xdg);
    }
  };
  wl_signal_add(&xdg_surface->surface->events.map, &handle->map);

  handle->destroy.notify = [](wl_listener* listener, void* data) {
    auto* handle = wl_container_of(listener, static_cast<XdgSurfaceHandle*>(nullptr), destroy);
    auto* xdg = static_cast<wlr_xdg_surface*>(data);
    handle->xdg = nullptr;
    wl_list_remove(&handle->map.link);
    wl_list_remove(&handle->commit.link);
    wl_list_remove(&handle->destroy.link);
    delete handle;
  };
  wl_signal_add(&xdg_surface->events.destroy, &handle->destroy);
}
