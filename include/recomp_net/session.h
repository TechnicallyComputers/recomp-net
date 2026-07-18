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
 * Returns 1 when every remote slot has wire row for sim_tick + D and local
 * sample is staged. On success, calls host publish with resolved slots.
 * Returns 0 if the session should stall (keep pumping).
 */
int rnet_session_try_admit(RNetSession *s, rnet_u32 sim_tick);

/* Call after the host completes one authoritative sim step. */
void rnet_session_advance(RNetSession *s);

/* Deliver an inbound signaling message from the lobby. */
void rnet_session_push_signal(RNetSession *s, const RNetSignal *msg);

rnet_u8 rnet_session_committed_delay(const RNetSession *s);
int rnet_session_local_slot(const RNetSession *s);
rnet_u32 rnet_session_sim_tick(const RNetSession *s);
int rnet_session_is_running(const RNetSession *s);
RNetIceState rnet_session_ice_state(const RNetSession *s);

#ifdef __cplusplus
}
#endif

#endif /* RECOMP_NET_SESSION_H */
