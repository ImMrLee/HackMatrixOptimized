# Building HackMatrix

## Quick start

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
./build/matrix            # or ./launch for the full session setup
```

Configuration takes well under a second and a full build takes about a minute
on a modern machine once dependencies are installed.

## Dependencies

Everything is found through `pkg-config` and CMake. On Arch:

```bash
sudo pacman -S --needed base-devel cmake git protobuf wayland wayland-protocols \
  libdrm libxkbcommon pixman mesa libglvnd lcms2 systemd-libs seatd \
  libdisplay-info libliftoff libinput libxcb xcb-util-errors xcb-util-wm \
  xcb-util-renderutil assimp fmt spdlog sqlite zeromq ffmpeg \
  gstreamer gst-plugins-base gst-plugins-good gst-plugins-bad gst-libav \
  libnice gtest
```

Window sharing uses GStreamer `webrtcbin`: `gst-plugins-bad` provides
`webrtcbin`/`h264parse`, `gst-plugins-good` provides `rtpbin`/`rtph264pay`,
`gst-libav` provides `avdec_h264`, and `libnice` provides ICE.

`wlroots` is the one dependency that differs by distribution:

- **Preferred:** the system package (`wlroots-0.19`, Arch calls it `wlroots0.19`).
  Its `pkg-config` file is detected automatically.
  Arch's `[extra]` now ships `wlroots0.20`; `wlroots0.19` is AUR-only. Install
  it with `yay -S wlroots0.19` (or `paru -S wlroots0.19`) before building the
  Arch package. `makepkg` cannot resolve AUR dependencies by itself.
- **Fallback:** the bundled `wlroots/` submodule is built with meson/ninja when
  no system package is present. This needs `meson` and `ninja`. The submodule is
  pinned to the same 0.19.x version as the distribution packages so that the
  private headers it provides match the library that gets linked. CMake warns
  when the versions differ (and fails with `MATRIX_STRICT_WLROOTS_VERSION=ON`),
  because the compositor reads private struct fields whose offsets are not ABI
  stable. Note that this fallback applies to direct `cmake` builds; the
  `distributions/arch/PKGBUILD` still declares `wlroots0.19` so the packaged
  binary has a matching runtime library.

Crow (used by the optional multiplayer web GUI) is discovered with
`find_package`. It is never downloaded implicitly.

## Options

| Option | Default | Purpose |
|---|---|---|
| `MATRIX_USE_SYSTEM_WLROOTS` | `ON` | Use the system wlroots when available; `OFF` forces the submodule build |
| `MATRIX_STRICT_WLROOTS_VERSION` | `OFF` | Fail (instead of warn) when the system wlroots version differs from the submodule |
| `MATRIX_BUILD_TESTS` | `ON` | Build the gtest suites |
| `MATRIX_ENABLE_INTEGRATION_TESTS` | `OFF` | Register compositor-spawning tests with CTest |
| `MATRIX_WARNINGS` | `ON` | Add `-Wall -Wextra` to project targets |
| `MATRIX_NATIVE_ARCH` | `OFF` | Add `-march=native` (not portable across CPUs) |
| `MATRIX_SANITIZE` | *(empty)* | Comma-separated sanitizers, e.g. `address,undefined` |
| `MATRIX_ENABLE_WEBGUI` | `ON` | Build the multiplayer web GUI (requires Crow) |
| `MATRIX_FETCH_CROW` | `OFF` | Allow downloading Crow when no system package is found |
| `MATRIX_CROW_SOURCE_DIR` | *(empty)* | Use a local Crow checkout instead of downloading |

`CMAKE_BUILD_TYPE=Debug` now really means `-O0` (previously the hardcoded `-O3`
made Debug builds indistinguishable from Release). Release keeps the previous
behaviour: `-O3 -g` plus CMake's `-DNDEBUG` from `CMAKE_CXX_FLAGS_RELEASE`.

## Sanitizers

```bash
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug \
  -DMATRIX_SANITIZE=address,undefined
cmake --build build-asan -j"$(nproc)"
ctest --test-dir build-asan --output-on-failure   # CTest sets the needed env
```

Two toolchain limitations are handled automatically:

1. **protobuf-generated code is not instrumented.** With ASan enabled on the
   generated `api.pb.cc`, virtual calls from `libprotobuf` jump to a bogus
   address (reproduced with both GCC 16 and Clang). CMake adds
   `-fno-sanitize=…` to that single translation unit; the rest of the code is
   fully instrumented.
2. **container-overflow detection is disabled for tests.** The system
   `libprotobuf` is not instrumented, so it writes into `std::string` capacity
   that ASan has annotated as poisoned, producing false positives. CTest sets
   `ASAN_OPTIONS=detect_container_overflow=0`.

When running the binaries manually, pass the same options:

```bash
ASAN_OPTIONS=detect_container_overflow=0 ./build-asan/matrix_unit_tests
```

## Tests

```bash
# Unit tests (default CTest set)
ctest --test-dir build --output-on-failure

# Integration tests need a live Wayland/DRM session and `foot`
cmake -S . -B build -DMATRIX_ENABLE_INTEGRATION_TESTS=ON
ctest --test-dir build --output-on-failure -L integration
```

See `tests/TESTING.md` for the harness details.

## Debug overlay (ImGui)

The compositor draws a HashMatrix debug window with FPS, a debug log, an entity
editor and multiplayer controls. It is enabled by default.

| Variable | Effect |
|---|---|
| `HACKMATRIX_ENABLE_GUI=0` | Disable the overlay (e.g. for benchmarking or CI) |
| `HACKMATRIX_IMGUI_FLIP=0` | Do not mirror the overlay vertically (default: mirrored for the wlroots FBO) |
| `HACKMATRIX_SCREENSHOT_FRAME=N` | Write `screenshots/<time>.png` on frame N (headless verification) |
| `HACKMATRIX_SCREENSHOT_AFTER_MS=N` | Write a screenshot N ms after the first frame (useful when a client connects later) |
| `HACKMATRIX_IMGUI_MULTIPLAYER_TAB=1` | Force the Multiplayer tab to be selected (UI screenshots/tests) |
| `HACKMATRIX_DEBUG_MULTIPLAYER_CONNECT=host:port` | Auto-connect the Multiplayer panel once (headless UI tests) |
| `HACKMATRIX_DEBUG_FOCUS_NEW_WINDOW=1` | Lock each new top-level window/input (default: only `R` locks a window) |
| `HACKMATRIX_DEBUG_PREVIEW_FIRST_WINDOW=1` | Create a local loopback preview of the first window (headless UI tests) |
| `HACKMATRIX_DEBUG_SHARE_FIRST_WINDOW=1` | Publish the first window once the scene is usable (matching peer or dedicated server, headless tests) |
| `HACKMATRIX_DEBUG_ALLOW_CONTROL_FIRST_WINDOW=1` | Also allow control for the auto-shared window (headless tests) |
| `HACKMATRIX_DEBUG_TAKE_CONTROL_FIRST_REMOTE=1` | Request control of the first available remote window (headless tests) |
| `HACKMATRIX_DEBUG_SEND_KEY_AFTER_CONTROL=1` | Send one `KEY_A` after control is granted (headless tests) |

The overlay is rendered into the same GL context and framebuffer as the scene,
from the wlroots output render callback. Mouse input is forwarded from the
Wayland pointer handlers; when the overlay wants the mouse, clicks are consumed
so they do not also reach game controls or clients. Keyboard text input is not
forwarded to ImGui yet, so text fields are read-only for now.

## Chinese input (fcitx5)

HackMatrix advertises `zwp_text_input_manager_v3`,
`zwp_input_method_manager_v2`, and `zwp_virtual_keyboard_manager_v1`, so fcitx5
can type into native Wayland clients.

1. If fcitx5 already runs on the host, start HackMatrix in a private D-Bus
   session (a second fcitx5 instance on the same bus exits immediately):

   ```bash
   dbus-run-session -- ./launch --headless
   ```

2. Inside that session, start and activate fcitx5 (e.g. from a HackMatrix
   terminal):

   ```bash
   fcitx5 -d
   fcitx5-remote -o
   fcitx5-remote -s pinyin
   ```

3. Client environment:
   - GTK3/GTK4 on Wayland: no extra variable (uses text-input-v3). If the host
     sets `GTK_IM_MODULE=fcitx`, either keep it (needs `fcitx5-gtk`) or set
     `GTK_IM_MODULE=wayland`.
   - Qt 6: `QT_IM_MODULE=wayland` (text-input-v3) or `QT_IM_MODULE=fcitx`.
   - XWayland apps: `XMODIFIERS=@im=fcitx` (fcitx5's XIM addon).

The candidate window is drawn by the ImGui overlay near the pointer. ImGui
panels themselves are not Wayland text-input clients, so Entity Editor /
Multiplayer text fields remain ASCII-only.

Notes:
- Only clients implementing `zwp_text_input_v3` receive IME text.
- `KEY_REPLAY` now also injects keys into the focused client (ASCII keysyms via
  the seat keymap); it is intended for tests/remote control, not a full IME.
- The compositor flushes text-input commits immediately; without that, some
  clients never see the committed string (this was the original bug).

## Tooling

- `compile_commands.json` is generated into the build directory by CMake; a
  gitignored symlink is placed at the repository root for clangd/ccls.
- `.clang-tidy` holds a conservative correctness/performance baseline:

  ```bash
  clang-tidy -p build src/*.cpp
  ```

## Why the build no longer needs the network

Earlier versions ran `FetchContent` for Crow and a meson/ninja build of wlroots
during `cmake` configuration, which required network access and could hang for
tens of minutes. Now:

1. `find_package` is tried first for every dependency.
2. Network access only happens for Crow when `MATRIX_FETCH_CROW=ON`.
3. The wlroots submodule is built only when no system package exists.
