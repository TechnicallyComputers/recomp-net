# Architecture

```text
Host game
  │  sample_local / publish / advance / on_signal
  ▼
RNetSession          (FSM + admission)
  ├── RNetInputRing  (per-slot history, indexed by wire tick)
  ├── RNetProtocol   (HELLO / READY / START / INPUT / DELAY_SYNC)
  └── RNetTransport  (LAN UDP  |  ICE send/recv mux)
         └── RNetIceAgent (optional, libjuice)
```

## Session phases

| Phase | Meaning |
|-------|---------|
| `IDLE` | Created; ICE gathering may still be in progress |
| `LINKING` | Datagram path up; exchanging `HELLO` |
| `READY` | Peers advertising `READY`; waiting for full barrier |
| `RUNNING` | Slot 0 emitted `START` (or remote `START` received); sim advances |

Slot **0** is the sim authority: it sends `START` once every slot has
signaled ready.

## Delay-sync admission

For sim tick `T` and committed delay `D`:

1. Host calls `rnet_session_try_admit(s, T)`.
2. Library samples local input and stores it at **wire tick** `T + D`.
3. Admission succeeds only when every **remote** slot has a valid ring row for
   wire tick `T + D`.
4. On success, `publish(T, by_slot, …)` is invoked with gameplay-indexed
   samples; host runs one sim step, then `rnet_session_advance`.
5. On failure, return `0` and keep calling `rnet_session_pump` (ingress /
   retransmit continue while the sim stalls).

There is no prediction window in v1.

## Transport mux

- `rnet_session_start_lan` binds UDP and targets a peer `host:port`.
- `rnet_session_start_ice` (when `RNET_ENABLE_ICE`) wires transport callbacks
  to the ICE agent; session stays `IDLE` until ICE reaches `COMPLETED`, then
  enters `LINKING` and runs the same bootstrap/admission path over ICE
  datagrams.

## Host-as-relay (LAN hub)

For 3+ seats without the lobby server input relay, the lobby owner calls
`rnet_session_start_lan_hub` (empty peer). Guests call `rnet_session_start_lan`
with the host endpoint. Transport hub role is independent of sim `local_slot`.
The hub learns seats from packet `local_slot` and fans out raw datagrams to
every other known seat (star topology).
