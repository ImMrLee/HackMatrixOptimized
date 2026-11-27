#include "wayland/pointer.h"
#include "wayland_app.h"
#include "engine.h"
#include "wayland/wlr_compositor.h"
#include "WindowManager/Space.h"
#include "input_activity.h"
#include "time_utils.h"

extern "C" {
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/util/log.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_relative_pointer_v1.h>
#include <wlr/types/wlr_pointer_constraints_v1.h>
#include <linux/input-event-codes.h>
}

std::pair<double, double>
output_local_pointer(WlrServer* server, wlr_output* output)
{
  if (!server) {
    return { 0.0, 0.0 };
  }

  double x = server->pointer_x;
  double y = server->pointer_y;
  if (server->output_layout && output) {
    wlr_box box = {};
    wlr_output_layout_get_box(server->output_layout, output, &box);
    x -= static_cast<double>(box.x);
    y -= static_cast<double>(box.y);
  }
  return { x, y };
}

static MultiPlayer::Client*
remoteWindowController(WlrServer* server)
{
  if (!server || !server->engine) {
    return nullptr;
  }
  auto client = server->engine->getClient();
  if (!client || !client->isControllingRemote()) {
    return nullptr;
  }
  return client.get();
}

static bool free_view_active(WlrServer* server);

static std::optional<WindowManager::AppRayHit>
remoteWindowUnderPointer(WlrServer* server)
{
  if (!server || !server->engine || !server->primary_output) {
    return std::nullopt;
  }
  auto wm = server->engine->getWindowManager();
  if (!wm) {
    return std::nullopt;
  }
  auto space = wm->getSpace();
  if (!space) {
    return std::nullopt;
  }
  auto pointer = output_local_pointer(server, server->primary_output);
  return space->raycastRemoteAppFromScreen(
    static_cast<float>(pointer.first),
    static_cast<float>(pointer.second),
    static_cast<float>(server->primary_output->width),
    static_cast<float>(server->primary_output->height),
    20.0f);
}

static std::optional<WindowManager::AppRayHit>
remoteWindowUnderCrosshair(WlrServer* server)
{
  if (!server || !server->engine || !server->primary_output) {
    return std::nullopt;
  }
  auto wm = server->engine->getWindowManager();
  if (!wm) {
    return std::nullopt;
  }
  auto space = wm->getSpace();
  if (!space) {
    return std::nullopt;
  }
  const float width = static_cast<float>(server->primary_output->width);
  const float height = static_cast<float>(server->primary_output->height);
  return space->raycastRemoteAppFromScreen(
    width * 0.5f, height * 0.5f, width, height, 10000.0f);
}

static std::optional<WindowManager::AppRayHit>
remoteWindowUnderAim(WlrServer* server)
{
  if (free_view_active(server)) {
    return remoteWindowUnderCrosshair(server);
  }
  return remoteWindowUnderPointer(server);
}

static void
clear_active_pointer_constraint_listener(WlrServer* server)
{
  if (!server) {
    return;
  }
  if (server->active_pointer_constraint_destroy_linked) {
    wl_list_remove(&server->active_pointer_constraint_destroy.link);
    server->active_pointer_constraint_destroy_linked = false;
  }
  server->active_pointer_constraint_destroy.link.prev = nullptr;
  server->active_pointer_constraint_destroy.link.next = nullptr;
}

void
update_pointer_constraint(WlrServer* server, wlr_surface* focused_surface)
{
  if (!server || !server->seat || !server->pointer_constraints) {
    return;
  }

  auto* desired = focused_surface
                    ? wlr_pointer_constraints_v1_constraint_for_surface(
                        server->pointer_constraints, focused_surface, server->seat)
                    : nullptr;
  if (desired == server->active_pointer_constraint) {
    return;
  }

  if (server->active_pointer_constraint) {
    wlr_pointer_constraint_v1_send_deactivated(server->active_pointer_constraint);
    server->active_pointer_constraint = nullptr;
  }
  clear_active_pointer_constraint_listener(server);

  if (!desired) {
    return;
  }

  server->active_pointer_constraint = desired;
  server->active_pointer_constraint_destroy.notify = [](wl_listener* listener, void* /*data*/) {
    auto* server = wl_container_of(
      listener, static_cast<WlrServer*>(nullptr), active_pointer_constraint_destroy);
    if (!server) {
      return;
    }
    server->active_pointer_constraint = nullptr;
    clear_active_pointer_constraint_listener(server);
  };
  wl_signal_add(&desired->events.destroy, &server->active_pointer_constraint_destroy);
  server->active_pointer_constraint_destroy_linked = true;
  wlr_pointer_constraint_v1_send_activated(desired);
}

bool
wayland_pointer_locked(WlrServer* server)
{
  return server && server->active_pointer_constraint &&
         server->active_pointer_constraint->type == WLR_POINTER_CONSTRAINT_V1_LOCKED;
}

bool isValidWaylandAppComponent(WlrServer* server, entt::entity entity) {
	return server->registry->valid(entity) &&
		server->registry->all_of<WaylandApp::Component>(entity);
}

static bool
cursor_override_visible(WlrServer* server)
{
  if (!server || !server->engine) {
    return false;
  }
  auto wm = server->engine->getWindowManager();
  if (!wm) {
    return false;
  }
  return wm->getCursorVisibleOverride().value_or(false);
}

static bool
hover_focus_enabled(WlrServer* server)
{
  if (!server || !server->engine) {
    return false;
  }
  auto wm = server->engine->getWindowManager();
  if (!wm) {
    return false;
  }
  if (!cursor_override_visible(server) || server->engine->imguiWantsMouse()) {
    return false;
  }
  return !wm->getCurrentlyFocusedApp().has_value();
}

static wlr_surface*
surface_for_entity(WlrServer* server, entt::entity entity)
{
  if (!server || entity == entt::null) {
    return nullptr;
  }
  for (const auto& [surface, mappedEntity] : server->surface_map) {
    if (mappedEntity == entity) {
      return surface;
    }
  }
  return nullptr;
}

struct HoveredSurfaceInfo
{
  wlr_surface* surface = nullptr;
  entt::entity entity = entt::null;
  double sx = 0.0;
  double sy = 0.0;
};

static std::optional<std::pair<double, double>>
project_pointer_to_surface(WlrServer* server, wlr_surface* surface)
{
  if (!server || !server->engine || !server->primary_output || !surface) {
    return std::nullopt;
  }
  if (!hover_focus_enabled(server)) {
    return std::nullopt;
  }
  auto it = server->surface_map.find(surface);
  if (it == server->surface_map.end()) {
    return std::nullopt;
  }
  auto wm = server->engine->getWindowManager();
  if (!wm) {
    return std::nullopt;
  }
  auto space = wm->getSpace();
  if (!space) {
    return std::nullopt;
  }
  auto pointer = output_local_pointer(server, server->primary_output);
  auto hit = space->raycastAppFromScreen(static_cast<float>(pointer.first),
                                         static_cast<float>(pointer.second),
                                         static_cast<float>(server->primary_output->width),
                                         static_cast<float>(server->primary_output->height),
                                         20.0f);
  if (!hit.has_value() || hit->entity != it->second) {
    return std::nullopt;
  }
  return std::make_pair(static_cast<double>(hit->surfacePixels.x),
                        static_cast<double>(hit->surfacePixels.y));
}

static std::optional<HoveredSurfaceInfo>
pick_hovered_surface(WlrServer* server)
{
  if (!server || !server->engine || !server->primary_output) {
    return std::nullopt;
  }
  auto wm = server->engine->getWindowManager();
  if (!wm) {
    return std::nullopt;
  }
  auto space = wm->getSpace();
  if (!space) {
    return std::nullopt;
  }
  auto pointer = output_local_pointer(server, server->primary_output);
  auto hit = space->raycastAppFromScreen(static_cast<float>(pointer.first),
                                         static_cast<float>(pointer.second),
                                         static_cast<float>(server->primary_output->width),
                                         static_cast<float>(server->primary_output->height),
                                         20.0f);
  if (!hit.has_value()) {
    return std::nullopt;
  }
  auto* surf = surface_for_entity(server, hit->entity);
  if (!surf) {
    return std::nullopt;
  }
  return HoveredSurfaceInfo{ surf,
                             hit->entity,
                             static_cast<double>(hit->surfacePixels.x),
                             static_cast<double>(hit->surfacePixels.y) };
}

static void
ensure_pointer_focus_at(WlrServer* server,
                        wlr_surface* surf,
                        double sx,
                        double sy,
                        uint32_t time_msec);

void
sync_cursor_mode_pointer_focus(WlrServer* server)
{
  if (!hover_focus_enabled(server) || !server || !server->seat) {
    return;
  }
  if (auto hovered = pick_hovered_surface(server)) {
    if (server->engine) {
      if (auto wm = server->engine->getWindowManager()) {
        wm->setCursorInputFocus(hovered->entity);
      }
    }
    ensure_pointer_focus_at(server,
                            hovered->surface,
                            hovered->sx,
                            hovered->sy,
                            0);
    return;
  }
  if (server->engine) {
    if (auto wm = server->engine->getWindowManager()) {
      wm->clearCursorInputFocus();
    }
  }
  if (server->seat->pointer_state.focused_surface) {
    wlr_seat_pointer_notify_clear_focus(server->seat);
    update_pointer_constraint(server, nullptr);
  }
}

void
clear_cursor_mode_input_focus(WlrServer* server)
{
  if (!server || !server->seat) {
    return;
  }
  if (server->engine) {
    if (auto wm = server->engine->getWindowManager()) {
      wm->clearCursorInputFocus();
    }
  }
  if (server->seat->pointer_state.focused_surface) {
    wlr_seat_pointer_notify_clear_focus(server->seat);
    update_pointer_constraint(server, nullptr);
  }
}

// Map the global pointer into surface-local coords assuming the surface is
// centered in the output (matches render overlay) and clamp to surface size.
//
// TODO: Remove the clamp which is causing inputs that aren't on the window to be passed into the window.
static std::pair<double, double>
map_pointer_to_surface(WlrServer* server, wlr_surface* surf)
{
  if (server && surf) {
    if (auto projected = project_pointer_to_surface(server, surf)) {
      return *projected;
    }
  }

  auto pointer = output_local_pointer(
    server, server ? server->primary_output : nullptr);
  double sx = pointer.first;
  double sy = pointer.second;
  double surf_w = surf ? surf->current.width : 0;
  double surf_h = surf ? surf->current.height : 0;

  bool handledPopupCoords = false;
  bool handledLayerCoords = false;
  // For popup/accessory surfaces, place the pointer relative to the parent's
  // top-left using the same math as renderWaylandPopup().
  if (server && surf && server->registry) {
    auto it = server->surface_map.find(surf);
    if (it != server->surface_map.end() && isValidWaylandAppComponent(server, it->second)) {
      auto& comp = server->registry->get<WaylandApp::Component>(it->second);
      if (comp.layer_shell) {
        // Layer surfaces (wofi, panels, notifications) are positioned by their
        // anchors/margins, not centered in the output. Use the same screen rect
        // the renderer blits to.
        sx -= comp.screen_x;
        sy -= comp.screen_y;
        handledLayerCoords = true;
      } else if (comp.accessory && isValidWaylandAppComponent(server, comp.parent)) {
        auto& parentComp = server->registry->get<WaylandApp::Component>(comp.parent);
        if (parentComp.app) {
          int parentW = parentComp.app->getWidth();
          int parentH = parentComp.app->getHeight();
          double out_w = server->primary_output->width;
          double out_h = server->primary_output->height;
          double parentLeft = std::max(0.0, (out_w - parentW) * 0.5);
          double parentTop = std::max(0.0, (out_h - parentH) * 0.5);
          double popupLeft = parentLeft + comp.offset_x;
          double popupTop = parentTop + comp.offset_y;
          sx -= popupLeft;
          sy -= popupTop;
          handledPopupCoords = true;
        }
      }
    }
  }
  // Subtract the center offset so (0,0) maps to the surface top-left.
  // Skip this for popups and layer shells, which already derived
  // screen-relative coords from their actual on-screen rectangles.
  if (!handledPopupCoords && !handledLayerCoords && server &&
      server->primary_output) {
    double out_w = server->primary_output->width;
    double out_h = server->primary_output->height;
    if (out_w > surf_w && surf_w > 0) {
      sx -= (out_w - surf_w) / 2.0;
    }
    if (out_h > surf_h && surf_h > 0) {
      sy -= (out_h - surf_h) / 2.0;
    }
  }
  // Use raw pointer coords in layout space and clamp into the surface.
  if (surf_w > 0) {
    sx = std::clamp(sx, 0.0, surf_w - 1.0);
  }
  if (surf_h > 0) {
    sy = std::clamp(sy, 0.0, surf_h - 1.0);
  }
  return { sx, sy };
}

static void
ensure_pointer_focus_at(WlrServer* server,
                        wlr_surface* surf,
                        double sx,
                        double sy,
                        uint32_t time_msec = 0)
{
  if (!server || !server->seat || !surf) {
    return;
  }
  if (time_msec == 0) {
    uint64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count();
    time_msec = static_cast<uint32_t>(now_ms & 0xffffffff);
  }
  wlr_seat_pointer_notify_enter(server->seat, surf, sx, sy);
  wlr_seat_pointer_notify_motion(server->seat, time_msec, sx, sy);
  wlr_seat_pointer_notify_frame(server->seat);
  update_pointer_constraint(server, surf);
}

static void
ensure_pointer_focus(WlrServer* server, uint32_t time_msec = 0, wlr_surface* preferred = nullptr)
{
  if (!server || !server->seat) {
    return;
  }
  // Prefer an existing focused surface if present.
  wlr_surface* surf = preferred ? preferred : server->seat->pointer_state.focused_surface;
  // If WM has a different focused Wayland app, prefer that surface so hotkey
  // focus also updates pointer focus.
  if (server->engine && server->registry) {
    if (auto wm = server->engine->getWindowManager()) {
      if (auto focused = wm->getCurrentlyFocusedApp();
          focused && server->registry->valid(*focused) &&
          server->registry->all_of<WaylandApp::Component>(*focused)) {
        auto& comp = server->registry->get<WaylandApp::Component>(*focused);
        wlr_surface* wmSurf = comp.app ? comp.app->getSurface() : nullptr;
        if (wmSurf && wmSurf != surf) {
          surf = wmSurf;
        }
      }
    }
  }
  if (!surf) {
    if (server->engine && server->registry) {
      if (auto wm = server->engine->getWindowManager()) {
        if (auto focused = wm->getCurrentlyFocusedApp();
            focused && server->registry->valid(*focused) &&
            server->registry->all_of<WaylandApp::Component>(*focused)) {
          auto& comp = server->registry->get<WaylandApp::Component>(*focused);
          if (comp.app) {
            surf = comp.app->getSurface();
          }
        }
      }
    }
    // Fallback: pick the first known Wayland surface.
    if (!surf && !server->surface_map.empty()) {
      surf = server->surface_map.begin()->first;
    }
  }
  if (!surf) {
    return;
  }
  auto mapped = map_pointer_to_surface(server, surf);
  ensure_pointer_focus_at(server, surf, mapped.first, mapped.second, time_msec);
}

static std::pair<wlr_surface*, entt::entity>
_pick_any_surface(WlrServer* server)
{
  if (!server) {
    return { nullptr, entt::null };
  }
  if (!server->surface_map.empty()) {
    auto it = server->surface_map.begin();
    return { it->first, it->second };
  }
  return { nullptr, entt::null };
}

// Free look owns the screen centre: there is no cursor to raycast from, so
// click-through always aims along the centre ray.
static bool
free_view_active(WlrServer* server)
{
  if (!server || !server->engine) {
    return false;
  }
  auto* controls = server->engine->getControls();
  auto wm = server->engine->getWindowManager();
  if (!controls || !wm || !controls->isCursorGrabbed()) {
    return false;
  }
  return !wm->hasCurrentOrPendingFocus();
}

// Free view has no cursor, so ImGui must not see the hidden pointer: otherwise
// the debug panels hover/click and can steal keys or resize themselves.
static void
feed_imgui_pointer(WlrServer* server, bool freeLook)
{
  if (!server || !server->engine) {
    return;
  }
  if (freeLook) {
    const bool noButtons[3] = { false, false, false };
    server->engine->updateImGuiPointer(-100000.0f, -100000.0f, noButtons);
    return;
  }
  server->engine->updateImGuiPointer(
    server->pointer_x, server->pointer_y, server->input.mouse_buttons);
}

bool
free_view_pointer_active(const WlrServer* server)
{
  if (!server) {
    return false;
  }
  return server->free_view_pointer_entity != entt::null ||
         !server->free_view_buttons.empty();
}

static bool
free_view_aim_hit(WlrServer* server, WindowManager::AppRayHit& hit)
{
  if (!server || !server->engine || !server->primary_output) {
    return false;
  }
  auto wm = server->engine->getWindowManager();
  if (!wm) {
    return false;
  }
  auto space = wm->getSpace();
  if (!space) {
    return false;
  }
  const float width = static_cast<float>(server->primary_output->width);
  const float height = static_cast<float>(server->primary_output->height);
  if (width <= 0.0f || height <= 0.0f) {
    return false;
  }
  // A window can be clicked anywhere it is visible, not just within R's
  // go-to range. The nearest hit still wins, so this only widens reach.
  constexpr float kCrosshairRayDistance = 10000.0f;
  auto found = space->raycastAppFromScreen(
    width * 0.5f, height * 0.5f, width, height, kCrosshairRayDistance);
  if (!found.has_value()) {
    return false;
  }
  hit = *found;
  return true;
}

// Aim for window move/resize: crosshair in free view, real pointer when the
// cursor is visible.
static bool
window_aim_hit(WlrServer* server, WindowManager::AppRayHit& hit)
{
  if (!server || !server->engine || !server->primary_output) {
    return false;
  }
  auto* controls = server->engine->getControls();
  if (controls && controls->isCursorGrabbed()) {
    return free_view_aim_hit(server, hit);
  }
  auto wm = server->engine->getWindowManager();
  if (!wm) {
    return false;
  }
  auto space = wm->getSpace();
  if (!space) {
    return false;
  }
  const float width = static_cast<float>(server->primary_output->width);
  const float height = static_cast<float>(server->primary_output->height);
  if (width <= 0.0f || height <= 0.0f) {
    return false;
  }
  auto pointer = output_local_pointer(server, server->primary_output);
  auto found = space->raycastAppFromScreen(
    static_cast<float>(pointer.first),
    static_cast<float>(pointer.second),
    width,
    height,
    10000.0f);
  if (!found.has_value()) {
    return false;
  }
  hit = *found;
  return true;
}

struct FreeViewHit
{
  wlr_surface* surface = nullptr;
  entt::entity entity = entt::null;
  double x = 0.0;
  double y = 0.0;
};

// Popups, menus and layer-shell panels are drawn in screen space above the
// world. Hit-test them first so the crosshair can operate dropdowns and
// context menus that overlap the window behind them.
static bool
pick_free_view_overlay(WlrServer* server, FreeViewHit& hit)
{
  if (!server || !server->registry || !server->primary_output) {
    return false;
  }
  const double centreX =
    static_cast<double>(server->primary_output->width) * 0.5;
  const double centreY =
    static_cast<double>(server->primary_output->height) * 0.5;

  bool found = false;
  auto view = server->registry->view<WaylandApp::Component>();
  for (auto entity : view) {
    auto& component = view.get<WaylandApp::Component>(entity);
    if (!component.app || (!component.accessory && !component.layer_shell)) {
      continue;
    }
    auto* surface = component.app->getSurface();
    if (!surface) {
      continue;
    }
    const int width = surface->current.width > 0
                        ? surface->current.width
                        : component.app->getWidth();
    const int height = surface->current.height > 0
                         ? surface->current.height
                         : component.app->getHeight();
    if (width <= 0 || height <= 0) {
      continue;
    }

    double left = 0.0;
    double top = 0.0;
    if (component.layer_shell) {
      left = static_cast<double>(component.screen_x);
      top = static_cast<double>(component.screen_y);
    } else {
      if (!server->registry->valid(component.parent) ||
          !server->registry->all_of<WaylandApp::Component>(
            component.parent)) {
        continue;
      }
      auto& parent =
        server->registry->get<WaylandApp::Component>(component.parent);
      if (!parent.app) {
        continue;
      }
      const double outputWidth =
        static_cast<double>(server->primary_output->width);
      const double outputHeight =
        static_cast<double>(server->primary_output->height);
      const double parentLeft =
        std::max(0.0, (outputWidth - parent.app->getWidth()) * 0.5);
      const double parentTop =
        std::max(0.0, (outputHeight - parent.app->getHeight()) * 0.5);
      left = parentLeft + static_cast<double>(component.offset_x);
      top = parentTop + static_cast<double>(component.offset_y);
    }

    if (centreX < left || centreX >= left + width || centreY < top ||
        centreY >= top + height) {
      continue;
    }
    // Later entities are registered later, so keep the last match as the
    // topmost popup.
    hit.surface = surface;
    hit.entity = entity;
    hit.x = centreX - left;
    hit.y = centreY - top;
    found = true;
  }
  return found;
}

static bool
pick_free_view_hit(WlrServer* server, FreeViewHit& hit)
{
  if (pick_free_view_overlay(server, hit)) {
    return true;
  }

  WindowManager::AppRayHit world;
  if (!free_view_aim_hit(server, world)) {
    return false;
  }
  wlr_surface* surface = surface_for_entity(server, world.entity);
  if (!surface) {
    return false;
  }
  hit.surface = surface;
  hit.entity = world.entity;
  hit.x = static_cast<double>(world.surfacePixels.x);
  hit.y = static_cast<double>(world.surfacePixels.y);
  return true;
}

static entt::entity
free_view_toplevel_owner(WlrServer* server, entt::entity entity)
{
  if (!server || !server->registry || !server->registry->valid(entity)) {
    return entt::null;
  }
  for (int depth = 0; depth < 8; ++depth) {
    auto* component =
      server->registry->try_get<WaylandApp::Component>(entity);
    if (!component) {
      return entt::null;
    }
    if (component->accessory && component->parent != entt::null) {
      entity = component->parent;
      continue;
    }
    return entity;
  }
  return entt::null;
}

static void
deactivate_free_view_owner(WlrServer* server)
{
  if (!server || !server->registry ||
      server->free_view_active_entity == entt::null ||
      !server->registry->valid(server->free_view_active_entity)) {
    server->free_view_active_entity = entt::null;
    if (server->engine) {
      if (auto wm = server->engine->getWindowManager()) {
        wm->setPopupOwner(entt::null);
      }
    }
    return;
  }
  if (auto* component = server->registry->try_get<WaylandApp::Component>(
        server->free_view_active_entity)) {
    if (component->app && !component->accessory &&
        !component->layer_shell) {
      component->app->setActivated(false);
    }
  }
  server->free_view_active_entity = entt::null;
  if (server->engine) {
    if (auto wm = server->engine->getWindowManager()) {
      wm->setPopupOwner(entt::null);
    }
  }
}

static void
activate_free_view_owner(WlrServer* server, entt::entity entity)
{
  if (!server || !server->registry) {
    return;
  }
  const entt::entity owner = free_view_toplevel_owner(server, entity);
  if (owner == server->free_view_active_entity) {
    return;
  }
  deactivate_free_view_owner(server);
  server->free_view_active_entity = owner;
  if (server->engine) {
    if (auto wm = server->engine->getWindowManager()) {
      wm->setPopupOwner(owner);
    }
  }
  if (owner == entt::null || !server->registry->valid(owner)) {
    return;
  }
  if (auto* component =
        server->registry->try_get<WaylandApp::Component>(owner)) {
    if (component->app && !component->accessory &&
        !component->layer_shell) {
      component->app->setActivated(true);
    }
  }
}

static void
focus_free_view_hit(WlrServer* server,
                    const FreeViewHit& hit,
                    uint32_t time_msec)
{
  if (!server || !server->seat || !hit.surface) {
    return;
  }
  if (server->free_view_pointer_entity == hit.entity &&
      server->seat->pointer_state.focused_surface == hit.surface) {
    wlr_seat_pointer_notify_motion(server->seat, time_msec, hit.x, hit.y);
    wlr_seat_pointer_notify_frame(server->seat);
    return;
  }
  server->free_view_pointer_entity = hit.entity;
  activate_free_view_owner(server, hit.entity);
  ensure_pointer_focus_at(server, hit.surface, hit.x, hit.y, time_msec);
}

static void
clear_free_view_focus(WlrServer* server)
{
  if (!server) {
    return;
  }
  server->free_view_pointer_entity = entt::null;
  deactivate_free_view_owner(server);
  if (!server->seat) {
    return;
  }
  if (server->seat->pointer_state.focused_surface) {
    wlr_seat_pointer_notify_clear_focus(server->seat);
    update_pointer_constraint(server, nullptr);
  }
}

static void
leave_free_view_input(WlrServer* server)
{
  if (!server) {
    return;
  }
  if (server->free_view_pointer_entity == entt::null &&
      server->free_view_click_entity == entt::null &&
      server->free_view_buttons.empty()) {
    return;
  }
  server->free_view_pointer_entity = entt::null;
  server->free_view_click_entity = entt::null;
  server->free_view_buttons.clear();
  auto wm = server->engine ? server->engine->getWindowManager() : nullptr;
  auto focused = wm ? wm->getCurrentlyFocusedApp() : std::nullopt;
  if (!focused.has_value()) {
    clear_free_view_focus(server);
    return;
  }
  // R took keyboard focus. Keep that window activated; deactivate only a
  // different window that free look happened to hover.
  if (server->free_view_active_entity != entt::null &&
      server->free_view_active_entity != *focused) {
    deactivate_free_view_owner(server);
  } else {
    server->free_view_active_entity = entt::null;
  }
  if (server->engine) {
    if (auto wm = server->engine->getWindowManager()) {
      wm->setPopupOwner(entt::null);
    }
  }
}

static bool
deliver_free_view_button(WlrServer* server,
                         uint32_t button,
                         bool pressed,
                         uint32_t time_msec)
{
  if (!server || !server->seat) {
    return false;
  }

  if (pressed) {
    FreeViewHit hit;
    if (!pick_free_view_hit(server, hit)) {
      return false;
    }
    focus_free_view_hit(server, hit, time_msec);
    server->free_view_click_entity = hit.entity;
    server->free_view_click_x = hit.x;
    server->free_view_click_y = hit.y;
    server->free_view_buttons.insert(button);

    wlr_seat_pointer_notify_button(
      server->seat, time_msec, button, WL_POINTER_BUTTON_STATE_PRESSED);
    wlr_seat_pointer_notify_frame(server->seat);
    return true;
  }

  if (server->free_view_buttons.find(button) ==
      server->free_view_buttons.end()) {
    return false;
  }
  server->free_view_buttons.erase(button);

  wlr_surface* surface =
    surface_for_entity(server, server->free_view_click_entity);
  if (surface) {
    if (server->seat->pointer_state.focused_surface != surface) {
      // Re-enter at the last aim point so the release reaches the app even if
      // the window was re-mapped or focus was cleared meanwhile.
      ensure_pointer_focus_at(server,
                              surface,
                              server->free_view_click_x,
                              server->free_view_click_y,
                              time_msec);
    } else {
      wlr_seat_pointer_notify_motion(server->seat,
                                     time_msec,
                                     server->free_view_click_x,
                                     server->free_view_click_y);
    }
    wlr_seat_pointer_notify_button(
      server->seat, time_msec, button, WL_POINTER_BUTTON_STATE_RELEASED);
    wlr_seat_pointer_notify_frame(server->seat);
  }

  if (server->free_view_buttons.empty()) {
    server->free_view_click_entity = entt::null;
    // Keep pointer focus on the window under the crosshair so hover state and
    // the following click work, but drop it if the aim moved away.
    FreeViewHit aim;
    if (!pick_free_view_hit(server, aim) ||
        aim.entity != server->free_view_pointer_entity) {
      clear_free_view_focus(server);
    }
  }
  return true;
}

static void
update_free_view_pointer(WlrServer* server, uint32_t time_msec)
{
  if (!server || !server->seat || server->free_view_buttons.empty() ||
      server->free_view_click_entity == entt::null) {
    return;
  }
  FreeViewHit hit;
  if (!pick_free_view_hit(server, hit) ||
      hit.entity != server->free_view_click_entity) {
    return;
  }
  server->free_view_click_x = hit.x;
  server->free_view_click_y = hit.y;
  if (server->seat->pointer_state.focused_surface == hit.surface) {
    wlr_seat_pointer_notify_motion(
      server->seat, time_msec, hit.x, hit.y);
    wlr_seat_pointer_notify_frame(server->seat);
  }
}

static void
update_free_view_hover(WlrServer* server, uint32_t time_msec)
{
  FreeViewHit hit;
  if (pick_free_view_hit(server, hit)) {
    focus_free_view_hit(server, hit, time_msec);
  } else {
    clear_free_view_focus(server);
  }
}

bool
wayland_pointer_focus_requested(WlrServer* server)
{
  if (!server || !server->engine) {
    return false;
  }
  // Free look keeps pointer focus only for crosshair clicks; it must not turn
  // the system cursor back on or disable camera look.
  if (free_view_active(server)) {
    return false;
  }
  if (server->seat) {
    // If the seat already has a focused surface (pointer or keyboard), treat that
    // as an active Wayland focus so we keep routing input and avoid flicker.
    if (server->seat->pointer_state.focused_surface) {
      return true;
    }
    if (server->seat->keyboard_state.focused_surface) {
      return true;
    }
  }
  // If ImGui wants the mouse, honor that as a pointer focus request.
  if (server->engine->imguiWantsMouse()) {
    return true;
  }
  auto wm = server->engine->getWindowManager();
  if (!wm || !server->registry) {
    return false;
  }
  auto focused = wm->getCurrentlyFocusedApp();
  if (!focused.has_value() || !server->registry->valid(*focused)) {
    return false;
  }
  return server->registry->all_of<WaylandApp::Component>(*focused);
}

void
handle_pointer_motion(wl_listener* listener, void* data)
{
  auto* handle =
    wl_container_of(listener, static_cast<WlrPointerHandle*>(nullptr), motion);
  auto* event = static_cast<wlr_pointer_motion_event*>(data);
  input_activity::lastLocalPointer.store(nowSeconds());
  handle->server->input.delta_x += event->delta_x;
  handle->server->input.delta_y += event->delta_y;
  if (handle->server->relative_pointer_manager && handle->server->seat) {
    uint64_t time_usec = static_cast<uint64_t>(event->time_msec) * 1000;
    wlr_relative_pointer_manager_v1_send_relative_motion(handle->server->relative_pointer_manager,
                                                         handle->server->seat,
                                                         time_usec,
                                                         event->delta_x,
                                                         event->delta_y,
                                                         event->unaccel_dx,
                                                         event->unaccel_dy);
  }
  bool locked_constraint = wayland_pointer_locked(handle->server);
  if (!locked_constraint && handle->server->cursor) {
    wlr_cursor_move(handle->server->cursor,
                    handle->server->last_pointer_device,
                    event->delta_x,
                    event->delta_y);
    handle->server->pointer_x = handle->server->cursor->x;
    handle->server->pointer_y = handle->server->cursor->y;
  }
  if (handle->server->engine) {
    feed_imgui_pointer(handle->server, free_view_active(handle->server));
    if (auto* controls = handle->server->engine->getControls()) {
      auto local =
        output_local_pointer(handle->server, handle->server->primary_output);
      controls->setPointerScreenPosition(static_cast<float>(local.first),
                                        static_cast<float>(local.second));
    }
  }
  if (handle->server->engine &&
      handle->server->engine->isExitConfirmOpen()) {
    // The exit dialog owns input; do not move hover focus or the camera.
    return;
  }

  if (auto* client = remoteWindowController(handle->server);
      client != nullptr && handle->server->engine &&
      !handle->server->engine->imguiWantsMouse()) {
    if (auto hit = remoteWindowUnderAim(handle->server);
        hit.has_value() && client->isControllingRemoteEntity(hit->entity)) {
      client->sendRemotePointerMotion(hit->surfacePixels.x,
                                      hit->surfacePixels.y);
    }
    return;
  }

  if (handle->server->engine) {
    if (auto* controls = handle->server->engine->getControls();
        controls && (controls->isWindowMoveActive() ||
                     controls->isWindowResizeActive())) {
      // The manipulation owns the mouse; do not move hover/pointer focus.
      return;
    }
  }
  if (free_view_active(handle->server)) {
    if (handle->server->free_view_buttons.empty()) {
      update_free_view_hover(handle->server, event->time_msec);
    } else {
      update_free_view_pointer(handle->server, event->time_msec);
    }
    return;
  }
  leave_free_view_input(handle->server);
  if (handle->server->engine &&
      handle->server->engine->imguiWantsMouse()) {
    // The overlay owns the pointer; never leak motion into a stale client.
    return;
  }

  bool focusRequested = wayland_pointer_focus_requested(handle->server);
  if (hover_focus_enabled(handle->server)) {
    if (auto hovered = pick_hovered_surface(handle->server)) {
      if (handle->server->engine) {
        if (auto wm = handle->server->engine->getWindowManager()) {
          wm->setCursorInputFocus(hovered->entity);
        }
      }
      ensure_pointer_focus_at(handle->server,
                              hovered->surface,
                              hovered->sx,
                              hovered->sy,
                              event->time_msec);
    } else if (handle->server->seat &&
               handle->server->seat->pointer_state.focused_surface) {
      clear_cursor_mode_input_focus(handle->server);
    }
  } else if (focusRequested) {
    auto surfEnt = _pick_any_surface(handle->server);
    auto* surf = surfEnt.first;
    ensure_pointer_focus(handle->server, event->time_msec, surf);
  }
  wlr_log(WLR_DEBUG,
          "pointer motion rel dx=%.3f dy=%.3f",
          event->delta_x,
          event->delta_y);
}

void
handle_pointer_motion_abs(wl_listener* listener, void* data)
{
  auto* handle = wl_container_of(
    listener, static_cast<WlrPointerHandle*>(nullptr), motion_abs);
  auto* event = static_cast<wlr_pointer_motion_absolute_event*>(data);
  input_activity::lastLocalPointer.store(nowSeconds());
  if (handle->server->input.have_abs) {
    // Wayland absolute motion arrives normalized to [0,1]. Scale it back into
    // output-space units so nested sessions don't feel like they only have a
    // fraction of the normal mouse range.
    double scaleX = 1000.0;
    double scaleY = 1000.0;
    if (handle->server && handle->server->primary_output) {
      if (handle->server->primary_output->width > 0) {
        scaleX = static_cast<double>(handle->server->primary_output->width);
      }
      if (handle->server->primary_output->height > 0) {
        scaleY = static_cast<double>(handle->server->primary_output->height);
      }
    }
    double dx = (event->x - handle->server->input.last_abs_x) * scaleX;
    double dy = (event->y - handle->server->input.last_abs_y) * scaleY;
    handle->server->input.delta_x += dx;
    handle->server->input.delta_y += dy;
  }
  handle->server->input.have_abs = true;
  handle->server->input.last_abs_x = event->x;
  handle->server->input.last_abs_y = event->y;
  if (handle->server->cursor) {
    wlr_cursor_warp_absolute(handle->server->cursor,
                             handle->server->last_pointer_device,
                             event->x,
                             event->y);
    handle->server->pointer_x = handle->server->cursor->x;
    handle->server->pointer_y = handle->server->cursor->y;
  }
  if (handle->server->engine) {
    feed_imgui_pointer(handle->server, free_view_active(handle->server));
    if (auto* controls = handle->server->engine->getControls()) {
      auto local =
        output_local_pointer(handle->server, handle->server->primary_output);
      controls->setPointerScreenPosition(static_cast<float>(local.first),
                                        static_cast<float>(local.second));
    }
  }
  if (handle->server->engine &&
      handle->server->engine->isExitConfirmOpen()) {
    // The exit dialog owns input; do not move hover focus or the camera.
    return;
  }

  if (auto* client = remoteWindowController(handle->server);
      client != nullptr && handle->server->engine &&
      !handle->server->engine->imguiWantsMouse()) {
    if (auto hit = remoteWindowUnderAim(handle->server);
        hit.has_value() && client->isControllingRemoteEntity(hit->entity)) {
      client->sendRemotePointerMotion(hit->surfacePixels.x,
                                      hit->surfacePixels.y);
    }
    return;
  }

  if (handle->server->engine) {
    if (auto* controls = handle->server->engine->getControls();
        controls && (controls->isWindowMoveActive() ||
                     controls->isWindowResizeActive())) {
      // The manipulation owns the mouse; do not move hover/pointer focus.
      return;
    }
  }
  if (free_view_active(handle->server)) {
    if (handle->server->free_view_buttons.empty()) {
      update_free_view_hover(handle->server, event->time_msec);
    } else {
      update_free_view_pointer(handle->server, event->time_msec);
    }
    return;
  }
  leave_free_view_input(handle->server);
  if (handle->server->engine &&
      handle->server->engine->imguiWantsMouse()) {
    // The overlay owns the pointer; never leak motion into a stale client.
    return;
  }

  bool focusRequested = wayland_pointer_focus_requested(handle->server);
  if (hover_focus_enabled(handle->server)) {
    if (auto hovered = pick_hovered_surface(handle->server)) {
      if (handle->server->engine) {
        if (auto wm = handle->server->engine->getWindowManager()) {
          wm->setCursorInputFocus(hovered->entity);
        }
      }
      ensure_pointer_focus_at(handle->server,
                              hovered->surface,
                              hovered->sx,
                              hovered->sy,
                              event->time_msec);
    } else if (handle->server->seat &&
               handle->server->seat->pointer_state.focused_surface) {
      clear_cursor_mode_input_focus(handle->server);
    }
  } else if (focusRequested) {
    auto surfEnt = _pick_any_surface(handle->server);
    auto* surf = surfEnt.first;
    ensure_pointer_focus(handle->server, event->time_msec, surf);
  }
  wlr_log(WLR_DEBUG,
          "pointer motion abs dx=%.3f dy=%.3f",
          handle->server->input.delta_x,
          handle->server->input.delta_y);
}

void
handle_pointer_axis(wl_listener* listener, void* data)
{
  auto* handle =
    wl_container_of(listener, static_cast<WlrPointerHandle*>(nullptr), axis);
  auto* event = static_cast<wlr_pointer_axis_event*>(data);
  input_activity::lastLocalPointer.store(nowSeconds());

  if (auto* client = remoteWindowController(handle->server);
      client != nullptr && handle->server->engine &&
      !handle->server->engine->imguiWantsMouse()) {
    if (auto hit = remoteWindowUnderAim(handle->server);
        hit.has_value() && client->isControllingRemoteEntity(hit->entity)) {
      client->sendRemotePointerAxis(event->delta,
                                    event->delta_discrete,
                                    event->source,
                                    event->orientation);
    }
    return;
  }

  const bool freeLook = free_view_active(handle->server);
  if (!freeLook && handle->server->engine &&
      handle->server->engine->imguiWantsMouse()) {
    // The overlay consumes the wheel; do not scroll the last hovered window.
    return;
  }
  if (freeLook) {
    FreeViewHit hit;
    if (!pick_free_view_hit(handle->server, hit)) {
      return;
    }
    focus_free_view_hit(handle->server, hit, event->time_msec);
    wlr_seat_pointer_notify_axis(handle->server->seat,
                                 event->time_msec,
                                 event->orientation,
                                 event->delta,
                                 event->delta_discrete,
                                 event->source,
                                 event->relative_direction);
    wlr_seat_pointer_notify_frame(handle->server->seat);
    return;
  }

  wlr_surface* target = nullptr;
  if (handle->server && handle->server->seat) {
    target = handle->server->seat->pointer_state.focused_surface;
  }

  // Only deliver scroll to a surface that actually has pointer focus. The old
  // code fell back to an arbitrary surface, so scrolling in WM mode (no focused
  // app) was handled by an unfocused window and left it in an undefined state
  // (readme.md # Bugs). Hover focus sets pointer focus on motion, so scrolling
  // over a window still works.
  if (!target) {
    return;
  }

  ensure_pointer_focus(handle->server, event->time_msec, target);
  if (handle->server->seat) {
    wlr_seat_pointer_notify_axis(handle->server->seat,
                                 event->time_msec,
                                 event->orientation,
                                 event->delta,
                                 event->delta_discrete,
                                 event->source,
                                 event->relative_direction);
    wlr_seat_pointer_notify_frame(handle->server->seat);
  }
}

bool
inject_pointer_button(WlrServer* server,
                      uint32_t button,
                      bool pressed,
                      uint32_t time_msec)
{
  if (!server) {
    return false;
  }
  if (time_msec == 0) {
    const uint64_t now_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch())
        .count();
    time_msec = static_cast<uint32_t>(now_ms & 0xffffffff);
  }

  input_activity::lastLocalPointer.store(nowSeconds());
  bool handled = false;
  Controls* controls =
    server->engine ? server->engine->getControls() : nullptr;
  // In free look the cursor is hidden and the overlay must not swallow clicks
  // aimed through the crosshair.
  const bool freeLook = free_view_active(server);
  // Feed the ImGui overlay first. When it owns the mouse, swallow the click so
  // it does not also reach game controls or a focused client.
  bool imgui_captured = false;
  if (server->engine) {
    Engine* engine = server->engine.get();
    int slot = -1;
    if (button == BTN_LEFT) {
      slot = 0;
    } else if (button == BTN_RIGHT) {
      slot = 1;
    } else if (button == BTN_MIDDLE) {
      slot = 2;
    }
    if (slot >= 0) {
      server->input.mouse_buttons[slot] = pressed;
    }
    feed_imgui_pointer(server, freeLook);
    imgui_captured = engine->imguiWantsMouse() && !freeLook;
  }

  if (server->engine && server->engine->isExitConfirmOpen()) {
    // ImGui already received the click above; swallow the rest so the scene
    // behind the dialog cannot be operated.
    return true;
  }

  if (!freeLook) {
    leave_free_view_input(server);
  }

  if (!imgui_captured) {
    if (auto* client = remoteWindowController(server)) {
      if (pressed) {
        if (auto hit = remoteWindowUnderAim(server);
            hit.has_value() &&
            client->isControllingRemoteEntity(hit->entity)) {
          client->sendRemotePointerMotion(hit->surfacePixels.x,
                                          hit->surfacePixels.y);
          client->sendRemotePointerButton(button, true);
          handled = true;
        } else if (button == BTN_LEFT) {
          client->releaseRemoteControl();
        }
      } else {
        client->sendRemotePointerButton(button, false);
      }
      return handled;
    }
  }

  // Window manipulation works in free view and while the cursor is visible but
  // no app owns focus. With a focused app, input belongs to that app.
  bool manipMode = false;
  if (controls && !imgui_captured && server->engine) {
    auto wm = server->engine->getWindowManager();
    manipMode = controls->isCursorGrabbed() ||
                (wm && !wm->hasCurrentOrPendingFocus());
  }

  // A G-drag owns the mouse; swallow clicks so they do not reach the window.
  if (controls && manipMode && controls->isWindowMoveActive()) {
    return true;
  }

  // Aiming at a window edge/corner and pressing the left button starts a
  // resize drag instead of delivering the click. Consume the matching release
  // so the app never sees a stray press.
  if (controls && manipMode && button == BTN_LEFT) {
    if (pressed) {
      WindowManager::AppRayHit aim;
      if (window_aim_hit(server, aim) && controls->beginWindowResize(aim)) {
        return true;
      }
    } else if (controls->isWindowResizeActive()) {
      controls->endWindowResize();
      return true;
    }
  }

  // Free look: the crosshair at the screen centre points into the window.
  // Deliver press, drag and release there without ever taking keyboard focus.
  if (!imgui_captured && freeLook) {
    if (deliver_free_view_button(server, button, pressed, time_msec)) {
      return true;
    }
  }

  const bool wayland_focus_requested = wayland_pointer_focus_requested(server);
  wlr_surface* pointer_surface = nullptr;
  if (server->seat) {
    pointer_surface = server->seat->pointer_state.focused_surface;
  }
  wlr_surface* preferred_surface = nullptr;
  bool button_consumed_by_controls = false;
  if (pressed && controls && !wayland_focus_requested && !imgui_captured) {
    // If a Wayland client has (or is requesting) focus, bypass game controls so
    // the click reaches the client immediately.
    button_consumed_by_controls =
      controls->handlePointerButton(button, true);
    handled = handled || button_consumed_by_controls;
  }
  if (server->seat && !button_consumed_by_controls && !imgui_captured) {
    if (!preferred_surface && hover_focus_enabled(server)) {
      if (auto hovered = pick_hovered_surface(server)) {
        if (server->engine) {
          if (auto wm = server->engine->getWindowManager()) {
            wm->setCursorInputFocus(hovered->entity);
          }
        }
        preferred_surface = hovered->surface;
      }
    }
    wlr_surface* surf = preferred_surface ? preferred_surface : pointer_surface;
    if (surf) {
      ensure_pointer_focus(server, time_msec, surf);
      auto mapped = map_pointer_to_surface(server, surf);
      wlr_seat_pointer_notify_motion(
        server->seat, time_msec, mapped.first, mapped.second);
      wlr_seat_pointer_notify_button(
        server->seat, time_msec, button, pressed ? WL_POINTER_BUTTON_STATE_PRESSED
                                                 : WL_POINTER_BUTTON_STATE_RELEASED);
      wlr_seat_pointer_notify_frame(server->seat);
      handled = true;
    }
  }
  return handled;
}

void
handle_pointer_button(wl_listener* listener, void* data)
{
  auto* handle =
    wl_container_of(listener, static_cast<WlrPointerHandle*>(nullptr), button);
  auto* event = static_cast<wlr_pointer_button_event*>(data);
  inject_pointer_button(handle->server,
                        event->button,
                        static_cast<uint32_t>(event->state) ==
                          static_cast<uint32_t>(WL_POINTER_BUTTON_STATE_PRESSED),
                        event->time_msec);
}

void
handle_pointer_destroy(wl_listener* listener, void* data)
{
  (void)data;
  auto* handle =
    wl_container_of(listener, static_cast<WlrPointerHandle*>(nullptr), destroy);
  wl_list_remove(&handle->motion.link);
  wl_list_remove(&handle->motion_abs.link);
  wl_list_remove(&handle->button.link);
  wl_list_remove(&handle->axis.link);
  wl_list_remove(&handle->destroy.link);
  delete handle;
};

void
set_default_cursor(WlrServer* server, wlr_output* output = nullptr)
{
  if (!server || !server->cursor || !server->cursor_mgr) {
    return;
  }
  float scale = 1.0f;
  if (output) {
    scale = output->scale;
  } else if (server->primary_output) {
    scale = server->primary_output->scale;
  }
  if (scale <= 0.0f) {
    scale = 1.0f;
  }
  // Load the theme for the target scale and set a sane default pointer image.
  int loaded = wlr_xcursor_manager_load(server->cursor_mgr, scale);
  if (loaded < 0) {
    wlr_xcursor_manager_destroy(server->cursor_mgr);
    server->cursor_mgr = wlr_xcursor_manager_create("Adwaita", 24);
    if (server->cursor_mgr) {
      loaded = wlr_xcursor_manager_load(server->cursor_mgr, scale);
    }
  }
  if (loaded < 0 ||
      !wlr_xcursor_manager_get_xcursor(server->cursor_mgr, "left_ptr", scale)) {
    return;
  }
  wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "left_ptr");
  server->pointer_x = server->cursor->x;
  server->pointer_y = server->cursor->y;
}



void
set_cursor_visible(WlrServer* server, bool visible, wlr_output* output)
{
  if (!server || !server->cursor) {
    return;
  }
  if (server->engine) {
    if (auto wm = server->engine->getWindowManager()) {
      // If WM tracks a visibility override, respect it.
      if (auto sp = wm->getCursorVisibleOverride()) {
        visible = *sp;
      }
    }
  }
  if (wayland_pointer_locked(server)) {
    visible = false;
  }
  if (visible == server->cursor_visible) {
    return;
  }
  if (visible) {
    // Always ensure a sane default image; clients can override via set_cursor.
    set_default_cursor(server, output);
  } else {
    wlr_cursor_unset_image(server->cursor);
  }
  server->cursor_visible = visible;
}

void
handle_new_pointer(WlrServer* server, wlr_input_device* device) {
      auto* pointer = wlr_pointer_from_input_device(device);
      server->last_pointer_device = device;
      wlr_cursor_attach_input_device(server->cursor, device);

      auto* handle = new WlrPointerHandle();
      handle->server = server;
      handle->pointer = pointer;

      handle->motion.notify = handle_pointer_motion;
      wl_signal_add(&pointer->events.motion, &handle->motion);

      handle->motion_abs.notify = handle_pointer_motion_abs;
      wl_signal_add(&pointer->events.motion_absolute, &handle->motion_abs);

      handle->axis.notify = handle_pointer_axis;
      wl_signal_add(&pointer->events.axis, &handle->axis);

      handle->button.notify = handle_pointer_button; 
      wl_signal_add(&pointer->events.button, &handle->button);

      handle->destroy.notify = handle_pointer_destroy;
      wl_signal_add(&device->events.destroy, &handle->destroy);
}

void
handle_request_set_cursor(wl_listener* listener, void* data)
{
  auto* server = wl_container_of(
    listener, static_cast<WlrServer*>(nullptr), request_set_cursor);
  auto* event = static_cast<wlr_seat_pointer_request_set_cursor_event*>(data);
  if (!server || !server->cursor || !server->seat) {
    return;
  }
  if (free_view_pointer_active(server) && free_view_active(server)) {
    // Free-look crosshair focus is pointer-only; never show a client cursor
    // image or switch out of camera look.
    wlr_cursor_set_surface(server->cursor, nullptr, 0, 0);
    wlr_cursor_unset_image(server->cursor);
    server->cursor_visible = false;
    return;
  }
  if (wayland_pointer_locked(server)) {
    wlr_cursor_set_surface(server->cursor, nullptr, 0, 0);
    wlr_cursor_unset_image(server->cursor);
    server->cursor_visible = false;
    return;
  }
  // Only honor cursor requests from the focused client.
  if (server->seat->pointer_state.focused_client != event->seat_client) {
    return;
  }
  if (event->surface) {
    wlr_cursor_set_surface(
      server->cursor, event->surface, event->hotspot_x, event->hotspot_y);
    server->cursor_visible = true;
  } else {
    set_cursor_visible(server, wayland_pointer_focus_requested(server));
  }
  server->pointer_x = server->cursor->x;
  server->pointer_y = server->cursor->y;
}

void
create_cursor(WlrServer* server) {
    // Prefer the default theme, but allow fallback later if it is missing.
    server->cursor_mgr = wlr_xcursor_manager_create("default", 24);
    if (server->cursor_mgr) {
      wlr_xcursor_manager_load(server->cursor_mgr, 1);
    }
    server->cursor = wlr_cursor_create();
    if (server->cursor && server->output_layout) {
      wlr_cursor_attach_output_layout(server->cursor, server->output_layout);
    }

    server->request_set_cursor.notify = handle_request_set_cursor;
    wl_signal_add(&server->seat->events.request_set_cursor, &server->request_set_cursor);
    // Set an initial default cursor.
    set_cursor_visible(server, wayland_pointer_focus_requested(server));
}
