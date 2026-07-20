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

### INPUT_CONFIRM (6)

`local_slot : u8`, `pad : u8×3`, `sim_tick : u32`, `input_hash : u32`

Peers agree on the resolved pad set for `sim_tick` before publish/advance.
`input_hash` is `rnet_proto_checksum` over `sim_tick` (LE u32) followed by each
slot's `size` (LE u16) and `bytes`. Session latches local/remote wire rows
(first-wins) so late retransmits cannot change the hash mid-confirm.
Mismatch flags an input desync; agreement across all slots allows admission.

### BYE (7)

`local_slot : u8`, `pad : u8×3`

Graceful leave. Best-effort UDP (hosts may retransmit a few times on shutdown).
Peer marks the sender gone and can exit without waiting for the RX timeout.

### STATE_BEGIN (8)

`local_slot : u8`, `op : u8` (0=save store, 1=load apply, 2=SRAM), `slot : u8`,
`pad : u8`, `xfer_id : u32`, `total_size : u32`, `payload_crc : u32`

Host (slot 0) announces a blob transfer. `op=1/2` stall `try_admit` until the
guest ACKs; `op=0` is async (sim keeps running). Max size `RNET_STATE_MAX`
(512 KiB).

### STATE_CHUNK (9)

`local_slot : u8`, `pad : u8×3`, `xfer_id : u32`, `offset : u32`,
`chunk_size : u16`, `pad : u16`, `bytes : chunk_size` (≤ 1024)

Stop-and-wait data. Host retransmits the chunk at the peer's ACK offset.

### STATE_ACK (10)

`local_slot : u8`, `pad : u8×3`, `xfer_id : u32`, `ack_bytes : u32`

Guest reports contiguous bytes received from offset 0.

## Wire vs sim

Hosts reason in **sim ticks**. Fresh local samples are stored at
`wire = sim + D`. Admission for sim `T` resolves gameplay from wire `T`
(inputs sampled at sim `T - D`). `INPUT_CONFIRM` carries a hash of the
resolved set for async desync detection (does not stall admit).
