# 构建 HackMatrix

## 快速开始

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
./build/matrix            # 完整会话环境也可运行 ./launch
```

依赖安装完成后，在普通现代机器上，配置通常不到一秒即可完成；完整构建约需一分钟。

## 依赖

所有依赖都通过 `pkg-config` 和 CMake 查找。在 Arch Linux 上可以运行：

```bash
sudo pacman -S --needed base-devel cmake git protobuf wayland wayland-protocols \
  libdrm libxkbcommon pixman mesa libglvnd lcms2 systemd-libs seatd \
  libdisplay-info libliftoff libinput libxcb xcb-util-errors xcb-util-wm \
  xcb-util-renderutil assimp fmt spdlog sqlite zeromq ffmpeg \
  gstreamer gst-plugins-base gst-plugins-good gst-plugins-bad gst-libav \
  libnice gtest
```

窗口共享使用 GStreamer 的 `webrtcbin`：

- `gst-plugins-bad` 提供 `webrtcbin` 和 `h264parse`
- `gst-plugins-good` 提供 `rtpbin` 和 `rtph264pay`
- `gst-libav` 提供 `avdec_h264`
- `libnice` 提供 ICE 支持

不同发行版之间最主要的差异是 `wlroots`：

- **首选：**系统软件包（`wlroots-0.19`；Arch Linux 软件包名为 `wlroots0.19`）。CMake 会自动检测其 `pkg-config` 文件。
  - Arch 官方仓库的 `[extra]` 现在提供 `wlroots0.20`，而 `wlroots0.19` 只能通过 AUR 安装。构建 Arch 软件包前，请使用 `yay -S wlroots0.19` 或 `paru -S wlroots0.19` 安装。
  - `makepkg` 本身无法解析 AUR 依赖。
- **后备方案：**没有系统软件包时，CMake 会使用 Meson/Ninja 构建仓库自带的 `wlroots/` 子模块。此方案需要安装 `meson` 和 `ninja`。
  - 子模块固定在与发行版软件包相同的 0.19.x 系列，以确保私有头文件和实际链接的库一致。
  - 当版本不一致时，CMake 会发出警告；设置 `MATRIX_STRICT_WLROOTS_VERSION=ON` 后会直接失败。项目会读取私有结构体字段，而这些字段偏移不保证跨 ABI 稳定。
  - 此后备方案适用于直接使用 CMake 构建；`distributions/arch/PKGBUILD`仍要求 `wlroots0.19`，以确保打包后的可执行文件使用同版本运行库。

Crow（可选的多人 Web GUI 使用）通过 `find_package` 查找，项目不会隐式下载它。

## 构建选项

| 选项 | 默认值 | 用途 |
|---|---|---|
| `MATRIX_USE_SYSTEM_WLROOTS` | `ON` | 系统中存在 wlroots 时优先使用；设为 `OFF` 时强制构建子模块 |
| `MATRIX_STRICT_WLROOTS_VERSION` | `OFF` | 系统 wlroots 与子模块版本不一致时失败，而不是只发出警告 |
| `MATRIX_BUILD_TESTS` | `ON` | 构建 GTest 测试套件 |
| `MATRIX_ENABLE_INTEGRATION_TESTS` | `OFF` | 向 CTest 注册需要启动合成器的集成测试 |
| `MATRIX_WARNINGS` | `ON` | 为项目目标添加 `-Wall -Wextra` |
| `MATRIX_NATIVE_ARCH` | `OFF` | 添加 `-march=native`（不能跨 CPU 架构通用） |
| `MATRIX_SANITIZE` | （空） | 逗号分隔的 Sanitizer，例如 `address,undefined` |
| `MATRIX_ENABLE_WEBGUI` | `ON` | 构建多人 Web GUI（需要 Crow） |
| `MATRIX_FETCH_CROW` | `OFF` | 找不到系统软件包时允许下载 Crow |
| `MATRIX_CROW_SOURCE_DIR` | （空） | 使用本地 Crow 检出目录，而不下载 |

现在的 `CMAKE_BUILD_TYPE=Debug` 确实会使用 `-O0`；此前硬编码的 `-O3` 会导致 Debug 与 Release 实际上没有区别。Release 保持原有行为，即 `-O3 -g`，并加上 CMake 在 `CMAKE_CXX_FLAGS_RELEASE` 中生成的 `-DNDEBUG`。

## Sanitizer

```bash
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug \
  -DMATRIX_SANITIZE=address,undefined
cmake --build build-asan -j"$(nproc)"
ctest --test-dir build-asan --output-on-failure   # CTest 会设置所需环境变量
```

当前构建系统自动处理两项工具链限制：

1. **不对 protobuf 生成代码进行检测。**若对生成的 `api.pb.cc` 启用 ASan，来自 `libprotobuf` 的虚函数调用会跳转到无效地址（GCC 16 与 Clang 均已复现）。CMake 会只为该翻译单元添加 `-fno-sanitize=…`，其余代码仍完整使用 Sanitizer。
2. **测试中关闭容器溢出检测。**系统中的 `libprotobuf` 未使用 Sanitizer 编译，因此可能向 ASan 标记为中毒的 `std::string` 容量写入数据，从而产生误报。CTest 会设置 `ASAN_OPTIONS=detect_container_overflow=0`。

手动运行二进制时应传入相同选项：

```bash
ASAN_OPTIONS=detect_container_overflow=0 ./build-asan/matrix_unit_tests
```

## 测试

```bash
# 单元测试（默认 CTest 测试集）
ctest --test-dir build --output-on-failure

# 集成测试需要真实的 Wayland/DRM 会话以及 foot
cmake -S . -B build -DMATRIX_ENABLE_INTEGRATION_TESTS=ON
ctest --test-dir build --output-on-failure -L integration
```

测试框架的详细信息请参阅 `tests/TESTING.md`。

## 调试覆盖层（ImGui）

合成器会绘制一个名为 HackMatrix 的调试窗口，其中包含 FPS、调试日志、实体编辑器和多人游戏控件。该覆盖层默认启用。

| 环境变量 | 作用 |
|---|---|
| `HACKMATRIX_ENABLE_GUI=0` | 禁用覆盖层（例如用于基准测试或 CI） |
| `HACKMATRIX_IMGUI_FLIP=0` | 不垂直镜像覆盖层（默认会为 wlroots FBO 镜像） |
| `HACKMATRIX_SCREENSHOT_FRAME=N` | 在第 N 帧将截图写入 `screenshots/<time>.png`（无头验证） |
| `HACKMATRIX_SCREENSHOT_AFTER_MS=N` | 首帧后等待 N 毫秒再截图（客户端稍后连接时很有用） |
| `HACKMATRIX_IMGUI_MULTIPLAYER_TAB=1` | 强制选中多人游戏标签页（UI 截图/测试） |
| `HACKMATRIX_DEBUG_MULTIPLAYER_CONNECT=host:port` | 自动连接多人面板一次（无头 UI 测试） |
| `HACKMATRIX_DEBUG_FOCUS_NEW_WINDOW=1` | 锁定每个新顶层窗口及其输入（默认只有按 `R` 才锁定窗口） |
| `HACKMATRIX_DEBUG_PREVIEW_FIRST_WINDOW=1` | 为第一个窗口创建本地回环预览（无头 UI 测试） |
| `HACKMATRIX_DEBUG_SHARE_FIRST_WINDOW=1` | 场景可用后发布第一个窗口（用于匹配的对等端或专用服务器，无头测试） |
| `HACKMATRIX_DEBUG_ALLOW_CONTROL_FIRST_WINDOW=1` | 同时允许控制自动共享的窗口（无头测试） |
| `HACKMATRIX_DEBUG_TAKE_CONTROL_FIRST_REMOTE=1` | 请求控制第一个可用的远程窗口（无头测试） |
| `HACKMATRIX_DEBUG_SEND_KEY_AFTER_CONTROL=1` | 获得控制权后发送一次 `KEY_A`（无头测试） |

覆盖层会与场景共用同一个 GL 上下文和帧缓冲，并从 wlroots 输出渲染回调中绘制。鼠标输入通过 Wayland 指针处理器转发；当覆盖层需要鼠标时，点击会被消耗，不会继续传递给游戏控制或客户端。键盘文本输入暂未转发给 ImGui，因此文本框目前只读。

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
