# Test Strategy

## Running

```bash
# Configure + build everything (unit and integration test binaries)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"

# Unit tests (no compositor, no display) - these run in CI
ctest --test-dir build --output-on-failure

# Integration tests opt-in: they spawn the real compositor through ./launch and
# need a Wayland/DRM session, so they are NOT registered with CTest by default.
cmake -S . -B build -DMATRIX_ENABLE_INTEGRATION_TESTS=ON
ctest --test-dir build --output-on-failure -L integration
```

## Targets

| Target | Source | Kind | Notes |
|---|---|---|---|
| `matrix_unit_tests` | `tests/api.cpp`, `chunk.cpp`, `dynamicObject.cpp` | unit | Pure logic + protobuf wire-format contract. 13 cases, <0.1s |
| `matrix_controls_spec_tests` | `controls_spec.cpp` | integration | Spawns compositor, replays keys. Needs `foot` |
| `matrix_wayland_basic_tests` | `wayland_basic.cpp` | integration | Headless compositor boot smoke test |
| `matrix_wayland_menu_spec_tests` | `wayland_menu_spec.cpp` | integration | Menu/terminal scenarios. Needs `foot` |

All of them link `matrix_core`, the real OBJECT library the `matrix` binary is
built from, so tests exercise shipped code rather than a copy of it.

## Harness shape

- **Compositor lifecycle:** tests own it. They truncate `/tmp/matrix-wlroots-*.log`,
  start the compositor via `./launch --headless --in-wm` (or `--drm` on a TTY),
  wait for pid/log readiness, and shut it down through the ZMQ `QUIT` command,
  falling back to `kill -9` to avoid orphans.
- **Binary selection:** CMake passes `MATRIX_TEST_DEFAULT_COMPOSITOR_BIN` (an
  absolute path to the built `matrix`) so tests never depend on a stale checked-in
  binary. `MATRIX_WLROOTS_BIN` still overrides it.
- **Working directory:** must be the repository root; `ctest` sets this via the
  `WORKING_DIRECTORY` test property.
- **Log assertion:** the engine writes `engine: VOXEL_API_BIND=...` to the path in
  `MATRIX_WLROOTS_OUTPUT`. Tests must set that variable; otherwise the log stays
  empty (the engine only writes when it is set).
- **Input simulation:** keyboard events go through the ZMQ `KEY_REPLAY` API.
- **ZMQ endpoints:** `VOXEL_API_ADDR_FOR_TEST` when set, otherwise
  `tcp://127.0.0.1:3345`.

## Known gaps

- `controls_spec` and `wayland_menu_spec` set `MENU_PROGRAM=foot`; `foot` is not
  always installed, so most of their cases fail for environmental reasons rather
  than code regressions. Use a stub program (a script that execs any terminal) or
  install `foot` before running them.
- The integration suites take ~25s per case (compositor start/stop dominates).
