# 在 Debian / Ubuntu 上构建与运行 HackMatrix

本文说明如何在 Debian 系发行版上安装依赖、配置构建以及启动 HackMatrix。项目使用 `wlroots 0.19`，因此发行版的 wlroots 版本或子模块来源是成功构建的关键。

> 初步结论：项目可以在 Debian/Ubuntu 系统上尝试构建，但尚未在目标发行版中实际完成验证。应首先确认可用的 wlroots 版本，并确认所有系统依赖均已安装。

## 兼容性要求

构建 HackMatrix 时需要以下环境：

- CMake 3.16 或更高版本
- C 和 C++ 编译器，且支持 C++20
- `pkg-config`、Git 和 Make 或 Ninja
- Wayland、Wayland 协议和 wlroots 0.19
- ZeroMQ、Protocol Buffers 和 `protoc`
- FFmpeg、GStreamer、spdlog、fmt、assimp、SQLite
- EGL、OpenGL ES 2、pixman、libdrm 和 libxkbcommon
- 可选：GTest、Crow、Meson、Ninja 和 Wayland 应用启动器

项目会强制检查以下事项：

- `protoc` 与系统 `libprotobuf` 版本必须一致。
- 必须能找到所需的 FFmpeg 和 GStreamer pkg-config 模块。
- 默认优先使用系统 wlroots；找不到兼容的系统版本时，可以构建仓库中的 wlroots 子模块。

仓库的主构建配置位于 [`CMakeLists.txt`](../CMakeLists.txt)，wlroots 路径位于仓库根目录的 `wlroots/` 子模块中。

## 1. 安装基础依赖

下面的命令适用于 Debian 或 Ubuntu，并补充了项目构建配置实际要求的 FFmpeg 和 GStreamer 包：

```bash
sudo apt update
sudo apt install -y \
  build-essential cmake pkg-config git meson ninja-build \
  libwayland-dev wayland-protocols \
  libxcb1-dev libxcb-composite0-dev libxcb-dri3-dev libxcb-ewmh-dev \
  libxcb-errors-dev libxcb-icccm4-dev libxcb-present-dev \
  libxcb-render0-dev libxcb-render-util0-dev libxcb-res0-dev \
  libxcb-shm0-dev libxcb-xfixes0-dev libxcb-xinput-dev \
  libzmq3-dev libprotobuf-dev protobuf-compiler \
  libspdlog-dev libfmt-dev libegl-dev libgles2-mesa-dev \
  libassimp-dev libsqlite3-dev libpixman-1-dev libdrm-dev \
  libxkbcommon-dev libavcodec-dev libavutil-dev libswscale-dev \
  libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
  libgstreamer-plugins-bad1.0-dev \
  libinput-dev libseat-dev libudev-dev libgbm-dev \
  wofi xwayland
```

可选地安装测试依赖：

```bash
sudo apt install -y libgtest-dev
```

如不需要多人 Web GUI，可以在配置阶段使用 `-DMATRIX_ENABLE_WEBGUI=OFF`，从而不要求安装 Crow。

## 2. 克隆仓库并初始化子模块

必须同时取得 `tracy` 和 `wlroots` 子模块：

```bash
git clone --recurse-submodules https://github.com/ImMrLee/HackMatrixOptimized.git
cd HackMatrixOptimized
```

如果仓库已经克隆但没有初始化子模块：

```bash
git submodule update --init --recursive
```

## 3. 棘手依赖：wlroots 0.19

### 检查系统 wlroots

先确认系统是否提供项目要求的 pkg-config 模块：

```bash
pkg-config --modversion wlroots-0.19
```

只有当该命令成功输出 `0.19.x` 时，系统 wlroots 路径才适用。仅安装名为 `libwlroots-dev` 的包不能代表匹配；该包必须实际提供 `wlroots-0.19` 模块和私有头文件兼容性。

从可用发行版角度：

| 发布版 | wlroots 0.19 状况 |
|---|---|
| Debian unstable（sid） | 曾提供 `libwlroots-0.19-dev`，后续可能已升级到 0.20；构建前需要再次检查包版本 |
| Debian testing | 曾提供 wlroots 0.19，但可能已升级到更新版本 |
| Ubuntu 26.04 LTS（Resolute Raccoon） | Ubuntu Universe 曾提供 `libwlroots-0.19-dev` 0.19.2 |
| Ubuntu 24.04 / 22.04 等较老版本 | 通常不能直接保证提供 wlroots 0.19 |

发行版软件包会随着时间变化，以上信息不应替代当前系统中的 `apt` 与 `pkg-config` 检查。

### 使用系统 wlroots

系统版本匹配且已能通过 `pkg-config` 找到时：

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DMATRIX_USE_SYSTEM_WLROOTS=ON \
  -DMATRIX_BUILD_TESTS=ON
cmake --build build -j"$(nproc)"
```

如果系统版本与仓库子模块版本不一致，CMake 默认只发出警告；需要严格匹配时，可启用：

```bash
cmake -S . -B build \
  -DMATRIX_USE_SYSTEM_WLROOTS=ON \
  -DMATRIX_STRICT_WLROOTS_VERSION=ON
```

### 使用仓库中的 wlroots 子模块

如果系统没有 wlroots 0.19，可以让 CMake 在配置阶段通过 Meson 和 Ninja 构建子模块：

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DMATRIX_USE_SYSTEM_WLROOTS=OFF \
  -DMATRIX_BUILD_TESTS=ON
cmake --build build -j"$(nproc)"
```

仓库已将 wlroots 固定为兼容的 0.19 版本。子模块构建通常耗时较长，并需要完整的 wlroots 构建依赖。

如需强制忽略已安装的系统 wlroots：

```bash
cmake -S . -B build \
  -DMATRIX_FORCE_BUILD_WLROOTS=ON
```

## 4. 常用构建选项

| CMake 选项 | 默认值 | 说明 |
|---|---:|---|
| `MATRIX_USE_SYSTEM_WLROOTS` | `ON` | 优先使用系统 wlroots |
| `MATRIX_FORCE_BUILD_WLROOTS` | `OFF` | 即使发现系统包也构建子模块 |
| `MATRIX_STRICT_WLROOTS_VERSION` | `OFF` | 系统 wlroots 与子模块版本不一致时直接报错 |
| `MATRIX_BUILD_TESTS` | `ON` | 构建 GTest 测试目标 |
| `MATRIX_BUILD_SERVER` | `ON` | 构建专用服务器 |
| `MATRIX_ENABLE_INTEGRATION_TESTS` | `OFF` | 注册需要真实 Wayland/DRM 会话的集成测试 |
| `MATRIX_ENABLE_WEBGUI` | `ON` | 构建多人 Web GUI；找不到 Crow 时自动关闭 |
| `MATRIX_FETCH_CROW` | `OFF` | 允许网络下载 Crow |
| `MATRIX_BUILD_EXAMPLES` | `OFF` | 构建客户端示例 |
| `MATRIX_NATIVE_ARCH` | `OFF` | 启用 `-march=native`；只适合本机，不能用于分发包 |

不需要测试时可以禁用：

```bash
cmake -S . -B build \
  -DMATRIX_BUILD_TESTS=OFF
```

## 5. 构建产物

默认构建流程：

```bash
cmake --build build -j"$(nproc)"
```

主要产物包括：

- `build/matrix`：桌面合成器客户端
- `build/matrix-server`：专用服务器
- `build/tools/deployTools/bootServer`：多人游戏辅助服务
- `build/matrix_unit_tests`：单元测试（可用时）

不要把构建目录添加到当前仓库的提交中。仓库可能会在构建配置和协议生成阶段更新某些生成文件。

## 6. 运行 HackMatrix

请不要使用 `sudo` 启动程序。HackMatrix 需要当前用户对 Wayland 或 DRM 设备拥有访问权限。

### 在现有 Wayland 桌面中嵌套运行

仓库根目录的启动脚本会复制默认配置到用户配置目录，并自动选择可用后端：

```bash
./launch
```

启动脚本会自动加载：

- `~/.config/HackMatrix/config.yaml` 或 `$HACKMATRIX_CONFIG_HOME/config.yaml`
- `build/matrix` 可执行文件
- 仓库中的 wlroots 子模块构建目录（如存在）
- Wofi、Kitty 等应用配置

### 在 TTY 中直接运行 DRM 后端

先切换到没有桌面管理器占用显示设备的 TTY，然后运行：

```bash
./launch --drm
```

若未配置 `logind` 或没有输入设备权限，可能需要将用户加入对应的系统组。具体权限管理因发行版而异，不应直接使用 `sudo ./launch`。

### 在嵌套 wlroots 或测试桌面中运行

```bash
./launch --wayland
./launch --x11
./launch --headless
```

仅在没有固定物理显示会话时使用 headless 模型；headless 模式主要适合测试，不代表完整桌面体验。

## 7. 默认运行时依赖

运行应用窗口时还可能需要：

- Wofi：从配置中的 `menu_program` 启动应用菜单
- XWayland：运行仍依赖 X11 的应用
- Kitty：作为默认终端（可在配置中修改）
- Vim：作为默认编辑器（可在配置中修改）
- 可工作的 GPU、DRM、Wayland 或嵌套显示环境

默认配置位于 [`config.yaml`](../config.yaml)。例如修改应用菜单：

```yaml
menu_program: "wofi --show drun"
scriptable_terminal_program: "kitty"
scriptable_editor_program: "vim"
```

默认配置第一次启动时会复制到：

```text
$XDG_CONFIG_HOME/HackMatrix/config.yaml
```

如果未设置 `XDG_CONFIG_HOME`，通常为：

```text
~/.config/HackMatrix/config.yaml
```

## 8. 协议和视频依赖检查

若 CMake 配置失败，可分别检查协议编译器和运行库：

```bash
protoc --version
pkg-config --modversion protobuf
pkg-config --modversion libavcodec libavutil libswscale
pkg-config --modversion gstreamer-1.0 gstreamer-app-1.0 \
  gstreamer-video-1.0 gstreamer-sdp-1.0 gstreamer-webrtc-1.0
```

`protoc` 与 `libprotobuf` 的版本必须匹配。若版本不一致，应安装同一发行版仓库提供的配套包，或者统一使用合适的自编译工具链。

## 9. 运行测试

构建测试目标：

```bash
cmake -S . -B build \
  -DMATRIX_BUILD_TESTS=ON
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

集成测试需要真实会话和额外程序时，可在支持的环境中启用：

```bash
cmake -S . -B build \
  -DMATRIX_ENABLE_INTEGRATION_TESTS=ON
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure -L integration
```

集成测试可能需要从 TTY 或嵌套 Wayland 会话运行，不能在纯无头的 CI 环境中保证可用。

## 10. 常见故障排查

### 找不到 `wlroots-0.19`

```bash
pkg-config --modversion wlroots-0.19
```

失败时：

1. 确认发行版是否提供 wlroots 0.19 开发包。
2. 或使用仓库子模块：`-DMATRIX_USE_SYSTEM_WLROOTS=OFF`。
3. 确保已初始化子模块：`git submodule update --init --recursive`。

### 找不到 FFmpeg 或 GStreamer

检查相应模块：

```bash
pkg-config --modversion libavcodec libavutil libswscale
pkg-config --modversion gstreamer-1.0 gstreamer-app-1.0
```

若失败，安装缺少的 `*-dev` 或 `*-devel` 包，并重新运行 CMake。

### 找不到 Crow

默认情况下，找不到系统 Crow 时不会失败或强制联网下载；CMake 会自动关闭多人 Web GUI。

```bash
cmake -S . -B build -DMATRIX_ENABLE_WEBGUI=OFF
```

若确实需要让 CMake 下载固定版本的 Crow：

```bash
cmake -S . -B build -DMATRIX_FETCH_CROW=ON
```

如果无法访问网络，也可以指定本地源码：

```bash
cmake -S . -B build \
  -DMATRIX_FORCE_FETCH_CROW=ON \
  -DMATRIX_CROW_SOURCE_DIR=/path/to/Crow
```

### `protoc` 与 libprotobuf 不匹配

CMake 会把生成代码工具链的版本与链接库版本进行比较。解决方法包括：

- 从同一发行版仓库安装配套版本。
- 不要混用另一发行版或手动编译出的 protobuf 包。
- 确认 `protoc --version` 和 `pkg-config --modversion protobuf` 的主次版本一致。

### 启动失败或无法打开窗口

检查以下项目：

- 是否提供了有效 Wayland 或 DRM 会话。
- 是否安装了 XWayland。
- 是否执行了 `git submodule update --init --recursive`。
- `LD_LIBRARY_PATH` 是否指向不兼容的 wlroots 庞库。
- GPU 驱动和 EGL/OpenGL ES 环境是否正常。
- 是否误用 `sudo` 启动或使用了错误的 `XDG_RUNTIME_DIR`。

## 11. 总结

Debian 系发行版对本项目不是默认已经完全兼容的发行版。推荐按以下顺序验证：

1. 安装基础依赖和 FFmpeg/GStreamer 包。
2. 检查 `pkg-config --modversion wlroots-0.19`。
3. 系统 wlroots 为 0.19 时优先使用。
4. 否则初始化子模块并使用 `MATRIX_USE_SYSTEM_WLROOTS=OFF` 构建。
5. 在 Wayland、TTY DRM 或嵌套会话中运行，不要使用 `sudo`。
6. 根据实际运行情况运行单元测试与集成测试。
