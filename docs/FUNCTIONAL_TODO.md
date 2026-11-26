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
>
> **当前阶段（2026-11-23）**：方案 A 场景指纹（数据库 + 被引用的模型/纹理）负责连接前置校验；**F14a v2 权威世界同步已实现**（首个客户端为 authority；体素增删 + 场景实体变换全序广播，迟到客户端取快照；运行时实体创建/删除留待后续）。
>
> **范围决策（2026-11-23）**：多人功能定位为**内网（LAN）互连**。STUN/TURN 公网穿透**明确不做**：WebRTC 只用 host 候选局域网直连（已实测），ENet 服务器只需局域网可达。跨 NAT 场景不在验收范围内。

| # | 子项 | 验收 |
|---|---|---|
| F14a | **共享场景基础（B1）** | 两个客户端看到同一份体素/实体世界与彼此头像；世界状态有权威来源（服务端或主机），支持增量同步 | ✅ **v2 已实现（2026-11-23）**：服务器选举**首个连接客户端为 world authority**，断线后自动改选；①**体素层**：authority 的 `ADD_VOXELS`/`CLEAR_VOXELS` 应用后按 `seq` 广播，非 authority 的编辑转发给 authority；②**场景实体层**：authority 以 ~20 Hz 扫描 `Persistable+Positionable` 实体，变化按 `seq` 广播，API 的 `MOVE` / `ADD/EDIT_COMPONENT(Positionable)` 在非 authority 上转发给 authority；迟到客户端通过 `Snapshot` + `EntitySnapshot` 取当前体素与实体变换。场景指纹继续作为连接前置校验。**限制**：不覆盖运行时创建/删除实体、Model/Light 组件的运行时变更，以及绕过 API 的本地实体改动（会被 authority 状态覆盖）。 |
| F14b | **窗口列表 UI** | 连接成功 / 开服后，Multiplayer 面板下方列出当前窗口（名称、尺寸）；每行「共享」「允许控制」两个开关，**默认都关**；「允许控制」仅在「共享」开启后可用 | ✅ **已实现**：列出非 layer-shell/accessory 的 `WaylandApp`；共享状态存入 `Client`（默认私有，取消共享会清掉控制授权）；场景不一致时整表禁用。Stream 列显示实际传输（WebRTC / ENet H.264 / MJPEG）、FPS、自适应质量与 RTCP RTT；`Share` 接入 H.264/WebRTC 串流，`Allow control` 接入 F14d。 |
| F14c | **窗口发布与远端显示** | 勾选共享后，其他客户端在**对应世界位置 / 朝向 / 尺寸**看到该窗口的实时画面；窗口移动、缩放、关闭同步 | ✅ **已实现**：`Share` 开关通过 ENet 发布窗口；源端 RGBA→**H.264（NVENC 优先，libx264 回退）**，主路径经 **WebRTC（`webrtcbin` + `rtph264pay`）** 点对点传输，ENet channel 6 保留为 H.264 回退；接收端 GStreamer 解码到 `RemoteApp` 自有纹理，WebRTC 未就绪时使用 ENet H.264/libavcodec，编码失败回退 MJPEG；`Transform` 同步移动/旋转/缩放，帧尺寸同步 resize，`Stop`/`OwnerLeft` 清理窗口。Window streaming 面板可调 Target FPS（1–30）和编码质量（10–95，映射到 H.264 码率），默认 5 FPS / 65。关键帧请求、SPS/PPS 每个 IDR 重复、RTCP 丢包/RTT 驱动的自适应质量均已接入。实测单/双 viewer 同时观看同一窗口，WebRTC 建连约 0.2 s，画面方向/颜色正确。 |
| F14d | **默认只读 + 授权控制** | 默认不可操作；勾选「允许控制」后，**同一时刻只允许一个控制者**，可随时收回；输入注入到共享者本地的对应 surface | ✅ **已实现**：默认只读；共享者勾选 Allow control 后通过 `ControlState` 广播；接收端在 Remote windows 表里请求控制（显示 Requesting… 状态），共享者按 shareId 只接受一个 controller；接收端转发指针移动/按键/滚轮/键盘，共享者通过 `wlr_seat_pointer_notify_*` / `wlr_seat_keyboard_notify_*` 注入到本地 `WaylandApp`；支持 Release / 共享者收回 / controller 断线清理。**控制期间自动把该窗口提升到至少 15 FPS**，并在共享画面里叠加远控光标；owner 端提示 “player #N took/released control”。远程注入会避让 owner 最近 0.75 s 的本地键鼠，避免抢焦点；keycode 有范围校验。 |
| F14e | **清理与权限** | 取消共享 / 断线立即停止并移除远端窗口；共享者退出后远端窗口消失；未授权输入被拒绝 | ✅ **已实现**：Stop / OwnerLeft / 本地 disconnect 都会移除远端窗口；服务器缓存活动的 Start/Transform/ControlState，迟到客户端在交换场景指纹后补发；控制请求要求控制者场景匹配；owner 校验 controller 后才注入输入。客户端连接改为非阻塞状态机（Connecting/Connected/Disconnecting），断线或超时自动释放会话，WebGui 请求通过渲染线程命令队列执行。**迟到客户端不会补发历史视频帧（等待下一帧）**。 |

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
- 传输：窗口纹理编码（先 MJPEG / 低帧率验证，后 H.264）；WebRTC（`webrtcbin` + `rtph264pay`/`rtph264depay`，自带 RTP 分片）为主路径，ENet reliable channel 6 保留 H.264 回退；控制/信令仍走 ENet
- 远端渲染：新增 `RemoteApp : AppSurface`（或「远端屏幕」实体），`getTextureId()` 返回解码纹理
- 控制：客户端发送归一化坐标 / 按键 → 共享者本地 `wlr_seat_pointer_notify_*` / `wlr_seat_keyboard_notify_*` 注入；单控制者锁
- 隐私默认：不共享任何窗口；断开连接即停止全部共享

**待确认的设计问题**

1. 窗口共享走点对点，还是经服务端转发？（多人观看建议经服务端，未来可换 SFU）
2. 远端看到的共享窗口，是否允许再被抓取 / 二次共享？（建议禁止）
3. 只读客户端能否自由移动视角靠近看？（建议可以，仅禁止输入事件）
4. 共享窗口的音频是否一起传？（先不做）
5. 首次授权控制是否需要被控端弹窗确认？（建议需要）

> 依赖：F14a 是「出现在场景对应位置」的前提。双方仍加载同一份 `db/matrix.db`；F14a v2 已把 **API 体素**与 **DB 持久化实体的 Positionable 变换**做成权威同步，运行时创建/删除实体与 Model/Light 组件变更仍留待后续。

## 完成记录

- 2026-11-26：**自由视角窗口“拎起”/缩放（松手提交）**：
  - **G = 把窗口拎到镜头前**：准星命中本地窗口后按住 `G`，窗口固定在相机前方原距离处，始终正对镜头；鼠标视角旋转和 `WASD` 玩家移动时窗口实时跟随，松手后停在当前位姿并提交一次；
  - **缩放**：准星在窗口四角/四边（变琥珀色 + 小方框提示）按住左键拖动，锚定对角/对边；拖动过程中按累计位移计算目标尺寸，并以 ~20Hz 向客户端发送真实 `requestSize`，客户端实际重排（实测 weston-terminal 从 1150x667 变为 1518x727），不是只拉伸纹理；
  - 松手时提交最终 `requestSize`，共享窗口的 `Transform` 广播在拖动/缩放期间被抑制，松手后只发一次；未共享窗口不影响；
  - 仅允许操作本地 `WaylandApp`；远端共享窗口需通过控制授权由 owner 执行；
  - `handleLogBlockType` 热键由 `T` 改为 `Y`，为后续聊天腾出 `T`；
  - 修复：`Camera` 的 `invertY` 会让 `projection[1][1]` 为负，`worldPerPixel` 之前直接使用它得到负值并被判非法，导致拖动/缩放完全不生效、同时相机被误判为“拖拽中”而停住；改用 `|p11|` 后位移正确；
  - 修复：缩放目标尺寸之前每帧都用“本帧鼠标位移”从初始尺寸重算，导致尺寸只反映最后一帧、松手后回原样；改为累计位移；
  - 自由视角下不再把隐藏光标喂给 ImGui，避免左上角调试面板被鼠标误拖缩放；
  - 光标可见但无应用聚焦时也支持窗口拎起/缩放（瞄准改用真实指针）；
  - 单测 87/87 通过。

- 2026-11-24：**直接 matrix↔matrix 联机的 scene mismatch 代码复核与修复**：
  - 复核确认：只有 dedicated server（authority = `kServerAuthorityID`）会绕过本地 DB 指纹；两个主程序直接联机时仍走 `hasSceneMismatch` / `sceneMatches` 门禁；
  - 原指纹仍包含运行时状态表，导致同一场景因上次保存的 `Positionable`、门/锁/钥匙状态、`RotateMovement`、`Bootable` 布局不同而误报；
  - 指纹升级到 v5：只哈希静态场景（`Entity`、`Model`、`Light`、`Parent`、`Scriptable` 等），忽略 `Positionable`、`ServerVoxels`、`Door`、`Lock`、`Key`、`RotateMovement`、`Bootable`；
  - 单测 87/87 通过，新增运行时状态表不影响指纹的断言；
  - 注意：两个主程序必须使用同一份新构建；不同版本的指纹算法本身就会 mismatch。

- 2026-11-24：**远控窗口自动聚焦/锁定视角与输入直通**：
  - 控制授权成功后，控制器收到 `ControlState` 会请求渲染线程 `focusRemoteApp`：相机移动到共享窗口、切换 cursor 模式，视角不再被鼠标旋转；
  - 相机聚焦完成后把光标移到屏幕中心，用户可直接移动鼠标到共享窗口；
  - 释放控制（Esc）或失去控制时 `returnToFreeLook`：恢复自由视角、隐藏光标；
  - 远控鼠标在自由视角下使用中心准星射线命中 remote window，cursor 模式使用真实指针；按键仍按原有通道注入 owner；
  - 实测双客户端：A 共享 EventDemo，B 授权控制后 `focusRemoteApp ent=42`，owner 收到 `button 272 press/release x=800 y=300` 和 `key 30`。

- 2026-11-24：**专用服务器下 scene mismatch 误报修复**：
  - 场景指纹不再哈希运行时状态表（`Positionable`、`ServerVoxels`），只比较静态场景/模型资源；位置与体素由服务器 snapshot 收敛；
  - 连接专用服务器（authority = `kServerAuthorityID`）时，窗口共享不再被客户端本地 DB 指纹差异阻断：`canShareWindows()` 允许共享，`sceneMatches()` 信任服务器权威世界；
  - UI 在专用服务器模式下即使本地 hash 不同也显示为可共享，并提示“world is server-authoritative”；
  - 87/87 单测通过（新增运行时状态不影响指纹的测试）。

- 2026-11-24：**自由视角下二级菜单/弹窗不显示**：
  - 根因：`Renderer::renderApps()` 只绘制 parent 等于 `getCurrentlyFocusedApp()` 的 accessory surface；自由视角没有 WM focus，所以 popup 被跳过，按 R 聚焦后才出现；
  - 修复：`WindowManager` 增加 `popupOwner`，由准星 hover/点击的窗口设置；renderer 在 `focusedEnt` 或 `popupOwner` 下绘制 accessory；
  - 同时沿 popup parent 链解析到顶层窗口并累加 offset，二级/三级 xdg popup 也能正确定位；
  - 实测：自由视角（`focus=0`）右键 Dolphin 后 362x395 的上下文菜单正常显示；不再需要先按 R。

- 2026-11-24：**修复准星悬停窗口时系统光标出现、自由视角旋转失效**：
  - 根因：准星 hover/click 设置 seat pointer focus 后，`wayland_pointer_focus_requested()` 把该 focus 当成“需要光标”，导致 `wlr_output` 每帧把系统光标显示出来，并跳过 `applyLookDelta`；
  - 修复：自由视角下 `wayland_pointer_focus_requested()` 返回 false；`handle_request_set_cursor` 在准星 focus 期间忽略客户端 cursor image；pointer focus 仍保留以完成 hover/click；
  - 从光标模式按 F 回到自由视角时顺带清理 `cursorInputFocusedApp`，避免旧的 hover focus 让窗口继续吃键盘；
  - 单测 86/86 通过。

- 2026-11-24：**自由视角点击修复（press/release 跨帧丢失）**：
  - 根因：`wlr_output` 每帧的 cursor-mode 清理会在准星点击的 press 与 release 之间清掉 pointer focus，客户端只收到按下、收不到松开，于是按钮高亮但不触发；键盘/滚轮也可能继续落到旧窗口；
  - 修复：自由视角准星持有 pointer focus 时跳过错帧清理；准星 hover 保持 focus 并按射线更新/清除；release 必定发到按下的表面；
  - 点击窗口时只设置 xdg `activated` 与 pointer focus，不发送 keyboard enter；键盘仍只在 R 时捕获；
  - 准星命中先测屏幕空间的 popup / layer-shell，菜单与下拉框可以点击；
  - ImGui 占用鼠标时不再把 motion/axis 泄漏给下方窗口（修复移动/滚轮菜单导致后台窗口滚动）；
  - 实测：`weston-eventdemo` 收到左/右键 press+release（272/273），点击前后 `focus=0`；Qt 按钮输出 `CLICKED`；按 R 后 `focus=1`。

- 2026-11-24：**主程序自由视角十字准星与点击直通窗口**：
  - 自由视角（光标抓取、无窗口焦点）屏幕中央绘制十字准星；光标模式与 R 锁定窗口后隐藏；
  - 左键不再把窗口“拉到面前并锁定”，改为从屏幕中心射线命中窗口后，把 press/motion/release 直接发给该窗口；右键同样直通；未命中窗口时左键仍保留体素放置；
  - 点击只建立临时的 pointer focus，松开后立即清除，不会捕获键盘；
  - 仅按 `R` 才 `focusApp` 并捕获键盘/IME；新开顶层窗口不再自动获得焦点（`HACKMATRIX_DEBUG_FOCUS_NEW_WINDOW` 仍可强制，供测试）；
  - `POINTER_REPLAY` API 改为走真实 wlroots 指针路径，便于无头测试；
  - 实测：`weston-eventdemo` 在无焦点状态下收到左键 272 / 右键 273 press+release，坐标落在窗口中心 (576,338)；点击前后 `focus=0`、相机不变；按 R 后 `focus=1` 并收到键盘焦点。

- 2026-11-24：**专用服务器单人预共享窗口**：
  - 问题：旧逻辑要求先收到 peer 的 scene fingerprint 才允许勾选 Share，连专用服务器且只有一人时一直显示 “Waiting for peer scene info...”，无法共享；
  - 新增 `Client::canShareWindows()`：本地场景就绪且无 mismatch 时，若已有 peer fingerprint **或** 当前 authority 是 `kServerAuthorityID` 即允许共享；
  - 服务器本来就会缓存 `Start/Transform/ControlState` 并在新玩家发送 scene info 后回放，因此单人先共享的窗口会在其他玩家加入后出现；接收端仍按 fingerprint 拒绝不匹配的共享；
  - UI 在专用服务器模式下显示 “Dedicated server: window sharing available (held for joining players)”；
  - 实测：单人连专用服务器 + 打开终端窗口即可 `sharing window 'Wayland Terminal' as share 1`；第二个客户端稍后加入收到 `showing shared window 'Wayland Terminal' from player ...`；`bootServer`（客户端当 authority）仍保持旧行为，单人不会提前共享。

- 2026-11-24：**matrix-server 玩家状态服务器化（第一版）**：
  - 服务器维护玩家列表/出生点：连接时分配 spawn、广播 `PLAYER_JOIN`，把现有玩家状态发给新玩家，并广播新玩家状态；
  - `PLAYER_UPDATE` 由服务器保存并广播；新增 `PLAYER_LEAVE`（channel 9），断线/超时后广播，客户端 `systems::unregisterPlayer` 删除远端头像；
  - `systems::registerPlayer` 去重，`clearPlayers` 在客户端断线重连时清理旧头像；
  - 实测双客户端：双方收到 `player ... joined`；强制杀掉一个客户端后，服务器 ~15s 超时广播 `player ... disconnected`，另一端收到 `player ... left`；
  - 限制：玩家位置仍由客户端上报，服务器只做列表/出生/离开和分发；输入驱动的移动权威是下一步。

- 2026-11-24：**matrix-server 持久化**：
  - `WorldSimulation::save()` 写回 `Positionable` 行，并用服务器专用的 `ServerVoxels` 表保存运行时体素；
  - 启动时恢复 `Positionable` + `ServerVoxels`，首帧 snapshot 即包含恢复后的世界；
  - 每 30 秒在 dirty 时自动保存，SIGTERM/SIGINT 退出时也保存；编辑/体素变化会标记 dirty；
  - 实测：客户端编辑实体 + 91 体素 → 服务器停机保存 → 重启后客户端收到 `91 voxels` / `35 entities` snapshot，实体位置恢复为 `(7,3,-2)`；
  - 服务器数据库应使用独立副本（服务器唯一写者），避免客户端退出时覆盖。

- 2026-11-24：**matrix-server 接管世界 authority**：
  - 新增 `kServerAuthorityID = 0xFFFFFFFF`；`matrix-server` 以 `Server(true)` 启动，服务器固定为权威，客户端不再自己当选；
  - `Server` 增加 world-sync 出站队列和 `worldSyncHandler`：客户端发往 server authority 的 `Add/ClearBox/ClearPositions/EntityMove/EntityTransform/SnapshotRequest` 由服务器处理，不再中继；
  - `WorldSimulation` 增加无 GL 的体素 overlay（`DynamicObjectSpace`）、实体变换缓存/扫描、snapshot/delta 广播；客户端编辑经服务器应用后以 `seq` 广播；
  - `bootServer`/客户端开服保持旧模式（`Server(false)`，首个客户端当 authority），避免破坏现有联机；
  - 验证：客户端收到 `authority = 4294967295` + `world/entity snapshot`；`voxel_demo` 从客户端发 91 体素 → 服务器应用 → 客户端收到 `authoritative add seq 1`；`EDIT_COMPONENT` 转发后客户端读到新坐标；主客户端/86 单测无回归。

- 2026-11-24：**matrix-server Phase 2（无 GL 世界 tick）**：
  - 把 `Positionable` 与 `PositionablePersister` 从 `src/model.cpp` 拆到 `src/components/Positionable.cpp`，避免服务器为了位置数据链接 Model/Assimp/GL；
  - `EntityRegistry` 新增显式数据库路径构造，服务器不再依赖 `Config::database_file`；
  - 新增 `server::WorldSimulation`：加载 `Entity + Positionable`，以 20Hz 执行 `applyRotation/applyTranslations` 和 damaged transform 更新；Model/Light 等 GL-backed persister 暂不加载；
  - `matrix-server` 现在输出 `world loaded: N entities, M positionables`、`server tick: X online, N entities`；
  - 验证：`ldd matrix-server` 无 GL/wlroots/Wayland/X11；真实客户端连入显示 `player ... connected (1 online)`；主客户端 `matrix`、`bootServer`、86/86 单测均构建/通过。

- 2026-11-24：**专用服务器骨架（`matrix-server`）** 已建立：
  - 新增 `src/server/main.cpp`，加载 `db/matrix.db`（只读）并打印 Entity/Positionable/Model/Light/Bootable 等表计数；
  - 复用 `MultiPlayer::Server` ENet 中继，服务器控制台显示玩家连接/断开和在线人数（`Server::clientCount()`）；
  - `matrix-server` **不链接 GL/wlroots/Wayland/GStreamer/ImGui**，`ldd` 仅见 sqlite3/spdlog/fmt 等；
  - 当前仍是“中继 + 第一个客户端当 authority”的过渡形态，world 模拟尚未搬进服务器。
  - 已用真实客户端连入验证：服务器日志 `player ... connected (1 online)`；`--help`/`--port`/`--db` 可用。
  - 详见 `docs/SERVER.md`。下一步：抽出 GL-free 的 `matrix_core_server`，把 World/实体/体素模拟、snapshot/delta、持久化搬进 `matrix-server`。

- 2026-11-24：**Intel/AMD（Mesa GLES）场景不显示修复**：
  - 根因：主 fragment shader 的 `allBlocks`（sampler2DArray）、`appTex`（sampler2D）和 `depthCubeMap*`（samplerCube）默认都指向 **texture unit 0**。Mesa 在 draw 时严格校验，直接返回 `GL_INVALID_OPERATION`，所有场景 draw 被丢弃；NVIDIA 的 GLES 驱动较宽容，所以之前只在 NVIDIA 上正常。
  - 修复：为每个 sampler 分配独立 unit（block=0、mesh=1..2、shadow cube=3..7、app=15），app 纹理绑定同步改到 unit 15；阴影 cubemap 的 texture unit 固定循环在 3..7，避免反复创建 light 时越界。
  - 阴影 FBO 兼容：移除 GLES 不存在的 `glDrawBuffer/glReadBuffer`，改用 `glDrawBuffers(GL_NONE)`；深度 cubemap 从 `GL_DEPTH_COMPONENT + GL_FLOAT`（GLES 无效）改为 `GL_DEPTH_COMPONENT24 + GL_UNSIGNED_INT`，并检查 FBO 完整性。
  - 0 顶点的 dynamic/line draw 增加保护，避免 Mesa 报无效 draw。
  - **验证**：Mesa llvmpipe（OpenGL ES 3.2 / GLSL ES 3.20）headless 截图场景完整，`GL_INVALID*` 计数为 0；NVIDIA 场景/窗口截图无回归；86/86 单测通过。可用 `MESA_DEBUG=1` 复现/确认。

- 2026-11-23：**中文输入（fcitx5 / text-input-v3 + input-method-v2）** 已实现：
  - compositor 新增 `zwp_text_input_manager_v3`、`zwp_input_method_manager_v2`、`zwp_virtual_keyboard_manager_v1` 三个 global；之前完全没有 IME 协议，fcitx5 即使启动也无法绑定。
  - 新增 `ImeRelay`（`src/wayland/ime.cpp`）：焦点 enter/leave、文本输入 enable/commit/disable、输入法 commit → `preedit/commit/delete/done`、键盘 grab 转发、候选窗口 popup surface。
  - 候选窗口用 ImGui overlay 绘制（`ImeRelay::renderOverlay`），按指针位置显示；无 GUI（`HACKMATRIX_ENABLE_GUI=0`）时只提交文本、不显示候选框。
  - **关键修复**：IME commit 后立即 `wl_display_flush_clients`；否则事件留在 compositor 发送缓冲里，客户端收不到。
  - 无头 seat 新增虚拟键盘（`create_virtual_keyboard`），使 seat 始终有 keymap，合成注入与 IME grab 在 headless 下也能工作。
  - `KEY_REPLAY` 现在会按 seat keymap 把 keysym 映射成 keycode 注入到焦点客户端（ASCII），修复了 F1 一直只能触发 WM 热键、不能向应用输入文本的限制。
  - **实测**：fcitx5 5.1.23 + pinyin，自写 text-input-v3 测试客户端收到 `commit_string("你好")`（Release 与 ASan 均通过）；候选 popup 读到 27x34/294x34/351x34 帧；ASan 修复了 popup 析构时把 `Popup*` 当 `ImeRelay*` 的堆越界。
  - **限制**：ImGui 面板本身不是 Wayland 文本输入客户端，仍只能输入 ASCII；XWayland 应用需 `XMODIFIERS=@im=fcitx`；fcitx5 需在 HackMatrix 的 Wayland 会话内运行（宿主已有实例时用 `dbus-run-session` 隔离）。详见 `docs/BUILDING.md` 的 “Chinese input”。

- 2026-11-23：**稳定性与实际体验修复（第一批）**：
  - **连接生命周期**：`Client` 改为 `Connecting / Connected / Disconnecting / Disconnected` 状态机，`connect()` 不再阻塞渲染线程 5 秒；超时、服务器关闭、主动断开都有明确状态与错误文案。`disconnect()` 无条件清理 peer/host，`sendPlayer`/`poll` 同时校验 state、peer、client，修复断线后再操作导致的空指针。
  - **WebGui 线程安全**：`~WebGui` 现在会 `app.stop()` 并 `join` Crow 线程；HTTP 的 connect 请求改为写入 `Engine` 命令队列，由渲染线程执行 ENet/GL 清理；不再跨线程 `registerClient`。
  - **开服/连接体验**：移除 `Host as Server` 的 `sleep(4)` 与 5 s/3 s 阻塞等待；连接中显示 Connecting… 并带 Cancel，失败显示具体错误。
  - **健壮性**：Server/Client 的玩家更新、加入包增加长度校验；`Api::handlePlayerMove` 增加空指针保护，`processBatchedRequest` 逐请求 try/catch 并返回失败响应；远程注入的 keycode 做范围校验。
  - **场景指纹**：从「直接 hash SQLite 文件」改为「按表规范化的行值 hash」（`Bootable.pid` 等易变字段忽略，模型路径相对安装根目录），避免不同机器/不同页面布局误判；指纹计算移到后台线程，面板显示 “Computing scene fingerprint…”，未知状态不再被当成 mismatch。
  - **抓帧/编码**：新增 `WindowCapture`（FBO + PBO + fence，一帧延迟）把 `glReadPixels` 从渲染关键路径移开；新增 `WindowEncodeWorker` 线程负责 H.264/MJPEG 编码，渲染线程只做 `isBusy` 判断与投递，避免大窗口编码卡帧。
  - **WebRTC 韧性**：新增 connection event 上报；sender/receiver 失败会自动拆除并按 1→2→4→8 s 退避重建（带 `Reconnect` 信令），静默 stall 3 s 也会触发重连；WebRTC 帧新鲜时忽略 ENet 回退，回退阈值从 2 s 降到 1 s，静止/丢包时能更快恢复。面板显示 WebRTC / ENet fallback（retry pending）/ stalled 等真实状态。
  - **远控体验**：控制生效时该窗口自动提升到 ≥15 FPS；共享画面内绘制高对比远控光标；请求控制显示 pending，owner 端显示 “player #N took/released control”；注入输入会避让 owner 最近 0.75 s 的本地键鼠；窗口尺寸未知时 Start 使用 1×1 占位，等首帧自动 resize。

- 2026-11-23：**F14a v2 场景实体变换同步** 已实现：authority 每 ~20Hz 扫描 `Persistable+Positionable` 实体，把变化以 `EntityTransform` 按 `seq` 广播；API 的 `MOVE` 与 `ADD/EDIT_COMPONENT(Positionable)` 在非 authority 上转发给 authority；新加入客户端在 voxel `Snapshot` 之后收到 `EntitySnapshot`（实测 35 个实体）。**实测**：非 authority 编辑实体 38 → authority 应用并广播 → 两端 `GET_COMPONENT` 均为 `(7,3,-2)`；authority 改为 `(-4,2,6)` 后两端一致；迟到客户端快照后同样为 `(-4,2,6)`。ASan 端到端无报错。
- 2026-11-23：**F14a v1 权威体素同步** 已实现：新增 channel 8（`world_sync_protocol.h`）：`Authority / Add / ClearBox / ClearPositions / SnapshotRequest / Snapshot`。服务器把首个客户端选为 authority 并在断线超时后改选；authority 的体素操作本地应用后按 `seq` 广播，其他客户端转发编辑、应用权威广播；迟到客户端请求快照（实测 91 体素快照、断线改选、新 authority 继续广播均通过）。`Api` 通过 client provider 判断是否转发；非 authority 不再本地应用，避免分叉。**实测**：非 authority 发送的 91 体素波在两端渲染一致；authority 被杀后 ~15s 改选；新客户端从新 authority 取快照并继续接收增量。**限制**：仅 API 体素层；id 清除在非 authority 返回失败。
- 2026-11-23：**服务器掉线检测修复**：ENet 对静默失联对等体发出的是 `ENET_EVENT_TYPE_DISCONNECT_TIMEOUT`（原代码只处理 `DISCONNECT`），导致崩溃/断网的玩家、窗口和 authority 永远留在服务器内存中。现在两种事件统一清理；服务器每 2s 主动 ping、连接时收紧超时（10/5s/15s），实测崩溃客户端 ~15s 内被移除并触发 authority 改选。
- 2026-11-23：**多人场景一致性校验（方案 A）** 已实现：连接时交换场景指纹（`db/matrix.db` + DB 引用的 OBJ/MTL/纹理），服务器中继；不一致时日志警告并在 Multiplayer 面板禁用窗口共享。实测同场景显示 “Scenes match”，改一条 `Positionable` 后显示 “Scene mismatch”。
- 2026-11-23：**F14b 窗口列表 UI** 已实现：Multiplayer 面板列出当前窗口、尺寸、共享/允许控制开关；默认私有，取消共享会撤销控制授权；场景不一致时整表禁用。共享状态目前只在 `Client` 本地记录，等待 F14c 串流接入。
- 2026-11-23：**F14c-1 本地 loopback 预览** 已实现：新增 `RemoteApp : AppSurface`，Multiplayer 面板的 Preview 按钮会在源窗口前方生成一个缩小副本；`renderer_apps` 增加远端窗口的 in-world 绘制路径。实测截图确认源窗口与预览副本同时显示。
- 2026-11-23：**F14c-2 MJPEG 双端串流** 已实现：`Share` 开关启动/停止发布；源端用 FBO + `glReadPixels` 抓帧，`stb_image_write` 编码 JPEG（5 FPS、quality 65），经 ENet channel 4（unreliable fragment）由服务器中继；接收端解码并用 `RemoteApp::setRgbaFrame` 上传纹理。服务器在断线时广播 `OwnerLeft`，接收端清理远端窗口。实测两个 headless 客户端：共享端日志 `sharing window ...`，接收端 `showing shared window ...`，截图确认窗口内容显示正确（修复了 JPEG 解码的垂直翻转）。
- 2026-11-23：**F14c-3 变换同步**：新增 `Transform` 消息，源窗口被移动/旋转/缩放时发送 `pos/origin/rotate/scale`，接收端更新 `Positionable`；窗口 resize 通过帧尺寸自动更新宽高比。协议单元测试覆盖 Start/Stop/Frame/Transform/OwnerLeft 与 owner 重写。
- 2026-11-23：**H.264 窗口串流**：新增 `H264Encoder` / `H264Decoder`（libavcodec/libx264/libswscale），源端 RGBA→YUV420P→H.264，接收端 H.264→RGBA→`RemoteApp`；视频走可靠 channel 6，MJPEG 保留为编码失败回退。修复奇数尺寸（1150x667）导致的 YUV 行数错误；实测远端终端画面颜色/方向正确。CMake 新增 FFmpeg 依赖，PKGBUILD 增加 `ffmpeg`。
- 2026-11-23：**H.264 优化**：编码器优先 `h264_nvenc`（本机 NVIDIA，实测启用），失败回退 libx264；新增 `KeyframeRequest` 控制消息，接收端在 Start/解码等待关键帧时请求 IDR；每个 IDR 重复缓存 SPS/PPS，迟到解码器无需等待 stream restart。自适应质量每 2 秒依据 **WebRTC RTCP `fraction-lost` / `round-trip-time`**（优先）或 ENet `packetLoss` / `roundTripTime`（回退）调整，用户设置的质量是上限。VAAPI encode 在该 NVIDIA 驱动上不可用（vainfo 只有 VLD），因此硬件路径选 NVENC。
- 2026-11-23：**WebRTC 窗口传输**：新增 `webrtc_protocol.h`（channel 7 信令：Request/Offer/Answer/ICE）、`WebRtcManager`、`webrtc_manager.cpp`。共享者按 viewer 建立 `webrtcbin` sender，viewer 建立 receiver；GStreamer 管线 `appsrc → h264parse → rtph264pay → webrtcbin`，接收端 `rtph264depay → h264parse → avdec_h264 → videoconvert → appsink`。RTP 分片由 `rtph264pay` 自动完成；DTLS/SRTP/ICE 由 `webrtcbin` + libnice 处理。`appsrc do-timestamp=TRUE` 修复了用程序相对 PTS 导致首帧被管线时钟压住 4+ 秒的问题；WebRTC 建连后主动强制 IDR，首帧约 0.2 s 到达。ENet H.264 继续发送作为回退，接收端在 WebRTC 帧新鲜时忽略 ENet 帧，避免双解码。WebRTC 连接/ICE 状态、stats 与首帧均有日志。CMake 增加 GStreamer（webrtc/app/sdp/video）依赖，PKGBUILD/BUILDING 增加 `gst-plugins-good`/`gst-plugins-bad`/`libnice`。实测两个 viewer 同时连接：两个 sender 各自建连、均能解码，控制权仍只允许一个 holder。
- 2026-11-23：**WebRTC 稳定性**：修复 `get-stats` promise 的所有权错误（回调里重复 unref 导致每 2 秒 `GstMiniObject` critical 与退出时 heap corruption）；所有 pipeline 的 start/stop/push 统一投递到 GStreamer GLib 线程，避免与渲染线程并发销毁；session 延迟回收直到 manager 析构。ASan+UBSan 端到端运行无内存错误。
- 2026-11-23：**验证**：75/75 单元测试（新增 6 个 WebRTC 信令协议测试 + 1 个 IDR SPS/PPS 重复测试）、3/3 集成测试、ASan+UBSan 单测与端到端均通过。ASan 单测的外部 CUDA/NVENC 一次性分配用 `tests/lsan.supp` 抑制。单 viewer 与双 viewer 同时共享均已实测，WebRTC 建连到首帧约 0.2–0.3 s，控制权仍严格单 holder。
- 2026-11-23：**F14e 清理/权限/迟到客户端** 已实现：服务器缓存 `Start` / 最新 `Transform` / 最新 `ControlState`，新客户端在 SceneInfo 交换后补发；客户端对未知场景的窗口消息排队到场景指纹到达再处理，避免 Start/ControlState 被丢弃；Stop、OwnerLeft、本地 disconnect 都会清理远端窗口与 controller；控制请求要求发送者场景匹配。实测第三个客户端在共享开始 8 秒后加入，仍显示远端窗口和 `Control available`。
- 2026-11-23：**F14d 默认只读 + 授权控制** 已实现：新增 channel 5 单播控制通道和 `ControlState` 广播；共享者 Allow control 后，接收端通过 Remote windows 表请求控制，owner 只接受一个 controller；指针/键盘/滚轮事件转发到 owner，由 `WaylandApp::inject*` 注入 `wlr_seat`；Release、owner 收回、controller 断线都会清理控制权。实测 control request/grant 日志与 `(you)` 状态；headless 环境没有实体键盘 keymap，文本注入需在真实会话验证。新增 3 个协议单元测试。
- 2026-11-23：**窗口共享画质/帧率设置**：Multiplayer 面板新增 `Window streaming` 区域，Target FPS 1–30、JPEG quality 10–95，默认 5 FPS / 65；设置按客户端保存在 `Client` 中，立即作用于后续抓帧。设置持久化到 `config.yaml` 的 `multiplayer_window_stream_fps` / `multiplayer_window_stream_quality`（滑条释放时写入，避免拖动过程中频繁写盘）。新增 5 个单元测试（默认值、FPS/quality 夹取、Config 读写、Client setter 持久化）。
- 2026-11-22：**F1 `KEY_REPLAY` 服务端**已实现。`Api::handleKeyReplay` 把 keysym 排入 `Controls::scheduleKeySym`，press/release 分帧调度并复用 `handleKeySym`；实测 `KEY_REPLAY 'p'` 触发截图、`p` 回显出现在画面上。向 Wayland 客户端注入文本仍需 keycode 映射，未覆盖。
- 2026-11-22：**F2 ImGui 键盘输入**已实现。`Engine::updateImGuiKey` 转发真实键盘到 `io.AddKeyEvent` + `io.AddInputCharacter`；`keysymToImGuiKey` 抽到 `include/imgui_keymap.h` 并有 6 个单测（35/35 总测试通过）。
- 2026-11-22：**F3 WM 滚轮**已修复：`handle_pointer_axis` 不再对无焦点的任意窗口发送滚动。
- 2026-11-22：**F9 Web GUI 端口**已修复：`readWebGuiPort()` 接入，Crow 构建实测自定义端口 HTTP 200。
- 2026-11-22：**F10 窗口 move/resize** 已实现：`WindowManager::moveFocusedApp` / `resizeFocusedApp` + `Super+方向键` / `Super+[`/`]`；实测移动 5×0.1、缩放至 1142x643，截图对比确认位移。
- 2026-11-22：**F13 API 体素应用示例** 已实现：`client_libs/cpp/voxel_demo.cpp`，45/45 帧 accepted，截图为动态体素波。
