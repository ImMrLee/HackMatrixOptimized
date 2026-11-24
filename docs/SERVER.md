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

- Loads the scene `Entity` + `Positionable` rows into a server-side
  `EntityRegistry` (see `src/server/WorldSimulation.cpp`).
- Ticks the GL-free movement systems at 20 Hz (`applyRotation`,
  `applyTranslations`, damaged transform updates).
- **Owns the world authority**: it advertises `kServerAuthorityID`
  (`0xFFFFFFFF`) on channel 8. Clients forward voxel/entity edits to the server;
  the server applies them, increments the sequence and broadcasts snapshots /
  deltas back. Late joiners receive a voxel + entity snapshot.
- Maintains the runtime voxel overlay (`DynamicObjectSpace`) without GL.
- Owns the player list: assigns a spawn point on connect, sends every existing
  player state to a newcomer, broadcasts `PLAYER_JOIN`/`PLAYER_UPDATE`, and
  broadcasts `PLAYER_LEAVE` (channel 9) on disconnect. Clients destroy the
  corresponding avatar entity on leave.
- Window sharing works without another player online: clients connected to the
  dedicated server may pre-share a window. The server caches `Start`/`Transform`
  /`ControlState` and replays them when a newcomer sends its scene fingerprint;
  receivers still reject shares whose fingerprint does not match.
- Starts the existing ENet relay (`MultiPlayer::Server`), accepts players and
  reports connected/disconnected players and the online count.
- Does **not** link OpenGL, wlroots, Wayland, X11, GStreamer or ImGui.
  `ldd build/matrix-server` should show only libc, sqlite3, enet (header
  implementation), spdlog/fmt and their dependencies.
- `bootServer` and client-hosted sessions still use the old behavior
  (`Server(false)`: first connected client is the authority). Only
  `matrix-server` constructs `Server(true)`.
- Only `Positionable` is loaded today. Model/Light persisters construct GL
  resources, so they are intentionally omitted from the headless registry until
  the visual data is made lazy/optional.
- Persistence: edits are saved to SQLite every 30 s and on SIGTERM/SIGINT.
  `Positionable` rows are written normally; the runtime voxel overlay uses a
  server-only `ServerVoxels` table. On startup both are restored before the
  first snapshot. Use a **server-owned database copy** (for example
  `cp db/matrix.db /srv/hackmatrix/world.db`) so clients cannot overwrite it
  when they persist their local scene.

Player movement is still client-reported (`PLAYER_UPDATE` is validated only for
packet length; the server stores and broadcasts it). Input-based movement
authority, NPCs, admin commands, Model/Light lazy loading and the separate Arch
package remain.

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
