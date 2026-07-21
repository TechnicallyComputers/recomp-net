#ifndef RNET_PROTOCOL_H
#define RNET_PROTOCOL_H

#include "recomp_net/config.h"
#include "recomp_net/input.h"
#include "recomp_net/types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RNET_PKT_HELLO 1
#define RNET_PKT_READY 2
#define RNET_PKT_START 3
#define RNET_PKT_INPUT 4
#define RNET_PKT_DELAY_SYNC 5
#define RNET_PKT_INPUT_CONFIRM 6
#define RNET_PKT_BYE 7
#define RNET_PKT_STATE_BEGIN 8
#define RNET_PKT_STATE_CHUNK 9
#define RNET_PKT_STATE_ACK 10
#define RNET_PKT_STATE_PROBE 11       /* host→guest: hash/size before transfer */
#define RNET_PKT_STATE_PROBE_REPLY 12 /* guest→host: match (skip xfer) or not */

#define RNET_MAX_PACKET 1200
#define RNET_MAX_BUNDLE 8
#define RNET_STATE_CHUNK_MAX 1024
/* PSX .pst + dual memcards need multi‑MB; chunked ACK path scales with this. */
#define RNET_STATE_MAX (8u * 1024u * 1024u)
#define RNET_STATE_MAX_CHUNKS ((RNET_STATE_MAX + RNET_STATE_CHUNK_MAX - 1u) / RNET_STATE_CHUNK_MAX)

typedef struct RNetWireFrame
{
    rnet_u32 tick;
    rnet_u16 size;
    rnet_u8 bytes[RNET_INPUT_MAX];
} RNetWireFrame;

rnet_u32 rnet_proto_checksum(const rnet_u8 *data, size_t len);

/* Encode helpers return byte count or -1. */
int rnet_proto_encode_hello(rnet_u8 *out, size_t cap, rnet_u32 magic, rnet_u32 session_id, rnet_u8 local_slot,
                            rnet_u8 slot_count, rnet_u8 delay);
int rnet_proto_encode_ready(rnet_u8 *out, size_t cap, rnet_u32 magic, rnet_u32 session_id, rnet_u8 local_slot);
int rnet_proto_encode_start(rnet_u8 *out, size_t cap, rnet_u32 magic, rnet_u32 session_id, rnet_u32 start_tick);
int rnet_proto_encode_input(rnet_u8 *out, size_t cap, rnet_u32 magic, rnet_u32 session_id, rnet_u8 local_slot,
                            rnet_u32 ack_tick, const RNetWireFrame *frames, int frame_count);
int rnet_proto_encode_delay_sync(rnet_u8 *out, size_t cap, rnet_u32 magic, rnet_u32 session_id, rnet_u8 new_delay,
                                 rnet_u32 effective_tick);
/* Agree on resolved pad hash for sim_tick before publish/advance. */
int rnet_proto_encode_input_confirm(rnet_u8 *out, size_t cap, rnet_u32 magic, rnet_u32 session_id, rnet_u8 local_slot,
                                    rnet_u32 sim_tick, rnet_u32 input_hash);
/* Graceful peer leave (best-effort UDP). */
int rnet_proto_encode_bye(rnet_u8 *out, size_t cap, rnet_u32 magic, rnet_u32 session_id, rnet_u8 local_slot);

/* Host→guest savestate transfer (chunked, ACK'd). */
int rnet_proto_encode_state_begin(rnet_u8 *out, size_t cap, rnet_u32 magic, rnet_u32 session_id, rnet_u8 local_slot,
                                  rnet_u8 op, rnet_u8 slot, rnet_u32 xfer_id, rnet_u32 total_size,
                                  rnet_u32 payload_crc);
int rnet_proto_encode_state_chunk(rnet_u8 *out, size_t cap, rnet_u32 magic, rnet_u32 session_id, rnet_u8 local_slot,
                                  rnet_u32 xfer_id, rnet_u32 offset, const rnet_u8 *data, rnet_u16 size);
int rnet_proto_encode_state_ack(rnet_u8 *out, size_t cap, rnet_u32 magic, rnet_u32 session_id, rnet_u8 local_slot,
                                rnet_u32 xfer_id, rnet_u32 ack_bytes);
/* Hash probe: skip transfer when guest already has identical blob. */
int rnet_proto_encode_state_probe(rnet_u8 *out, size_t cap, rnet_u32 magic, rnet_u32 session_id, rnet_u8 local_slot,
                                  rnet_u8 op, rnet_u8 slot, rnet_u32 total_size, rnet_u32 payload_crc);
int rnet_proto_encode_state_probe_reply(rnet_u8 *out, size_t cap, rnet_u32 magic, rnet_u32 session_id,
                                        rnet_u8 local_slot, rnet_u8 op, rnet_u8 slot, rnet_u8 match);

typedef struct RNetDecodedPacket
{
    rnet_u16 type;
    rnet_u32 session_id;
    rnet_u8 local_slot;
    rnet_u8 slot_count;
    rnet_u8 delay;
    rnet_u32 start_tick;
    rnet_u32 ack_tick;
    rnet_u8 new_delay;
    rnet_u32 effective_tick;
    rnet_u32 confirm_sim_tick;
    rnet_u32 confirm_hash;
    int frame_count;
    RNetWireFrame frames[RNET_MAX_BUNDLE];
    /* STATE_* */
    rnet_u8 state_op;
    rnet_u8 state_slot;
    rnet_u32 state_xfer_id;
    rnet_u32 state_total_size;
    rnet_u32 state_payload_crc;
    rnet_u32 state_offset;
    rnet_u16 state_chunk_size;
    rnet_u32 state_ack_bytes;
    rnet_u8 state_chunk[RNET_STATE_CHUNK_MAX];
    rnet_u8 state_probe_match; /* PROBE_REPLY only */
} RNetDecodedPacket;

/* Returns 0 on success. */
int rnet_proto_decode(const rnet_u8 *data, size_t len, rnet_u32 expect_magic, RNetDecodedPacket *out);

#ifdef __cplusplus
}
#endif

#endif /* RNET_PROTOCOL_H */
