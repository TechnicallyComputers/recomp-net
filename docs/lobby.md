# Lobby protocol (client-facing)

`recomp-net` is a delay-sync library only — it does **not** include a lobby
server. The open-source control plane lives in the sibling repo
[`recomp-net-server`](https://github.com/TechnicallyComputers/recomp-net-server).

Authoritative wire documentation for the MotK / psxrecomp WebSocket JSON
protocol:

- https://github.com/TechnicallyComputers/recomp-net-server/blob/main/docs/WS_LOBBY.md
- Architecture: https://github.com/TechnicallyComputers/recomp-net-server/blob/main/docs/HOW_IT_WORKS.md

Default client URL: `ws://netplay.technicallycomputers.ca:8765`  
Override with env `PSX_NET_LOBBY_URL` (PSX) or `SNES_NET_LOBBY_URL` (SNES).
Local bring-up: `ws://127.0.0.1:8765` (run `recomp-net-server` yourself).

Host `match_caps` (opaque JSON on create/start) are echoed to guests so
sim-affecting settings stay aligned; see the server `WS_LOBBY.md`.

Lobbies also carry `game_name` + `game_version` (release pin). Create/join
must match; `list` can filter by either. Empty version normalizes to `dev`.

## Summary (for host integrators)

- One WebSocket per player; text frames are JSON objects with an `"op"` field.
- Server assigns `player_id` on connect (`welcome`).
- Hosts `create` lobbies; guests `list` / `join` (optional password).
- Server returns `session_id`, slot map, and rewritten `host_endpoint` /
  `guest_endpoint` for LAN (or ICE `signal` relay).
- After handoff, peers use **this** library (`rnet_session_*`) for INPUT
  exchange. The lobby server is not on the input path.

Open-source clients (e.g. psxrecomp `psx_lobby_client` + vendored
`runtime/src/lobby_ws/`) implement the protocol; they do not embed the server.

## Local LAN room registry

`recomp_net/lan_lobby.h` provides the small, server-independent room registry
used by launchers running multiple instances on one machine. A host publishes
an `RNetLanLobby` beside its configuration, and launchers merge that row with
the remote WebSocket list. Joining claims the guest slot but does not start the
game; both launchers continue showing their shared room until the host calls
`rnet_lan_lobby_set_started()`.

The registry complements the lobby server. Hosts should still publish the same
LAN endpoint remotely so other machines can discover it. It does not carry
input or replace `rnet_session_start_lan()`.
