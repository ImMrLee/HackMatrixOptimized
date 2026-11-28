# 构建 HackMatrix

> 本文是 HackMatrix **构建、打包、运行与测试**的唯一说明。`readme.md` 只保留快速开始和本文链接。

## 快速开始

依赖安装完成后：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
./launch
```

在普通现代机器上，配置通常不到一秒；完整构建约需一分钟。产物在 `build/matrix`。

## 依赖

### 构建依赖

- CMake、pkg-config、C/C++ 编译器（`base-devel` / `build-essential`）
- `wayland`、`wayland-protocols`
- **wlroots 0.19**（优先系统包；没有则用仓库 submodule，需要 `meson` + `ninja`）
- wlroots X11 后端 / XWayland 所需 XCB 库
- ZeroMQ（`libzmq`）
- Protocol Buffers（`libprotobuf` + `protoc`，两者版本必须一致）
- `spdlog`、`fmt`
- EGL、OpenGL ES 2
- `assimp`
- `sqlite3`
- `pixman`、`libdrm`、`libxkbcommon`（通常由 wlroots 带出，建议显式安装）

### 窗口共享 / WebRTC 依赖

窗口共享使用 GStreamer 的 `webrtcbin`：

- `gst-plugins-bad`：`webrtcbin`、`h264parse`
- `gst-plugins-good`：`rtpbin`、`rtph264pay`
- `gst-libav`：`avdec_h264`
- `libnice`：ICE 支持
- `ffmpeg`：软件编码/回退路径

### 运行时依赖

- `wofi`：默认应用菜单（可在 `config.yaml` 的 `menu_program` 修改）
- `xorg-xwayland` / `xwayland`：运行 X11 程序时需要
- 一个可用的 Wayland 会话或 DRM 设备；仓库根目录的 `./launch` 会处理

### 可选依赖

- Crow：多人 Web GUI（`MATRIX_ENABLE_WEBGUI`，默认 ON，找不到 Crow 会自动关闭）
- GTest：单元测试（`MATRIX_BUILD_TESTS`，默认 ON）
- `meson` + `ninja`：只在使用 bundled wlroots 时需要

> `pthread` 属于 glibc，无需单独安装。`glm`、ImGui、EnTT、Tracy、ENet 等已随仓库提供。

### 发行版安装命令

> 以下包名以各发行版当前仓库为准。若某个 GStreamer 插件包不存在，安装提供 `webrtcbin` / `h264parse` 的对应插件即可。

#### Arch Linux

```bash
# 先安装 AUR 的 wlroots 0.19（官方 [extra] 现在提供的是 0.20）
yay -S wlroots0.19     # 或 paru -S wlroots0.19

sudo pacman -S --needed base-devel cmake git pkgconf \
  wayland wayland-protocols \
  libxcb xcb-util-errors xcb-util-wm xcb-util-renderutil \
  protobuf zeromq spdlog fmt mesa libglvnd lcms2 systemd-libs seatd \
  assimp sqlite libdrm pixman libxkbcommon libinput libdisplay-info libliftoff \
  ffmpeg gstreamer gst-plugins-base gst-plugins-good gst-plugins-bad \
  gst-libav libnice gtest \
  wofi xorg-xwayland
```

也可以直接用仓库里的 `distributions/arch/PKGBUILD` 打包（见下文“打包（Arch Linux）”）。`makepkg` 本身无法解析 AUR 依赖，所以仍需先装 `wlroots0.19`。

#### Ubuntu / Debian

参考：[在 Debian/Ubuntu 构建](debianBUILDING.md)。

#### Fedora / CentOS

```bash
sudo dnf install cmake git pkgconf-pkg-config @development-tools \
  wayland-devel wayland-protocols-devel wlroots-devel \
  libxcb-devel xcb-util-errors-devel xcb-util-wm-devel xcb-util-renderutil-devel \
  zeromq-devel protobuf-devel protobuf-compiler \
  spdlog-devel fmt-devel mesa-libEGL-devel mesa-libGLES-devel \
  assimp-devel sqlite-devel pixman-devel libdrm-devel libxkbcommon-devel \
  ffmpeg-devel gstreamer1-devel gstreamer1-plugins-base-devel \
  gstreamer1-plugins-good gstreamer1-plugins-bad-free gstreamer1-libav \
  libnice-devel gtest-devel \
  wofi xorg-x11-server-Xwayland
```

> `wlroots-devel` 必须是 0.19；版本不符时用 submodule 方式。

#### Gentoo

```bash
sudo emerge --autounmask-write \
  dev-build/cmake dev-vcs/git dev-util/pkgconf \
  dev-libs/wayland dev-libs/wayland-protocols gui-libs/wlroots \
  x11-libs/libxcb x11-libs/xcb-util-errors x11-libs/xcb-util-wm \
  x11-libs/xcb-util-renderutil net-libs/zeromq dev-libs/protobuf \
  dev-libs/spdlog dev-libs/libfmt media-libs/mesa dev-db/sqlite \
  media-libs/assimp x11-libs/pixman x11-libs/libdrm x11-libs/libxkbcommon \
  media-video/ffmpeg media-libs/gstreamer media-libs/gst-plugins-base \
  media-libs/gst-plugins-good media-libs/gst-plugins-bad media-libs/gst-libav \
  net-libs/libnice dev-cpp/gtest \
  gui-apps/wofi x11-base/xwayland
```

> USE flag 和 mask 需要按自己的系统处理；`gui-libs/wlroots` 必须是 0.19。

## wlroots

默认使用系统 wlroots：

```bash
cmake -S . -B build
```

没有系统 0.19 时，初始化 submodule 并让 CMake 编译它：

```bash
git submodule update --init --recursive
cmake -S . -B build -DMATRIX_USE_SYSTEM_WLROOTS=OFF
```

- 需要 `meson` 和 `ninja`
- submodule 固定为 **0.19.3**
- 系统版本与 submodule 不一致时默认只警告；加 `-DMATRIX_STRICT_WLROOTS_VERSION=ON` 可改为直接报错
- 也可用 `-DMATRIX_FORCE_BUILD_WLROOTS=ON`，即使系统有也强制编译 submodule
- 项目会读取 wlroots 私有结构体字段，这些偏移不保证跨 ABI 稳定，因此不建议混用不同小版本

## Crow（可选，多人 Web GUI）

默认流程：

1. `find_package(Crow)` 找系统包
2. 找不到 → **不会联网下载**，自动关闭 `MATRIX_ENABLE_WEBGUI` 并给出警告
3. 需要联网下载：`-DMATRIX_FETCH_CROW=ON`（固定 revision `c61a26e`）
4. 离线/本地源码：`-DMATRIX_CROW_SOURCE_DIR=/path/to/Crow`
5. 系统已装 Crow 也想强制源码构建：`-DMATRIX_FORCE_FETCH_CROW=ON`

示例：

```bash
# 允许联网下载 Crow
cmake -S . -B build -DMATRIX_FETCH_CROW=ON

# 使用本地 Crow 源码（离线）
cmake -S . -B build \
  -DMATRIX_FORCE_FETCH_CROW=ON \
  -DMATRIX_CROW_SOURCE_DIR=/path/to/Crow

# 不需要 Web GUI
cmake -S . -B build -DMATRIX_ENABLE_WEBGUI=OFF
```

## 配置与构建

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DMATRIX_BUILD_EXAMPLES=ON
cmake --build build -j"$(nproc)"
```

可按需指定 target：

```bash
cmake --build build -j"$(nproc)" --target matrix
cmake --build build -j"$(nproc)" --target matrix-server
cmake --build build -j"$(nproc)" --target matrix_unit_tests
cmake --build build -j"$(nproc)" --target voxel_demo   # 需要 MATRIX_BUILD_EXAMPLES=ON
```

构建产物：

| 产物 | 说明 |
|---|---|
| `build/matrix` | 主程序（合成器 + 3D 桌面） |
| `build/matrix-server` | 无头专用服务器（`MATRIX_BUILD_SERVER=ON`，默认开启） |
| `build/matrix_unit_tests` | 单元测试 |
| `build/voxel_demo` | API 示例（`MATRIX_BUILD_EXAMPLES=ON`） |

## 构建选项

| 选项 | 默认 | 说明 |
|---|---|---|
| `MATRIX_USE_SYSTEM_WLROOTS` | `ON` | 使用系统 wlroots；`OFF` 编译 submodule |
| `MATRIX_FORCE_BUILD_WLROOTS` | `OFF` | 即使系统有也强制编译 submodule |
| `MATRIX_STRICT_WLROOTS_VERSION` | `OFF` | 版本不一致时直接报错 |
| `MATRIX_BUILD_TESTS` | `ON` | 构建单元测试 |
| `MATRIX_ENABLE_INTEGRATION_TESTS` | `OFF` | 注册需要真实会话的集成测试 |
| `MATRIX_WARNINGS` | `ON` | 开启 `-Wall -Wextra` |
| `MATRIX_NATIVE_ARCH` | `OFF` | 加 `-march=native`（不可跨机器分发） |
| `MATRIX_SANITIZE` | 空 | 例如 `address,undefined` |
| `MATRIX_ENABLE_WEBGUI` | `ON` | 多人 Web GUI（找不到 Crow 时自动关闭） |
| `MATRIX_FETCH_CROW` | `OFF` | 允许联网下载 Crow |
| `MATRIX_FORCE_FETCH_CROW` | `OFF` | 系统有 Crow 也强制源码构建 |
| `MATRIX_CROW_SOURCE_DIR` | 空 | 使用本地 Crow 源码 |
| `MATRIX_BUILD_SERVER` | `ON` | 构建独立 `matrix-server` |
| `MATRIX_BUILD_EXAMPLES` | `OFF` | 构建 `voxel_demo` 等 API 示例 |

`CMAKE_BUILD_TYPE=Debug` 使用 `-O0`；Release 为 `-O3 -g`，并带 `-DNDEBUG`。

## 运行

推荐从 tty 或桌面会话里用仓库根目录的脚本启动：

```bash
./launch
```

- 默认使用 `./build/matrix`，可用 `MATRIX_WLROOTS_BIN=/path/to/matrix` 覆盖
- 自动选择 `wayland` / `x11` 后端；没有可用会话时用 `--drm` 或 `--headless`
- 会处理 `XDG_RUNTIME_DIR`、`WLR_BACKENDS`、光标主题等环境
- **不要用 `sudo`**：它需要当前用户的 Wayland / DRM 权限

强制后端：

```bash
./launch --headless     # wlroots headless 后端（TTY/无头验证）
./launch --drm          # 直接作为 DRM 合成器运行
./launch --x11          # 嵌套在 X11 会话里
```

无头日志 / 截图：

```bash
MATRIX_WLROOTS_OUTPUT=/tmp/matrix-controls.log \
HACKMATRIX_SCREENSHOT_AFTER_MS=5000 \
./launch --headless
```

`MATRIX_WLROOTS_OUTPUT` 是控制/调试日志路径（默认 `/tmp/matrix-wlroots-output.log`），不是输出文件；截图仍写到 `screenshots/`。

配置文件默认在 `$XDG_CONFIG_HOME/HackMatrix`（通常是 `~/.config/HackMatrix`），可用 `HACKMATRIX_CONFIG_HOME` 覆盖。

### 专用服务器（Beta）

用一份数据库副本，避免服务器和客户端互相写 `db/matrix.db`：

```bash
cp db/matrix.db /tmp/hackmatrix-server.db
./build/matrix-server --port 1234 --db /tmp/hackmatrix-server.db
```

- `--port`：ENet 客户端使用的 UDP 端口，默认 `1234`
- `--db`：场景数据库，默认 `db/matrix.db`
- 服务器使用 ENet UDP，跨机器联机时需要放行防火墙，例如 `sudo ufw allow 1234/udp`
- Web GUI 端口由 `MULTIPLAYER_WEBGUI_PORT` 控制（`./launch` 默认注入 `7777`；直接运行 `build/matrix` 时回退 `7775`）
- API 绑定由 `VOXEL_API_BIND` 控制（`./launch` 默认 `tcp://*:3345`；直接运行 `build/matrix` 时默认 `tcp://*:4455`）

## 测试

```bash
# 单元测试（默认 CTest 测试集）
ctest --test-dir build --output-on-failure

# 集成测试需要真实的 Wayland/DRM 会话以及 foot
cmake -S . -B build -DMATRIX_ENABLE_INTEGRATION_TESTS=ON
ctest --test-dir build --output-on-failure -L integration
```

也可以直接运行 `./build/matrix_unit_tests`。测试框架细节见 `tests/TESTING.md`。

## Sanitizer

```bash
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug \
  -DMATRIX_SANITIZE=address,undefined
cmake --build build-asan -j"$(nproc)"
ctest --test-dir build-asan --output-on-failure   # CTest 会设置所需环境变量
```

当前构建系统自动处理两项工具链限制：

1. **不对 protobuf 生成代码进行检测。** 若对生成的 `api.pb.cc` 启用 ASan，来自 `libprotobuf` 的虚函数调用会跳转到无效地址（GCC 16 与 Clang 均已复现）。CMake 会只为该翻译单元添加 `-fno-sanitize=…`，其余代码仍完整使用 Sanitizer。
2. **测试中关闭容器溢出检测。** 系统中的 `libprotobuf` 未使用 Sanitizer 编译，因此可能向 ASan 标记为中毒的 `std::string` 容量写入数据，从而产生误报。CTest 会设置 `ASAN_OPTIONS=detect_container_overflow=0`。

手动运行二进制时应传入相同选项：

```bash
ASAN_OPTIONS=detect_container_overflow=0 ./build-asan/matrix_unit_tests
```

## 打包（Arch Linux）

### 准备一个干净的打包目录

```bash
rm -rf /tmp/hm-pkg
mkdir -p /tmp/hm-pkg
cp distributions/arch/PKGBUILD \
   distributions/arch/HackMatrix.desktop \
   distributions/arch/matrix-script \
   /tmp/hm-pkg/
```

### 输出目录

```bash
mkdir -p "$HOME/HackMatrix-arch"

cd /tmp/hm-pkg

HACKMATRIX_LOCAL_SOURCE=<project_dir> \
PKGDEST="$HOME/HackMatrix-arch" \
MAKEFLAGS="-j$(nproc)" \
makepkg -f
```

产物会直接出现在：

```text
~/HackMatrix-arch/hackmatrix-git-<版本>-x86_64.pkg.tar.zst
```

### 安装

```bash
sudo pacman -U "$HOME"/HackMatrix-arch/hackmatrix-git-*.pkg.tar.zst
```

### 专用服务器包

只打包无头服务器，不影响主程序包：

```bash
rm -rf /tmp/hm-pkg-server
mkdir -p /tmp/hm-pkg-server
cp distributions/arch/PKGBUILD-server /tmp/hm-pkg-server/PKGBUILD
cp distributions/arch/matrix-server-script /tmp/hm-pkg-server/

mkdir -p "$HOME/HackMatrix-arch"
cd /tmp/hm-pkg-server
HACKMATRIX_LOCAL_SOURCE=<project_dir> \
PKGDEST="$HOME/HackMatrix-arch" \
MAKEFLAGS="-j$(nproc)" \
makepkg -f
```

产物：

```text
~/HackMatrix-arch/hackmatrix-server-git-<版本>-x86_64.pkg.tar.zst
```

- 包名 `hackmatrix-server-git`，与主程序 `hackmatrix-git` 可以并存；
- 运行时只依赖 `sqlite`、`spdlog`、`fmt`（`ldd build/matrix-server` 可验证）；但 CMake 配置仍会探测桌面依赖，因此 `makedepends` 与主程序基本相同；
- 安装后运行 `hackmatrix-server`：包装脚本会把 seed DB 复制到 `$XDG_DATA_HOME/hackmatrix-server/matrix.db`，服务器只写这份副本，升级包不会覆盖玩家状态；
- 默认端口 1234；`hackmatrix-server --port 7777`，`--db` 仍可覆盖默认数据库。

### 注意点

- 不要用 `sudo makepkg`，用当前用户跑，最后安装才用 `sudo pacman -U`。
- `HACKMATRIX_LOCAL_SOURCE` 必须指向仓库根目录，不是 `build/`。
- 想强制全量重编，加 `-C`：`makepkg -f -C`。
- 先安装 AUR 的 `wlroots0.19`；`makepkg` 本身无法解析 AUR 依赖。
- 构建会重写 `client_libs/python/hackMatrix/protos/api_pb2.py`，如果不想让它出现在 `git status` 里：
  ```bash
  git checkout HEAD -- client_libs/python/hackMatrix/protos/api_pb2.py
  ```
- 打包的是当前工作区，未提交的改动也会进包；版本号来自 `git describe`。
- 包名固定是 `hackmatrix-git`，重复构建会覆盖/升级已安装的同名包。

## 调试覆盖层（ImGui）

合成器会绘制一个名为 HackMatrix 的调试窗口，包含 FPS、调试日志、实体编辑器和多人游戏控件。该覆盖层默认启用。

| 环境变量 | 作用 |
|---|---|
| `HACKMATRIX_ENABLE_GUI=0` | 禁用覆盖层（例如用于基准测试或 CI） |
| `HACKMATRIX_IMGUI_FLIP=0` | 不垂直镜像覆盖层（默认会为 wlroots FBO 镜像） |
| `HACKMATRIX_SCREENSHOT_FRAME=N` | 在第 N 帧将截图写入 `screenshots/<time>.png`（无头验证） |
| `HACKMATRIX_SCREENSHOT_AFTER_MS=N` | 首帧后等待 N 毫秒再截图（客户端稍后连接时很有用） |
| `HACKMATRIX_IMGUI_MULTIPLAYER_TAB=1` | 自动打开左侧菜单并强制选中多人游戏标签页（UI 截图/测试） |
| `HACKMATRIX_IMGUI_MENU_OPEN=1` | 启动时自动打开左侧菜单（UI 截图/测试） |
| `HACKMATRIX_IMGUI_SYSTEM_TAB=1` | 自动打开左侧菜单并选中 System Config（UI 截图/测试） |
| `HACKMATRIX_IMGUI_SCENE_EDITOR=1` | 启动时直接打开 Scene Editor（UI 截图/测试） |
| `HACKMATRIX_IMGUI_SCENE_SELECT_FIRST=1` | 配合上一项，自动选中第一个 Model 实体（UI 截图/测试） |
| `HACKMATRIX_DEBUG_MULTIPLAYER_CONNECT=host:port` | 自动连接多人面板一次（无头 UI 测试） |
| `HACKMATRIX_DEBUG_FOCUS_NEW_WINDOW=1` | 锁定每个新顶层窗口及其输入（默认只有按 `R` 才锁定窗口） |
| `HACKMATRIX_DEBUG_PREVIEW_FIRST_WINDOW=1` | 为第一个窗口创建本地回环预览（无头 UI 测试） |
| `HACKMATRIX_DEBUG_SHARE_FIRST_WINDOW=1` | 场景可用后发布第一个窗口（无头测试） |
| `HACKMATRIX_DEBUG_ALLOW_CONTROL_FIRST_WINDOW=1` | 同时允许控制自动共享的窗口（无头测试） |
| `HACKMATRIX_DEBUG_TAKE_CONTROL_FIRST_REMOTE=1` | 请求控制第一个可用的远程窗口（无头测试） |
| `HACKMATRIX_DEBUG_SEND_POINTER_AFTER_CONTROL=1` | 获得控制权后发送一次指针事件（无头测试） |
| `HACKMATRIX_DEBUG_SEND_KEY_AFTER_CONTROL=1` | 获得控制权后发送一次 `KEY_A`（无头测试） |
| `HACKMATRIX_DEBUG_WINDOW_RESIZE=边,dx,dy` | 在按 `G` 时强制对瞄准窗口做一次缩放（无头测试，例如 `R,300,0`） |
| `HACKMATRIX_DEBUG_CHAT_OPEN=1` | 连接后自动打开聊天栏（UI 截图/测试） |
| `HACKMATRIX_DEBUG_CHAT_SEND=文本` | 连接后自动发送一条聊天消息（中继测试） |
| `HACKMATRIX_DEBUG_HOST_SERVER=1` | 自动切到 Server 模式并主持服务器（UI 截图/测试） |
| `HACKMATRIX_DEBUG_EXIT_CONFIRM=1` | 自动打开 Esc 退出确认框（UI 截图/测试） |
| `HACKMATRIX_DEBUG_EXIT_FLASH=1` | 打开确认框时模拟同帧 Esc 输入（回归测试用） |

覆盖层会与场景共用同一个 GL 上下文和帧缓冲，并从 wlroots 输出渲染回调中绘制。鼠标输入通过 Wayland 指针处理器转发；当覆盖层需要鼠标时，点击会被消耗，不会继续传递给游戏控制或客户端。键盘输入通过 `Engine::updateImGuiKey` 转发，实体编辑器的文本输入框可用；但覆盖层不是 Wayland 文本输入客户端，文本字段目前只支持 ASCII（中文输入见下一节）。

## 中文输入（fcitx5）

HackMatrix 宣告支持以下协议：

- `zwp_text_input_manager_v3`
- `zwp_input_method_manager_v2`
- `zwp_virtual_keyboard_manager_v1`

因此 fcitx5 可以向原生 Wayland 客户端输入文字。

1. 如果 fcitx5 已在宿主机运行，应在私有 D-Bus 会话中启动 HackMatrix；在同一 D-Bus 上启动第二个 fcitx5 实例会立即退出：

   ```bash
   dbus-run-session -- ./launch --headless
   ```

2. 在该会话中启动并激活 fcitx5，例如从 HackMatrix 终端运行：

   ```bash
   fcitx5 -d
   fcitx5-remote -o
   fcitx5-remote -s pinyin
   ```

3. 客户端环境：
   - GTK3/GTK4 Wayland：无需额外变量（使用 text-input-v3）。如果宿主机设置了 `GTK_IM_MODULE=fcitx`，可以保留该变量（需要 `fcitx5-gtk`），也可以改为 `GTK_IM_MODULE=wayland`。
   - Qt 6：使用 `QT_IM_MODULE=wayland`（text-input-v3）或 `QT_IM_MODULE=fcitx`。
   - XWayland 应用：使用 `XMODIFIERS=@im=fcitx`（fcitx5 的 XIM 附加组件）。

候选词窗口由 ImGui 覆盖层绘制在指针附近。ImGui 面板本身不是 Wayland 文本输入客户端，因此实体编辑器和多人游戏中的文本字段仍只允许 ASCII 字符。

注意：

- 只有实现了 `zwp_text_input_v3` 的客户端才能接收 IME 文本。
- `KEY_REPLAY` 现在也会通过 seat keymap 把按键注入到聚焦的客户端（由 ASCII keysym 组成）；该功能用于测试或远程控制，并不是完整 IME。
- 合成器会立即刷新文本输入提交；否则某些客户端可能永远无法看到提交的字符串（这是最初的 bug）。

## 工具

- CMake 会把 `compile_commands.json` 生成到构建目录，并在仓库根目录放置一个被 Git 忽略的符号链接，供 clangd/ccls 使用。
- `.clang-tidy` 保存了一套保守的正确性/性能检查基线：

  ```bash
  clang-tidy -p build src/*.cpp
  ```

## 为什么构建不再需要网络

早期版本在 CMake 配置阶段通过 `FetchContent` 获取 Crow，并通过 Meson/Ninja 构建 wlroots，因此需要网络访问，并可能挂起数十分钟。现在的行为如下：

1. 每个依赖都首先尝试使用 `find_package` 查找。
2. 只有在设置 `MATRIX_FETCH_CROW=ON` 时才会访问网络下载 Crow。
3. 只有找不到系统 wlroots 软件包时才构建 wlroots 子模块。

## 故障排查

| 现象 | 处理 |
|---|---|
| 找不到 / 版本不符的 wlroots | 安装 0.19（Arch 用 AUR `wlroots0.19`），或 `-DMATRIX_USE_SYSTEM_WLROOTS=OFF` 编译 submodule |
| `protoc` 与 `libprotobuf` 版本不一致 | 两者必须来自同一版本；按报错安装匹配的 `protobuf` / `protobuf-compiler` |
| Web GUI 被自动关闭 | 安装 Crow，或 `-DMATRIX_FETCH_CROW=ON`，或 `-DMATRIX_CROW_SOURCE_DIR=...` |
| 启动报 Wayland / DRM 权限错误 | 不要用 `sudo`；确认在 Wayland 会话或 tty 中，且当前用户可访问 `/dev/dri/*` |
| 多人连不上 | 检查 UDP 端口（默认 1234）和防火墙，例如 `sudo ufw allow 1234/udp` |
| 服务器和客户端互相写库 | 给 `matrix-server` 使用独立的 `--db` 副本 |
