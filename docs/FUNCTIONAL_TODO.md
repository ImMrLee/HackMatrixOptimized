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
