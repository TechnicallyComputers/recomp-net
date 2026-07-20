#ifndef RECOMP_NET_SESSION_H
#define RECOMP_NET_SESSION_H

#include "recomp_net/config.h"
#include "recomp_net/ice.h"
#include "recomp_net/input.h"
#include "recomp_net/types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct RNetSession RNetSession;

typedef struct RNetHostVTable
{
    /* Fill local pad sample for the given sim tick (called during try_admit). */
    void (*sample_local)(rnet_u32 tick, RNetInputSample *out, void *ctx);
    /* Publish resolved inputs for all slots after admission succeeds. */
    void (*publish)(rnet_u32 tick, const RNetInputSample *by_slot, int slots, void *ctx);
    /* Optional wall/monotonic clock; NULL uses platform monotonic ms. */
    rnet_u64 (*now_ms)(void *ctx);
    /* Emit ICE signaling toward the host lobby (may be NULL for LAN-only). */
    void (*on_signal)(const RNetSignal *msg, void *ctx);
    void *ctx;
} RNetHostVTable;

RNetSession *rnet_session_create(const RNetConfig *cfg, const RNetHostVTable *host);
void rnet_session_destroy(RNetSession *s);

/* Start raw UDP peer session. bind/peer are "host:port" or ":port" / "ip:port". */
int rnet_session_start_lan(RNetSession *s, const char *bind_hostport, const char *peer_hostport);

/*
 * Start ICE-backed session (requires RNET_ENABLE_ICE). After create, host must
 * exchange signals via on_signal / rnet_session_push_signal until COMPLETED.
 */
int rnet_session_start_ice(RNetSession *s, const RNetIceConfig *ice);

/* Recv datagrams, poll ICE, send pending INPUT, drive bootstrap. */
void rnet_session_pump(RNetSession *s);

/*
 * Park until a LAN datagram may be readable (or timeout_ms). Prefer this over
 * Sleep/Delay in admit barriers: under dual-process load, 1ms sleeps often
 * stretch to several ms and serialize FMV lockstep. ICE falls back to sleep.
 * Returns 1 if the socket reported readable, 0 on timeout/unavailable.
 */
int rnet_session_wait_recv(RNetSession *s, int timeout_ms);

/*
 * Returns 1 when gameplay inputs for sim_tick (wire=sim) are present for
 * every remote, publish has been called, and a fresh local sample was stored
 * at wire=sim+D. INPUT_CONFIRM is sent for async desync checks (non-blocking).
 * Returns 0 to stall (keep pumping), including on input desync.
 */
int rnet_session_try_admit(RNetSession *s, rnet_u32 sim_tick);

/* Call after the host completes one authoritative sim step. */
void rnet_session_advance(RNetSession *s);

/*
 * Returns 1 if an INPUT_CONFIRM hash disagreement was flagged. Optional outs
 * receive the desync sim tick and local/remote hashes.
 */
int rnet_session_input_desync(const RNetSession *s, rnet_u32 *tick, rnet_u32 *local_hash, rnet_u32 *remote_hash);

/** Best-effort BYE so the peer can drop immediately instead of waiting for timeout. */
int rnet_session_send_bye(RNetSession *s);

/**
 * Non-zero if peer sent BYE or no valid UDP for timeout_ms after first RX.
 * Pass ~1500 for a snappy disconnect while still tolerating brief stalls.
 */
int rnet_session_peer_disconnected(const RNetSession *s, rnet_u64 timeout_ms);

/* Deliver an inbound signaling message from the lobby. */
void rnet_session_push_signal(RNetSession *s, const RNetSignal *msg);

rnet_u8 rnet_session_committed_delay(const RNetSession *s);
int rnet_session_local_slot(const RNetSession *s);
rnet_u32 rnet_session_sim_tick(const RNetSession *s);
int rnet_session_is_running(const RNetSession *s);
RNetIceState rnet_session_ice_state(const RNetSession *s);

/* Savestate / SRAM transfer ops for rnet_session_state_begin. */
#define RNET_STATE_OP_SAVE 0 /* peer stores file; sim keeps running */
#define RNET_STATE_OP_LOAD 1 /* stalls admit until guest has blob */
#define RNET_STATE_OP_SRAM 2 /* stalls admit until guest has blob */

/*
 * Host-only (local_slot == 0) blob transfer. LOAD/SRAM stall try_admit until
 * the peer ACKs the full payload; SAVE does not stall (async catch-up).
 */
int rnet_session_state_begin(RNetSession *s, rnet_u8 op, rnet_u8 slot, const void *data, size_t size);
int rnet_session_state_busy(const RNetSession *s);
/* 1 when transfer complete and blob ready for guest apply/store (or host finish). */
int rnet_session_state_take_ready(RNetSession *s, rnet_u8 *op_out, rnet_u8 *slot_out, const void **data_out,
                                  size_t *size_out);
/* After apply/store: clear transfer; hard_resync clears input rings on LOAD. */
void rnet_session_state_finish(RNetSession *s, int hard_resync);
void rnet_session_hard_resync(RNetSession *s);

#ifdef __cplusplus
}
#endif

#endif /* RECOMP_NET_SESSION_H */
