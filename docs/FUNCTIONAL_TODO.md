# 功能收尾清单（Phase 5 + 已知缺口）

> 工程地基（Phase 0–4）已完成。本清单只列**功能**相关、尚未完成的部分，按优先级排序。
> 每项完成后更新状态。

## P0 — 先修「测试依赖但没实现」的功能

| # | 项 | 现状 | 验收 |
|---|---|---|---|
| F1 | **`KEY_REPLAY` 服务端** | 协议有 `KeyReplay`，集成测试在发，但 `src/` 完全没处理 | ✅ **已实现**：`Api::handleKeyReplay` 解析 keysym 并通过 `Controls::scheduleKeySym` 按时间调度 press/release（分帧，复用 `handleKeySym` 路径）。实测 `KEY_REPLAY 'p'` 成功触发截图。注：向 Wayland 客户端注入文本（需要 keycode 映射）仍未覆盖 |
| F2 | **ImGui 键盘输入** | 只接了鼠标；文本输入框不可用 | ✅ **已实现**：`Engine::updateImGuiKey` 把真实键盘事件转发为 `io.AddKeyEvent` + `io.AddInputCharacter`；keysym→`ImGuiKey` 映射抽到 `include/imgui_keymap.h` 并有 **6 个单测**。`disableKeysIfImguiActive` 已保证输入框激活时游戏按键不触发。交互式打字需真实键盘验证 |

## P1 — 修 `readme.md` / `todo.md` 已记录的 bug

| # | 问题 | 来源 |
|---|---|---|
| F3 | WM 模式下滚轮被失焦窗口处理，导致未定义状态 | `readme.md` `# Bugs` | ✅ **已修复**：`handle_pointer_axis` 只把滚动发给真正有指针焦点的表面，移除了「无焦点时随机挑一个窗口」的 fallback。hover 焦点仍会在移动时设置焦点，悬停滚动不受影响 |
| F4 | 手动 focus（按 `r`）偶发失效 | `todo.md` / 历史记录 |
| F5 | 透明物体与 app 渲染叠加错误 | 历史记录 |
| F6 | 光照 acne、光照距离圆形条纹 | 历史记录 |
| F7 | 最多同时显示 10 个 app | 历史记录 |
| F8 | 加载的 app 朝向错误（"load app spun around"） | 历史记录 |
| F9 | `readWebGuiPort()` 死代码 → `MULTIPLAYER_WEBGUI_PORT` 无效 | 代码实测 | ✅ **已修复**：`WebGui` 构造函数改用 `readWebGuiPort()`；默认回退保持 7775。用 Crow 构建实测：`MULTIPLAYER_WEBGUI_PORT=5699` → HTTP 200，旧 7775 无响应。注意 `launch` 默认注入 7777 |

## P2 — 新功能（`todo.md` Milestones）

| # | 项 | 说明 |
|---|---|---|
| F10 | 窗口 **resize / move** | 注册控制键、移动时 blit、更新坐标 | ✅ **已实现**：`WindowManager::moveFocusedApp` / `resizeFocusedApp`；聚焦 app 时 `Super+方向键` 沿相机屏幕平面移动、`Super+[`/`]` 缩放（同步 `requestSize` 通知 client）。实测 `focusApp` → 5×`moveFocusedApp delta=(0.1,0,0)` → `resizeFocusedApp 1142x643`，截图对比确认窗口位移 |
| F11 | **可变长宽比窗口**（半屏 app 等） | 需要 `WindowManager` 的窗口元素概念 |
| F12 | **Workspace 概念** | 持久化动态 app、选择/命名 workspace、按需动态启动（项目当前零实现） |
| F13 | API 驱动的体素应用 | map solver / 音频可视化 / 神经网络可视化 | ✅ **示例完成**：`client_libs/cpp/voxel_demo.cpp`（`-DMATRIX_BUILD_EXAMPLES=ON`）——连接 ZMQ API，每帧 `ADD_VOXELS` + `replace=true` 驱动动态体素波。实测 45/45 帧 accepted，截图确认波面渲染。map solver / 音频可视化可在此模式上扩展 |

## P3 — 多人窗口共享（B2）

> 目标：服务端与客户端**共用一个 3D 场景**，但**窗口默认私有**；在 Multiplayer 面板里逐个发布窗口，默认只读，可额外授权控制。
> 类比 Minecraft：一起建东西，同时各用各的软件，互不打扰。

| # | 子项 | 验收 |
|---|---|---|
| F14a | **共享场景基础（B1）** | 两个客户端看到同一份体素/实体世界与彼此头像；世界状态有权威来源（服务端或主机），支持增量同步 |
| F14b | **窗口列表 UI** | 连接成功 / 开服后，Multiplayer 面板下方列出当前窗口（名称、尺寸）；每行「共享」「允许控制」两个开关，**默认都关**；「允许控制」仅在「共享」开启后可用 |
| F14c | **窗口发布与远端显示** | 勾选共享后，其他客户端在**对应世界位置 / 朝向 / 尺寸**看到该窗口的实时画面；窗口移动、缩放、关闭同步 |
| F14d | **默认只读 + 授权控制** | 默认不可操作；勾选「允许控制」后，**同一时刻只允许一个控制者**，可随时收回；输入注入到共享者本地的对应 surface |
| F14e | **清理与权限** | 取消共享 / 断线立即停止并移除远端窗口；共享者退出后远端窗口消失；未授权输入被拒绝 |

**UI 草案**

```
Multiplayer
  [Client/Server]  ...
  Address [127.0.0.1]   Port [7777]
  [Connect as Client] / [Host as Server]

  ── 共享窗口（连接成功后显示）──
  [x] 共享   [ ] 允许控制    kitty — 1200x800
  [ ] 共享   [ ] 允许控制    Firefox — 1600x900
  ...
  （默认全不勾；「允许控制」仅在「共享」勾选后可用）
```

**实现要点**

- 窗口枚举：`registry->view<WaylandApp::Component>()`（`WindowManager::registerWaylandApp` 创建的实体）
- 采集：`AppSurface::getTextureId()` / `appTexture()` → FBO，**不依赖 screencopy**
- 位置同步：发布时发送 `Positionable` 的 pos / rotate + 窗口宽高；远端按同一世界坐标放置
- 传输：窗口纹理编码（先 MJPEG / 低帧率验证，后 H.264）；建议 WebRTC；**不要走现有 ENet reliable**
- 远端渲染：新增 `RemoteApp : AppSurface`（或「远端屏幕」实体），`getTextureId()` 返回解码纹理
- 控制：客户端发送归一化坐标 / 按键 → 共享者本地 `wlr_seat_pointer_notify_*` / `wlr_seat_keyboard_notify_*` 注入；单控制者锁
- 隐私默认：不共享任何窗口；断开连接即停止全部共享

**待确认的设计问题**

1. 窗口共享走点对点，还是经服务端转发？（多人观看建议经服务端，未来可换 SFU）
2. 远端看到的共享窗口，是否允许再被抓取 / 二次共享？（建议禁止）
3. 只读客户端能否自由移动视角靠近看？（建议可以，仅禁止输入事件）
4. 共享窗口的音频是否一起传？（先不做）
5. 首次授权控制是否需要被控端弹窗确认？（建议需要）

> 依赖：F14a 是「出现在场景对应位置」的前提。若两个客户端加载同一份 `db/matrix.db`，可先不做世界同步，用最小版验证 F14b–F14d。

## 完成记录

- 2026-11-22：**F1 `KEY_REPLAY` 服务端**已实现。`Api::handleKeyReplay` 把 keysym 排入 `Controls::scheduleKeySym`，press/release 分帧调度并复用 `handleKeySym`；实测 `KEY_REPLAY 'p'` 触发截图、`p` 回显出现在画面上。向 Wayland 客户端注入文本仍需 keycode 映射，未覆盖。
- 2026-11-22：**F2 ImGui 键盘输入**已实现。`Engine::updateImGuiKey` 转发真实键盘到 `io.AddKeyEvent` + `io.AddInputCharacter`；`keysymToImGuiKey` 抽到 `include/imgui_keymap.h` 并有 6 个单测（35/35 总测试通过）。
- 2026-11-22：**F3 WM 滚轮**已修复：`handle_pointer_axis` 不再对无焦点的任意窗口发送滚动。
- 2026-11-22：**F9 Web GUI 端口**已修复：`readWebGuiPort()` 接入，Crow 构建实测自定义端口 HTTP 200。
- 2026-11-22：**F10 窗口 move/resize** 已实现：`WindowManager::moveFocusedApp` / `resizeFocusedApp` + `Super+方向键` / `Super+[`/`]`；实测移动 5×0.1、缩放至 1142x643，截图对比确认位移。
- 2026-11-22：**F13 API 体素应用示例** 已实现：`client_libs/cpp/voxel_demo.cpp`，45/45 帧 accepted，截图为动态体素波。
