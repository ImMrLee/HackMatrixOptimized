#include "renderer.h"

#include <glad/glad.h>
#include <glm/glm.hpp>
#include <unordered_set>

#include "screen.h"
#include "tracy/Tracy.hpp"
#include "tracy/TracyOpenGL.hpp"

static bool
supportsFramebufferBlit()
{
  return (GLAD_GL_VERSION_3_0
#ifdef GLAD_GL_ES_VERSION_3_0
          || GLAD_GL_ES_VERSION_3_0
#endif
#ifdef GLAD_GL_EXT_framebuffer_blit
          || GLAD_GL_EXT_framebuffer_blit
#endif
          ) &&
         glBlitFramebuffer != nullptr;
}

void
Renderer::drawAppDirect(AppSurface* app, Bootable* bootable)
{
  int screenWidth = SCREEN_WIDTH;
  int screenHeight = SCREEN_HEIGHT;
  int appWidth = app->getWidth();
  int appHeight = app->getHeight();
  auto pos = app->getPosition();
  auto fbIt = frameBuffers.find(app->getTextureId());

    if (!bootable) {
      GLint prevReadFbo = 0;
      GLint prevDrawFbo = 0;
      glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevReadFbo);
      glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prevDrawFbo);
      glBindFramebuffer(GL_READ_FRAMEBUFFER, fbIt->second);
      glBindFramebuffer(GL_DRAW_FRAMEBUFFER, prevDrawFbo);
      glBlitFramebuffer(
        // src x1, y1 (flip)
        0,
        appHeight,
        // src x2, y2 (flip)
        appWidth,
        0,
        // dest x1,y1,x2,y2
        pos[0],
        pos[1],
        pos[0] + appWidth,
        pos[1] + appHeight,
        GL_COLOR_BUFFER_BIT,
        GL_NEAREST);
      glBindFramebuffer(GL_READ_FRAMEBUFFER, prevReadFbo);
    } else {
      glActiveTexture(GL_TEXTURE0);
      glBindTexture(GL_TEXTURE_2D, app->getTextureId());
      glBindVertexArray(DIRECT_RENDER_VAO);
      shader->setBool("directRender", true);
      static int x = -1;
      static int y = -1;
      static glm::mat4 model;
      if (x == -1 || y == -1 || x != bootable->x || y != bootable->y) {
        model = glm::mat4(1.0);

        model = glm::translate(
          model,
          glm::vec3((-0.5 + (float)appWidth / (float)screenWidth / 2 +
                     (float)bootable->x / (float)screenWidth) *
                      2,

                    -((-0.5 + (float)appHeight / (float)screenHeight / 2 +
                       (float)bootable->y / (float)screenHeight) *
                      2),

                    0));

        model = glm::scale(model,
          glm::vec3((float)appWidth / ((float)screenWidth),

                                     (float)appHeight / ((float)screenHeight),
                                     1));

        x = bootable->x;
        y = bootable->y;
      }
      shader->setBool("appTransparent", bootable->transparent);
      shader->setMatrix4("model", model);
      glDrawArrays(GL_TRIANGLES, 0, 6);
      shader->setBool("directRender", false);
    }
}

void Renderer::renderPopup(WaylandApp::Component& popup, WaylandApp::Component& parent) {
    auto* popupApp = popup.app.get();
    auto* parentApp = parent.app.get();
    if (!popupApp || !parentApp) {
      return;
    }
    int popupW = popupApp->getWidth();
    int popupH = popupApp->getHeight();
    int parentW = parentApp->getWidth();
    int parentH = parentApp->getHeight();
    if (popupW <= 0 || popupH <= 0 || parentW <= 0 || parentH <= 0) {
      return;
    }
    float parentLeft = std::max(0.0f, (SCREEN_WIDTH - parentW) * 0.5f);
    float parentTop = std::max(0.0f, (SCREEN_HEIGHT - parentH) * 0.5f);
    int destX = static_cast<int>(parentLeft + popup.offset_x);
    // Keep popups positioned in the same top-origin space as their parent.
    int destY = static_cast<int>(parentTop + popup.offset_y);

    if (popupApp->needsTextureImport()) {
      popupApp->appTexture();
    }
    auto fbIt = frameBuffers.find(popupApp->getTextureId());
    if (fbIt == frameBuffers.end()) {
      return;
    }
    if (!supportsFramebufferBlit()) {
      return;
    }
    GLint prevReadFbo = 0;
    GLint prevDrawFbo = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevReadFbo);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prevDrawFbo);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbIt->second);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, prevDrawFbo);
    glBlitFramebuffer(0,
                      0,
                      popupW,
                      popupH,
                      destX,
                      destY,
                      destX + popupW,
                      destY + popupH,
                      GL_COLOR_BUFFER_BIT,
                      GL_NEAREST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, prevReadFbo);
}

void
Renderer::renderApps()
{
  auto lookedAtAppEntity = windowManagerSpace->getLookedAtApp();
  static std::unordered_set<void*> loggedApps;
  auto wlPositionable =
    registry->view<WaylandApp::Component, Positionable>(entt::exclude<Bootable>);
  size_t wlCount = wlPositionable.size_hint();
  TracyGpuZone("render apps");
  shader->setBool("appSelected", false);

  shader->setBool("isApp", true);
  shader->setBool("directRender", false);
  glBindVertexArray(APP_VAO);
  // Defensive: ensure app quad attributes are enabled/bound even if VAO state
  // was clobbered by other GL paths.
  glBindBuffer(GL_ARRAY_BUFFER, APP_VBO);
  glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)0);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(
    1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)(3 * sizeof(float)));
  glEnableVertexAttribArray(1);
  glDisable(GL_CULL_FACE);

  auto bindAppTexture = [&](WaylandApp* app) {
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, app->getTextureId());
    return true;
  };
  auto renderLayerShell = [&](WaylandApp::Component& comp) {
    auto* app = comp.app.get();
    if (!app) {
      return;
    }
    int w = app->getWidth();
    int h = app->getHeight();
    if (w <= 0 || h <= 0) {
      return;
    }
    if (app->needsTextureImport()) {
      app->appTexture();
    }
    auto fbIt = frameBuffers.find(app->getTextureId());
    if (fbIt == frameBuffers.end()) {
      return;
    }
    if (!supportsFramebufferBlit()) {
      return;
    }
    GLint prevReadFbo = 0;
    GLint prevDrawFbo = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevReadFbo);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prevDrawFbo);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbIt->second);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, prevDrawFbo);
    const int targetW = comp.screen_w > 0 ? comp.screen_w : static_cast<int>(SCREEN_WIDTH);
    const int targetH = comp.screen_h > 0 ? comp.screen_h : static_cast<int>(SCREEN_HEIGHT);
    int destW = w;
    int destH = h;
    int destX = comp.screen_x;
    // comp.screen_y is in top-origin space; OpenGL blit target uses bottom-origin.
    int destY = targetH - comp.screen_y - destH;
    glBlitFramebuffer(0,
                      0,
                      w,
                      h,
                      destX,
                      destY,
                      destX + destW,
                      destY + destH,
                      GL_COLOR_BUFFER_BIT,
                      GL_NEAREST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, prevReadFbo);
  };

  // Wayland apps: render any that were registered by the wlroots backend.
  for (auto [entity, comp, positionable] : wlPositionable.each()) {
    auto* app = comp.app.get();
    if (!app) {
      continue;
    }

    // Upload latest buffer only when a new commit arrived, then bind to the
    // app's dedicated unit to avoid stale or shared textures when multiple
    // Wayland apps are present.
    if (app->needsTextureImport()) {
      app->appTexture();
    }
    if (!bindAppTexture(app)) {
      continue;
    }
    // Keep the in-world quad at the positionable's size; just track the app's
    // pixel size for logging and aspect ratio.
    float sx = static_cast<float>(app->getWidth()) /
               static_cast<float>(SCREEN_WIDTH);
    float sy = static_cast<float>(app->getHeight()) /
               static_cast<float>(SCREEN_HEIGHT);
    glm::mat4 model = positionable.modelMatrix;
    shader->setMatrix4("model", model);
    shader->setMatrix4("bootableScale", app->getHeightScalar());
    shader->setBool("appTransparent", false);
    glDrawArrays(GL_TRIANGLES, 0, 6);


    // If focused, also draw directly to screen to ensure visibility.
    if (wm && wm->getCurrentlyFocusedApp().has_value() &&
        wm->getCurrentlyFocusedApp().value() == entity) {
      if (!bindAppTexture(app)) {
        continue;
      }
      shader->setBool("directRender", true);
      glm::mat4 model = glm::mat4(1.0f);
      float sx = static_cast<float>(app->getWidth()) /
                 static_cast<float>(SCREEN_WIDTH);
      float sy = static_cast<float>(app->getHeight()) /
                 static_cast<float>(SCREEN_HEIGHT);
      model = glm::scale(model, glm::vec3(sx, sy, 1.0f));
      shader->setMatrix4("model", model);
      glBindVertexArray(DIRECT_RENDER_VAO);
      GLboolean depthMask = GL_TRUE;
      glGetBooleanv(GL_DEPTH_WRITEMASK, &depthMask);
      glDisable(GL_DEPTH_TEST);
      glDepthMask(GL_FALSE); // avoid clobbering depth for in-world apps
      glDrawArrays(GL_TRIANGLES, 0, 6);
      glDepthMask(depthMask);
      glEnable(GL_DEPTH_TEST);
      shader->setBool("directRender", false);
      // Restore model matrix/VAO for subsequent in-world draws.
      shader->setMatrix4("model", positionable.modelMatrix);
      glBindVertexArray(APP_VAO);
    }
  }

  // Direct-render Wayland accessory apps (popups/menus) relative to their
  // parent surface so they land at the correct on-screen position.
  if (wm && registry) {
    auto focused = wm->getCurrentlyFocusedApp();
    entt::entity focusedEnt =
      focused.has_value() ? focused.value() : entt::null;
    auto layerShells =
      registry->view<WaylandApp::Component>(entt::exclude<Positionable>);
    for (auto [ent, comp] : layerShells.each()) {
      if (!comp.layer_shell) {
        continue;
      }
      renderLayerShell(comp);
    }
    if (focusedEnt != entt::null) {
      auto accessories =
        registry->view<WaylandApp::Component>(entt::exclude<Positionable>);
      for (auto [ent, comp] : accessories.each()) {
        if (!comp.accessory) {
          continue;
        }
        if (comp.layer_shell) {
          continue;
        }
        if (comp.parent == entt::null || comp.parent != focusedEnt) {
          continue;
        }
        if (!registry->valid(comp.parent)) {
          continue;
        }
        auto* parentComp = registry->try_get<WaylandApp::Component>(comp.parent);
        if (!parentComp || !parentComp->app) {
          continue;
        }
        renderPopup(comp, *parentComp);
      }
    }
  }

  shader->setBool("isApp", false);
}

void
Renderer::registerApp(AppSurface* app)
{
  if (!app) {
    return;
  }
  // Use index 0 slot, unit 0; avoids consuming extra texture units.
  auto textureId = app->createTexture();
  app->appTexture();

  unsigned int framebufferId;
  glGenFramebuffers(1, &framebufferId);
  frameBuffers[textureId] = framebufferId;
  glBindFramebuffer(GL_READ_FRAMEBUFFER, framebufferId);
  glFramebufferTexture2D(GL_READ_FRAMEBUFFER,
                         GL_COLOR_ATTACHMENT0,
                         GL_TEXTURE_2D,
                         textureId,
                         0);
}

void
Renderer::deregisterApp(AppSurface* surface)
{
  glDeleteFramebuffers(1, &frameBuffers[surface->getTextureId()]);
  frameBuffers.erase(surface->getTextureId());
}
