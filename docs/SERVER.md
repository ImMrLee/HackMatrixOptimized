# 专用服务器（实验性）

`matrix-server` 是服务器拆分工作的第一个无头目标。该目标刻意保持精简，主要用于验证场景数据库和网络模块能够脱离合成器、GPU 和 ImGui运行。

## 构建与运行

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)" --target matrix-server
./build/matrix-server --port 7777 --db db/matrix.db
```

选项：

```text
--port N    ENet 客户端使用的 UDP 端口（默认 1234）
--db PATH   场景数据库路径（默认 db/matrix.db）
```

## 当前功能

- 从场景数据库中加载 `Entity` 与 `Positionable` 行，并写入服务器端的 `EntityRegistry`（参见 `src/server/WorldSimulation.cpp`）。
- 在没有 GL 的情况下，以 20 Hz 执行移动系统，包括 `applyRotation`、`applyTranslations` 和受损变换更新。
- **拥有世界权威**：在通道 8 上发布 `kServerAuthorityID`（`0xFFFFFFFF`）。客户端向服务器提交体素或实体修改；服务器应用修改、递增序列号，并广播快照或增量更新。后加入的客户端会收到体素与实体快照。
- 在不依赖 GL 的情况下维护运行时体素覆盖层（`DynamicObjectSpace`）。
- 管理玩家列表：连接时分配出生点；向新玩家发送所有现有玩家状态；通过通道 9 广播 `PLAYER_JOIN` 和 `PLAYER_UPDATE`；断开连接时广播 `PLAYER_LEAVE`。客户端会在玩家离开时销毁对应的化身实体。
- 即使没有其他玩家在线，窗口共享仍可工作：连接到专用服务器的客户端可以提前共享窗口。服务器会缓存 `Start`、`Transform` 和 `ControlState`，当新加入者发送场景指纹时再重新播放这些状态。
  - 在专用服务器模式下，本地数据库哈希漂移不会阻止窗口共享：运行时状态（`Positionable`、`ServerVoxels`）不会计入场景指纹，`sceneMatches()` 会信任服务器拥有的世界状态。
  - 在客户端权威/中继模式下，指纹仍用于限制窗口共享。
- 启动现有的 ENet 中继（`MultiPlayer::Server`），接受玩家，并报告已连接玩家、断开玩家和在线人数。
- **不链接** OpenGL、wlroots、Wayland、X11、GStreamer 或 ImGui。
  - `ldd build/matrix-server` 理论上只应显示 libc、sqlite3、enet（头文件实现）、spdlog/fmt及其依赖。
- `bootServer` 和客户端托管会话仍保留旧行为（`Server(false)`：第一个连接的客户端拥有权威）。只有 `matrix-server` 会构造 `Server(true)`。
- 当前只加载 `Positionable`。Model 和 Light 持久化代码会创建 GL资源，因此在可视数据能够支持延迟加载或可选加载之前，服务器端注册表会刻意省略这些组件。
- 持久化：每 30 秒保存一次修改，并在收到 SIGTERM 或 SIGINT 时保存。
  - `Positionable` 行会正常写入；运行时体素覆盖层使用服务器专用的 `ServerVoxels` 表。
  - 启动时会先恢复这两类数据，再发送首份快照。
  - 应使用**服务器拥有的数据库副本**（例如 `cp db/matrix.db /srv/hackmatrix/world.db`），以防客户端持久化本地场景时覆盖服务器数据库。

玩家移动目前仍由客户端上报（服务端只验证 `PLAYER_UPDATE` 的包长度，随后保存并广播）。输入驱动的移动权威、NPC、管理员命令、Model/Light 延迟加载以及独立 Arch 软件包仍有待实现。

## 为什么采用这种结构

客户端包含大量 GL 与 Wayland代码，但世界模型和协议是共享的。将 `matrix-server` 保留在同一仓库并为它设置独立目标，使客户端和服务器能够使用相同的头文件与数据库结构，无需维护第二份源代码树。等到仿真核心被抽离后，该目标会链接 `matrix_core_server` 库，而不是完整客户端。

## 后续步骤

1. 将不依赖 GL 的世界、实体和系统代码提取到 `matrix_core_server` 库中。
   - `World` 当前假定存在 `Renderer`，并在构造函数中创建演示体素方块；这些调用需要改为可选。
2. 添加服务器拥有的注册表和仿真循环：
   - 加载不依赖 GL 的组件（`Positionable`、Door/Key/Lock、Parent、Scriptable、Bootable），而不构造依赖 GL 的 `Model` 或 `Light` 对象；
   - 在服务器上更新玩家、体素和实体变换；
   - 使用服务器作为发送者，通过通道 8 发送快照和增量更新。
3. 移除客户端侧的权威选举逻辑；服务器始终是权威方。
4. 让服务器成为 `matrix.db` 的唯一写入者；客户端加载只读副本，或直接接收服务器发送的状态。
5. 添加 systemd/Docker 打包及独立 Arch 软件包（`hackmatrix-server`），并实现配置、能力和协议版本握手。

窗口共享仍采用客户端到客户端的 WebRTC；服务器只负责中继信令、窗口元数据和控制授权。跨 NAT 部署可以额外配置 TURN 中继。
