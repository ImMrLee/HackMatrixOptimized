#include "engine.h"
#include "engineGui.h"
#include "components/Bootable.h"
#include "components/BoundingSphere.h"
#include "components/Door.h"
#include "components/Key.h"
#include "components/Lock.h"
#include "components/Parent.h"
#include "components/RotateMovement.h"
#include "components/Scriptable.h"
#include "components/Light.h"
#include "components/TranslateMovement.h"
#include "MultiPlayer/Gui.h"
#include "glm/gtc/type_ptr.hpp"
#include "imgui/imgui.h"
#include "logger.h"
#include "persister.h"
#include "string"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <cstdlib>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "Config.h"
#include "time_utils.h"
#include "entity.h"
#include "systems/Boot.h"
#include "systems/Door.h"
#include "systems/Intersections.h"
#include "systems/KeyAndLock.h"
#include "systems/Scripts.h"
#include "systems/Update.h"
#include "tracy/Tracy.hpp"
#include "screen.h"
#include "time_utils.h"

#include <glm/glm.hpp>

EngineGui::EngineGui(Engine* engine,
                     shared_ptr<EntityRegistry> registry,
                     shared_ptr<LoggerVector> loggerVector)
  : engine(engine)
  , registry(registry)
  , loggerVector(loggerVector)
{
  // Setup Dear ImGui context
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.ConfigFlags |=
    ImGuiConfigFlags_NavEnableKeyboard; // Enable Keyboard Controls
  io.ConfigFlags |=
    ImGuiConfigFlags_NavEnableGamepad; // Enable Gamepad Controls

  static const char* kGlslVersion = "#version 100";
  ImGui_ImplOpenGL3_Init(kGlslVersion);
  io.DisplaySize = ImVec2(SCREEN_WIDTH, SCREEN_HEIGHT);
  io.BackendPlatformName = "hackmatrix-wlroots";
  io.BackendFlags |= ImGuiBackendFlags_HasMouseCursors;

  if (const char* flip = std::getenv("HACKMATRIX_IMGUI_FLIP")) {
    imguiFlipY = std::atoi(flip) != 0;
  }
  // Headless UI tests open the sidebar to reach the tabs; normal runs start
  // with it hidden behind the left-edge tab.
  if (std::getenv("HACKMATRIX_IMGUI_MULTIPLAYER_TAB") != nullptr ||
      std::getenv("HACKMATRIX_IMGUI_SYSTEM_TAB") != nullptr ||
      std::getenv("HACKMATRIX_IMGUI_MENU_OPEN") != nullptr) {
    menuVisible = true;
    menuAnim = 1.0f;
  }
}

void
EngineGui::render(double& fps, int frameIndex, vector<double>& frameTimes)
{
  ZoneScoped;
  static MultiPlayer::Gui gui(engine);
  GLint prevProgram = 0;
  GLint prevArray = 0;
  GLint prevTexture = 0;
  glGetIntegerv(GL_CURRENT_PROGRAM, &prevProgram);
  glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prevArray);
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTexture);
  if (!imguiGlInitialized) {
    imguiGlInitialized = true;
  }
  ImGui_ImplOpenGL3_NewFrame();
  ImGuiIO& io = ImGui::GetIO();
  // The GL backend does not know the framebuffer size; keep it in sync so the
  // overlay tracks output resizes.
  io.DisplaySize = ImVec2(SCREEN_WIDTH, SCREEN_HEIGHT);
  if (io.DeltaTime <= 0.0f) {
    io.DeltaTime = 1.0f / 60.0f;
  }
  ImGui::NewFrame();
  // Headless UI tests / screenshots can open the Scene Editor directly.
  static bool sceneEditorEnvChecked = false;
  if (!sceneEditorEnvChecked) {
    sceneEditorEnvChecked = true;
    if (std::getenv("HACKMATRIX_IMGUI_SCENE_EDITOR") != nullptr) {
      setSceneEditorOpen(true);
      // Select the first model so screenshots/tests exercise the Inspector
      // without synthetic mouse input.
      if (std::getenv("HACKMATRIX_IMGUI_SCENE_SELECT_FIRST") != nullptr) {
        for (auto [entity, persistable] :
             registry->view<Persistable>().each()) {
          if (registry->any_of<Model>(entity)) {
            sceneSelection = entity;
            break;
          }
        }
      }
      if (const char* selectedId =
            std::getenv("HACKMATRIX_IMGUI_SCENE_SELECT_ID")) {
        if (auto entity = registry->locateEntity(std::atoi(selectedId))) {
          sceneSelection = entity.value();
        }
      }
    }
  }
  // ImGui::ShowDemoWindow();
  // ImGui::SetNextWindowSize(ImVec2(120, 5));
  if (frameIndex == 0) {
    fps = 0;
    for (int i = 0; i < 20; i++) {
      fps = fps + frameTimes[i];
    }
    fps /= 20.0;
    if (fps > 0)
      fps = 1.0 / fps;
  }
  renderMenu(gui, fps);
  renderSceneEditor();

  renderChat();
  renderExitConfirm();

  // Free-look crosshair: the centre ray is what clicks and R aim with, so show
  // it whenever the cursor is grabbed and no window owns the input.
  if (auto* controls = engine->getControls()) {
    auto wm = engine->getWindowManager();
    if (controls->isCursorGrabbed() && wm &&
        !wm->hasCurrentOrPendingFocus()) {
      ImDrawList* draw = ImGui::GetForegroundDrawList();
      const float cx = io.DisplaySize.x * 0.5f;
      const float cy = io.DisplaySize.y * 0.5f;
      constexpr float gap = 4.0f;
      constexpr float arm = 7.0f;
      const auto cross = [&](ImU32 color, float thickness) {
        draw->AddLine(ImVec2(cx - gap - arm, cy),
                      ImVec2(cx - gap, cy),
                      color,
                      thickness);
        draw->AddLine(ImVec2(cx + gap, cy),
                      ImVec2(cx + gap + arm, cy),
                      color,
                      thickness);
        draw->AddLine(ImVec2(cx, cy - gap - arm),
                      ImVec2(cx, cy - gap),
                      color,
                      thickness);
        draw->AddLine(ImVec2(cx, cy + gap),
                      ImVec2(cx, cy + gap + arm),
                      color,
                      thickness);
      };
      // Amber crosshair when the centre ray is over a window edge/corner, so
      // free-view resize is discoverable.
      const unsigned edgeMask = controls->windowResizeHoverMask();
      const ImU32 fg = edgeMask != 0 ? IM_COL32(255, 190, 60, 235)
                                     : IM_COL32(255, 255, 255, 220);
      cross(IM_COL32(0, 0, 0, 180), 3.0f);
      cross(fg, 1.0f);
      draw->AddCircleFilled(ImVec2(cx, cy), 1.25f, fg);
      if (edgeMask != 0) {
        constexpr float s = 9.0f;
        draw->AddRect(ImVec2(cx - s, cy - s),
                      ImVec2(cx + s, cy + s),
                      fg,
                      0.0f,
                      0,
                      1.5f);
      }
    }
  }

  engine->setImguiWantsMouse(ImGui::GetIO().WantCaptureMouse);
  engine->renderImguiOverlays();
  ImGui::Render();

  ImDrawData* drawData = ImGui::GetDrawData();
  if (drawData) {
    if (imguiFlipY) {
      // Mirror vertices and clip rects so the overlay is not upside down in the
      // wlroots output FBO. Disable with HACKMATRIX_IMGUI_FLIP=0.
      const float fbHeight =
        drawData->DisplaySize.y * drawData->FramebufferScale.y;
      for (int n = 0; n < drawData->CmdListsCount; ++n) {
        ImDrawList* cmdList = drawData->CmdLists[n];
        for (int v = 0; v < cmdList->VtxBuffer.Size; ++v) {
          cmdList->VtxBuffer[v].pos.y = fbHeight - cmdList->VtxBuffer[v].pos.y;
        }
        for (int c = 0; c < cmdList->CmdBuffer.Size; ++c) {
          ImDrawCmd& cmd = cmdList->CmdBuffer[c];
          const float y0 = cmd.ClipRect.y;
          const float y1 = cmd.ClipRect.w;
          cmd.ClipRect.y = fbHeight - y1;
          cmd.ClipRect.w = fbHeight - y0;
        }
      }
    }
    ImGui_ImplOpenGL3_RenderDrawData(drawData);
  }

  // Restore GL bindings to minimize GL_INVALID_* spam on GLES2 drivers.
  glUseProgram(prevProgram);
  glBindBuffer(GL_ARRAY_BUFFER, prevArray);
  glBindTexture(GL_TEXTURE_2D, prevTexture);
}

void
EngineGui::renderMenu(MultiPlayer::Gui& gui, float fps)
{
  ImGuiViewport* viewport = ImGui::GetMainViewport();
  const float workW = viewport->WorkSize.x;
  const float workH = viewport->WorkSize.y;
  const float tabW = 26.0f;
  const float tabH = std::min(140.0f, workH * 0.25f);
  const float sideW = std::clamp(workW * 0.2f, 260.0f, workW * 0.4f);
  const float panelW = std::max(200.0f, sideW - tabW);
  const float margin = 8.0f;
  const float panelH = std::max(200.0f, workH - margin * 2.0f);

  const float dt = std::clamp(ImGui::GetIO().DeltaTime, 0.0f, 0.1f);
  const float target = menuVisible ? 1.0f : 0.0f;
  const float speed = 1.0f / 0.16f;
  if (menuAnim < target) {
    menuAnim = std::min(target, menuAnim + dt * speed);
  } else if (menuAnim > target) {
    menuAnim = std::max(target, menuAnim - dt * speed);
  }

  const bool drawPanel = menuAnim > 0.001f || menuVisible;
  if (drawPanel) {
    const float panelX = -panelW + menuAnim * (tabW + panelW);
    ImGui::SetNextWindowPos(
      ImVec2(viewport->WorkPos.x + panelX, viewport->WorkPos.y + margin),
      ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(panelW, panelH), ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6.0f, 6.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.0f, 4.0f));
    ImGui::Begin("##hm_menu_panel",
                 nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                   ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                   ImGuiWindowFlags_NoCollapse);
    ImGui::TextUnformatted("HackMatrix");
    ImGui::SameLine(ImGui::GetWindowWidth() - 38.0f);
    if (ImGui::SmallButton("<<")) {
      menuVisible = false;
    }
    ImGui::Separator();

    vector<string> debugMessages = loggerVector->fetch();
    if (ImGui::BeginTabBar("Developer Menu",
                           ImGuiTabBarFlags_FittingPolicyScroll)) {
      if (ImGui::BeginTabItem("FPS")) {
        ImGui::Text("%f fps", fps);
        ImGui::EndTabItem();
      }
      if (ImGui::BeginTabItem("Debug Log")) {
        for (const auto& msg : debugMessages) {
          ImGui::TextWrapped("%s", msg.c_str());
        }
        ImGui::EndTabItem();
      }
      if (ImGui::BeginTabItem("Scene Editor")) {
        if (ImGui::Button("Open Scene Editor", ImVec2(-1.0f, 0.0f))) {
          setSceneEditorOpen(true);
        }
        ImGui::TextDisabled("Outliner + Inspector open as panels.");
        ImGui::TextDisabled("%zu entities", registry->storage<Persistable>().size());
        if (ImGui::Button("Persist All", ImVec2(-1.0f, 0.0f))) {
          registry->saveAll();
        }
        ImGui::EndTabItem();
      }
      const bool forceSystemTab =
        std::getenv("HACKMATRIX_IMGUI_SYSTEM_TAB") != nullptr;
      if (ImGui::BeginTabItem("System Config",
                              nullptr,
                              forceSystemTab ? ImGuiTabItemFlags_SetSelected
                                             : 0)) {
        renderSystemConfig();
        ImGui::EndTabItem();
      }
      const bool forceMultiplayerTab =
        std::getenv("HACKMATRIX_IMGUI_MULTIPLAYER_TAB") != nullptr;
      if (ImGui::BeginTabItem("Multiplayer",
                              nullptr,
                              forceMultiplayerTab ? ImGuiTabItemFlags_SetSelected
                                                  : 0)) {
        gui.Render();
        ImGui::EndTabItem();
      }
      ImGui::EndTabBar();
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
  }

  // Edge tab: always visible, vertically centred on the left edge.
  ImGui::SetNextWindowPos(
    ImVec2(viewport->WorkPos.x, viewport->WorkPos.y + (workH - tabH) * 0.5f),
    ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(tabW, tabH), ImGuiCond_Always);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
  ImGui::Begin("##hm_menu_tab",
               nullptr,
               ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                 ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBackground |
                 ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
  ImGui::InvisibleButton("##hm_menu_tab_btn", ImVec2(tabW, tabH));
  const bool hovered = ImGui::IsItemHovered();
  if (ImGui::IsItemClicked()) {
    menuVisible = !menuVisible;
  }
  ImDrawList* draw = ImGui::GetWindowDrawList();
  // Window draw lists take screen coordinates, not window-local ones.
  const ImVec2 p0 = ImGui::GetWindowPos();
  const ImVec2 p1(p0.x + tabW, p0.y + tabH);
  const ImU32 bg = hovered ? IM_COL32(60, 90, 140, 235)
                           : IM_COL32(32, 42, 58, 225);
  const ImU32 border = IM_COL32(110, 150, 200, 230);
  draw->AddRectFilled(ImVec2(p0.x + 1.0f, p0.y + 1.0f),
                      ImVec2(p1.x, p1.y - 1.0f),
                      bg,
                      6.0f);
  draw->AddRect(ImVec2(p0.x + 1.0f, p0.y + 1.0f),
                ImVec2(p1.x, p1.y - 1.0f),
                border,
                6.0f);
  const ImVec2 centre(p0.x + tabW * 0.5f, p0.y + tabH * 0.5f);
  constexpr float kArrow = 6.0f;
  const ImU32 fg = IM_COL32(230, 238, 248, 255);
  if (menuVisible) {
    draw->AddLine(ImVec2(centre.x + kArrow * 0.5f, centre.y - kArrow),
                  ImVec2(centre.x - kArrow * 0.5f, centre.y),
                  fg,
                  2.0f);
    draw->AddLine(ImVec2(centre.x - kArrow * 0.5f, centre.y),
                  ImVec2(centre.x + kArrow * 0.5f, centre.y + kArrow),
                  fg,
                  2.0f);
  } else {
    draw->AddLine(ImVec2(centre.x - kArrow * 0.5f, centre.y - kArrow),
                  ImVec2(centre.x + kArrow * 0.5f, centre.y),
                  fg,
                  2.0f);
    draw->AddLine(ImVec2(centre.x + kArrow * 0.5f, centre.y),
                  ImVec2(centre.x - kArrow * 0.5f, centre.y + kArrow),
                  fg,
                  2.0f);
  }
  ImGui::End();
  ImGui::PopStyleVar();
}

void
EngineGui::renderSystemConfig()
{
  const double now = nowSeconds();
  auto slider = [](const char* label,
                   float* value,
                   float low,
                   float high,
                   const char* format) {
    ImGui::TextUnformatted(label);
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::PushID(label);
    const bool changed = ImGui::SliderFloat("##value", value, low, high, format);
    ImGui::PopID();
    return changed;
  };
  auto persistFloat = [](const char* key, float value) {
    try {
      Config::singleton()->setFloatAndSave(key, value);
    } catch (...) {
      // Read-only config should not crash the UI.
    }
  };

  ImGui::TextUnformatted("Audio");
  ImGui::Separator();
  systemSettings_.refreshAudioIfDue(now);
  const AudioSettings& audio = systemSettings_.audio();
  if (!audio.available) {
    if (audio.backend.empty()) {
      ImGui::TextDisabled("No supported audio backend");
      ImGui::TextDisabled("(wpctl / pactl / amixer)");
    } else {
      ImGui::TextDisabled("%s: no server", audio.backend.c_str());
    }
  } else {
    static float volumeUi = 50.0f;
    if (!ImGui::IsAnyItemActive()) {
      volumeUi = audio.volume * 100.0f;
    }
    slider("Volume", &volumeUi, 0.0f, 100.0f, "%.0f%%");
    if (ImGui::IsItemDeactivatedAfterEdit()) {
      systemSettings_.setVolume(volumeUi / 100.0f);
    }
    bool muted = audio.muted;
    if (ImGui::Checkbox("Mute", &muted)) {
      systemSettings_.toggleMute();
    }
    ImGui::TextDisabled("Backend: %s", audio.backend.c_str());
  }

  ImGui::Separator();
  ImGui::TextUnformatted("Display");
  ImGui::Separator();
  systemSettings_.refreshBrightnessIfDue(now);
  const BrightnessSettings& brightness = systemSettings_.brightness();
  if (!brightness.available) {
    ImGui::TextDisabled("No backlight device");
  } else {
    static float brightnessUi = 50.0f;
    if (!ImGui::IsAnyItemActive()) {
      brightnessUi = brightness.max > 0
                       ? 100.0f * static_cast<float>(brightness.current) /
                           static_cast<float>(brightness.max)
                       : 0.0f;
    }
    ImGui::BeginDisabled(!brightness.writable);
    slider("Brightness", &brightnessUi, 0.0f, 100.0f, "%.0f%%");
    if (ImGui::IsItemDeactivatedAfterEdit()) {
      systemSettings_.setBrightness(brightnessUi / 100.0f);
    }
    ImGui::EndDisabled();
    ImGui::TextDisabled("Device: %s", brightness.device.c_str());
    if (!brightness.writable) {
      ImGui::TextDisabled("Read-only (permission denied)");
    }
  }

  ImGui::Separator();
  ImGui::TextUnformatted("HackMatrix");
  ImGui::Separator();
  Camera* camera = engine ? engine->getCamera() : nullptr;
  if (camera) {
    static float fovUi = 45.0f;
    if (!ImGui::IsAnyItemActive()) {
      fovUi = camera->getFovDegrees();
    }
    slider("FOV", &fovUi, 30.0f, 110.0f, "%.0f");
    if (ImGui::IsItemDeactivatedAfterEdit()) {
      camera->setFovDegrees(fovUi);
      persistFloat("fov", fovUi);
    }

    static float farUi = 400.0f;
    if (!ImGui::IsAnyItemActive()) {
      farUi = camera->getFarPlane();
    }
    slider("Draw distance", &farUi, 20.0f, 1000.0f, "%.0f");
    if (ImGui::IsItemDeactivatedAfterEdit()) {
      camera->setFarPlane(farUi);
      persistFloat("zFar", farUi);
    }

    static float sensitivityUi = 0.1f;
    if (!ImGui::IsAnyItemActive()) {
      sensitivityUi = camera->getLookSensitivity();
    }
    slider("Mouse sensitivity", &sensitivityUi, 0.01f, 0.5f, "%.2f");
    if (ImGui::IsItemDeactivatedAfterEdit()) {
      camera->setLookSensitivity(sensitivityUi);
      persistFloat("mouse_sensitivity", sensitivityUi);
    }

    static float speedUi = 0.03f;
    if (!ImGui::IsAnyItemActive()) {
      speedUi = camera->getCameraSpeed();
    }
    slider("Camera speed", &speedUi, 0.005f, 0.2f, "%.3f");
    if (ImGui::IsItemDeactivatedAfterEdit()) {
      camera->setCameraSpeed(speedUi);
      persistFloat("camera_speed", speedUi);
    }
  }

  if (auto* controls = engine ? engine->getControls() : nullptr) {
    static float flopUi = 0.25f;
    if (!ImGui::IsAnyItemActive()) {
      flopUi = controls->getWindowFlop();
    }
    slider("Window animation (s)", &flopUi, 0.01f, 1.0f, "%.2f");
    if (ImGui::IsItemDeactivatedAfterEdit()) {
      controls->setWindowFlop(flopUi);
      persistFloat("window_flop", flopUi);
    }
  }
}

namespace {

std::string
sceneLower(std::string value)
{
  std::transform(value.begin(),
                 value.end(),
                 value.begin(),
                 [](unsigned char c) {
                   return static_cast<char>(std::tolower(c));
                 });
  return value;
}

bool
sceneProjectToScreen(const glm::mat4& projection,
                     const glm::mat4& view,
                     const glm::vec3& world,
                     ImVec2& screen)
{
  const glm::vec4 clip = projection * view * glm::vec4(world, 1.0f);
  if (clip.w <= 1e-5f) {
    return false;
  }
  const float ndcX = clip.x / clip.w;
  const float ndcY = clip.y / clip.w;
  // createMouseRay() flips pointer NDC when the wlroots projection does.
  const float pointerNdcY = projection[1][1] < 0.0f ? -ndcY : ndcY;
  screen.x = (ndcX * 0.5f + 0.5f) * SCREEN_WIDTH;
  screen.y = (1.0f - (pointerNdcY * 0.5f + 0.5f)) * SCREEN_HEIGHT;
  return true;
}

float
sceneDistanceToSegment(const ImVec2& point,
                       const ImVec2& start,
                       const ImVec2& end)
{
  const float dx = end.x - start.x;
  const float dy = end.y - start.y;
  const float lengthSq = dx * dx + dy * dy;
  if (lengthSq < 1e-6f) {
    return std::hypot(point.x - start.x, point.y - start.y);
  }
  float t = ((point.x - start.x) * dx + (point.y - start.y) * dy) / lengthSq;
  t = std::clamp(t, 0.0f, 1.0f);
  const float nearestX = start.x + t * dx;
  const float nearestY = start.y + t * dy;
  return std::hypot(point.x - nearestX, point.y - nearestY);
}

const glm::vec3 kGizmoAxes[3] = { glm::vec3(1.0f, 0.0f, 0.0f),
                                  glm::vec3(0.0f, 1.0f, 0.0f),
                                  glm::vec3(0.0f, 0.0f, 1.0f) };
const ImU32 kGizmoColors[3] = { IM_COL32(235, 90, 90, 255),
                                IM_COL32(100, 215, 100, 255),
                                IM_COL32(100, 150, 255, 255) };
const char* const kGizmoNames[3] = { "X", "Y", "Z" };

std::string
sceneEntityLabel(const std::shared_ptr<EntityRegistry>& registry,
                 entt::entity entity)
{
  if (auto* bootable = registry->try_get<Bootable>(entity)) {
    if (bootable->name.has_value() && !bootable->name->empty()) {
      return bootable->name.value();
    }
    if (!bootable->cmd.empty()) {
      const std::string name =
        std::filesystem::path(bootable->cmd).filename().string();
      if (!name.empty()) {
        return name;
      }
    }
    return "Application";
  }
  if (auto* model = registry->try_get<Model>(entity)) {
    if (!model->path.empty()) {
      const std::string name =
        std::filesystem::path(model->path).filename().string();
      if (!name.empty()) {
        return name;
      }
    }
    return "Model";
  }
  if (registry->any_of<Light>(entity)) {
    return "Light";
  }
  if (registry->any_of<Scriptable>(entity)) {
    return "Script";
  }
  return "Entity";
}

std::string
sceneEntityTypeTag(const std::shared_ptr<EntityRegistry>& registry,
                   entt::entity entity)
{
  if (registry->any_of<Bootable>(entity)) {
    return "app";
  }
  if (registry->any_of<Model>(entity)) {
    return "model";
  }
  if (registry->any_of<Light>(entity)) {
    return "light";
  }
  if (registry->any_of<Scriptable>(entity)) {
    return "script";
  }
  return "entity";
}

bool
sceneEntityMatches(const std::shared_ptr<EntityRegistry>& registry,
                   entt::entity entity,
                   int64_t entityId,
                   const std::string& filterLower)
{
  if (filterLower.empty()) {
    return true;
  }

  std::string haystack = sceneEntityLabel(registry, entity) + " " +
                         sceneEntityTypeTag(registry, entity) + " " +
                         std::to_string(entityId);
  if (auto* model = registry->try_get<Model>(entity)) {
    haystack += " " + model->path + " model mesh";
  }
  if (auto* bootable = registry->try_get<Bootable>(entity)) {
    haystack += " " + bootable->cmd + " " + bootable->args + " app bootable";
  }
  if (registry->any_of<Positionable>(entity)) {
    haystack += " transform positionable";
  }
  if (registry->any_of<Light>(entity)) {
    haystack += " light";
  }
  if (registry->any_of<Door>(entity)) {
    haystack += " door";
  }
  if (registry->any_of<Key>(entity)) {
    haystack += " key";
  }
  if (registry->any_of<Lock>(entity)) {
    haystack += " lock";
  }
  if (registry->any_of<Parent>(entity)) {
    haystack += " parent";
  }
  if (registry->any_of<RotateMovement>(entity)) {
    haystack += " rotate rotate-movement";
  }
  if (registry->any_of<TranslateMovement>(entity)) {
    haystack += " translate translation";
  }
  if (registry->any_of<Scriptable>(entity)) {
    haystack += " script scriptable";
  }
  return sceneLower(std::move(haystack)).find(filterLower) !=
         std::string::npos;
}

} // namespace

void
EngineGui::setSceneEditorOpen(bool open)
{
  if (sceneEditorOpen == open) {
    return;
  }
  sceneEditorOpen = open;

  auto* controls = engine ? engine->getControls() : nullptr;
  if (open) {
    // The sidebar and the editor both want the left edge; close the narrow
    // developer menu so the outliner has room.
    menuVisible = false;
    sceneAddComponentOpen = false;
    sceneAddComponentEntity = entt::null;
    sceneGizmoHoverAxis = -1;
    sceneGizmoDragAxis = -1;
    sceneGizmoEntity = entt::null;
    if (controls) {
      controls->beginPointerMode();
    }
  } else {
    if (sceneGizmoDragAxis >= 0) {
      // Closing mid-drag should still commit the last previewed transform.
      endSceneGizmoDrag();
    }
    sceneGizmoHoverAxis = -1;
    sceneAddComponentOpen = false;
    sceneAddComponentEntity = entt::null;
    if (controls) {
      controls->endPointerMode();
    }
  }
}

entt::entity
EngineGui::duplicateEntity(entt::entity source)
{
  if (source == entt::null || !registry->valid(source) ||
      !registry->any_of<Persistable>(source)) {
    return entt::null;
  }

  const entt::entity duplicate = registry->createPersistent();

  if (auto* model = registry->try_get<Model>(source)) {
    registry->emplace<Model>(duplicate, model->path);
  }
  if (auto* positionable = registry->try_get<Positionable>(source)) {
    const float offset =
      0.5f * std::max(0.05f, std::abs(positionable->scale));
    registry->emplace<Positionable>(
      duplicate,
      positionable->pos + glm::vec3(offset, 0.0f, 0.0f),
      positionable->origin,
      positionable->rotate,
      positionable->scale);
  }
  if (auto* light = registry->try_get<Light>(source)) {
    registry->emplace<Light>(duplicate, light->color);
  }
  if (auto* rotate = registry->try_get<RotateMovement>(source)) {
    registry->emplace<RotateMovement>(
      duplicate,
      RotateMovement{ rotate->degrees, rotate->degreesPerSecond, rotate->axis });
  }
  if (auto* translate = registry->try_get<TranslateMovement>(source)) {
    registry->emplace<TranslateMovement>(
      duplicate,
      TranslateMovement{ translate->delta, translate->unitsPerSecond });
  }
  if (auto* door = registry->try_get<Door>(source)) {
    registry->emplace<Door>(
      duplicate,
      Door{ RotateMovement{ door->openMovement.degrees,
                            door->openMovement.degreesPerSecond,
                            door->openMovement.axis },
            RotateMovement{ door->closeMovement.degrees,
                            door->closeMovement.degreesPerSecond,
                            door->closeMovement.axis },
            door->state });
  }
  if (auto* key = registry->try_get<Key>(source)) {
    registry->emplace<Key>(
      duplicate,
      Key{ key->lockable,
           key->state,
           RotateMovement{ key->turnMovement.degrees,
                           key->turnMovement.degreesPerSecond,
                           key->turnMovement.axis },
           RotateMovement{ key->unturnMovement.degrees,
                           key->unturnMovement.degreesPerSecond,
                           key->unturnMovement.axis } });
  }
  if (auto* lock = registry->try_get<Lock>(source)) {
    registry->emplace<Lock>(
      duplicate, Lock{ lock->position, lock->tolerance, lock->state });
  }
  if (auto* script = registry->try_get<Scriptable>(source)) {
    registry->emplace<Scriptable>(
      duplicate, script->getScript(), script->language);
  }

  // Parent and Bootable are intentionally not copied yet: duplicating a
  // hierarchy or launching a second application needs its own semantics.
  return duplicate;
}

void
EngineGui::renderSceneToolbar(const ImVec2& pos, const ImVec2& size)
{
  ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
  ImGui::SetNextWindowSize(size, ImGuiCond_Always);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 6.0f));
  ImGui::Begin("##hm_scene_toolbar",
               nullptr,
               ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoSavedSettings |
                 ImGuiWindowFlags_NoCollapse |
                 ImGuiWindowFlags_NoScrollbar);
  const auto selectionValid = [&]() {
    return sceneSelection != entt::null && registry->valid(sceneSelection) &&
           registry->any_of<Persistable>(sceneSelection);
  };

  ImGui::TextUnformatted("Scene Editor");
  ImGui::SameLine();
  if (ImGui::Button("New")) {
    createNewEntity();
  }
  ImGui::SameLine();
  ImGui::BeginDisabled(!selectionValid());
  if (ImGui::Button("Duplicate")) {
    const entt::entity duplicate = duplicateEntity(sceneSelection);
    if (duplicate != entt::null) {
      sceneSelection = duplicate;
      sceneAddComponentOpen = false;
      sceneAddComponentEntity = entt::null;
    }
  }
  ImGui::SameLine();
  if (ImGui::Button("Delete")) {
    if (selectionValid()) {
      registry->depersist(sceneSelection);
      sceneSelection = entt::null;
      sceneAddComponentOpen = false;
      sceneAddComponentEntity = entt::null;
    }
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (ImGui::Button("Save")) {
    registry->saveAll();
  }
  ImGui::SameLine();
  if (ImGui::Button("Close")) {
    setSceneEditorOpen(false);
  }

  ImGui::SameLine();
  ImGui::TextDisabled("%zu entities", registry->storage<Persistable>().size());
  if (selectionValid()) {
    const auto& persistable = registry->get<Persistable>(sceneSelection);
    ImGui::SameLine();
    ImGui::TextDisabled("| selected #%lld", (long long)persistable.entityId);
    ImGui::SameLine();
    ImGui::TextDisabled("| drag X/Y/Z to move");
  }
  ImGui::End();
  ImGui::PopStyleVar();
}

void
EngineGui::renderSceneOutliner(const ImVec2& pos, const ImVec2& size)
{
  ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
  ImGui::SetNextWindowSize(size, ImGuiCond_Always);
  ImGui::Begin("Outliner",
               nullptr,
               ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoSavedSettings |
                 ImGuiWindowFlags_NoCollapse);

  ImGui::SetNextItemWidth(-1.0f);
  ImGui::InputTextWithHint("##scene_filter",
                           "Search entities...",
                           sceneFilter,
                           IM_ARRAYSIZE(sceneFilter));

  struct SceneNode
  {
    entt::entity entity;
    int64_t id;
    std::string label;
    std::string tag;
  };

  std::vector<SceneNode> nodes;
  for (auto [entity, persistable] : registry->view<Persistable>().each()) {
    nodes.push_back(SceneNode{ entity,
                               persistable.entityId,
                               sceneEntityLabel(registry, entity),
                               sceneEntityTypeTag(registry, entity) });
  }
  std::sort(nodes.begin(),
            nodes.end(),
            [](const SceneNode& a, const SceneNode& b) { return a.id < b.id; });

  if (sceneSelection != entt::null &&
      (!registry->valid(sceneSelection) ||
       !registry->any_of<Persistable>(sceneSelection))) {
    sceneSelection = entt::null;
  }

  ImGui::Separator();
  ImGui::BeginChild("##scene_outliner_list", ImVec2(0.0f, 0.0f), false);

  std::unordered_map<int64_t, size_t> indexById;
  for (size_t i = 0; i < nodes.size(); ++i) {
    indexById[nodes[i].id] = i;
  }

  const std::string filterLower = sceneLower(sceneFilter);

  const auto drawRow = [&](const SceneNode& node, int depth) {
    if (!sceneEntityMatches(registry, node.entity, node.id, filterLower)) {
      return;
    }
    if (depth > 0) {
      ImGui::Indent(14.0f);
    }
    ImGui::PushID(static_cast<int>(node.id));
    const bool selected = node.entity == sceneSelection;
    const std::string text = "[" + node.tag + "] " + node.label + "  #" +
                             std::to_string(node.id) + "###scene_node";
    if (ImGui::Selectable(text.c_str(), selected)) {
      sceneSelection = node.entity;
      sceneAddComponentOpen = false;
      sceneAddComponentEntity = entt::null;
    }
    ImGui::PopID();
    if (depth > 0) {
      ImGui::Unindent(14.0f);
    }
  };

  if (!filterLower.empty()) {
    for (const auto& node : nodes) {
      drawRow(node, 0);
    }
  } else {
    std::unordered_map<int64_t, std::vector<int64_t>> children;
    std::unordered_set<int64_t> hasParent;
    for (const auto& node : nodes) {
      if (auto* parent = registry->try_get<Parent>(node.entity)) {
        for (const int childId : parent->childrenIds) {
          if (childId == node.id ||
              indexById.find(childId) == indexById.end()) {
            continue;
          }
          children[node.id].push_back(childId);
          hasParent.insert(childId);
        }
      }
    }

    std::unordered_set<int64_t> visited;
    std::function<void(const SceneNode&, int)> drawTree =
      [&](const SceneNode& node, int depth) {
        if (!visited.insert(node.id).second) {
          return;
        }
        drawRow(node, depth);
        const auto childrenIt = children.find(node.id);
        if (childrenIt == children.end()) {
          return;
        }
        for (const int64_t childId : childrenIt->second) {
          const auto childIt = indexById.find(childId);
          if (childIt != indexById.end()) {
            drawTree(nodes[childIt->second], depth + 1);
          }
        }
      };

    for (const auto& node : nodes) {
      if (!hasParent.contains(node.id)) {
        drawTree(node, 0);
      }
    }
    // Defensive: malformed/cyclic Parent data must still leave every entity
    // reachable in the outliner.
    for (const auto& node : nodes) {
      if (!visited.contains(node.id)) {
        drawTree(node, 0);
      }
    }
  }

  ImGui::EndChild();
  ImGui::End();
}

void
EngineGui::renderSceneInspector(const ImVec2& pos, const ImVec2& size)
{
  ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
  ImGui::SetNextWindowSize(size, ImGuiCond_Always);
  ImGui::Begin("Inspector",
               nullptr,
               ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoSavedSettings |
                 ImGuiWindowFlags_NoCollapse);

  if (sceneSelection == entt::null || !registry->valid(sceneSelection) ||
      !registry->any_of<Persistable>(sceneSelection)) {
    sceneSelection = entt::null;
    ImGui::TextDisabled("Select an entity in the Outliner.");
    ImGui::TextDisabled("New creates an entity with a transform.");
    ImGui::End();
    return;
  }

  const auto& persistable = registry->get<Persistable>(sceneSelection);
  ImGui::TextUnformatted(sceneEntityLabel(registry, sceneSelection).c_str());
  ImGui::TextDisabled("Entity #%lld  (raw %d)",
                      (long long)persistable.entityId,
                      (int)entt::to_integral(sceneSelection));
  ImGui::Separator();

  if (ImGui::Button("Duplicate")) {
    const entt::entity duplicate = duplicateEntity(sceneSelection);
    if (duplicate != entt::null) {
      sceneSelection = duplicate;
      sceneAddComponentOpen = false;
      sceneAddComponentEntity = entt::null;
    }
  }
  ImGui::SameLine();
  if (ImGui::Button("Delete")) {
    registry->depersist(sceneSelection);
    sceneSelection = entt::null;
    sceneAddComponentOpen = false;
    sceneAddComponentEntity = entt::null;
    ImGui::End();
    return;
  }
  ImGui::Separator();

  renderComponentPanel(sceneSelection);

  ImGui::Separator();
  if (ImGui::Button("+ Add Component", ImVec2(-1.0f, 0.0f))) {
    sceneAddComponentOpen = true;
    sceneAddComponentEntity = sceneSelection;
  }
  if (sceneAddComponentOpen && sceneAddComponentEntity == sceneSelection) {
    if (ImGui::Button("Cancel##scene_add_component")) {
      sceneAddComponentOpen = false;
      sceneAddComponentEntity = entt::null;
    }
    addComponentPanel(sceneSelection, sceneAddComponentOpen);
  }

  ImGui::End();
}

entt::entity
EngineGui::pickSceneEntity(float mouseX, float mouseY)
{
  auto* camera = engine ? engine->getCamera() : nullptr;
  if (!camera) {
    return entt::null;
  }

  const Ray ray = createMouseRay(mouseX,
                                 mouseY,
                                 SCREEN_WIDTH,
                                 SCREEN_HEIGHT,
                                 camera->getProjectionMatrix(),
                                 camera->getViewMatrix());

  entt::entity best = entt::null;
  float bestDistance = std::numeric_limits<float>::max();

  for (auto [entity, positionable, model] :
       registry->view<Positionable, Model>().each()) {
    auto* sphere = registry->try_get<BoundingSphere>(entity);
    if (!sphere) {
      systems::emplaceBoundingSphere(registry, entity);
      sphere = registry->try_get<BoundingSphere>(entity);
    }
    if (!sphere) {
      continue;
    }
    if (!systems::intersect(*sphere, ray.origin, ray.direction, 100000.0f)) {
      continue;
    }
    // Approximate depth along the ray; good enough to pick the front-most
    // bounding sphere when several overlap.
    const float distance =
      glm::dot(sphere->center - ray.origin, ray.direction);
    if (distance >= 0.0f && distance < bestDistance) {
      bestDistance = distance;
      best = entity;
    }
  }
  return best;
}

void
EngineGui::renderSceneGizmo(const ImVec2& clipMin, const ImVec2& clipMax)
{
  if (sceneSelection == entt::null || !registry->valid(sceneSelection)) {
    return;
  }
  auto* positionable = registry->try_get<Positionable>(sceneSelection);
  auto* camera = engine ? engine->getCamera() : nullptr;
  if (!positionable || !camera) {
    return;
  }

  const glm::mat4& projection = camera->getProjectionMatrix();
  const glm::mat4& view = camera->getViewMatrix();
  ImVec2 origin;
  if (!sceneProjectToScreen(projection, view, positionable->pos, origin)) {
    return;
  }

  const float distance =
    std::max(0.05f, glm::length(positionable->pos - camera->position));
  const float p11 = std::abs(projection[1][1]);
  const float worldPerPixel =
    p11 > 1e-6f ? (2.0f * distance) / (p11 * SCREEN_HEIGHT) : 0.0f;
  if (worldPerPixel <= 0.0f) {
    return;
  }
  const float axisLength = worldPerPixel * 90.0f;

  ImDrawList* draw = ImGui::GetForegroundDrawList();
  draw->PushClipRect(clipMin, clipMax, true);
  for (int axis = 0; axis < 3; ++axis) {
    ImVec2 end;
    if (!sceneProjectToScreen(
          projection,
          view,
          positionable->pos + kGizmoAxes[axis] * axisLength,
          end)) {
      continue;
    }
    const bool active = sceneGizmoDragAxis == axis ||
                        (sceneGizmoDragAxis < 0 && sceneGizmoHoverAxis == axis);
    draw->AddLine(origin, end, kGizmoColors[axis], active ? 4.0f : 2.0f);
    draw->AddCircleFilled(end, active ? 6.0f : 4.0f, kGizmoColors[axis]);
    draw->AddText(ImVec2(end.x + 5.0f, end.y - 7.0f),
                  kGizmoColors[axis],
                  kGizmoNames[axis]);
  }
  draw->AddCircleFilled(origin, 4.0f, IM_COL32(245, 245, 245, 245));
  draw->PopClipRect();
}

bool
EngineGui::handleSceneGizmoInput(const ImVec2& mouse, bool mouseInView)
{
  if (sceneGizmoDragAxis >= 0) {
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
      endSceneGizmoDrag();
      return true;
    }
    updateSceneGizmoDrag(mouse);
    return true;
  }

  sceneGizmoHoverAxis = -1;
  if (!mouseInView || sceneSelection == entt::null ||
      !registry->valid(sceneSelection)) {
    return false;
  }
  auto* positionable = registry->try_get<Positionable>(sceneSelection);
  auto* camera = engine ? engine->getCamera() : nullptr;
  if (!positionable || !camera) {
    return false;
  }

  const glm::mat4& projection = camera->getProjectionMatrix();
  const glm::mat4& view = camera->getViewMatrix();
  ImVec2 origin;
  if (!sceneProjectToScreen(projection, view, positionable->pos, origin)) {
    return false;
  }

  const float distance =
    std::max(0.05f, glm::length(positionable->pos - camera->position));
  const float p11 = std::abs(projection[1][1]);
  const float worldPerPixel =
    p11 > 1e-6f ? (2.0f * distance) / (p11 * SCREEN_HEIGHT) : 0.0f;
  if (worldPerPixel <= 0.0f) {
    return false;
  }
  const float axisLength = worldPerPixel * 90.0f;

  int bestAxis = -1;
  float bestDistance = 9.0f;
  glm::vec2 bestScreenDirection(0.0f);
  for (int axis = 0; axis < 3; ++axis) {
    ImVec2 end;
    if (!sceneProjectToScreen(
          projection,
          view,
          positionable->pos + kGizmoAxes[axis] * axisLength,
          end)) {
      continue;
    }
    const float dx = end.x - origin.x;
    const float dy = end.y - origin.y;
    const float screenLength = std::hypot(dx, dy);
    // A nearly camera-facing axis has almost no screen length and cannot be
    // dragged reliably; leave it out of hit testing.
    if (screenLength < 8.0f) {
      continue;
    }
    const float hitDistance = sceneDistanceToSegment(mouse, origin, end);
    if (hitDistance < bestDistance) {
      bestDistance = hitDistance;
      bestAxis = axis;
      bestScreenDirection = glm::vec2(dx / screenLength, dy / screenLength);
    }
  }

  if (bestAxis < 0) {
    return false;
  }

  sceneGizmoHoverAxis = bestAxis;
  ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
  if (!ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
    return false;
  }

  sceneGizmoDragAxis = bestAxis;
  sceneGizmoEntity = sceneSelection;
  sceneGizmoStartMouse = mouse;
  sceneGizmoStartPos = positionable->pos;
  sceneGizmoAxisScreen = bestScreenDirection;
  sceneGizmoWorldPerPixel = worldPerPixel;
  return true;
}

void
EngineGui::updateSceneGizmoDrag(const ImVec2& mouse)
{
  if (sceneGizmoDragAxis < 0 || sceneGizmoEntity == entt::null ||
      !registry->valid(sceneGizmoEntity)) {
    sceneGizmoDragAxis = -1;
    sceneGizmoEntity = entt::null;
    return;
  }
  auto* positionable = registry->try_get<Positionable>(sceneGizmoEntity);
  if (!positionable) {
    sceneGizmoDragAxis = -1;
    sceneGizmoEntity = entt::null;
    return;
  }

  const float dx = mouse.x - sceneGizmoStartMouse.x;
  const float dy = mouse.y - sceneGizmoStartMouse.y;
  const float pixelsAlongAxis =
    dx * sceneGizmoAxisScreen.x + dy * sceneGizmoAxisScreen.y;
  const float worldDelta = pixelsAlongAxis * sceneGizmoWorldPerPixel;
  positionable->pos =
    sceneGizmoStartPos + kGizmoAxes[sceneGizmoDragAxis] * worldDelta;
  positionable->damage();
}

void
EngineGui::endSceneGizmoDrag()
{
  const entt::entity edited = sceneGizmoEntity;
  sceneGizmoDragAxis = -1;
  sceneGizmoEntity = entt::null;
  sceneGizmoHoverAxis = -1;
  if (edited != entt::null && registry->valid(edited)) {
    commitSceneTransform(edited);
  }
}

void
EngineGui::commitSceneTransform(entt::entity entity)
{
  auto* positionable = registry->try_get<Positionable>(entity);
  auto client = engine ? engine->getClient() : nullptr;
  if (!positionable || !client || !client->isConnected() ||
      client->worldAuthorityID() == 0 || client->isWorldAuthority()) {
    return;
  }
  client->sendWorldEntityEdit(static_cast<std::uint32_t>(entt::to_integral(entity)),
                              positionable->pos,
                              positionable->origin,
                              positionable->rotate,
                              positionable->scale);
}

void
EngineGui::drawSceneSelectionOverlay(const ImVec2& clipMin,
                                     const ImVec2& clipMax)
{
  if (sceneSelection == entt::null || !registry->valid(sceneSelection) ||
      !registry->any_of<Model, Positionable>(sceneSelection)) {
    return;
  }

  auto* sphere = registry->try_get<BoundingSphere>(sceneSelection);
  if (!sphere) {
    systems::emplaceBoundingSphere(registry, sceneSelection);
    sphere = registry->try_get<BoundingSphere>(sceneSelection);
  }
  auto* camera = engine ? engine->getCamera() : nullptr;
  if (!sphere || !camera) {
    return;
  }

  const glm::mat4& projection = camera->getProjectionMatrix();
  const glm::mat4& view = camera->getViewMatrix();
  const glm::vec4 clip =
    projection * view * glm::vec4(sphere->center, 1.0f);
  if (clip.w <= 0.0f) {
    return;
  }

  const float ndcX = clip.x / clip.w;
  const float ndcY = clip.y / clip.w;
  // Match createMouseRay(): the wlroots render path can flip projection Y.
  const float pointerNdcY = projection[1][1] < 0.0f ? -ndcY : ndcY;
  const float screenX = (ndcX * 0.5f + 0.5f) * SCREEN_WIDTH;
  const float screenY =
    (1.0f - (pointerNdcY * 0.5f + 0.5f)) * SCREEN_HEIGHT;
  const float radius = std::clamp(std::abs(projection[1][1]) *
                                    sphere->radius / clip.w * SCREEN_HEIGHT *
                                    0.5f,
                                  4.0f,
                                  4000.0f);

  ImDrawList* draw = ImGui::GetForegroundDrawList();
  const ImU32 color = IM_COL32(255, 196, 64, 235);
  draw->PushClipRect(clipMin, clipMax, true);
  // A large/clipped object's bounding circle can sit entirely outside the
  // viewport, so always mark the projected centre as well.
  draw->AddCircle(ImVec2(screenX, screenY), radius, color, 40, 2.0f);
  draw->AddLine(ImVec2(screenX - 10.0f, screenY),
                ImVec2(screenX - 3.0f, screenY),
                color,
                2.0f);
  draw->AddLine(ImVec2(screenX + 3.0f, screenY),
                ImVec2(screenX + 10.0f, screenY),
                color,
                2.0f);
  draw->AddLine(ImVec2(screenX, screenY - 10.0f),
                ImVec2(screenX, screenY - 3.0f),
                color,
                2.0f);
  draw->AddLine(ImVec2(screenX, screenY + 3.0f),
                ImVec2(screenX, screenY + 10.0f),
                color,
                2.0f);

  std::string selectionLabel = sceneEntityLabel(registry, sceneSelection);
  if (auto* persistable = registry->try_get<Persistable>(sceneSelection)) {
    selectionLabel += "  #" + std::to_string(persistable->entityId);
  }
  draw->AddText(ImVec2(clipMin.x + 8.0f, clipMin.y + 8.0f),
                color,
                selectionLabel.c_str());
  draw->PopClipRect();
}

void
EngineGui::renderSceneEditor()
{
  if (!sceneEditorOpen) {
    return;
  }

  if (sceneSelection != entt::null &&
      (!registry->valid(sceneSelection) ||
       !registry->any_of<Persistable>(sceneSelection))) {
    sceneSelection = entt::null;
  }

  // Keys are disabled while the editor is open (Engine::disableKeysIfImguiActive),
  // so this Escape is consumed here instead of opening the quit dialog.
  if (!ImGui::IsAnyItemActive() &&
      !ImGui::IsPopupOpen("",
                          ImGuiPopupFlags_AnyPopupId |
                            ImGuiPopupFlags_AnyPopupLevel) &&
      ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
    setSceneEditorOpen(false);
    return;
  }

  ImGuiViewport* viewport = ImGui::GetMainViewport();
  const ImVec2 origin = viewport->WorkPos;
  const ImVec2 work = viewport->WorkSize;
  const float toolbarHeight = 42.0f;
  const float gap = 6.0f;
  const float leftWidth = std::clamp(work.x * 0.22f, 240.0f, 320.0f);
  const float rightWidth = std::clamp(work.x * 0.28f, 300.0f, 420.0f);
  const float sideTop = origin.y + toolbarHeight + gap;
  const float sideHeight = std::max(120.0f, work.y - toolbarHeight - gap * 2.0f);

  renderSceneToolbar(ImVec2(origin.x, origin.y),
                     ImVec2(work.x, toolbarHeight));
  renderSceneOutliner(ImVec2(origin.x + 30.0f, sideTop),
                      ImVec2(leftWidth, sideHeight));
  renderSceneInspector(
    ImVec2(origin.x + work.x - rightWidth - gap, sideTop),
    ImVec2(rightWidth, sideHeight));

  // Click in the open centre area to pick an entity. The panels own their
  // rectangles, so this does not steal clicks from Outliner/Inspector/toolbar.
  const ImVec2 mouse = ImGui::GetIO().MousePos;
  const float viewLeft = origin.x + 30.0f + leftWidth + gap;
  const float viewRight = origin.x + work.x - rightWidth - gap;
  const float viewBottom = sideTop + sideHeight;
  const bool mouseInView = mouse.x > viewLeft && mouse.x < viewRight &&
                           mouse.y > sideTop && mouse.y < viewBottom;

  const bool popupOpen =
    ImGui::IsPopupOpen("",
                       ImGuiPopupFlags_AnyPopupId |
                         ImGuiPopupFlags_AnyPopupLevel);
  const bool gizmoCaptured =
    !popupOpen && handleSceneGizmoInput(mouse, mouseInView);
  if (!gizmoCaptured && !popupOpen &&
      ImGui::IsMouseClicked(ImGuiMouseButton_Left) && mouseInView) {
    sceneSelection = pickSceneEntity(mouse.x, mouse.y);
    sceneGizmoHoverAxis = -1;
    sceneAddComponentOpen = false;
    sceneAddComponentEntity = entt::null;
  }

  drawSceneSelectionOverlay(ImVec2(viewLeft, sideTop),
                            ImVec2(viewRight, viewBottom));
  renderSceneGizmo(ImVec2(viewLeft, sideTop),
                   ImVec2(viewRight, viewBottom));
}

void
EngineGui::renderChat()
{
  if (!engine) {
    chatOpen = false;
    return;
  }
  auto client = engine->getClient();
  auto wm = engine->getWindowManager();
  const bool connected = client && client->isConnected();
  const bool appFocused = wm && wm->hasCurrentOrPendingFocus();
  if (!connected || appFocused) {
    chatOpen = false;
    return;
  }

  // Headless UI test hooks.
  static bool debugChatSent = false;
  if (!debugChatSent) {
    if (const char* message = std::getenv("HACKMATRIX_DEBUG_CHAT_SEND")) {
      debugChatSent = true;
      client->sendChat(message);
    }
  }
  if (std::getenv("HACKMATRIX_DEBUG_CHAT_OPEN")) {
    chatOpen = true;
  }

  ImGuiIO& io = ImGui::GetIO();
  if (!chatOpen) {
    if (!io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_T, false)) {
      chatOpen = true;
      chatJustOpened = true;
      chatScrollToBottom = true;
      // Drop the 't' that opened the chat so it is not inserted into the input.
      io.ClearInputCharacters();
    }
    return;
  }

  if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
    chatOpen = false;
    return;
  }

  ImGuiViewport* viewport = ImGui::GetMainViewport();
  const ImVec2 size(460.0f, 280.0f);
  const ImVec2 pos(viewport->WorkPos.x + viewport->WorkSize.x - size.x - 16.0f,
                   viewport->WorkPos.y + viewport->WorkSize.y - size.y - 16.0f);
  ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
  ImGui::SetNextWindowSize(size, ImGuiCond_Always);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.05f, 0.05f, 0.07f, 0.92f));
  ImGui::Begin("##hackmatrix_chat",
               nullptr,
               ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                 ImGuiWindowFlags_NoScrollbar |
                 ImGuiWindowFlags_NoFocusOnAppearing);
  ImGui::TextUnformatted("Chat  (Enter send, Esc close)");
  ImGui::Separator();

  const float footer = ImGui::GetFrameHeightWithSpacing() + 6.0f;
  ImGui::BeginChild("##chat_log",
                    ImVec2(0.0f, -footer),
                    false,
                    ImGuiWindowFlags_NoScrollbar);
  auto log = client->chatLog();
  if (log.size() != lastChatCount) {
    lastChatCount = log.size();
    chatScrollToBottom = true;
  }
  for (const auto& entry : log) {
    if (entry.system) {
      ImGui::TextColored(ImVec4(0.65f, 0.65f, 0.65f, 1.0f),
                         "* %s",
                         entry.text.c_str());
    } else {
      ImGui::TextColored(ImVec4(0.55f, 0.80f, 1.0f, 1.0f),
                         "%s:",
                         entry.name.c_str());
      ImGui::SameLine();
      ImGui::TextWrapped("%s", entry.text.c_str());
    }
  }
  if (chatScrollToBottom) {
    ImGui::SetScrollHereY(1.0f);
    chatScrollToBottom = false;
  }
  ImGui::EndChild();

  if (chatJustOpened) {
    ImGui::SetKeyboardFocusHere();
    chatJustOpened = false;
  }
  ImGui::SetNextItemWidth(-1.0f);
  const bool submitted =
    ImGui::InputText("##chat_input",
                     chatInput,
                     sizeof(chatInput),
                     ImGuiInputTextFlags_EnterReturnsTrue);
  if (submitted) {
    if (chatInput[0] != '\0') {
      client->sendChat(chatInput);
    }
    chatInput[0] = '\0';
    chatScrollToBottom = true;
    ImGui::SetKeyboardFocusHere(-1);
  }
  ImGui::End();
  ImGui::PopStyleColor();
}

void
EngineGui::renderExitConfirm()
{
  if (!exitConfirmOpen) {
    return;
  }
  const bool ignoreKeys = exitConfirmIgnoreFrames > 0;
  if (exitConfirmIgnoreFrames > 0) {
    --exitConfirmIgnoreFrames;
  }
  ImGuiViewport* viewport = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(
    ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
           viewport->WorkPos.y + viewport->WorkSize.y * 0.5f),
    ImGuiCond_Always,
    ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(320.0f, 0.0f), ImGuiCond_Always);
  ImGui::Begin("Exit HackMatrix?",
               nullptr,
               ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                 ImGuiWindowFlags_AlwaysAutoResize);
  ImGui::TextUnformatted("Are you sure you want to exit?");
  ImGui::TextDisabled("Enter = Exit, Esc = Cancel");
  ImGui::Spacing();

  // Free view hides the cursor, so Enter/Esc must also work.
  const bool confirm =
    !ignoreKeys && ImGui::IsKeyPressed(ImGuiKey_Enter, false);
  if (confirm || ImGui::Button("Exit", ImVec2(90.0f, 0.0f))) {
    exitConfirmOpen = false;
    ImGui::End();
    if (engine) {
      engine->requestQuit();
    }
    return;
  }
  ImGui::SameLine();
  if (ImGui::Button("Cancel", ImVec2(90.0f, 0.0f)) ||
      (!ignoreKeys && ImGui::IsKeyPressed(ImGuiKey_Escape, false))) {
    exitConfirmOpen = false;
    if (engine) {
      if (auto* controls = engine->getControls()) {
        controls->endExitDialog();
      }
    }
  }
  ImGui::End();
}

void
EngineGui::createNewEntity()
{
  const entt::entity newEntity = registry->createPersistent();

  // A Scene Editor entity is only useful if it can be placed in the world, so
  // start with a transform in front of the camera. Components can be added
  // afterwards from the Inspector.
  glm::vec3 spawn(0.0f);
  if (auto* camera = engine ? engine->getCamera() : nullptr) {
    spawn = camera->position + camera->front * 3.0f;
  }
  registry->emplace<Positionable>(
    newEntity, spawn, glm::vec3(0.0f), glm::vec3(0.0f), 1.0f);

  sceneSelection = newEntity;
  sceneAddComponentOpen = false;
  sceneAddComponentEntity = entt::null;
}

void
EngineGui::addComponentPanel(entt::entity entity, bool& showAddComponentPanel)
{
  static int LIGHT_TYPE = 0;
  static int POSITIONABLE_TYPE = 1; // Adjusted indexes
  static int MODEL_TYPE = 2;
  static int ROTATE_MOVEMENT_TYPE = 3;
  static int DOOR_TYPE = 4;
  static int KEY_TYPE = 5;
  static int LOCK_TYPE = 6;
  static int PARENT_TYPE = 7;
  static int SCRIPTABLE_TYPE = 8;
  static int TRANSLATE_MOVEMENT_TYPE = 9;
  static int BOOTABLE_TYPE = 10;
  static int selectedComponentType = LIGHT_TYPE; // Initialize

  ImGui::Combo("Component Type",
               &selectedComponentType,
               "Light\0Positionable\0Model\0RotateMovement\0Door\0Key\0Lock\0Pa"
               "rent\0Scriptable\0TranslateMovement\0Bootable\0");

  if (selectedComponentType == LIGHT_TYPE) {
    static glm::vec3 lightColor = glm::vec3(1.0f, 1.0f, 1.0f); // Default color

    ImGui::ColorEdit3("Color", (float*)&lightColor);

    if (ImGui::Button("Add Light Component")) {
      registry->emplace<Light>(entity, lightColor);
      showAddComponentPanel = false;
    }
  } else if (selectedComponentType == POSITIONABLE_TYPE) {
    static glm::vec3 position = glm::vec3(0.0f);
    static glm::vec3 origin = glm::vec3(0.0f);
    static glm::vec3 rotation = glm::vec3(0.0f);
    static float scale = 1.0f;

    ImGui::InputFloat3("Position", (float*)&position);
    ImGui::InputFloat3("Origin", (float*)&origin);
    ImGui::InputFloat3("Rotation", (float*)&rotation);
    ImGui::InputFloat("Scale", &scale);

    if (ImGui::Button("Add Positionable Component")) {
      registry->emplace<Positionable>(
        entity, position, origin, rotation, scale);
      showAddComponentPanel = false;
    }
  } else if (selectedComponentType == MODEL_TYPE) {
    static char modelPath[512] = "";
    ImGui::InputText("Model Path##New", modelPath, IM_ARRAYSIZE(modelPath));
    ImGui::TextDisabled("e.g. vox/model.obj (Assimp formats supported)");

    if (ImGui::Button("Add Model Component")) {
      registry->emplace<Model>(entity, modelPath);
      showAddComponentPanel = false;
    }
  } else if (selectedComponentType == ROTATE_MOVEMENT_TYPE) {
    static float degreesToRotate = 0;
    static float degreesPerSecond = 0;
    static glm::vec3 axis = glm::vec3(0.0f);
    ImGui::InputFloat("Degrees", &degreesToRotate);
    ImGui::InputFloat("Degrees/s", &degreesPerSecond);
    ImGui::InputFloat3("Rotation Axis", glm::value_ptr(axis));

    if (ImGui::Button("Add RotateMovement Component")) {
      registry->emplace<RotateMovement>(
        entity, degreesToRotate, degreesPerSecond, axis);
      showAddComponentPanel = false;
    }
  } else if (selectedComponentType == DOOR_TYPE) {
    static float openDegreesToRotate = 0;
    static float openDegreesPerSecond = 0;
    static float closeDegreesToRotate = 0;
    static float closeDegreesPerSecond = 0;
    static glm::vec3 openAxis = glm::vec3(0.0f);
    static glm::vec3 closeAxis = glm::vec3(0.0f);
    static DoorState doorState;

    ImGui::Text("Open RotateMovement");
    ImGui::InputFloat("Degrees##toOpen", &openDegreesToRotate);
    ImGui::InputFloat("Degrees/s##toOpen", &openDegreesPerSecond);
    ImGui::InputFloat3("Rotation Axis##toOpen", glm::value_ptr(openAxis));

    ImGui::Text("Closed RotateMovement");
    ImGui::InputFloat("Degrees##toClose", &closeDegreesToRotate);
    ImGui::InputFloat("Degrees/s##toClose", &closeDegreesPerSecond);
    ImGui::InputFloat3("Rotation Axis##toClose", glm::value_ptr(closeAxis));

    ImGui::RadioButton("Open", (int*)&doorState, (int)DoorState::OPEN);
    ImGui::RadioButton("Closed", (int*)&doorState, (int)DoorState::CLOSED);

    if (ImGui::Button("Add Door Component")) {
      auto open =
        RotateMovement{ openDegreesToRotate, openDegreesPerSecond, openAxis };
      auto close = RotateMovement{ closeDegreesToRotate,
                                   closeDegreesPerSecond,
                                   closeAxis };
      registry->emplace<Door>(entity, open, close, doorState);
      showAddComponentPanel = false;
    }
  } else if (selectedComponentType == KEY_TYPE) {
    static float turnDegreesToRotate = 0;
    static float turnDegreesPerSecond = 0;
    static float unturnDegreesToRotate = 0;
    static float unturnDegreesPerSecond = 0;
    static glm::vec3 turnAxis = glm::vec3(0.0f);
    static glm::vec3 unturnAxis = glm::vec3(0.0f);
    static TurnState turnState;
    static int lockable = 0;

    ImGui::InputInt("Lockable entity ID", &lockable);

    ImGui::Text("Turn RotateMovement");
    ImGui::InputFloat("Degrees##toTurn", &turnDegreesToRotate);
    ImGui::InputFloat("Degrees/s##toTurn", &turnDegreesPerSecond);
    ImGui::InputFloat3("Rotation Axis##toTurn", glm::value_ptr(turnAxis));

    ImGui::Text("Unturnd RotateMovement");
    ImGui::InputFloat("Degrees##toUnturn", &unturnDegreesToRotate);
    ImGui::InputFloat("Degrees/s##toUnturn", &unturnDegreesPerSecond);
    ImGui::InputFloat3("Rotation Axis##toUnturn", glm::value_ptr(unturnAxis));

    ImGui::RadioButton("Turned", (int*)&turnState, (int)TurnState::TURNED);
    ImGui::RadioButton("Unturned", (int*)&turnState, (int)TurnState::UNTURNED);

    if (ImGui::Button("Add Turn Component")) {
      auto turn =
        RotateMovement{ turnDegreesToRotate, turnDegreesPerSecond, turnAxis };
      auto unturn = RotateMovement{ unturnDegreesToRotate,
                                    unturnDegreesPerSecond,
                                    unturnAxis };
      registry->emplace<Key>(entity, lockable, turnState, turn, unturn);
      showAddComponentPanel = false;
    }
  } else if (selectedComponentType == LOCK_TYPE) {
    static glm::vec3 lockPosition = glm::vec3(0.0f);
    static glm::vec3 lockTolerance = glm::vec3(0.0f);
    static LockState lockState;

    ImGui::InputFloat3("Position", (float*)&lockPosition);
    ImGui::InputFloat3("Tolerance", (float*)&lockTolerance);

    ImGui::RadioButton("Locked", (int*)&lockState, (int)LockState::LOCKED);
    ImGui::RadioButton("Unlocked", (int*)&lockState, (int)LockState::UNLOCKED);

    if (ImGui::Button("Add Lock Component")) {
      registry->emplace<Lock>(entity, lockPosition, lockTolerance, lockState);
      showAddComponentPanel = false;
    }
  } else if (selectedComponentType == PARENT_TYPE) {
    static vector<int> childrenIds;
    for (int i = 0; i < childrenIds.size(); i++) {
      ImGui::InputInt(("Children ID##" + std::to_string(i)).c_str(),
                      &childrenIds[i]);
    }
    if (ImGui::Button("+ Add Another Child")) {
      childrenIds.push_back(0);
    }
    if (childrenIds.size() > 0 && ImGui::Button("+ Remove Child")) {
      childrenIds.pop_back();
    }
    if (ImGui::Button("Add Parent Component")) {
      registry->emplace<Parent>(entity, childrenIds);
      childrenIds = vector<int>();
      showAddComponentPanel = false;
    }
  } else if (selectedComponentType == SCRIPTABLE_TYPE) {
    static ScriptLanguage scriptLanguage;
    ImGui::RadioButton("C++", (int*)&scriptLanguage, (int)CPP);
    ImGui::RadioButton("Javascript", (int*)&scriptLanguage, (int)JAVASCRIPT);
    ImGui::RadioButton("Python", (int*)&scriptLanguage, (int)PYTHON);

    if (ImGui::Button("Add Scriptable Component")) {
      registry->emplace<Scriptable>(entity, "", scriptLanguage);
      systems::emplaceBoundingSphere(registry, entity);
      showAddComponentPanel = false;
    }
  } else if (selectedComponentType == TRANSLATE_MOVEMENT_TYPE) {
    static glm::vec3 translateDelta(0.0f);
    static float unitsPerSecond = 0.0f;

    ImGui::InputFloat3("Translation Delta", glm::value_ptr(translateDelta));
    ImGui::InputFloat("Units/s", &unitsPerSecond);

    if (ImGui::Button("Add TranslateMovement Component")) {
      registry->emplace<TranslateMovement>(
        entity, translateDelta, unitsPerSecond);
      showAddComponentPanel = false;
    }
  } else if (selectedComponentType == BOOTABLE_TYPE) {
    static char cmd[128] = "";
    static char args[128] = "";
    static char name[128] = "";
    static int killOnExit;
    static int transparent = 0;
    static int bootOnStartup = 1;
    static int xy[2];
    ImGui::InputText("Command##NewBootable", cmd, IM_ARRAYSIZE(cmd));
    ImGui::InputText("Args##NewBootable", args, IM_ARRAYSIZE(args));
    ImGui::InputText("Name##NewBootable", name, IM_ARRAYSIZE(name));
    ImGui::InputInt2("XY:", xy);
    ImGui::Text("Kill on exit:");
    ImGui::RadioButton("True##KillOnExit", &killOnExit, 1);
    ImGui::RadioButton("False##KillOnExit", &killOnExit, 0);
    ImGui::Text("Transparent:");
    ImGui::RadioButton("True##Transparent", &transparent, 1);
    ImGui::RadioButton("False##Transparent", &transparent, 0);
    ImGui::Text("Boot on startup:");
    ImGui::RadioButton("True##BootOnStartup", &bootOnStartup, 1);
    ImGui::RadioButton("False##BootOnStartup", &bootOnStartup, 0);

    if (ImGui::Button("Add Bootable Component")) {
      std::string strName = name;
      optional<std::string> optname;
      if (strName.length() > 0) {
        optname = strName;
      }
      registry->emplace<Bootable>(entity,
                                  cmd,
                                  args,
                                  killOnExit == 1 ? true : false,
                                  nullopt,
                                  transparent == 1 ? true : false,
                                  optname,
                                  bootOnStartup);
      showAddComponentPanel = false;
    }
  }
}

function<string(string)>
makeLabeler(entt::entity entity, string component = "")
{
  return [entity, component](string name) -> string {
    stringstream rv;
    rv << name << "##" << to_string((int)entity) << "#" << component;
    return rv.str();
  };
}

void
EngineGui::renderComponentPanel(entt::entity entity)
{
  if (registry->any_of<Light>(entity)) {
    auto& light = registry->get<Light>(entity);
    ImGui::Text("Light Component:");
    ImGui::BeginGroup();
    ImGui::ColorEdit3(("Color##" + to_string((int)entity)).c_str(),
                      (float*)&light.color);
    if (ImGui::Button(
          ("Delete Component##Light" + to_string((int)entity)).c_str())) {
      registry->removePersistent<Light>(entity);
    }
    ImGui::EndGroup();
    ImGui::Spacing();
  }
  if (registry->any_of<Positionable>(entity)) {
    auto& positionable = registry->get<Positionable>(entity);

    auto copiedPos = positionable.pos;
    auto copiedOrigin = positionable.origin;
    auto copiedRotate = positionable.rotate;
    auto copiedScale = positionable.scale;

    ImGui::Text("Positioner Component:");
    ImGui::BeginGroup();
    ImGui::InputFloat3(("Position##" + to_string((int)entity)).c_str(),
                       (float*)&positionable.pos);
    ImGui::InputFloat3(("Origin##" + to_string((int)entity)).c_str(),
                       (float*)&positionable.origin);
    ImGui::InputFloat3(("Rotation##" + to_string((int)entity)).c_str(),
                       (float*)&positionable.rotate);
    ImGui::InputFloat(("Scale##" + to_string((int)entity)).c_str(),
                      &positionable.scale);
    if (ImGui::Button(
          ("Delete Component##Positioner" + to_string((int)entity)).c_str())) {
      registry->removePersistent<Positionable>(entity);
    }
    ImGui::EndGroup();
    ImGui::Spacing();

    if (copiedPos != positionable.pos || positionable.origin != copiedOrigin ||
        copiedScale != positionable.scale ||
        copiedRotate != positionable.rotate) {
      // systems::update(registry, entity);
      positionable.damage();
    }
  }
  if (registry->any_of<Model>(entity)) {
    auto& model = registry->get<Model>(entity);
    ImGui::Text("Model Component:");
    ImGui::BeginGroup();
    ImGui::TextWrapped("%s", model.path.c_str());
    ImGui::TextDisabled("Remove and re-add to change (import lands next).");
    if (ImGui::Button(
          ("Delete Component##Model" + to_string((int)entity)).c_str())) {
      registry->removePersistent<Model>(entity);
    }
    ImGui::EndGroup();
    ImGui::Spacing();
  }
  if (registry->any_of<RotateMovement>(entity)) {
    // not a reference, because I'm just displaying text
    auto rotateMovement = registry->get<RotateMovement>(entity);
    ImGui::Text("RotateMovement Component");
    ImGui::Text("Degrees: %f", rotateMovement.degrees);
    ImGui::Text("Degrees/s: %f", rotateMovement.degreesPerSecond);
    auto axis = rotateMovement.axis;
    ImGui::Text("Axis: (%f, %f, %f)", axis.x, axis.y, axis.z);
    if (ImGui::Button(
          ("Delete Component##RotateMovement" + to_string((int)entity))
            .c_str())) {
      registry->remove<RotateMovement>(entity);
    }
    ImGui::Spacing();
  }
  if (registry->any_of<TranslateMovement>(entity)) {
    // not a reference, because I'm just displaying text
    auto translateMovement = registry->get<TranslateMovement>(entity);

    ImGui::Text("TranslateMovement Component");
    ImGui::Text("Delta: (%.2f, %.2f, %.2f)",
                translateMovement.delta.x,
                translateMovement.delta.y,
                translateMovement.delta.z);
    ImGui::Text("Units/s: %.2f", translateMovement.unitsPerSecond);

    if (ImGui::Button(
          ("Delete Component##TranslateMovement" + to_string((int)entity))
            .c_str())) {
      registry->remove<TranslateMovement>(entity);
    }

    ImGui::Spacing();
  }
  if (registry->any_of<Door>(entity)) {
    auto& door = registry->get<Door>(entity);

    ImGui::Text("Door Component");
    ImGui::Text("Open RotateMovement");
    ImGui::InputDouble(("Degrees##toOpen" + to_string((int)entity)).c_str(),
                       &door.openMovement.degrees);
    ImGui::InputDouble(("Degrees/s##toOpen" + to_string((int)entity)).c_str(),
                       &door.openMovement.degreesPerSecond);
    ImGui::InputFloat3(
      ("Rotation Axis##toOpen" + to_string((int)entity)).c_str(),
      glm::value_ptr(door.openMovement.axis));

    ImGui::Text("Close RotateMovement");
    ImGui::InputDouble(("Degrees##toClose" + to_string((int)entity)).c_str(),
                       &door.closeMovement.degrees);
    ImGui::InputDouble(("Degrees/s##toClose" + to_string((int)entity)).c_str(),
                       &door.closeMovement.degreesPerSecond);
    ImGui::InputFloat3(
      ("Rotation Axis##toClose" + to_string((int)entity)).c_str(),
      glm::value_ptr(door.closeMovement.axis));

    ImGui::RadioButton(("Open" + to_string((int)entity)).c_str(),
                       (int*)&door.state,
                       (int)DoorState::OPEN);
    ImGui::RadioButton(("Closed" + to_string((int)entity)).c_str(),
                       (int*)&door.state,
                       (int)DoorState::CLOSED);

    if (ImGui::Button(("Open Door##" + to_string((int)entity)).c_str())) {
      systems::openDoor(registry, entity);
    }
    if (ImGui::Button(("Close Door##" + to_string((int)entity)).c_str())) {
      systems::closeDoor(registry, entity);
    }
    if (ImGui::Button(
          ("Delete Component##Door" + to_string((int)entity)).c_str())) {
      registry->removePersistent<Door>(entity);
    }
    ImGui::Spacing();
  }
  if (registry->any_of<Key>(entity)) {
    auto& key = registry->get<Key>(entity);

    ImGui::Text("Key Component");
    ImGui::InputInt(("Lockable##" + to_string((int)entity)).c_str(),
                    &key.lockable);
    ImGui::Text("Open RotateMovement##Key");
    ImGui::InputDouble(("Degrees##toTurn" + to_string((int)entity)).c_str(),
                       &key.turnMovement.degrees);
    ImGui::InputDouble(("Degrees/s##toTurn" + to_string((int)entity)).c_str(),
                       &key.turnMovement.degreesPerSecond);
    ImGui::InputFloat3(
      ("Rotation Axis##toTurn" + to_string((int)entity)).c_str(),
      glm::value_ptr(key.turnMovement.axis));

    ImGui::Text("Close RotateMovement");
    ImGui::InputDouble(("Degrees##toUnturn" + to_string((int)entity)).c_str(),
                       &key.unturnMovement.degrees);
    ImGui::InputDouble(("Degrees/s##toUnturn" + to_string((int)entity)).c_str(),
                       &key.unturnMovement.degreesPerSecond);
    ImGui::InputFloat3(
      ("Rotation Axis##toUnturn" + to_string((int)entity)).c_str(),
      glm::value_ptr(key.unturnMovement.axis));

    ImGui::RadioButton(("Turned##" + to_string((int)entity)).c_str(),
                       (int*)&key.state,
                       (int)TurnState::TURNED);
    ImGui::RadioButton(("Unturned##" + to_string((int)entity)).c_str(),
                       (int*)&key.state,
                       (int)TurnState::UNTURNED);

    if (ImGui::Button(("Turn Key##" + to_string((int)entity)).c_str())) {
      systems::turnKey(registry, entity);
    }
    if (ImGui::Button(("Unturn Key##" + to_string((int)entity)).c_str())) {
      systems::unturnKey(registry, entity);
    }
    if (ImGui::Button(
          ("Delete Component##Key" + to_string((int)entity)).c_str())) {
      registry->removePersistent<Key>(entity);
    }
    ImGui::Spacing();
  }
  if (registry->any_of<Lock>(entity)) {
    auto& lock = registry->get<Lock>(entity);

    ImGui::Text("Positioner Component:");
    ImGui::BeginGroup();
    ImGui::InputFloat3(("Position##Lock" + to_string((int)entity)).c_str(),
                       (float*)&lock.position);
    ImGui::InputFloat3(("Tolerance##Lock" + to_string((int)entity)).c_str(),
                       (float*)&lock.tolerance);
    ImGui::RadioButton(("Locked##" + to_string((int)entity)).c_str(),
                       (int*)&lock.state,
                       (int)LockState::LOCKED);
    ImGui::RadioButton(("Unlocked##" + to_string((int)entity)).c_str(),
                       (int*)&lock.state,
                       (int)LockState::UNLOCKED);
    if (ImGui::Button(("Lock##" + to_string((int)entity)).c_str())) {
      // lock
    }
    if (ImGui::Button(("Unlock##" + to_string((int)entity)).c_str())) {
      // unlock
    }

    if (ImGui::Button(
          ("Delete Component##Lock" + to_string((int)entity)).c_str())) {
      registry->removePersistent<Lock>(entity);
    }
    ImGui::EndGroup();
    ImGui::Spacing();
  }
  if (registry->any_of<Parent>(entity)) {
    auto& parent = registry->get<Parent>(entity);
    ImGui::Text("Parent Component:");
    for (int i = 0; i < parent.childrenIds.size(); i++) {
      ImGui::InputInt(
        ("Child Id##" + to_string(i) + to_string((int)entity)).c_str(),
        &parent.childrenIds[i]);
    }
    if (ImGui::Button(("- Remove Child##" + to_string((int)entity)).c_str())) {
      parent.childrenIds.pop_back();
    }
    if (ImGui::Button(("+ Add Child##" + to_string((int)entity)).c_str())) {
      parent.childrenIds.push_back(0);
    }
    if (ImGui::Button(
          ("Delete Component##Parent" + to_string((int)entity)).c_str())) {
      registry->removePersistent<Positionable>(entity);
    }
    ImGui::Spacing();
  }
  if (registry->any_of<Scriptable>(entity)) {
    ImGui::Text("Scriptable Component:");
    auto& scriptable = registry->get<Scriptable>(entity);

    auto label = makeLabeler(entity);

    ImGui::RadioButton(
      label("C++").c_str(), (int*)&scriptable.language, (int)CPP);
    ImGui::RadioButton(
      label("Javascript").c_str(), (int*)&scriptable.language, (int)JAVASCRIPT);
    ImGui::RadioButton(
      label("Python").c_str(), (int*)&scriptable.language, (int)PYTHON);
    if (ImGui::Button(label("Edit Script").c_str())) {
      systems::editScript(registry, entity);
    }
    if (ImGui::Button(label("Delete Component##Scriptable").c_str())) {
      registry->removePersistent<Scriptable>(entity);
    }
    ImGui::Spacing();
  }

  if (registry->any_of<BoundingSphere>(entity)) {
    ImGui::Text("Bounding Sphere Component:");
    auto boundingSphere = registry->get<BoundingSphere>(entity);

    auto label = makeLabeler(entity);
    ImGui::Text("Center: (%f, %f, %f)",
                boundingSphere.center.x,
                boundingSphere.center.y,
                boundingSphere.center.z);
    ImGui::Text("Radius: %f", boundingSphere.radius);
    ImGui::Spacing();
  }

  if (registry->any_of<Bootable>(entity)) {
    ImGui::Text("Bootable Component:");
    auto& bootable = registry->get<Bootable>(entity);
    auto label = makeLabeler(entity, "Bootable");

    char cmd[128] = "";
    strcpy(cmd, bootable.cmd.c_str());

    char args[128] = "";
    strcpy(args, bootable.args.c_str());

    char name[128] = "";
    if (bootable.name.has_value()) {
      strcpy(name, bootable.name.value().c_str());
    }

    int killOnExit = bootable.killOnExit ? 1 : 0;
    int transparent = bootable.transparent ? 1 : 0;
    int bootOnStartup = bootable.bootOnStartup ? 1 : 0;
    int width = bootable.getWidth();
    int height = bootable.getHeight();
    int oldWidth = width;
    int oldHeight = height;
    int xy[2];
    xy[0] = bootable.x;
    xy[1] = bootable.y;
    int oldX = xy[0];
    int oldY = xy[1];

    ImGui::InputText(label("Command").c_str(), cmd, IM_ARRAYSIZE(cmd));
    ImGui::InputText(label("Args").c_str(), args, IM_ARRAYSIZE(args));
    ImGui::InputText(label("Name").c_str(), name, IM_ARRAYSIZE(name));
    ImGui::InputInt(label("Width").c_str(), &width);
    ImGui::InputInt(label("Height").c_str(), &height);
    ImGui::InputInt2(label("X,Y").c_str(), xy);
    ImGui::Text("Kill on Exit:");
    ImGui::RadioButton(label("True##KillOnExit").c_str(), &killOnExit, 1);
    ImGui::RadioButton(label("False##KillOnExit").c_str(), &killOnExit, 0);
    if (bootable.pid.has_value()) {
      ImGui::Text("PID: %d", bootable.pid.value());
    } else {
      ImGui::Text("PID: N/A");
    }
    ImGui::Text("Transparent:");
    ImGui::RadioButton(label("True##Transparent").c_str(), &transparent, 1);
    ImGui::RadioButton(label("False##Transparent").c_str(), &transparent, 0);
    ImGui::Text("Boot on Startup");
    ImGui::RadioButton(label("True##BootOnStartup").c_str(), &bootOnStartup, 1);
    ImGui::RadioButton(
      label("False##BootOnStartup").c_str(), &bootOnStartup, 0);
    if (ImGui::Button(label("Delete Component##Bootable").c_str())) {
      registry->removePersistent<Bootable>(entity);
    }
    ImGui::Spacing();


    bootable.cmd = cmd;
    bootable.args = args;
    if (string(name) != "") {
      bootable.name = name;
    } else {
      bootable.name = nullopt;
    }
    bootable.killOnExit = killOnExit == 1 ? true : false;
    bootable.transparent = transparent == 1 ? true : false;
    bootable.bootOnStartup = bootOnStartup == 1 ? true : false;
    if (oldWidth != width || oldHeight != height || oldX != xy[0] ||
        oldY != xy[1]) {
      bootable.x = xy[0];
      bootable.y = xy[1];
      systems::resizeBootable(registry, entity, width, height);
    }
  }
}

shared_ptr<LoggerVector>
EngineGui::getLoggerVector()
{
  return loggerVector;
}
