# HackMatrix-优化版

<img src="images/header_img.png" width="800">

一个3D桌面环境 (也是一个游戏引擎！！！)

<a href="#">演示视频正在准备哦</a>

## 使用方式

### 在3D环境自由活动（就像在Minecraft）
使用鼠标转动鼠标

<img src="vids/lookAround.gif" width="420">

使用WASD移动 <br>
<img src="images/wasd.webp" width="100">

<img src="vids/move.gif" width="420">

### 打开窗口

HackMatrix 使用 `wofi`.

`d` 原本用于移动, 所以你得按下 `v` 来打开启动器

输入程序名然后按下 `<enter>` 启动应用程序

窗口将会在你面朝的方向启动

按下 `r` 来 [focus](#manually-focus-window) 在窗口上

<img src="vids/openWindow.gif" width="420">

### 手动锁定视角到窗口
看向窗口然后按下 `r`

提示: 该仓库修复了偶尔按下r不能锁定视角的bug

如果还是不行，你就可以试试按下Super + 1/2/3...来锁定视角


<img src="vids/focus.gif" width="420">

### 退出锁定
视角锁定到特定窗口时按下 `Super+e`

<img src="vids/exitWindow.gif" width="420">

### 关闭窗口
视角锁定到特定窗口然后按下 `Super+q`

### 快捷键

为了便于使用和调试，这个桌面环境有很多特殊快捷键

`Super+<num>` 来定位窗口

<img src="vids/hotkey.gif" width="420">

### 退出 HackMatrix
在自由视角下按下 `<esc>`

(存在一个bug，大致是不退出窗口时退出hackmartix后窗口对应的应用程序还在运行，在明确修复这个问题前先关闭所有窗口再关闭HackMartix)

### 屏幕截图

按下 `p` 可以把屏幕截图保存到 `<project_dir>/screenshots`
（我还在确定这个project_dir是啥）

### Hackmatrix 菜单
HackMatrix主界面的顶部有一个菜单.

你可以用这个菜单查看修改增删模型 (HackMatrix的游戏引擎哲学喵~)

当处于自由视角时，按下 `f` 呼出鼠标（就像二游按下Alt呼出鼠标）

点击菜单栏左侧的箭头打开/收起菜单

找到 Entity Editor

See the [wiki page](https://github.com/collinalexbell/HackMatrix/wiki/Game-Engine) for more info about the game engine and how to use the editor.

这个菜单还有Multyplayer模式，功能大致是可以让其他使用hackmartix的用户可以加入其他人的环境（有点意思，原作者没有说明这个功能，但我发现这个只是能相互看到各自的头像，不能多窗口互联）
## 编译 / 安装

### 依赖

**构建依赖**

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

**运行时依赖**（构建时不需要）

- `wofi`：默认应用菜单（可在 `config.yaml` 的 `menu_program` 改）
- `xorg-xwayland` / `xwayland`：运行 X11 程序时需要
- 一个可用的 Wayland 会话或 DRM 设备；仓库根目录的 `./launch` 会处理

**可选**

- Crow：多人 Web GUI（`MATRIX_ENABLE_WEBGUI`，默认 ON，但找不到 Crow 会自动关闭）
- GTest：单元测试（`MATRIX_BUILD_TESTS`，默认 ON）
- `meson` + `ninja`：只在使用 bundled wlroots 时需要

> `pthread` 属于 glibc，无需单独安装。`glm`、ImGui、EnTT、Tracy、ENet 等已随仓库提供。

#### Arch Linux

```bash
sudo pacman -S --needed base-devel cmake git pkgconf \
  wayland wayland-protocols wlroots0.19 \
  libxcb xcb-util-errors xcb-util-wm xcb-util-renderutil \
  protobuf zeromq spdlog fmt mesa assimp sqlite \
  libdrm pixman libxkbcommon \
  wofi xorg-xwayland
```

也可以直接用 AUR 包，或仓库里的 `distributions/arch/PKGBUILD`：

```bash
yay -S hackmatrix-git
```

#### Ubuntu / Debian

```bash
sudo apt-get install cmake pkg-config build-essential git \
  libwayland-dev wayland-protocols libwlroots-dev \
  libxcb1-dev libxcb-composite0-dev libxcb-dri3-dev libxcb-ewmh-dev \
  libxcb-errors-dev libxcb-icccm4-dev libxcb-present-dev libxcb-render0-dev \
  libxcb-render-util0-dev libxcb-res0-dev libxcb-shm0-dev libxcb-xfixes0-dev \
  libxcb-xinput-dev libzmq3-dev libprotobuf-dev protobuf-compiler \
  libspdlog-dev libfmt-dev libegl-dev libgles2-mesa-dev libassimp-dev \
  libsqlite3-dev libpixman-1-dev libdrm-dev libxkbcommon-dev \
  wofi xwayland
```

> `libwlroots-dev` 必须是 **0.19**。发行版只有旧版本时，改用下面的 submodule 方式。

#### Fedora / CentOS

```bash
sudo dnf install cmake git pkgconf-pkg-config @development-tools \
  wayland-devel wayland-protocols-devel wlroots-devel \
  libxcb-devel xcb-util-errors-devel xcb-util-wm-devel xcb-util-renderutil-devel \
  zeromq-devel protobuf-devel protobuf-compiler \
  spdlog-devel fmt-devel mesa-libEGL-devel mesa-libGLES-devel \
  assimp-devel sqlite-devel pixman-devel libdrm-devel libxkbcommon-devel \
  wofi xorg-x11-server-Xwayland
```

> `wlroots-devel` 需要 0.19；版本不符时用 submodule 方式。

#### Gentoo

```bash
sudo emerge --autounmask-write \
  dev-build/cmake dev-vcs/git dev-util/pkgconf \
  dev-libs/wayland dev-libs/wayland-protocols gui-libs/wlroots \
  x11-libs/libxcb x11-libs/xcb-util-errors x11-libs/xcb-util-wm \
  x11-libs/xcb-util-renderutil net-libs/zeromq dev-libs/protobuf \
  dev-libs/spdlog dev-libs/libfmt media-libs/mesa dev-db/sqlite \
  media-libs/assimp x11-libs/pixman x11-libs/libdrm x11-libs/libxkbcommon \
  gui-apps/wofi x11-base/xwayland
```

> USE flag 和 mask 需要按自己的系统处理；`gui-libs/wlroots` 必须是 0.19。

---

### wlroots

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

---

### Crow（可选，多人 Web GUI）

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

---

## 构建

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
```

产物是 `build/matrix`。推荐从 tty 或桌面会话里用仓库根目录的脚本启动：

```bash
./launch
```

不要用 `sudo` 运行；它需要当前用户的 Wayland / DRM 权限。

## Arch系编译为tar.zst

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
~/HackMatrix-arch/hackmatrix-git-<版本>-x86_64.pkg.tar.zst

#### 安装
```bash
sudo pacman -U "$HOME"/HackMatrix-arch/hackmatrix-git-*.pkg.tar.zst
```

几个注意点
- 不要用 sudo makepkg，用当前用户跑，最后安装才用 sudo pacman -U。
- HACKMATRIX_LOCAL_SOURCE 必须指向仓库根目录，不是 build/。
- 想强制全量重编，加 -C：
makepkg -f -C
- 构建会重写 client_libs/python/hackMatrix/protos/api_pb2.py，如果不想让它出现在 git status 里：
git checkout HEAD -- client_libs/python/hackMatrix/protos/api_pb2.py
- 打包的是当前工作区，未提交的改动也会进包；版本号来自 git describe。
- 包名固定是 hackmatrix-git，重复构建会覆盖/升级已安装的同名包。

### 常用选项

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
| `MATRIX_ENABLE_WEBGUI` | `ON` | 找不到 Crow 时自动关闭 |
| `MATRIX_FETCH_CROW` | `OFF` | 允许联网下载 Crow |
| `MATRIX_CROW_SOURCE_DIR` | 空 | 使用本地 Crow 源码 |
| `MATRIX_BUILD_EXAMPLES` | `OFF` | 构建 `voxel_demo` 等 API 示例 |

### 测试（可选）

```bash
ctest --test-dir build --output-on-failure
```

集成测试需要真实 Wayland/DRM 会话和 `foot`：

```bash
cmake -S . -B build -DMATRIX_ENABLE_INTEGRATION_TESTS=ON
ctest --test-dir build --output-on-failure -L integration
```

### Sanitizer（可选）

```bash
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug \
  -DMATRIX_SANITIZE=address,undefined
cmake --build build-asan -j"$(nproc)"
ASAN_OPTIONS=detect_container_overflow=0 ctest --test-dir build-asan --output-on-failure
```

---

## 运行
建议在tty环境运行，可执行文件在项目的根目录
运行 `./launch` 
如果你在AUR安装了HarkMartix，可以直接执行hackmartix运行（但是我还没有AUR账户，所以晚些才能上传到AUR）

```bash
hackmatrix
```

### 如何让 client_libraries 正常工作
执行 `scripts/install-python-clientlib.sh` 以手动安装

#### Python
```bash
# Install python hackMatrix lib
python -m venv hackmatrix_python
cd hackmatrix_python
source bin/activate
cd client_libs/python
pip install .
cd ../..

# 测试
python scripts/player-move.py
```
但代码大量改动，这个不一定好用了
