#ifndef RECOMP_NET_ROLLBACK_H
#define RECOMP_NET_ROLLBACK_H

/*
 * Shared rollback episode orchestration — game-agnostic core.
 *
 * recomp-net owns the episode FSM (when to rewind vs promote), the correction
 * tuple, the sealed input table, and the resolved-through / shared frontier
 * watermarks. The host owns everything game-specific: snapshot save/load, the
 * deterministic sim step, state digests, and (initially) input prediction and
 * the wire transport that delivers seal/baseline/sync packets.
 *
 * This is the second rollback layer after the portable input contract
 * (recomp_net/input_contract.h). It is transport-agnostic: hosts call the API
 * from their own network ingress (Phase 2 keeps BattleShip's netpeer as the
 * transporter); protocol/ICE alignment is Phase 3.
 *
 * Single-threaded session ownership, same as delay-sync RNetSession.
 */

#include <stdint.h>
#include <stddef.h>

#include "recomp_net/input_contract.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Seal span covers the deepest resim span a host can issue; matches the
 * frame-commit validation cadence + slack used by the reference host. */
#define RNET_RB_SEAL_MAX_SPAN 128u
#define RNET_RB_MAX_SLOTS 8

typedef enum RNetRbPhase
{
    nRNetRbPhaseLive = 0,
    nRNetRbPhaseSealInputs,
    nRNetRbPhaseAwaitingBaseline,
    nRNetRbPhaseReplay,
    nRNetRbPhaseVerify,
    nRNetRbPhaseCommit,
    nRNetRbPhaseAbort
} RNetRbPhase;

typedef enum RNetRbRole
{
    nRNetRbRoleInitiator = 0,
    nRNetRbRoleFollower
} RNetRbRole;

typedef enum RNetRbEventType
{
    nRNetRbEventNone = 0,
    nRNetRbEventInputMismatch,
    nRNetRbEventPeerSymmetric,
    nRNetRbEventStateDiverge,
    nRNetRbEventFrameCommit
} RNetRbEventType;

typedef struct RNetRbCorrection
{
    uint32_t epoch_id;
    uint32_t mismatch_tick;
    uint32_t load_tick;
    uint32_t target_tick;
    int32_t slot;            /* corrected player slot (-1 = whole state) */
    uint8_t initiator;       /* 1 when this host started the episode */
    uint8_t from_peer_notify;
} RNetRbCorrection;

/* Opaque per-slot input row the library seals and replays. The library never
 * interprets the payload beyond the stick-replace contract view. */
typedef struct RNetRbFrame
{
    uint32_t tick;
    uint16_t buttons;
    int8_t stick_x;
    int8_t stick_y;
    uint8_t is_predicted;
    uint8_t is_valid;
} RNetRbFrame;

typedef struct RNetRbEvent
{
    RNetRbEventType type;
    int32_t slot;
    uint32_t mismatch_tick;
    uint32_t target_tick;
    uint32_t load_tick;
    uint32_t epoch_id;
    uint8_t follower_local_auth;
} RNetRbEvent;

/*
 * Host callbacks. save/load/advance/digest are required; the gates mirror the
 * input contract and may be NULL (portable defaults). All runs on the host's
 * thread; the library does not spawn work.
 */
typedef struct RNetRollbackVTable
{
    void *ctx;
    /* Persist a snapshot at sim tick (library requests the deepest needed). */
    int (*save_state)(void *ctx, uint32_t tick);
    /* Restore the snapshot captured at sim tick before replay. */
    int (*load_state)(void *ctx, uint32_t tick);
    /* Advance exactly one deterministic sim tick using resolved inputs the
     * host published (sealed local + confirmed/peer-sealed remote rows). */
    int (*advance_sim)(void *ctx, uint32_t tick);
    /* Digest of canonical state at tick for agreement comparison; partition
     * selects a subsystem (0 = master). Must be identical across peers. */
    uint32_t (*state_digest)(void *ctx, uint32_t tick, uint32_t partition);
    /* 1 when the peer state/master-hash watermark has agreed through tick
     * (frame-commit). Backs input-contract hash_confirm_promote. */
    uint8_t (*hash_confirm_through)(void *ctx, uint32_t tick);
    /* Sample the authoritative row for a slot at a wire tick from the host's
     * input history (for sealing + self-seal fallback). */
    uint8_t (*get_input_row)(void *ctx, int32_t slot, uint32_t tick, RNetRbFrame *out_frame);
    /* Stick-replace contract gates; NULL = portable defaults. */
    RNetInputContractHostGates stick_gates;
} RNetRollbackVTable;

typedef struct RNetRbSession RNetRbSession;

typedef struct RNetRbConfig
{
    uint32_t local_slot;       /* this host's player slot */
    uint32_t delay;            /* committed input delay D */
    uint32_t seal_max_span;    /* <= RNET_RB_SEAL_MAX_SPAN; 0 = default */
} RNetRbConfig;

/* Lifecycle. */
RNetRbSession *rnet_rb_create(const RNetRbConfig *cfg, const RNetRollbackVTable *vt);
void rnet_rb_destroy(RNetRbSession *s);
void rnet_rb_session_reset(RNetRbSession *s);

/* Phase / tuple introspection (read-only). */
RNetRbPhase rnet_rb_get_phase(const RNetRbSession *s);
uint8_t rnet_rb_is_active(const RNetRbSession *s);
uint8_t rnet_rb_is_resimulating(const RNetRbSession *s);
uint32_t rnet_rb_get_epoch_id(const RNetRbSession *s);
uint32_t rnet_rb_get_mismatch_tick(const RNetRbSession *s);
uint32_t rnet_rb_get_load_tick(const RNetRbSession *s);
uint32_t rnet_rb_get_target_tick(const RNetRbSession *s);
int32_t rnet_rb_get_corrected_slot(const RNetRbSession *s);
uint8_t rnet_rb_is_from_peer_notify(const RNetRbSession *s);

/*
 * Correction entry points. Host calls begin_episode when it (or a symmetric
 * peer notice) identifies a mismatch; the library takes the phase to
 * SealInputs. The host then drives sealing and peer exchange, advancing the
 * FSM with set_phase as its transport confirms baseline/replay/verify.
 */
void rnet_rb_begin_episode(RNetRbSession *s, const RNetRbCorrection *corr);
void rnet_rb_set_phase(RNetRbSession *s, RNetRbPhase phase);

/* Stick-replace decision over a published vs authoritative wire row; thin
 * wrapper so hosts can resolve rewind-vs-promote with the shared contract and
 * their gates before queueing an episode. */
RNetInputContractDecision rnet_rb_decide_stick_replace(RNetRbSession *s,
                                                       const RNetInputContractFrame *published,
                                                       const RNetInputContractFrame *wire,
                                                       uint8_t completed_sim);

/* Sealed input table. Local-authority rows seal from the host's input history;
 * peer-authority rows arrive via apply_peer_seal_rows. The sealed table is the
 * sole replay read set. */
void rnet_rb_seal_inputs(RNetRbSession *s, uint32_t mismatch_tick, uint32_t target_tick,
                         int32_t correction_slot);
uint8_t rnet_rb_inputs_sealed(const RNetRbSession *s);
uint8_t rnet_rb_tick_in_sealed_span(const RNetRbSession *s, uint32_t tick);
uint8_t rnet_rb_get_sealed_frame(const RNetRbSession *s, int32_t slot, uint32_t tick,
                                 RNetRbFrame *out_frame);
uint32_t rnet_rb_get_seal_span(const RNetRbSession *s);

/* Peer seal-row exchange (host transports the chunks; library validates tuple
 * compatibility, marks completion, and gates forward replay). */
uint8_t rnet_rb_apply_peer_seal_rows(RNetRbSession *s, uint32_t epoch_id, uint32_t mismatch_tick,
                                     uint32_t target_tick, int32_t slot, uint32_t row_begin,
                                     const RNetRbFrame *rows, uint32_t row_count);
uint8_t rnet_rb_peer_seal_rows_complete(const RNetRbSession *s, int32_t slot);
uint8_t rnet_rb_all_peer_seal_rows_complete(const RNetRbSession *s);
uint8_t rnet_rb_export_seal_rows_chunk(const RNetRbSession *s, int32_t slot, uint32_t row_begin,
                                       uint32_t max_rows, RNetRbFrame *out_frames,
                                       uint32_t *out_row_count);

/* Shared frontier / resolved-through watermark (highest sim tick agreed with
 * peers). Drives live-sim caps and input-contract hash_confirm_promote. */
uint32_t rnet_rb_resolved_through(const RNetRbSession *s);
void rnet_rb_set_peer_convergence(RNetRbSession *s, uint32_t peer_target);

/* Episode resolution. Host calls on_post_match / on_post_diverge after the
 * post-replay digest comparison; library commits the sealed rows or deepens /
 * aborts. */
void rnet_rb_on_post_match(RNetRbSession *s);
void rnet_rb_on_post_diverge(RNetRbSession *s);
void rnet_rb_commit_promote_sealed(RNetRbSession *s);

/* Event queue (peer symmetric notices, frame-commit) drained by the host. */
void rnet_rb_enqueue_event(RNetRbSession *s, const RNetRbEvent *event);
uint8_t rnet_rb_drain_next_event(RNetRbSession *s, RNetRbEvent *out_event);
uint8_t rnet_rb_has_pending_events(const RNetRbSession *s);

#ifdef __cplusplus
}
#endif

#endif /* RECOMP_NET_ROLLBACK_H */
