#include "engine.h"
#include "MultiPlayer/Gui.h"
#include "MultiPlayer/Server.h"
#include "MultiPlayer/Client.h"
#include "model.h"
#include "remote_app.h"
#include "wayland_app.h"
#include "imgui.h"
#include <algorithm>
#include <cstdlib>
#include <enet/enet.h>
#include <string>
#include <vector>

namespace MultiPlayer {

Gui::Gui(Engine* engine)
  : connect(false)
  , address("127.0.0.1")
  , port(7777)
  , engine(engine)
{
  if (const char* env = std::getenv("HACKMATRIX_DEBUG_MULTIPLAYER_CONNECT")) {
    debugConnect = env;
  }
}

Gui::~Gui() {}

void
Gui::Render()
{
  const int cAddressSize = 1024;
  static char cAddress[cAddressSize];
  static int selectedMode = 0;
  const char* modes[] = { "Client", "Server" };

  ImGui::Combo("Server/Client", &selectedMode, modes, IM_ARRAYSIZE(modes));

  if (selectedMode == 0) {
    ImGui::InputText("Address", cAddress, cAddressSize);
    address = cAddress;
  }

  ImGui::InputInt("Port", &port);

  // Headless UI test hook: connect once to HACKMATRIX_DEBUG_MULTIPLAYER_CONNECT.
  if (!debugConnect.empty() && !debugConnectAttempted && engine) {
    debugConnectAttempted = true;
    const auto separator = debugConnect.rfind(':');
    if (separator != std::string::npos && separator + 1 < debugConnect.size()) {
      const std::string host = debugConnect.substr(0, separator);
      const int debugPort = std::atoi(debugConnect.c_str() + separator + 1);
      if (!host.empty() && debugPort > 0 && debugPort <= 65535) {
        auto debugClient = std::make_shared<Client>(engine->getRegistry());
        if (debugClient->connect(host, debugPort)) {
          client = debugClient;
          engine->registerClient(client);
        }
      }
    }
  }

  // The engine owns the multiplayer objects. WebGui requests are drained on
  // the render thread before this panel runs, so this read is safe.
  if (engine) {
    client = engine->getClient();
    server = engine->getServer();
  }
  auto activeClient = client;
  auto activeServer = server;

  if (selectedMode == 0) {
    if (activeClient && activeClient->isConnected()) {
      pruneLoopbackPreviews();
      if (!streamSettingsInitialized) {
        pendingShareFps = activeClient->windowStreamFps();
        pendingShareQuality = activeClient->windowStreamQuality();
        streamSettingsInitialized = true;
      }
      ImGui::TextColored(
        ImVec4(0.45f, 1.0f, 0.45f, 1.0f), "%s",
        activeClient->connectionStateText().c_str());

      const std::string notice = activeClient->controlNotice();
      if (!notice.empty()) {
        ImGui::TextColored(
          ImVec4(1.0f, 0.75f, 0.25f, 1.0f), "%s", notice.c_str());
      }

      if (activeClient->isComputingScene()) {
        ImGui::TextDisabled("Computing scene fingerprint...");
      } else if (!activeClient->hasLocalScene()) {
        ImGui::TextDisabled("Scene fingerprint unavailable");
      } else if (activeClient->hasSceneMismatch()) {
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f),
                           "Scene mismatch: window sharing disabled");
        auto mismatched = activeClient->mismatchedPeers();
        if (!mismatched.empty()) {
          ImGui::SameLine();
          ImGui::TextDisabled("(%zu peer(s))", mismatched.size());
        }
      } else if (activeClient->hasPeerSceneInfo()) {
        ImGui::TextColored(ImVec4(0.45f, 1.0f, 0.45f, 1.0f),
                           "Scenes match: window sharing available");
      } else {
        ImGui::TextDisabled("Waiting for peer scene info...");
      }
      if (activeClient->hasLocalScene()) {
        ImGui::TextDisabled("Local scene %s",
                            activeClient->localSceneFingerprint().hex().c_str());
      }

      ImGui::Separator();
      if (ImGui::CollapsingHeader("Window streaming",
                                  ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::SetNextItemWidth(180.0f);
        ImGui::SliderInt("Target FPS",
                         &pendingShareFps,
                         1,
                         30,
                         "%d fps");
        if (ImGui::IsItemDeactivatedAfterEdit()) {
          activeClient->setWindowStreamFps(pendingShareFps);
          pendingShareFps = activeClient->windowStreamFps();
        }

        ImGui::SetNextItemWidth(180.0f);
        ImGui::SliderInt("Encoder quality",
                         &pendingShareQuality,
                         10,
                         95);
        if (ImGui::IsItemDeactivatedAfterEdit()) {
          activeClient->setWindowStreamQuality(pendingShareQuality);
          pendingShareQuality = activeClient->windowStreamQuality();
        }

        const int intervalMs = 1000 / std::max(1, pendingShareFps);
        ImGui::TextDisabled(
          "Capture target: %d FPS, quality %d (%d ms/frame)",
          pendingShareFps,
          pendingShareQuality,
          intervalMs);
      }

      ImGui::Separator();
      ImGui::TextUnformatted("Shared windows");

      const bool localSceneReady = activeClient->hasLocalScene();
      const bool sceneKnown = activeClient->hasPeerSceneInfo();
      const bool sceneMismatch = activeClient->hasSceneMismatch();
      const bool canShare = localSceneReady && sceneKnown && !sceneMismatch;

      auto registry = engine ? engine->getRegistry() : nullptr;
      if (!registry) {
        ImGui::TextDisabled("Entity registry unavailable");
      } else {
        // Drop state for windows that have been closed since the last frame.
        for (auto entity : activeClient->sharedWindows()) {
          if (!registry->valid(entity) ||
              !registry->all_of<WaylandApp::Component>(entity)) {
            activeClient->setWindowShared(entity, false);
          }
        }

        auto view = registry->view<WaylandApp::Component, Positionable>();
        std::vector<entt::entity> apps;
        for (auto entity : view) {
          const auto& component = view.get<WaylandApp::Component>(entity);
          if (!component.app || component.accessory || component.layer_shell) {
            continue;
          }
          apps.push_back(entity);
        }
        std::sort(apps.begin(), apps.end(), [&](entt::entity a, entt::entity b) {
          const auto& appA = view.get<WaylandApp::Component>(a);
          const auto& appB = view.get<WaylandApp::Component>(b);
          return appA.app->getWindowName() < appB.app->getWindowName();
        });

        // Headless UI test hook: spawn one loopback preview without a mouse.
        static bool debugPreviewSpawned = false;
        if (!debugPreviewSpawned &&
            std::getenv("HACKMATRIX_DEBUG_PREVIEW_FIRST_WINDOW") != nullptr &&
            !apps.empty() && loopbackPreviews.empty()) {
          createLoopbackPreview(apps.front());
          debugPreviewSpawned = true;
        }

        // Headless test hook: publish the first window once a matching peer is
        // present, without clicking the Share checkbox.
        static bool debugShareSpawned = false;
        if (!debugShareSpawned && canShare && !apps.empty() &&
            std::getenv("HACKMATRIX_DEBUG_SHARE_FIRST_WINDOW") != nullptr) {
          activeClient->setWindowShared(apps.front(), true);
          if (std::getenv("HACKMATRIX_DEBUG_ALLOW_CONTROL_FIRST_WINDOW") !=
              nullptr) {
            activeClient->setWindowAllowControl(apps.front(), true);
          }
          debugShareSpawned = true;
        }

        if (apps.empty()) {
          ImGui::TextDisabled("No shareable windows");
        } else {
          ImGui::BeginDisabled(!canShare);
          if (ImGui::BeginTable("##shared_windows",
                                6,
                                ImGuiTableFlags_Borders |
                                  ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Window");
            ImGui::TableSetupColumn("Size");
            ImGui::TableSetupColumn("Stream");
            ImGui::TableSetupColumn("Share");
            ImGui::TableSetupColumn("Control");
            ImGui::TableSetupColumn("Preview");
            ImGui::TableHeadersRow();

            for (auto entity : apps) {
              const auto& component = view.get<WaylandApp::Component>(entity);
              if (!component.app) {
                continue;
              }
              const auto settings = activeClient->windowShare(entity);
              ImGui::PushID(
                static_cast<int>(entt::to_integral(entity)));
              ImGui::TableNextRow();

              ImGui::TableSetColumnIndex(0);
              std::string name = component.app->getWindowName();
              if (name.empty()) {
                name = "Window " + std::to_string(
                                    static_cast<unsigned int>(
                                      entt::to_integral(entity)));
              }
              ImGui::TextUnformatted(name.c_str());

              ImGui::TableSetColumnIndex(1);
              ImGui::Text("%dx%d",
                          component.app->getWidth(),
                          component.app->getHeight());

              ImGui::TableSetColumnIndex(2);
              const std::string transport =
                activeClient->windowStreamTransport(entity);
              if (transport.empty()) {
                ImGui::TextDisabled("-");
              } else {
                ImGui::TextUnformatted(transport.c_str());
              }

              ImGui::TableSetColumnIndex(3);
              bool shared = settings.shared;
              if (ImGui::Checkbox("##shared", &shared)) {
                activeClient->setWindowShared(entity, shared);
              }

              ImGui::TableSetColumnIndex(4);
              bool allowControl = settings.allowControl;
              ImGui::BeginDisabled(!shared);
              if (ImGui::Checkbox("##allow_control", &allowControl)) {
                activeClient->setWindowAllowControl(entity, allowControl);
              }
              ImGui::EndDisabled();

              ImGui::TableSetColumnIndex(5);
              const bool hasPreview =
                loopbackPreviews.find(entity) != loopbackPreviews.end();
              if (ImGui::SmallButton(hasPreview ? "Stop" : "Preview")) {
                if (hasPreview) {
                  destroyLoopbackPreview(entity);
                } else {
                  createLoopbackPreview(entity);
                }
              }

              ImGui::PopID();
            }
            ImGui::EndTable();
          }
          ImGui::EndDisabled();

          if (!localSceneReady) {
            ImGui::TextWrapped(
              "Waiting for the local scene fingerprint before sharing");
          } else if (!sceneKnown) {
            ImGui::TextWrapped(
              "Waiting for peer scene info before sharing");
          } else if (sceneMismatch) {
            ImGui::TextWrapped("Scene mismatch: sharing disabled");
          } else {
            ImGui::TextWrapped(
              "Shared windows stream to other players (H.264, MJPEG fallback)");
          }
        }
      }

      ImGui::Separator();
      ImGui::TextUnformatted("Remote windows");
      auto remote = activeClient->remoteWindows();
      std::sort(remote.begin(),
                remote.end(),
                [](const RemoteWindowInfo& a, const RemoteWindowInfo& b) {
                  if (a.ownerID != b.ownerID) {
                    return a.ownerID < b.ownerID;
                  }
                  return a.shareId < b.shareId;
                });
      static bool debugRemoteControlRequested = false;
      if (!debugRemoteControlRequested &&
          std::getenv("HACKMATRIX_DEBUG_TAKE_CONTROL_FIRST_REMOTE") !=
            nullptr) {
        for (const auto& window : remote) {
          if (window.allowControl && window.controllerID == 0) {
            activeClient->requestRemoteControl(window.ownerID, window.shareId);
            debugRemoteControlRequested = true;
            break;
          }
        }
      }

      static bool debugControlPointerSent = false;
      if (!debugControlPointerSent &&
          std::getenv("HACKMATRIX_DEBUG_SEND_POINTER_AFTER_CONTROL") !=
            nullptr &&
          activeClient->isControllingRemote()) {
        activeClient->sendRemotePointerMotion(800.0, 300.0);
        debugControlPointerSent = true;
      }

      static bool debugControlKeySent = false;
      if (!debugControlKeySent &&
          std::getenv("HACKMATRIX_DEBUG_SEND_KEY_AFTER_CONTROL") != nullptr &&
          activeClient->isControllingRemote()) {
        activeClient->sendRemoteKey(30, true); // KEY_A
        activeClient->sendRemoteKey(30, false);
        debugControlKeySent = true;
      }

      if (remote.empty()) {
        ImGui::TextDisabled("No remote shared windows");
      } else if (ImGui::BeginTable("##remote_windows",
                                   5,
                                   ImGuiTableFlags_Borders |
                                     ImGuiTableFlags_RowBg |
                                     ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Window");
        ImGui::TableSetupColumn("Owner");
        ImGui::TableSetupColumn("Transport");
        ImGui::TableSetupColumn("Status");
        ImGui::TableSetupColumn("Action");
        ImGui::TableHeadersRow();

        const uint32_t localPlayerID = activeClient->localPlayerID();
        for (const auto& window : remote) {
          ImGui::PushID(static_cast<int>(window.ownerID));
          ImGui::PushID(static_cast<int>(window.shareId));
          ImGui::TableNextRow();

          ImGui::TableSetColumnIndex(0);
          ImGui::TextUnformatted(
            window.name.empty() ? "(unnamed)" : window.name.c_str());

          ImGui::TableSetColumnIndex(1);
          ImGui::Text("#%u", window.ownerID);

          ImGui::TableSetColumnIndex(2);
          if (window.transport.empty()) {
            ImGui::TextDisabled("-");
          } else {
            ImGui::TextUnformatted(window.transport.c_str());
          }

          ImGui::TableSetColumnIndex(3);
          const bool controlledByMe =
            window.controllerID != 0 && window.controllerID == localPlayerID;
          if (controlledByMe) {
            ImGui::TextColored(
              ImVec4(0.45f, 1.0f, 0.45f, 1.0f), "%s",
              window.controlStatus.c_str());
          } else if (!window.controlStatus.empty()) {
            ImGui::TextUnformatted(window.controlStatus.c_str());
          } else if (window.controllerID != 0) {
            ImGui::Text("Controlled by #%u", window.controllerID);
          } else if (window.allowControl) {
            ImGui::TextDisabled("Read-only (control available)");
          } else {
            ImGui::TextDisabled("Read-only");
          }

          ImGui::TableSetColumnIndex(4);
          if (controlledByMe) {
            if (ImGui::SmallButton("Release")) {
              activeClient->releaseRemoteControl();
            }
          } else if (window.controlPending) {
            ImGui::TextDisabled("Requesting...");
          } else if (window.allowControl && window.controllerID == 0) {
            if (ImGui::SmallButton("Control")) {
              activeClient->requestRemoteControl(window.ownerID,
                                                 window.shareId);
            }
          } else {
            ImGui::TextDisabled("-");
          }

          ImGui::PopID();
          ImGui::PopID();
        }
        ImGui::EndTable();
      }

      if (ImGui::Button("Disconnect")) {
        activeClient->disconnect();
        clearLoopbackPreviews();
        streamSettingsInitialized = false;
        // Keep the client registered; the non-blocking state machine finishes
        // the ENet disconnect and Engine releases it on the render thread.
      }
    } else if (activeClient && activeClient->isConnecting()) {
      ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.25f, 1.0f), "%s",
                         activeClient->connectionStateText().c_str());
      if (ImGui::Button("Cancel")) {
        activeClient->disconnect();
      }
    } else if (activeClient && activeClient->isDisconnecting()) {
      ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.25f, 1.0f), "%s",
                         activeClient->connectionStateText().c_str());
    } else {
      if (activeClient) {
        activeClient->clearRemoteWindows();
      }
      clearLoopbackPreviews();
      streamSettingsInitialized = false;
      if (engine && !engine->multiplayerError().empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "%s",
                           engine->multiplayerError().c_str());
      }
      if (ImGui::Button("Connect as Client")) {
        auto newClient = std::make_shared<Client>(engine->getRegistry());
        if (newClient->connect(address, port)) {
          client = newClient;
          engine->registerClient(client);
        } else {
          ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "%s",
                             newClient->connectionError().c_str());
        }
      }
    }
  } else {
    if (activeServer && activeServer->IsRunning()) {
      if (ImGui::Button("Stop Server")) {
        activeServer->Stop();
        if (activeClient) {
          activeClient->disconnect();
        }
        clearLoopbackPreviews();
        server = nullptr;
        engine->registerServer(server);
      }
    } else {
      if (ImGui::Button("Host as Server")) {
        server = std::make_shared<Server>();
        engine->registerServer(server);
        if (!server->Start(port)) {
          ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f),
                             "Failed to start server on port %d", port);
          server = nullptr;
          engine->registerServer(server);
        } else {
          auto newClient = std::make_shared<Client>(engine->getRegistry());
          if (newClient->connect("127.0.0.1", port)) {
            client = newClient;
            engine->registerClient(client);
          } else {
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "%s",
                               newClient->connectionError().c_str());
          }
        }
      }
    }
  }
}

entt::entity
Gui::createLoopbackPreview(entt::entity source)
{
  if (!engine) {
    return entt::null;
  }
  auto registry = engine->getRegistry();
  if (!registry || !registry->valid(source)) {
    return entt::null;
  }

  auto* component = registry->try_get<WaylandApp::Component>(source);
  auto* sourcePositionable = registry->try_get<Positionable>(source);
  if (!component || !component->app || !sourcePositionable) {
    return entt::null;
  }

  auto existing = loopbackPreviews.find(source);
  if (existing != loopbackPreviews.end()) {
    if (registry->valid(existing->second)) {
      return existing->second;
    }
    loopbackPreviews.erase(existing);
  }

  auto remote = std::make_shared<RemoteApp>(
    component->app,
    "preview: " + component->app->getWindowName(),
    /*useSourceTexture=*/true);
  remote->createTexture();

  const entt::entity entity = registry->create();
  // Local preview only: float a smaller copy in front of the source so both
  // windows are visible while testing the pipeline. Network-shared windows
  // will use the source transform instead.
  glm::vec3 previewOffset(0.0f, 0.08f, 0.0f);
  if (auto* camera = engine->getCamera()) {
    const glm::vec3 towardCamera = camera->position - sourcePositionable->pos;
    const float distance = glm::length(towardCamera);
    if (distance > 0.01f) {
      // Move partway toward the camera, but never past it.
      previewOffset += glm::normalize(towardCamera) *
                       std::min(0.45f, distance * 0.4f);
    }
  }
  const glm::vec3 position = sourcePositionable->pos + previewOffset;
  registry->emplace<Positionable>(entity,
                                  position,
                                  sourcePositionable->origin,
                                  sourcePositionable->rotate,
                                  sourcePositionable->scale * 0.5f);
  if (auto* positionable = registry->try_get<Positionable>(entity)) {
    positionable->update();
  }
  registry->emplace<RemoteApp::Component>(entity, remote);
  loopbackPreviews[source] = entity;
  return entity;
}

void
Gui::destroyLoopbackPreview(entt::entity source)
{
  auto it = loopbackPreviews.find(source);
  if (it == loopbackPreviews.end()) {
    return;
  }
  if (engine) {
    if (auto registry = engine->getRegistry();
        registry && registry->valid(it->second)) {
      registry->destroy(it->second);
    }
  }
  loopbackPreviews.erase(it);
}

void
Gui::clearLoopbackPreviews()
{
  if (engine) {
    if (auto registry = engine->getRegistry()) {
      for (const auto& [source, entity] : loopbackPreviews) {
        (void)source;
        if (registry->valid(entity)) {
          registry->destroy(entity);
        }
      }
    }
  }
  loopbackPreviews.clear();
}

void
Gui::pruneLoopbackPreviews()
{
  if (!engine) {
    loopbackPreviews.clear();
    return;
  }
  auto registry = engine->getRegistry();
  if (!registry) {
    loopbackPreviews.clear();
    return;
  }

  for (auto it = loopbackPreviews.begin(); it != loopbackPreviews.end();) {
    const bool sourceOk =
      registry->valid(it->first) &&
      registry->all_of<WaylandApp::Component>(it->first);
    const bool remoteOk =
      registry->valid(it->second) &&
      registry->all_of<RemoteApp::Component>(it->second);
    if (!sourceOk || !remoteOk) {
      if (remoteOk) {
        registry->destroy(it->second);
      }
      it = loopbackPreviews.erase(it);
    } else {
      ++it;
    }
  }
}

bool
Gui::IsConnectButtonClicked() const
{
  return connect;
}

const std::string&
Gui::GetAddress() const
{
  return address;
}

int
Gui::GetPort() const
{
  return port;
}

}
