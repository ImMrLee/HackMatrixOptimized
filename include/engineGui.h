#pragma once
#include <cstdlib>
#include "MultiPlayer/Client.h"
#include "MultiPlayer/Server.h"
#include "imgui/imgui.h"
#include "imgui/imgui_impl_opengl3.h"
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include "model.h"

#include "logger.h"
#include "entity.h"
#include "system_settings.h"
#include "components/Bootable.h"
#include "components/Door.h"
#include "components/Key.h"
#include "components/Light.h"
#include "components/Lock.h"
#include "components/Parent.h"
#include "components/RotateMovement.h"
#include "components/Scriptable.h"
#include "components/TranslateMovement.h"

using namespace std;

namespace MultiPlayer {
class Gui;
}

class Engine;
class EngineGui
{
  Engine* engine;
  shared_ptr<EntityRegistry> registry;
  shared_ptr<LoggerVector> loggerVector;
  bool imguiGlInitialized = false;
  // wlroots presents the output FBO top-down; ImGui draws for the opposite
  // orientation. Override with HACKMATRIX_IMGUI_FLIP=0 if it renders correctly.
  bool imguiFlipY = true;
  void addComponentPanel(entt::entity, bool&);
  void renderComponentPanel(entt::entity);

  // Left-edge collapsible sidebar. Hidden by default; the edge tab slides the
  // panel out and it covers about 20% of the output width.
  bool menuVisible = false;
  float menuAnim = 0.0f;
  void renderMenu(MultiPlayer::Gui& gui, float fps);

  // System Config tab (audio / display / HackMatrix-specific settings).
  SystemSettings systemSettings_;
  void renderSystemConfig();

  // Scene Editor: outliner + inspector dock around the existing 3D viewport.
  // Phase 1 keeps the runtime data model (Entity/Positionable/components) and
  // provides selection, creation, duplication, deletion and component editing.
  // Close discards everything edited since the last Save; Save recaptures
  // this baseline. Snapshot covers all persisted scene components but leaves
  // runtime-only components (Wayland apps, remote windows) untouched.
  struct SceneEntitySnapshot
  {
    int64_t entityId = 0;
    bool hasModel = false;
    std::string modelPath;
    std::string modelNode;
    bool hasPositionable = false;
    glm::vec3 position{ 0.0f };
    glm::vec3 origin{ 0.0f };
    glm::vec3 rotation{ 0.0f };
    float scale = 1.0f;
    bool hasLight = false;
    glm::vec3 lightColor{ 1.0f };
    std::optional<Door> door;
    std::optional<Key> key;
    std::optional<Lock> lock;
    std::optional<Parent> parent;
    std::optional<RotateMovement> rotateMovement;
    std::optional<TranslateMovement> translateMovement;
    bool hasScriptable = false;
    std::string script;
    ScriptLanguage scriptLanguage = CPP;
    bool hasBootable = false;
    std::string bootCmd;
    std::string bootArgs;
    bool bootKillOnExit = false;
    bool bootTransparent = false;
    bool bootOnStartup = false;
    std::optional<std::string> bootName;
    int bootWidth = 0;
    int bootHeight = 0;
    int bootX = 0;
    int bootY = 0;
  };
  std::vector<SceneEntitySnapshot> sceneBaseline;
  bool sceneBaselineValid = false;
  void captureSceneBaseline();
  void restoreSceneBaseline();
  void saveSceneChanges();

  bool sceneEditorOpen = false;
  entt::entity sceneSelection = entt::null;
  std::vector<entt::entity> sceneSelectionSet;
  entt::entity sceneAddComponentEntity = entt::null;
  bool sceneAddComponentOpen = false;
  char sceneFilter[256] = {};
  void setSceneEditorOpen(bool open);
  bool isSceneEntitySelected(entt::entity entity) const;
  void selectSceneEntity(entt::entity entity, bool additive);
  void pruneSceneSelection();
  void deleteSelectedSceneEntities();
  void duplicateSelectedSceneEntities();
  bool sceneStructureLocked() const;
  bool reparentSceneEntity(entt::entity child, entt::entity newParent);
  void applySceneReparent(entt::entity child, entt::entity newParent);
  struct SceneCommand
  {
    std::string label;
    std::function<void()> undo;
    std::function<void()> redo;
  };
  std::vector<SceneCommand> sceneUndoStack;
  std::vector<SceneCommand> sceneRedoStack;
  void pushSceneCommand(SceneCommand command);
  void undoSceneEdit();
  void redoSceneEdit();
  void renderSceneEditor();
  void renderSceneToolbar(const ImVec2& pos, const ImVec2& size);
  void renderSceneOutliner(const ImVec2& pos, const ImVec2& size);
  void renderSceneInspector(const ImVec2& pos, const ImVec2& size);
  void renderSceneAssetBrowser(const ImVec2& pos, const ImVec2& size);
  void refreshSceneAssets();
  void importSceneAsset(const std::string& path);
  void importSceneAssetAsParts(const std::string& path);
  std::vector<std::string> sceneAssets;
  double sceneAssetsScannedAt = -1.0;
  char sceneAssetFilter[256] = {};
  bool sceneImportAsParts = false;
  bool sceneSimulationPaused = true;
  entt::entity pickSceneEntity(float mouseX, float mouseY);
  void drawSceneSelectionOverlay(const ImVec2& clipMin, const ImVec2& clipMax);
  entt::entity duplicateEntity(entt::entity source);

  // Scene Editor camera navigation: WASD moves, middle-drag looks around.
  bool sceneCameraDragActive = false;
  void handleSceneCameraInput(const ImVec2& mouse, bool mouseInView);

  // Translate gizmo: three screen-space axis handles at the selected
  // Positionable. Edits are applied locally while dragging and committed to
  // the network on release.
  enum class SceneGizmoMode
  {
    Translate,
    Rotate,
    Scale,
  };
  SceneGizmoMode sceneGizmoMode = SceneGizmoMode::Translate;
  int sceneGizmoHoverAxis = -1;
  int sceneGizmoDragAxis = -1;
  entt::entity sceneGizmoEntity = entt::null;
  ImVec2 sceneGizmoStartMouse{ 0.0f, 0.0f };
  ImVec2 sceneGizmoStartOrigin{ 0.0f, 0.0f };
  float sceneGizmoStartAngle = 0.0f;
  float sceneGizmoStartRadiusPixels = 1.0f;
  glm::vec2 sceneGizmoAxisScreen{ 0.0f };
  float sceneGizmoWorldPerPixel = 0.0f;
  std::vector<entt::entity> sceneGizmoTargets;
  std::unordered_map<entt::entity, glm::vec3> sceneGizmoStartPositions;
  std::unordered_map<entt::entity, glm::vec3> sceneGizmoStartRotations;
  std::unordered_map<entt::entity, float> sceneGizmoStartScales;
  void renderSceneGizmo(const ImVec2& clipMin, const ImVec2& clipMax);
  bool handleSceneGizmoInput(const ImVec2& mouse, bool mouseInView);
  void updateSceneGizmoDrag(const ImVec2& mouse);
  void endSceneGizmoDrag();
  void commitSceneTransform(entt::entity entity);

  // Multiplayer chat overlay (T to open, Enter send, Esc close).
  bool chatOpen = false;
  bool chatJustOpened = false;
  bool chatScrollToBottom = false;
  std::size_t lastChatCount = 0;
  char chatInput[1024] = {};
  void renderChat();

  // Esc-to-exit confirmation dialog.
  bool exitConfirmOpen = false;
  // The Esc press that opened the dialog can still be visible to ImGui for a
  // frame or two, so ignore Enter/Esc briefly or it cancels immediately.
  int exitConfirmIgnoreFrames = 0;
  void renderExitConfirm();

public:
  bool isChatOpen() const { return chatOpen; }
  bool isExitConfirmOpen() const { return exitConfirmOpen; }
  bool isSceneEditorOpen() const { return sceneEditorOpen; }
  entt::entity getSceneSelection() const { return sceneSelection; }
  // Used by Engine shutdown before its final saveAll(): unsaved editor state
  // must not leak into the database when the process exits.
  void discardSceneChangesIfOpen();
  void openExitConfirm()
  {
    exitConfirmOpen = true;
    exitConfirmIgnoreFrames = 2;
    // Headless regression hook: mimic the real keyboard feeding the same Esc
    // press to ImGui on the opening frame.
    if (std::getenv("HACKMATRIX_DEBUG_EXIT_FLASH")) {
      ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, true);
    }
  }
  EngineGui(Engine* engine,
            shared_ptr<EntityRegistry> registry,
            shared_ptr<LoggerVector>);
  void render(double&, int, vector<double>&);
  shared_ptr<LoggerVector> getLoggerVector();
  void createNewEntity();
};
