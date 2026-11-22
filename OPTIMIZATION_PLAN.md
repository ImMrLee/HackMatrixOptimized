# HackMatrix 优化计划

> 状态：**已取消 Archived**（readme.md 第 1 行 `# (ARCHIVED) HackMatrix` → `# HackMatrix`）
> 范围：架构与代码质量 / 构建与工程化 / 性能与渲染 / 稳定性与测试 / 功能与体验
> 生成日期：2026-11-22

---

## 完成总结（2026-11-22）

### 交付指标

| 指标 | 改造前 | 改造后 |
|---|---|---|
| `cmake` 配置 | 联网克隆 Crow **挂死**（>30 分钟） | **0.5 秒** |
| 全量构建 | 不可达 | **~60 秒**，0 错误 |
| wlroots 来源 | configure 阶段现场编译 submodule | 系统 `libwlroots-0.19.so`；submodule 对齐 **0.19.3** |
| 运行时本地依赖 | 自带 `wlroots/build` | **零**本地 wlroots 依赖 |
| 可构建测试 | 45 个用例**零构建入口** | **29 个单元测试** + 4 个测试目标，已接入 CTest |
| 实拍验证 | — | compositor / `xdg-shell` / `layer-shell` / **XWayland** 均通过 |
| 死代码 | 98 个无用文件 | 已删（cereal / catch / 空文件等） |
| 告警 | — | 261 条，vendored 之外仅 2 处 |

### 各阶段状态

| Phase | 内容 | 状态 |
|---|---|---|
| **0** | 构建恢复：系统依赖优先、离线化、缺失协议头自动生成 | ✅ |
| **1** | 工程化：CI、git hooks、`.clang-tidy`、测试接入、sanitizer | ✅ |
| **2** | 架构：4 个大文件拆分、DDA 合并、所有权、死代码、日志统一、**EngineGui 复活** | ✅ |
| **3** | 性能：P1–P6 | ✅（P7 为防御性代码，保留） |
| **4** | 稳定性：B1–B6 | ✅（README 已知 bug 待逐条复现） |
| **5** | 功能：workspace、窗口 resize/move 等 | ⏳ 未开始 |

### 关键数值

- `VoxelSpace::has()` 空间索引：**~100×**（33.4ms vs 3338.7ms）
- `wlr_compositor.cpp`：**1735 → 576 行**（+ `wlr_output.cpp` / `wlr_shells.cpp` / `wlr_xwayland.cpp`）
- `renderer.cpp`：**1431 → 851 行**（+ `renderer_resources.cpp` / `renderer_apps.cpp`）
- `api.cpp`：`processBatchedRequest` **447 → 30 行**（handler 表）
- `WaylandApp::appTexture`：**507 → 24 行**（+ 3 个子函数）
- 单元测试：**29/29**；ASan + UBSan **29/29**；集成测试 3/3

---

## 0. 现状基线（已实测证据）

### 0.1 规模
| 项目 | 数值 |
|---|---|
| 自有 C/C++ 代码（排除 imgui/glm/cereal/entt 等 vendored） | ~13.4 万行 |
| 根级 `src/*.cpp` 合计 | 33,238 行 |
| `src/imgui`（vendored） | 42,257 行 |
| 测试代码 | 3,221 行 / 45 个 gtest |
| Git 提交数 | 1,151 |

### 0.2 最大文件与最长函数
| 文件 | 行数 | 最长函数 |
|---|---|---|
| `src/wayland/wlr_compositor.cpp` | 1,735 | `handle_new_xwayland_surface` 207 行、`handle_output_frame` 195 行 |
| `src/wayland/WaylandApp.cpp` | 847 | **`appTexture()` 507 行** |
| `src/renderer.cpp` | 1,431 | `renderApps()` 172 行 |
| `src/world.cpp` | 1,287 | 两份 DDA raycast 各 85/82 行（重复） |
| `src/api.cpp` | 996 | **`processBatchedRequest()` 446 行** |
| `src/controls.cpp` | 925 | 95 个函数 |

### 0.3 构建基线（本次实测，**失败**）
```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
→ Crow not found via find_package; fetching https://github.com/CrowCpp/Crow.git at c61a26e
→ 错误：RPC 失败。curl 56 Recv failure: 连接超时
→ fetch-pack: unexpected disconnect while reading sideband packet
→ CMake Error: Build step for crow failed
```
**根因**：`CMakeLists.txt:59-64` 在 configure 阶段强制联网 `FetchContent` 克隆 Crow。
本机无 `libcrow`，且网络到 GitHub 不稳定 → **完全无法配置**。
另：`CMakeLists.txt:83-126` 的 `build_wlroots_if_needed()` 还会在 configure 阶段用
meson/ninja 现场编译整个 wlroots，**即使系统已装 wlroots 0.19.3**（本机实测有）。

---

## Phase 0 — 恢复可构建（前置阻塞项）

> 不解决这条，后续所有阶段都无法验证。**优先级最高。**

### ✅ 已完成（2026-11-22 实测）

| # | 任务 | 结果 |
|---|---|---|
| 0.1 | Crow 支持离线/系统包优先 | ✅ 解析顺序改为 ①系统包 → ②`MATRIX_CROW_SOURCE_DIR` → ③仅 `MATRIX_FETCH_CROW=ON` 才联网 → ④否则**禁用 WebGui 并 WARNING**，不再 `FetchContent` 卡死。新增 `MATRIX_ENABLE_WEBGUI` 开关，`engine.cpp` / `engine.h` / `WebGui.cpp` 全部按宏守卫 |
| 0.2 | wlroots 优先用系统包 | ✅ 新增 `MATRIX_USE_SYSTEM_WLROOTS`（默认 ON），检测到 `wlroots-0.19` 即跳过 meson/ninja 现场编译；`add_dependencies(wlroots_build)`、RPATH、`PKG_CONFIG_PATH` 全部按模式分支 |
| 0.3 | 拆分 configure 与构建阶段 | ✅ **configure 0.47s**（原为联网克隆挂死 30 分钟）；全量 `make -j` **41s** |
| 0.7 | 生成系统包漏掉的 wlroots 协议头 | ✅ 新增 `matrix_generate_missing_protocol_headers()`：自动扫描系统 wlroots 头 + 项目源码的 `#include "*-protocol.h"`，双向搜索 xml（`wlroots/protocol/` 与系统 `wayland-protocols/`），用 `wayland-scanner` 生成 server+client 两版，共 **20 个头**。Arch `wlroots0.19` 一个都不装，这是所有 wlroots 消费者的通病 |
| 0.8 | 生成 wlroots 私有 `config.h` | ✅ 新增 `matrix_generate_wlroots_private_config()`：`HAVE_XCB_ERRORS` **位于 `struct wlr_x11_backend` 内部**（ABI 敏感），从系统 .pc 的 `Requires.private` 推导而非硬编码，实测得 `1` |
| 0.9 | SQLiteCpp 缺 `<cstdint>` | ✅ `Column.h` / `Statement.h` 补 include（同 AUR `sqlitecpp-cstdint.patch`） |
| 0.10 | 删除死代码 | ✅ `include/cereal/`（90 文件 / 1.6MB，零引用）、`catch_amalgamated.cpp`+`.hpp`（24,187 行，零引用）、`mmo/makefile`（空）、`src/wayland/makefile`（指向 wlroots 0.18 且目标 `main.c` 不存在）、`src/objectObserver.cpp`（0 行）、`src/diagnosis.cpp`（被 CMake 排除）—— 共 **96 个文件** |

**验收**：`cmake -S . -B build && make -j` 一次通过；`ldd matrix` 全部指向 `/usr/lib`，**零本地 `wlroots/build` 依赖**，无 `not found`。

### ✅ 已完成（Phase 0 收尾）

| # | 任务 | 结果 |
|---|---|---|
| 0.4 | 加 `-Wall -Wextra`（先不 `-Werror`） | ✅ 新增 `MATRIX_WARNINGS`（默认 ON），全量告警写入 `docs/WARNINGS.md`（261 条，真 bug 仅剩 2 处且都在 vendored 代码） |
| 0.5 | 移除 `-march=native` | ✅ 改为 `MATRIX_NATIVE_ARCH`（默认 OFF）；默认只保留可移植的 `-funroll-loops`，二进制不再受构建机 CPU 限制 |
| 0.6 | 删除入库的 `compile_commands.json` | ✅ `git rm --cached`，加入 `.gitignore`；`CMAKE_EXPORT_COMPILE_COMMANDS ON` 生成到 build 目录，根目录留符号链接 |

### 回退路径（vendored wlroots）修复

系统无 `wlroots0.19` 时走 submodule 构建。**submodule 已从 0.19.2 对齐到 0.19.3**，与系统库版本一致（消除了私有头与库的 ABI 错配风险）。实测修复的缺陷：

| 缺陷 | 现象 | 修复 |
|---|---|---|
| 私有头/库版本错配 | submodule 私有头是 0.19.2，系统库是 0.19.3；`wlr_compositor.cpp` 读取 `wlr_wl_output->backend`、`wlr_x11_output->x11` 等 `wlr_output` 之后的字段，布局不一致会读到错误指针 | ✅ submodule 固定到 **0.19.3**（`88a8698`），头与库同版本。0.19.3 已在上游修复 GCC 16 的两处告警（`switch.c` 兜底 + `xcursor.c` const），无需补丁，`-Dwerror=true` 亦可干净编译 |
| `meson setup` 二次配置失败 | 已存在的 build 目录报 "already configured" | 检出 `build.ninja` 时改用 `meson setup --reconfigure` |
| 半成品被误判为已构建 | meson **configure** 阶段就生成 `.pc`，但库未编完；中断后下次配置直接跳过 | 早退条件增加 `libwlroots-0.19.so` 真实存在检查 |

**验证**：`-DMATRIX_USE_SYSTEM_WLROOTS=OFF` 全流程配置 + 构建通过，`ldd matrix` 指向 `wlroots/build/libwlroots-0.19.so`；系统路径仍指向 `/usr/lib/libwlroots-0.19.so`。

### Phase 0 附带发现（转入 Phase 1）

- **`api_pb2.py` 每次 fresh build 被覆写**：`CMakeLists.txt:514` 把生成的 Python proto 写回源码树，入库版由 protoc **5.28.0** 生成、系统是 **7.36.1** → 每个新 build dir 都会让工作区变脏。需决定：改写到 build 目录，或接受版本号随系统 protoc 漂移。
- **`.githooks/pre-commit` 从未生效**：`core.hooksPath` 未配置且 `.git/hooks/` 里没有 pre-commit。已改为 cloc 缺失时不阻塞提交；`post-commit` 仍会 `cp matrix _matrix`、`cp db/matrix.db ~/backup_assets`（有副作用，待移除）。
- **`WebGui` 实为活依赖但形同虚设**：构造函数真的会起 Crow HTTP 服务器线程（端口 7775），但 `Render()` 是**空函数**、`webGui` 创建后无任何使用点；`readWebGuiPort()` 定义了**却从未被调用**，导致 `launch` 里设的 `MULTIPLAYER_WEBGUI_PORT=7777` 实际无效（硬编码 7775）。
- **`wlroots/include` 是纯头文件依赖**：168 个私有头（`backend/wayland.h` 等）系统从不安装，但**只需 include 路径、无需编译 submodule** —— 这是「系统优先」能成立的关键。

---

## Phase 1 — 构建与工程化

| # | 任务 | 证据 | 说明 |
|---|---|---|---|
| 1.1 | **接入测试到 CMake** | 原：45 个 gtest **无任何构建入口** | ✅ 抽出 `matrix_core` OBJECT 库；4 个测试目标全部构建：`matrix_unit_tests`（13 例，CTest 默认跑）、`matrix_controls_spec_tests`、`matrix_wayland_basic_tests`、`matrix_wayland_menu_spec_tests`（后三者 opt-in，`MATRIX_ENABLE_INTEGRATION_TESTS=ON`） |
| 1.2 | 修复测试二进制引用 | 原：`tests/wayland_menu_spec.cpp:331` 硬编码 `./matrix-wlroots-debug` | ✅ 改为 CMake 传入 `MATRIX_TEST_DEFAULT_COMPOSITOR_BIN="$<TARGET_FILE:matrix>"`，三个集成测试均按此定位 |
| 1.3 | 建 CI（GitHub Actions） | 原：无 `.github/` | ✅ `.github/workflows/build.yml`：Arch 容器 × Release/Debug，构建 vendored wlroots（`wlroots0.19` 不在官方源），跑单元测试 + **headless compositor 启动冒烟测试**（`matrix_wayland_basic`，`LIBGL_ALWAYS_SOFTWARE=1`） |
| 1.4 | 修复 git hooks | 原：`pre-commit` 执行 `make cloc` 但无根 Makefile；`post-commit` 拷贝到 `~/backup_assets` | ✅ `pre-commit` cloc 缺失时跳过、永不阻塞；✅ `post-commit` 改为需显式配置备份目录（`git config hackmatrix.backupDir`）才生效；⚠️ `core.hooksPath` 仍未配置（hook 需手动 `git config core.hooksPath .githooks` 启用） |
| 1.5 | 清理散落 makefile | `mmo/makefile`（**空文件**）、`src/wayland/makefile`（指向 wlroots-**0.18**，实际是 0.19） | ✅ 两者已删除（后者的目标 `main.c` 根本不存在） |
| 1.6 | 移除死代码 | `src/objectObserver.cpp`（0 行）、`src/diagnosis.cpp`（33 行）均被 CMake 排除却留在树里；`trampoline.cpp` 被排除但**文件不存在** | ✅ 已连同 `include/cereal/`、`catch_amalgamated.*` 一起删除（共 96 文件）；CMake 对应过滤行已清理 |
| 1.7 | 隔离 vendored 源码 | `src/imgui`（42k 行）与业务代码同级 | ⏳ 待做：移到 `third_party/` |
| 1.8 | 仓库体积治理 | `vids/*.gif` ~40MB 已入库，但 `.gitignore` 已写 `vids/`（矛盾）；`.git` = 133MB | ⏳ 待做：git-lfs 或外链 |
| 1.9 | 加 `.clang-tidy` | 仅有 `.clang-format` | ✅ 已加：`bugprone-*` + `performance-*` + 少量 `modernize-*`，排除 vendored 头。实测 `src/shader.cpp` 输出可操作、低噪 |

### 测试套件现状（2026-11-22 实测）

| 目标 | 用例 | 状态 |
|---|---|---|
| `matrix_unit_tests` (`tests/api.cpp` + `chunk.cpp` + `dynamicObject.cpp` + `voxelRaycast.cpp`) | 20 | ✅ **全部通过**（0.05s，CTest 默认运行） |
| `matrix_controls_spec_tests` (`controls_spec.cpp`) | 21 | ⚠️ 可构建，需 `foot`（未装）+ 显示会话 |
| `matrix_wayland_basic_tests` (`wayland_basic.cpp`) | 3 | ✅ **全部通过**（headless 拉起真实 compositor，28s） |
| `matrix_wayland_menu_spec_tests` (`wayland_menu_spec.cpp`) | 11 | ⚠️ 可构建，需 `foot`（未装）+ 显示会话 |

测试腐化修复（本次已处理）：
- `tests/api.cpp` 原测试的是**已被 ECS 取代的 Api/DynamicObjectSpace 架构**（`GET_IDS`/`mutable_getids` 已删除、`Api` 构造签名已变、`DynamicCube` 需要 color）→ 重写为 6 个**协议契约测试**。
- `tests/wayland_basic.cpp` 的 3 个用例断言 `startup: backend kind=` / `startup: XDG_RUNTIME_DIR=` / `startup: WAYLAND_DISPLAY chosen=` —— 这些日志行**已从源码移除** → 重写为 `engine: VOXEL_API_BIND=` 启动日志 + 存活检查。
- `wayland_basic` 未设置 `MATRIX_WLROOTS_OUTPUT`，导致日志永远为空（engine 仅在该变量存在时写日志）→ 已在 SetUp 中设置。

**待办**：`foot` 缺失导致 `controls_spec` / `wayland_menu_spec` 无法本地跑通；CI 中可用 `foot` 或改为注入脚本替代终端。

### Sanitizer 基线（2026-11-22）

新增 `-DMATRIX_SANITIZE=address,undefined`（`matrix_apply_sanitizers()`），并修好了 `CMAKE_BUILD_TYPE=Debug` 实际仍是 `-O3` 的问题（改用生成器表达式，Debug 为 `-O0`）。

- **结果**：20/20 单元测试在 ASan+UBSan 下通过，无内存错误、无 UB、无泄漏；ASan 版 compositor 也能正常 headless 启动。
- **两个工具链坑（已在 CMake 中自动处理）**：
  1. `api.pb.cc`（protobuf 生成代码）**带 ASan 插桩即崩**（GCC 16 与 Clang 均复现，已做成最小独立复现；非项目 bug）→ 只对该文件加 `-fno-sanitize=…`。
  2. 系统 `libprotobuf` 未插桩，改写 `std::string` 容量会触发 ASan 假阳性 `container-overflow` → CTest 设 `ASAN_OPTIONS=detect_container_overflow=0`。
- **注意**：单测覆盖仅 13 例，sanitizer "干净"只说明这 13 例涉及的范围干净。Phase 2 重构前应先补目标区域的测试。

### 本轮发现并修复的一个回归（API REP 状态机）

修复"畸形帧被当作默认 MOVE 执行"时，最初写成"解析失败直接 `return`"——但 `CommandServer` 用的是 `zmq::socket_type::rep`，REP 要求严格 `recv → send` 交替。不回复会让 socket 进入 EFSM，而 `Api::poll()` 是 `while(continuePolling){ commandServer->poll(); }` 紧循环且 `catch` 吞异常 → **整个 API 永久卡死并 100% CPU 空转**。

- **修复**：解析失败时仍发送一个 `success=false` 的 `ApiRequestResponse`（满足 REP 约定）；`catch` 中加 1ms sleep 兜底防热自旋。
- **端到端验证**（真实 headless compositor + 独立 C++ 客户端）：畸形帧 → 收到回复 → 合法 STATUS 正常 → 再畸形 → 再 STATUS 仍正常，日志出现两次 `api: unparseable request`。
- **待办**：这个路径没有单测（`Api` 构造需要完整 Engine），建议后续用可注入的 socket/假 Engine 覆盖。

---

## Phase 2 — 架构与代码质量

### 2.1 拆分超大文件
| 目标 | 现状 | 拆分建议 |
|---|---|---|
| `wlr_compositor.cpp` | ~~1,735 行 / 19 处 `fprintf(stderr)`~~ | ✅ **已完成拆分**：`wlr_compositor.cpp` **1735 → 576 行**。拆为四个 TU + 内部共享头：`wlr_output.cpp`（608 行：output frame/commit/state、GL/FBO/depth 初始化、Wayland/X11 全屏——**wlroots 私有头依赖被彻底隔离到此单文件**）、`wlr_shells.cpp`（364 行：`xdg_shell` + `layer_shell`）、`wlr_xwayland.cpp`（266 行：XWayland）、`wlr_compositor.cpp`（576 行：WlrServer 生命周期、协议初始化、input 分发）、`wlr_internal.h`（跨 TU 声明）。已用真实客户端实拍截图分别验证：`weston-simple-shm`（`xdg_shell`）、`wofi --show dmenu`（`layer_shell` 覆盖层）、`xmessage`（XWayland X11 客户端） |
| `renderer.cpp` | ~~1,431 行~~ | ✅ **已完成拆分**：`renderer.cpp` **1431 → 851 行**。拆出 `renderer_resources.cpp`（GL 资源生成/属性绑定，~180 行）与 `renderer_apps.cpp`（`drawAppDirect`/`renderPopup`/`renderApps`/`registerApp`/`deregisterApp` + `supportsFramebufferBlit`，~345 行）；共享几何放 `src/renderer_internal.h`（inline 变量，单一定义且 `sizeof` 可用）。已用 `weston-simple-shm` 客户端分别验证「仅应用」与「应用 + ImGui 同屏」渲染正确 |
| `api.cpp` | ~~`processBatchedRequest` 单函数 446 行 switch~~ | ✅ **已完成**：拆成 `std::map<MessageType, void (Api::*)(const BatchedRequest&)>` 分发表 + 16 个 `handleXxx` 成员函数；`processBatchedRequest` 从 **447 行降到 30 行**。e2e 探针验证 `CREATE_ENTITY`/`LIST_ENTITIES`（实体数 41→42） |
| `WaylandApp.cpp` | ~~`appTexture()` 507 行~~ | ✅ **已完成**：拆为 `destroyImportedImage()`（EGLImage 生命周期）、`tryImportDmabuf()`（零拷贝 dmabuf 导入）、`uploadCpuFallback()`（CPU 读回 + 直接上传/格式转换）、`appTexture()`（编排，约 23 行）。逐行比对确认代码搬移无损；用 `weston-simple-shm` 客户端实拍截图验证渲染正确 |

### 2.2 消除重复代码
| 重复 | 位置 | 处理 |
|---|---|---|
| **两份 DDA voxel raycast** | `world.cpp` `raycastVoxelSpace` 与 `getLookedAtCube` 的 `tMax/tDelta` 逻辑完全一致 | ✅ **已合并**为 `include/voxel_raycast.h` 的 `ddaVoxelRaycast(origin, dir, limit, maxSteps, stepAtLeastOnce, isSolid)`；两种循环语义（预检查 vs do-while）用参数保留。新增 `tests/voxelRaycast.cpp` **7 个用例**覆盖正/负向命中、法线、未命中、步数上限、距离上限、do-while 差异 |
| 注释掉的完整实现 | `wlr_compositor.cpp:474-512` 被 `/* */` 包裹的 `compute_layer_shell_position` 真实版本 | ✅ 已随 B1 恢复并删除重复 |

### 2.3 资源与所有权 ✅（2026-11-22）

| 问题 | 证据 | 处理 |
|---|---|---|
| `Engine` 成员裸指针 | `World* / Api* / Renderer* / Controls* / Camera*` + 手写 `delete` | ✅ 改为 `std::unique_ptr`；访问器返回 `.get()`；析构显式 `reset()` 保持原顺序（**先 `api`** 停线程，再 controls/renderer/world/camera） |
| `Api` 泄漏 | `//delete api;` 被注释，轮询线程从不停止 | ✅ `unique_ptr` 管理；`~Api` 关 context + join 线程；QUIT 实测约 1s 干净退出 |
| **`Renderer` shader 所有权 bug** | `shader` 只是 `cameraShader`/`depthShader` 的**别名**，析构 `delete shader` 只删别名指向的对象，其余 shader 泄漏；换 shader 后可能重复释放 | ✅ 三个 shader 改 `unique_ptr`，`shader` 改为**非拥有别名**（`= .get()`），删除 `delete shader` |
| 死成员 | `Shader* appShader` 声明但从未赋值/使用 | ✅ 删除 |
| `textures` 裸指针 map | `std::map<string, Texture*>` + 析构手动 `delete` | ✅ 改 `std::map<string, std::unique_ptr<Texture>>` |
| RAII 已部分落地 | `gl_resource.h` 的 `GlBuffer`/`GlVertexArray` 仅 renderer 用 | ⏳ 仍待推广到 `texture.cpp`/FBO（后续） |

**验证**：QUIT 干净退出（约 1s）；ASan 版 compositor 走完整析构路径**零 ASan 报告**；20/20 单测 + 3/3 集成测试 + ASan 20/20 全部通过。

### 2.4 死代码 / 空壳

| 项 | 证据 | 状态 |
|---|---|---|
| `Renderer::renderLookedAtFace()` | 定义 + 声明都在，**全仓库零调用** | ✅ 已删（连同只被它使用的 `VOXEL_SELECTIONS`/`VOXEL_SELECTION_POSITIONS`/`VOXEL_SELECTION_TEX_COORDS` 三个 GL 资源） |
| 空 `if` 块 | `engine.cpp` `if (engineGui) { }` | ✅ 已删 |
| 浪费的 GL 查询 + 被注释的恢复 | `engine.cpp` 三次 `glGetIntegerv` 取状态，恢复代码却被注释 | ✅ 已删（查询无副作用、恢复从未执行 → 行为不变） |
| 注释掉的层壳定位实现 | `wlr_compositor.cpp:474`，活版本 `:462` 返回 `{0,0}` | ✅ 已随 B1 修复 |
| 7 个无调用的静态函数 | 编译器 `-Wunused-function` 报出 | ✅ 已删：`keysym_name`、`wayland_app_for_surface`、`wayland_app_for_entity`（级联）、`matches_key_binding`、`matches_any_key_binding`、`key_not_handled_by_wm()`（被同名局部变量遮蔽）、`is_shifted_digit_sym` → 该类告警**清零** |

**已复活（2026-11-22）**：`EngineGui` 之前**从未被实例化**（全仓库无 `make_shared<EngineGui>`），`engineGui->render()` 也从未被调用，ImGui 覆盖层是死功能。现已修复：

- `Engine` 构造函数中在 `initializeMemberObjs()` 之后创建 `EngineGui`（此时 GL 上下文由 wlroots 渲染回调持有）
- `Engine::frame()` 中调用 `engineGui->render(fps, frameIndex, frameTimes)`，并在 `controls->poll()` 前调用 `disableKeysIfImguiActive()`
- `EngineGui::render()` 补回 `ImGui_ImplOpenGL3_RenderDrawData()`（原本被注释，导致即使接线也不显示）
- 修正 Y 方向：wlroots 输出 FBO 是 top-down，对顶点/裁剪矩形做垂直镜像（可用 `HACKMATRIX_IMGUI_FLIP=0` 关闭）
- 鼠标输入接线：`pointer.cpp` 的 motion/motion_abs/button 调用 `updateImGuiPointer()`；ImGui 捕获鼠标时点击不再传给游戏或 Wayland 客户端
- 移除入库的 `imgui.ini`（含 `Collapsed=1`，且 `.gitignore` 早已忽略它），窗口改用默认尺寸 `520x560`
- 新增 `HACKMATRIX_ENABLE_GUI=0` 关闭覆盖层、`HACKMATRIX_SCREENSHOT_FRAME=N` 在第 N 帧截图（headless 验证用）

**已用截图实际验证**：窗口标题栏、FPS/Debug Log/Entity Editor/Multiplayer 四个标签页、FPS 数值均正确渲染，方向正常。

**仍缺**：键盘事件未接入 ImGui（`io.AddKeyEvent`/`AddInputCharacter`），因此文本输入框暂时无法输入；鼠标点击/拖拽/标签切换可用。

### 2.5 日志统一 ✅（2026-11-22）

- **`wlr_compositor.cpp`**：19 处 `fprintf(stderr, ...)` → `wlr_log(WLR_ERROR/INFO/DEBUG, ...)`
- **热路径日志移除**：`renderer.cpp` 每帧 `lightIndex`（阴影 pass）、`components/Light.cpp` 构造函数与 `lightspaceTransform`（`renderDepthMap` 每帧调用）
- **统一到 spdlog**：`Engine::setupLogger()` 里 `spdlog::set_default_logger(logger)`，之后 `MultiPlayer/Client|Server`、`assets`、`model`、`shader`、`systems/Boot|Scripts`、`texture` 共 **23 处 `cout`** 全部改为 `spdlog::error/info/warn/debug`，走同一个 logger（file + ImGui sinks）
- **修复 flush 策略**：`logger->flush_on(debug)`，否则进程被 SIGTERM/崩溃时文件日志为 0 字节（实测复现）
- **结果**：项目代码 `cout` **清零**（vendored `imgui` 除外）；`bootServer` 补充 `spdlog`/`fmt` 链接
- 现状：spdlog（业务 + 启动路径）+ `wlr_log`（wayland 层）两层，热路径无同步日志

### 2.6 `refactor_ideas.txt` 十条落地状态核对
| # | 想法 | 状态 |
|---|---|---|
| 1 | RAII GL 资源 | 🟡 部分（`gl_resource.h` 仅 renderer 用） |
| 2 | Renderer 状态 RAII guard | 🔴 未做 |
| 3 | App 渲染策略接口（X11/Wayland/direct 统一） | 🟡 部分（`AppSurface` 已存在） |
| 4 | 纹理单元管理器封装 `IndexPool` | 🔴 未做（`IndexPool` 仅 66 行，裸用） |
| 5 | 日志门面 | 🔴 未做（见 2.5） |
| 6 | Wayland 渲染拆成独立模块 | 🔴 未做 |
| 7 | 热键命令模式 | 🔴 未做 |
| 8 | 枚举化渲染 pass（替代布尔开关） | 🔴 未做（`renderer.h` 仍有 `isWireframe/invertY/voxelsEnabled/shadowsEnabled` 等） |
| 9 | 帧 pass 调度器（FBO/viewport 转换声明化） | 🔴 未做 |
| 10 | Space 命中测试辅助（隔离 ray-plane 相交） | 🟡 部分（`raycastApp` 已较独立） |

---

## Phase 3 — 性能与渲染

> 已有 Tracy 集成（`engine.cpp:122,237` `TracyGpuContext/Collect`，`renderer.cpp:968,1265,1325`），但**多数热点未打点**。

### 3.1 已定位的热点（按影响排序）

| # | 问题 | 证据 | 影响 |
|---|---|---|---|
| **P1** | **`VoxelSpace::has()` 是 O(n) 线性扫描**，而 `raycastVoxelSpace` 每步调用它、最多 **256 步** | `VoxelSpace.cpp` 遍历 `voxels`；`world.cpp` 循环上限 `steps < 256`；`renderer.cpp` `voxelSpace.has(pos, s)` | ✅ **已优化**：改为 1 单位网格空间哈希（3×3×3 邻域保证 EPS 边界正确），≤64 个体素仍走线性扫描；`remove`/`clear` 维护索引。基准（2 万体素 × 10 万查询）：**哈希 33.4ms vs 线性 3338.7ms ≈ 100×**，命中数完全一致。新增 `tests/voxelSpace.cpp` 9 个用例 |
| **P2** | **`shader->set*` 无 uniform 位置缓存** | `shader.cpp` 每次调 `glGetUniformLocation(ID, name.c_str())` + `name` 字符串进 map | ✅ **已优化**：新增 `uniformLocations` 缓存（含 -1，`glUniform*` 会忽略），重链接时清空；73 次/帧的驱动查找→哈希查找。29/29 单测通过，渲染截图正常 |
| **P3** | **每帧 `fs::last_write_time()` ×2-3 次**（shader 热重载检查） | `shader.cpp` `sourceCodeChanged()`；`renderer.cpp` 每帧调 `reloadIfChanged()` | ✅ **已优化**：每 `Shader` 实例 500ms 节流，stat 调用从 ~350/s 降到 ~4/s |
| **P4** | **每帧 `glGetIntegerv` ×3**，且结果被注释掉不用 | `engine.cpp` 保存 prevProgram/prevArray/prevTexture；恢复代码**已注释** | ✅ **已删除**（Phase 2.4 死代码清理） |
| **P5** | `lightUniforms` 每帧字符串拼接 + O(MAX_LIGHTS) 内层 hack 循环 | `renderer.cpp` `"lightPos[" + std::to_string(i) + "]"`；每盏灯重设全部 `depthCubeMap` | ✅ **已优化**：`lightPos/lightColor/far_plane/shadowMatrices/depthCubeMap` 名称数组启动时预生成，去掉每帧 `to_string`+拼接；`MAX_LIGHTS` 改 `constexpr` |
| **P6** | `raycastApp` 遍历所有 `Positionable`，每个做 `glm::inverse` 4×4 求逆 | `Space.cpp` `raycastApp`；`renderApps` 每帧调 `getLookedAtApp()` | ✅ **已优化**：`Space` 按实体缓存 `localToWorld`/`worldToLocal`，**直接比较 `modelMatrix` 与 `heightScalar`** 判断失效（不依赖 `damaged`，因为 `systems::Update` 每帧会清标志）；`removeApp` 清理缓存条目 |
| **P7** | `renderApps` 每帧重设 VAO 顶点属性 | `renderer.cpp:976-987` 注释自认 "Defensive: ensure … even if VAO state was clobbered" | 每帧重复 `glVertexAttribPointer`×2 + `glEnableVertexAttribArray`×2 |

### 3.2 建议顺序
1. **P1**（体素索引）— 收益最大，改动局部
2. **P4**（删死代码）— 零风险，立即删
3. **P3**（stat 降频）— 改为 mtime 缓存 + 按需检查（如仅 debug 构建或每 N 帧）
4. **P2**（uniform 缓存）— `unordered_map<string, GLint>`，shader 重编译时清空
5. **P5**（uniform 预生成）— `lightPos[0..4]` 名字预分配，去掉内层 hack 循环
6. **P6**（逆矩阵缓存）、**P7**（VAO 状态断言化）

### 3.3 补充打点
用已有 Tracy 覆盖未打点路径：`World::tick`、`Api::mutateEntities`、`VoxelSpace::has`、`raycastApp`、`appTexture`。

---

## Phase 4 — 稳定性与测试

### 4.1 已确认的缺陷

| # | 缺陷 | 证据 | 严重度 | 状态 |
|---|---|---|---|---|
| **B1** | **layer-shell 定位永远返回 `{0,0}`**，真实实现被注释 | `wlr_compositor.cpp:462-471` 活版本 `return {0,0}`；`:474-512` 真实版本在 `/* */` 内；`:567` 调用点 | **高** — wofi 菜单/覆盖层定位异常，且 `x`/`y` 是死变量（-Wunused 必报） | ✅ **已修**：恢复按 anchor/margin 计算，删除空壳与注释块 |
| **B2** | **`abs(float)` 依赖 `using namespace std;` 才不崩溃** | `world.cpp:891,894,897` `1 / abs(camera->front.x)`；`world.cpp:28` `using namespace std;` | **高** — 去掉 `using namespace std` 后 `abs(0.5f)` → `0` → **SIGFPE 整数除零崩溃** | ✅ **已修**：6 处（两份 DDA，`x/y/z` 各两处）改为 `std::fabs`；剩余 `abs` 只作用于 int |
| **B3** | 未初始化成员 | `renderer.cpp` `int lastTextureUnit;`（只写不读） | 中 | ✅ **已删除**（死变量） |
| **B4** | `MAX_LIGHTS` 声明为 `float` | `renderer.cpp` `float MAX_LIGHTS = 5;` 却用于 `int i < MAX_LIGHTS` | 低 | ✅ **已改为 `int`** |
| **B5** | 热路径 `cout` 输出 | `renderer.cpp` `cout << "lightIndex: "` 在每帧光照循环内 | 中 | ✅ **已删除**（Phase 2.5） |
| **B6** | `server->engine->` 直接解引用 | 多数已有 `if (server->engine)` 包裹 | 中 | ✅ **已修两处真实风险**：`add_action` 判空前解引用 + 死变量；`remove_action` 的 `getRenderer()` 补判空 |

### 4.2 README / todo.md 已知 bug（待复现归档）
- 手动 focus（按 `r`）偶发失效，需 Super+1 绕过
- WM 模式下滚轮被失焦窗口处理 → 未定义状态，只能 focus/unfocus 恢复
- 透明物体与 app 渲染叠加错误
- 光照 acne、光照距离圆形条纹
- 最多同时显示 10 个 app
- 加载的 app 朝向错误（"load app spun around"）

> 处理：每条建 issue，附复现步骤 + 自动化测试（现有 45 个 gtest 可扩展）。

### 4.3 测试补强

现有覆盖（已全部接入 CMake）：`controls_spec`(21)、`wayland_menu_spec`(11)、`chunk`(5)、`wayland_basic`(3)、`api`(6)、`dynamicObject`(2)、`voxelRaycast`(7) = **55 个**。

优先补：
1. ✅ **Phase 0 修复后的冒烟测试** — CI 已跑 `matrix_wayland_basic`（headless compositor 启动）
2. `VoxelSpace::has` 正确性 + 性能回归（配合 Phase 3 改造）
3. `compute_layer_shell_position` 定位（B1 修复后）
4. ✅ `raycastVoxelSpace` / `getLookedAtCube` 合并后的等价性（`tests/voxelRaycast.cpp`，7 例）
5. shader uniform 缓存（Phase 3 改造后）
6. **ImGui 键盘输入接入**（`io.AddKeyEvent`/`AddInputCharacter`）：当前调试覆盖层只支持鼠标，文本输入框不可用

---

## Phase 5 — 功能与体验

依据 `todo.md` 与 `distro_ideas`：

| # | 特性 | 来源 | 备注 |
|---|---|---|---|
| F1 | **Workspace 概念** | `todo.md` Milestone（整个里程碑） | 全仓库 `grep workspace` **零命中** — 完全未实现。含：持久化动态 app、选择/命名 workspace、按需动态启动 |
| F2 | **窗口 resize / move** | `todo.md` "move app / resize app" | `WindowManager` 仅有 `removeApp`；无 resize/move API。需注册控制键 + 拖动时 blit + 更新坐标 |
| F3 | 可变长宽比窗口（半屏 app 等） | `distro_ideas` | `window_manager_elements` 在仓库中**零命中**，需重写 |
| F4 | API 驱动的体素应用 | `distro_ideas` | map solver / 音频可视化 / 神经网络可视化 |
| F5 | 窗口热键栈（顶为 "1"） | `todo.md` "Maybe not bugs" | 现为按创建顺序分配（`WindowManager.cpp:339 assignHotkeySlot`），仅支持 1-9（`controls.cpp:872` `for i<9`） |
| F6 | 重力系统 | `todo.md` | 未实现 |
| F7 | 修复并发 10 app 限制 | `todo.md` | 未定位根因，需先复现 |

---

## 执行顺序建议

```
Phase 0  恢复可构建          ← 阻塞一切，必须先做
   │
   ├─→ Phase 1  工程化（CI/测试接入/告警清单）
   │       │
   │       ├─→ Phase 4  稳定性（B1/B2 是真崩溃级 bug，可与 Phase 1 并行）
   │       │
   │       └─→ Phase 2  架构重构（依赖测试网兜底）
   │               │
   │               └─→ Phase 3  性能（依赖重构后的可测性）
   │
   └─→ Phase 5  功能（workspace / resize，可独立并行）
```

**关键约束**：Phase 2/3 的重构必须在 Phase 1 测试接入**之后**进行 ——
当前 45 个 gtest 没有构建入口，重构时**没有任何安全网**。

---

## 附：本次实测记录

| 项 | 结果 |
|---|---|
| `cmake -S . -B build` | ❌ **失败**：Crow FetchContent 联网克隆超时（`curl 56 Recv failure`） |
| 系统 wlroots | ✅ 0.19.3 已安装（但 CMake 仍尝试现场编译） |
| 系统 Crow | ❌ 未安装（无 `libcrow`、无头文件） |
| `compile_commands.json` | ⚠️ 已入库，345 处 `/home/collin/` 原作者路径，本机无效 |
| `.githooks/pre-commit` | ❌ 引用不存在的根 Makefile（`make cloc`） |
| `abs(float)` 崩溃验证 | ✅ 复现 SIGFPE（`exit=136`），加 `using namespace std` 后正常 |
