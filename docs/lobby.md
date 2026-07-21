# Lobby protocol (client-facing)

`recomp-net` is a delay-sync library only — it does **not** include a lobby
server. The proprietary control plane lives in the sibling private repo
**`recomp-net-server`**.

Authoritative wire documentation for the MotK / psxrecomp WebSocket JSON
protocol:

- `recomp-net-server/docs/WS_LOBBY.md`
- Architecture: `recomp-net-server/docs/HOW_IT_WORKS.md`

Default client URL: `ws://netplay.technicallycomputers.ca:8765`  
Override with env `PSX_NET_LOBBY_URL` (PSX) or `SNES_NET_LOBBY_URL` (SNES).
Local bring-up: `ws://127.0.0.1:8765`.

Host `match_caps` (opaque JSON on create/start) are echoed to guests so
sim-affecting settings stay aligned; see `recomp-net-server/docs/WS_LOBBY.md`.

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
