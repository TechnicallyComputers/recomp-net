# Rollback mode (feat/rollback)

This branch extends recomp-net beyond delay-sync into shared rollback
architecture. Delay-sync `RNetSession` remains the shipped v0.1 path; rollback
lands here in layers so hosts opt in without breaking MotK / snes / psx titles.

## Layers

| Layer | Status | Location |
|-------|--------|----------|
| Portable input contract | Landed | [`include/recomp_net/input_contract.h`](../include/recomp_net/input_contract.h), [`src/input/rnet_input_contract.c`](../src/input/rnet_input_contract.c) |
| Rollback episode orchestration | Planned | `include/recomp_net/rollback.h` + `src/rollback/` |
| Transport/protocol extensions | Planned | new opcodes beside existing INPUT/CONFIRM |

## Portable input contract

Pure decision core (no engine includes) for "published row vs late authoritative
row → rewind or promote?". All game-specific behavior enters through
`RNetInputContractParams` (numeric thresholds) and `RNetInputContractHostGates`
(optional host callbacks queried lazily in decision order).

Master entry point:

```c
RNetInputContractDecision d = rnet_input_contract_stick_replace_decide(
    &published, &wire, completed_sim, &params, &gates);
if (rnet_input_contract_decision_is_rewind(d)) { /* queue rollback */ }
```

Host-binding guidance (master-hash + savestate recomp host): bind only
`hash_confirm_promote` (= "peer master hash agreed through tick") and leave the
rest NULL (portable defaults).

## Invariants (soak-derived; do not relax without a new soak)

- Predicted rows never get a bare deadband promote — only `hash_confirm` or a
  host protect.
- Release always rewinds on both completed-sim and runway paths.
- Dash-gate X disagree blocks all same-intent promotes.
- `hash_confirm_promote` fails closed when NULL.

## Host guarantees for rollback mode

- Deterministic `advance_sim` for a given input set.
- Snapshot save/load at any sim tick the library requests.
- State digests suitable for agreement comparison across peers.
- Single-thread session ownership (same as delay-sync).

## Not in this library

- Automatch / lobby server (see `recomp-net-server`).
- Game-specific pad layouts, snapshots, or determinism fixes.
- Delay-sync behavior changes (existing `RNetSession` is untouched).
