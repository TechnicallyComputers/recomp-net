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
