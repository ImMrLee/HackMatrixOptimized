#include <cstdlib>
#include <memory>
#include <string>

// wlroots is a C library: include its headers with C linkage like the rest of
// the compositor does, otherwise the symbols get C++ mangling.
extern "C" {
#include <wlr/util/log.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_keyboard.h>
#define namespace namespace_
#include <wlr/types/wlr_layer_shell_v1.h>
#undef namespace
#define class class_
#include <wlr/xwayland.h>
#undef class
}

#include "wayland_app.h"
#include "wayland/wlr_compositor.h"
#include "wlr_internal.h"

static std::shared_ptr<WaylandApp>
create_xwayland_app_object(WlrServer* server, wlr_xwayland_surface* xsurface)
{
  if (!server || !xsurface || !xsurface->surface) {
    return nullptr;
  }
  std::string title = "xwayland-app";
  if (xsurface->title && xsurface->title[0] != '\0') {
    title = xsurface->title;
  } else if (xsurface->class_ && xsurface->class_[0] != '\0') {
    title = xsurface->class_;
  }
  auto app = std::make_shared<WaylandApp>(server->renderer,
                                          server->allocator,
                                          xsurface->surface,
                                          title,
                                          0,
                                          xsurface);
  app->setSeat(server->seat, xsurface->surface);
  return app;
}

void
handle_xwayland_ready(wl_listener* listener, void* /*data*/)
{
  auto* server =
    wl_container_of(listener, static_cast<WlrServer*>(nullptr), xwayland_ready);
  if (!server || !server->xwayland || !server->xwayland->display_name) {
    return;
  }
  setenv("HACKMATRIX_DISPLAY", server->xwayland->display_name, true);
  setenv("DISPLAY", server->xwayland->display_name, true);
  wlr_log(WLR_INFO, "XWayland ready; DISPLAY=%s", server->xwayland->display_name);
}

void
handle_new_xwayland_surface(wl_listener* listener, void* data)
{
  auto* server =
    wl_container_of(listener, static_cast<WlrServer*>(nullptr), new_xwayland_surface);
  auto* xsurface = static_cast<wlr_xwayland_surface*>(data);
  if (!server || !xsurface) {
    return;
  }

  auto* handle = new XwaylandSurfaceHandle();
  handle->server = server;
  handle->xsurface = xsurface;
  handle->accessory = xsurface->override_redirect;
  {
    auto [width, height] = get_initial_toplevel_size(server);
    wlr_xwayland_surface_configure(
      xsurface, 0, 0, static_cast<uint16_t>(width), static_cast<uint16_t>(height));
  }


  handle->request_configure.notify = [](wl_listener* listener, void* data) {
    auto* handle = wl_container_of(listener,
                                   static_cast<XwaylandSurfaceHandle*>(nullptr),
                                   request_configure);
    auto* event = static_cast<wlr_xwayland_surface_configure_event*>(data);
    if (!handle || !handle->xsurface || !event) {
      return;
    }
    auto [default_width, default_height] = get_initial_toplevel_size(handle->server);
    uint16_t width = static_cast<uint16_t>(
      event->width > 0 ? event->width : default_width);
    uint16_t height = static_cast<uint16_t>(
      event->height > 0 ? event->height : default_height);
    wlr_xwayland_surface_configure(
      handle->xsurface, 0, 0, width, height);
    if (handle->app) {
      handle->app->resizeMove(width, height, event->x, event->y);
    }
  };
  wl_signal_add(&xsurface->events.request_configure, &handle->request_configure);

  handle->associate.notify = [](wl_listener* listener, void* /*data*/) {
    auto* handle =
      wl_container_of(listener, static_cast<XwaylandSurfaceHandle*>(nullptr), associate);
    if (!handle || !handle->server || !handle->xsurface || !handle->xsurface->surface) {
      return;
    }
    if ((handle->commit.link.prev || handle->commit.link.next) &&
        handle->surface == handle->xsurface->surface) {
      return;
    }
    handle->surface = handle->xsurface->surface;
    handle->accessory = handle->xsurface->override_redirect;
    handle->parent_surface =
      handle->xsurface->parent ? handle->xsurface->parent->surface : nullptr;
    if (handle->parent_surface && handle->xsurface->parent) {
      handle->offset_x = handle->xsurface->x - handle->xsurface->parent->x;
      handle->offset_y = handle->xsurface->y - handle->xsurface->parent->y;
    } else {
      handle->offset_x = 0;
      handle->offset_y = 0;
    }
    if (!handle->app) {
      handle->app = create_xwayland_app_object(handle->server, handle->xsurface);
    }
    if (!handle->app) {
      return;
    }
    handle->commit.notify = [](wl_listener* listener, void* data) {
      auto* handle =
        wl_container_of(listener, static_cast<XwaylandSurfaceHandle*>(nullptr), commit);
      auto* surf = static_cast<wlr_surface*>(data);
      if (!handle->server || !surf || handle->registered) {
        return;
      }
      int w = surf->current.width;
      int h = surf->current.height;
      if (w <= 0 || h <= 0) {
        return;
      }
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
    wl_signal_add(&handle->surface->events.commit, &handle->commit);

    handle->unmap.notify = [](wl_listener* listener, void* /*data*/) {
      auto* handle =
        wl_container_of(listener, static_cast<XwaylandSurfaceHandle*>(nullptr), unmap);
      if (handle->server && handle->surface) {
        handle->server->pending_wl_actions.push_back(
          PendingWlAction{ PendingWlAction::Remove, handle->app, handle->surface });
        handle->registered = false;
        ensure_wayland_apps_registered(handle->server);
      }
      safe_remove_listener(&handle->unmap);
    };
    wl_signal_add(&handle->surface->events.unmap, &handle->unmap);

    if (handle->server->primary_output) {
      wlr_surface_send_enter(handle->surface, handle->server->primary_output);
    }
  };
  wl_signal_add(&xsurface->events.associate, &handle->associate);

  handle->dissociate.notify = [](wl_listener* listener, void* /*data*/) {
    auto* handle =
      wl_container_of(listener, static_cast<XwaylandSurfaceHandle*>(nullptr), dissociate);
    if (!handle) {
      return;
    }
    if (handle->server && handle->surface) {
      handle->server->pending_wl_actions.push_back(
        PendingWlAction{ PendingWlAction::Remove, handle->app, handle->surface });
      ensure_wayland_apps_registered(handle->server);
    }
    handle->registered = false;
    safe_remove_listener(&handle->commit);
    safe_remove_listener(&handle->unmap);
    handle->surface = nullptr;
  };
  wl_signal_add(&xsurface->events.dissociate, &handle->dissociate);

  handle->destroy.notify = [](wl_listener* listener, void* /*data*/) {
    auto* handle =
      wl_container_of(listener, static_cast<XwaylandSurfaceHandle*>(nullptr), destroy);
    if (!handle) {
      return;
    }
    if (handle->server && handle->surface) {
      handle->server->pending_wl_actions.push_back(
        PendingWlAction{ PendingWlAction::Remove, handle->app, handle->surface });
      ensure_wayland_apps_registered(handle->server);
    }
    safe_remove_listener(&handle->associate);
    safe_remove_listener(&handle->dissociate);
    safe_remove_listener(&handle->destroy);
    safe_remove_listener(&handle->request_configure);
    safe_remove_listener(&handle->commit);
    safe_remove_listener(&handle->unmap);
    delete handle;
  };
  wl_signal_add(&xsurface->events.destroy, &handle->destroy);

  if (xsurface->surface) {
    handle->surface = xsurface->surface;
    handle->accessory = xsurface->override_redirect;
    handle->parent_surface = xsurface->parent ? xsurface->parent->surface : nullptr;
    if (handle->parent_surface && xsurface->parent) {
      handle->offset_x = xsurface->x - xsurface->parent->x;
      handle->offset_y = xsurface->y - xsurface->parent->y;
    }
    handle->app = create_xwayland_app_object(server, xsurface);
    handle->commit.notify = [](wl_listener* listener, void* data) {
      auto* handle =
        wl_container_of(listener, static_cast<XwaylandSurfaceHandle*>(nullptr), commit);
      auto* surf = static_cast<wlr_surface*>(data);
      if (!handle->server || !surf || handle->registered) {
        return;
      }
      int w = surf->current.width;
      int h = surf->current.height;
      if (w <= 0 || h <= 0) {
        return;
      }
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
    wl_signal_add(&handle->surface->events.commit, &handle->commit);

    handle->unmap.notify = [](wl_listener* listener, void* /*data*/) {
      auto* handle =
        wl_container_of(listener, static_cast<XwaylandSurfaceHandle*>(nullptr), unmap);
      if (handle->server && handle->surface) {
        handle->server->pending_wl_actions.push_back(
          PendingWlAction{ PendingWlAction::Remove, handle->app, handle->surface });
        handle->registered = false;
        ensure_wayland_apps_registered(handle->server);
      }
      safe_remove_listener(&handle->unmap);
    };
    wl_signal_add(&handle->surface->events.unmap, &handle->unmap);
    if (handle->server->primary_output) {
      wlr_surface_send_enter(handle->surface, handle->server->primary_output);
    }
  }
}
