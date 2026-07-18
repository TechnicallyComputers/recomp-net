# Protocol

All multi-byte integers are **little-endian**. Every packet ends with a 32-bit
FNV-1a-style checksum over the preceding bytes (`rnet_proto_checksum`).

## Common header

| Field | Size | Notes |
|-------|------|-------|
| magic | u32 | Default `0x524E4554` (`RNET`) |
| type | u16 | Packet id |
| session_id | u32 | Must match config |

## Packet types

### HELLO (1)

`local_slot : u8`, `slot_count : u8`, `delay : u8`, `pad : u8`

Peers discover each other and confirm slot layout / advertised delay.

### READY (2)

`local_slot : u8`, `pad : u8×3`

Barrier before start. Session marks the sender ready.

### START (3)

`start_tick : u32`

Emitted by slot 0 when all slots are ready. Sets `sim_tick` and enters
`RUNNING`.

### INPUT (4)

`local_slot : u8`, `frame_count : u8`, `pad : u8×2`, `ack_tick : u32`, then
`frame_count` frames:

| Field | Size |
|-------|------|
| tick | u32 (wire tick) |
| size | u16 |
| bytes | `size` (≤ `RNET_INPUT_MAX`) |

Bundles retransmit recent local wire rows (`bundle_redundancy`).

### DELAY_SYNC (5)

`new_delay : u8`, `pad : u8×3`, `effective_tick : u32`

Optional mid-session delay change. Applied when not past the effective tick
(or while not running).

## Wire vs sim

Hosts reason in **sim ticks**. Inputs on the wire are indexed by
`wire = sim + D`. Admission for sim `T` requires remote rows at wire `T + D`.
