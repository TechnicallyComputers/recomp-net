# Host integration

## Required loop

```c
while (running) {
    rnet_session_pump(session);   /* recv, ICE poll, bootstrap, INPUT send */

    if (!rnet_session_is_running(session)) {
        /* still linking / waiting for START */
        continue;
    }

    uint32_t t = rnet_session_sim_tick(session);
    if (rnet_session_try_admit(session, t)) {
        /* Apply published pads for tick t, then step the authoritative sim once. */
        host_sim_step(t);
        rnet_session_advance(session);
    }
    /* else: stall — do not advance local sim */
}
```

**Rule:** only one authoritative sim tick may advance after a successful
`try_admit`. Do not sample pads for tick `T+1` until `advance` has run.

`try_admit` **latches** a fresh local pad at wire `T+D`, resolves gameplay
from wire `T`, and `publish`es when remotes for wire `T` are present.
`INPUT_CONFIRM` is async (desync flag via `rnet_session_input_desync`); it
does not block admit. Remote INPUT frames are first-wins.

On host shutdown call `rnet_session_send_bye` before destroy so the peer can
exit immediately. While waiting on admit, poll
`rnet_session_peer_disconnected(session, 1500)` (~1.5s silence or peer BYE)
and leave the session instead of spinning forever.

## Host vtable

| Callback | Role |
|----------|------|
| `sample_local` | Fill opaque pad bytes for the current sim tick (called inside `try_admit`) |
| `publish` | Receive resolved inputs for all slots; apply before sim step |
| `now_ms` | Optional; defaults to platform monotonic ms |
| `on_signal` | ICE SDP/candidates toward your lobby (LAN-only may leave NULL) |

## N64 / PSX recomp notes

- Hook pad read so the runtime **does not** inject local-only input into the
  shared sim; use `publish` as the sole source of pads for locked ticks.
- Keep RNG, timers, and VI/frame pacing deterministic across peers; the library
  does not fix host desyncs.
- Prefer a single thread that owns both `pump` and sim advance, or protect the
  session with an external mutex (API is not internally locked).

## Config

`RNetConfig` fields (`slot_count`, `local_slot`, `input_delay`,
`bundle_redundancy`, `session_id`, `protocol_magic`) must match across peers
except `local_slot`. Negotiate them out-of-band (lobby) before `create`.

## Savestate / SRAM transfer (optional)

Host slot 0 may call `rnet_session_state_begin(op, slot, blob, size)` while
`RUNNING` (`op`: save / load / SRAM). Load and SRAM stall `try_admit` until the
guest ACKs; save does not. Chunks use a sliding window with contiguous ACK.
When `rnet_session_state_take_ready` returns 1, the guest (and host, if it has
not already applied) should store/apply the blob, then
`rnet_session_state_finish(s, hard_resync)` — use `hard_resync=1` on load.
