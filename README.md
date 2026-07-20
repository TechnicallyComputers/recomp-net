# recomp-net

Portable **delay-sync** netcode library for recompilation / modern-runtime hosts
(N64Recomp, PSX recomp, and similar). Classic lockstep: every peer stalls until
remote inputs for gameplay wire `sim` arrive (fresh samples ride at `sim + D`).
Optional **ICE** transport via
[libjuice](https://github.com/paullouisageneau/libjuice).

BattleShip’s netplay stack is a **design reference only** — this repo does not
vendor SSB64 code and does not implement rollback, automatch, or game UI.

## Features (v0.1)

- Opaque per-slot input blobs (`RNetInputSample`)
- Fixed input delay `D` with `try_admit` / `advance` host loop
- UDP LAN transport and optional ICE mux
- Host-owned signaling callbacks (ICE); lobby / matchmaking is **not** in this
  repo — see [`docs/lobby.md`](docs/lobby.md) and the private `recomp-net-server`
- C11, CMake, MIT license

## Lobby

This library has no lobby binary. The MotK / psxrecomp WebSocket lobby server is
the closed-source sibling project **`recomp-net-server`** (default
`ws://127.0.0.1:8765`). Client-facing protocol notes:
[`docs/lobby.md`](docs/lobby.md).

## Build (LAN, no ICE)

```bash
cmake -S . -B build -DRNET_ENABLE_ICE=OFF
cmake --build build -j
ctest --test-dir build --output-on-failure
```

## Build with ICE

Optional ICE transport uses [libjuice](https://github.com/paullouisageneau/libjuice).
CMake looks for a system install, `RNET_LIBJUICE_ROOT`, or `third_party/libjuice`;
if none is present it **FetchContent**-clones libjuice automatically (network required
at configure time).

```bash
cmake -S . -B build-ice -DRNET_ENABLE_ICE=ON
cmake --build build-ice -j
```

ICE needs juice at link time — without a local copy or FetchContent access,
`-DRNET_ENABLE_ICE=ON` will fail configure. Host apps supply signaling via
`RNetHostVTable.on_signal` / `rnet_session_push_signal` (no automatch server).

## Quick LAN demo

```bash
# terminal A (slot 0 / sim authority)
./build/lan_delay_2p 0 7777 127.0.0.1:7778

# terminal B (slot 1)
./build/lan_delay_2p 1 7778 127.0.0.1:7777
```

## Integrate

```cmake
add_subdirectory(path/to/recomp-net)
target_link_libraries(your_host PRIVATE recomp_net)
```

```c
#include "recomp_net/recomp_net.h"

/* Implement RNetHostVTable: sample_local, publish, optional on_signal/now_ms */
RNetSession *s = rnet_session_create(&cfg, &host);
rnet_session_start_lan(s, "0.0.0.0:7777", "127.0.0.1:7778");
for (;;) {
    rnet_session_pump(s);
    if (rnet_session_try_admit(s, rnet_session_sim_tick(s))) {
        /* run one authoritative sim step */
        rnet_session_advance(s);
    }
}
```

See [docs/host_integration.md](docs/host_integration.md).

## Docs

| Doc | Topic |
|-----|--------|
| [docs/architecture.md](docs/architecture.md) | Layers, phases, admission |
| [docs/protocol.md](docs/protocol.md) | Wire packets |
| [docs/signaling.md](docs/signaling.md) | ICE signaling contract |
| [docs/host_integration.md](docs/host_integration.md) | Hooking a recomp host |

## Non-goals (v1)

- Rollback / prediction / state hashes
- Automatch or matchmaking HTTP clients
- Game-specific pad layouts or determinism fixes (host responsibility)
