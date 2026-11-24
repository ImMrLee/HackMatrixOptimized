# Dedicated server (experimental)

`matrix-server` is the first headless target of the server split. It is
deliberately small: it validates that the scene database and networking can run
without a compositor, GPU, or ImGui.

## Build and run

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)" --target matrix-server
./build/matrix-server --port 1234 --db db/matrix.db
```

Options:

```text
--port N    UDP port for ENet clients (default 1234)
--db PATH   scene database (default db/matrix.db)
```

## What it does today

- Opens the scene database read-only and prints table counts.
- Starts the existing ENet relay (`MultiPlayer::Server`).
- Accepts players, reports connected/disconnected players and the online count.
- Does **not** link OpenGL, wlroots, Wayland, X11, GStreamer, ImGui or
  GStreamer. `ldd build/matrix-server` should show only libc, sqlite3, enet
  headers are compiled in, spdlog/fmt and their dependencies.
- Does **not** simulate the world yet: the first connected client is still the
  F14a world authority, exactly like `bootServer`.

## Why this shape

The client contains a lot of GL/Wayland code, but the world model and protocols
are shared. Keeping `matrix-server` in the same repository with its own target
lets both sides use the same headers and database schema without a second
source tree. Once the simulation core is extracted, this target will link a
`matrix_core_server` library instead of the full client.

## Next steps

1. Extract GL-free world/entity/systems code into a `matrix_core_server`
   library. `World` currently assumes a `Renderer` and creates a demo voxel
   block in its constructor; those calls need to become optional.
2. Add a server-owned registry/simulation loop:
   - load GL-free components (Positionable, Door/Key/Lock, Parent, Scriptable,
     Bootable) without constructing GL-backed `Model`/`Light` objects;
   - tick players, voxels and entity transforms on the server;
   - send snapshots/deltas over channel 8 with the server as sender.
3. Remove the client-side authority election; the server is always authority.
4. Make the server the only writer of `matrix.db`; clients load a read-only
   copy or receive the state from the server.
5. Add systemd/Docker packaging and a separate Arch package
   (`hackmatrix-server`), with config/capability/protocol-version handshake.

Window sharing stays client-to-client (WebRTC); the server only relays
signaling, window metadata and control authorization. A TURN relay is optional
for cross-NAT deployments.
