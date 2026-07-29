/*
 * Shared rollback episode orchestration — game-agnostic core.
 *
 * The library owns the FSM, correction tuple, sealed input table, and the
 * resolved-through watermark. The host owns snapshots, the sim step, digests,
 * and transport. See include/recomp_net/rollback.h and docs/rollback.md.
 *
 * Modelled on BattleShip's netrollback_episode FSM; SSB-specific gates, span
 * digests over subsystem hashes, and packet opcodes stay host-side.
 */

#include "recomp_net/rollback.h"

#include <stdlib.h>
#include <string.h>

#define RNET_RB_EVENT_QUEUE_MAX 8u

struct RNetRbSession
{
    RNetRbConfig cfg;
    RNetRollbackVTable vt;

    RNetRbPhase phase;
    RNetRbCorrection corr;

    /* Sealed input table: [span][slot]. Span indexes offset from mismatch. */
    RNetRbFrame *sealed;      /* sealed_max_span * RNET_RB_MAX_SLOTS */
    uint32_t sealed_span;
    uint8_t inputs_sealed;

    /* Per-slot peer-seal completion bitmask over the sealed span. */
    uint64_t peer_seal_mask[RNET_RB_MAX_SLOTS];

    uint32_t resolved_through; /* shared frontier watermark */

    RNetRbEvent events[RNET_RB_EVENT_QUEUE_MAX];
    uint32_t event_head;
    uint32_t event_tail;
};

static size_t rnet_rb_seal_index(uint32_t offset, uint32_t slot)
{
    return (size_t)offset * RNET_RB_MAX_SLOTS + (size_t)slot;
}

static uint8_t rnet_rb_slot_valid(int32_t slot)
{
    return ((slot >= 0) && ((uint32_t)slot < RNET_RB_MAX_SLOTS)) ? 1u : 0u;
}

RNetRbSession *rnet_rb_create(const RNetRbConfig *cfg, const RNetRollbackVTable *vt)
{
    RNetRbSession *s;

    if ((cfg == NULL) || (vt == NULL) || (vt->load_state == NULL) ||
        (vt->advance_sim == NULL) || (vt->get_input_row == NULL))
    {
        return NULL;
    }
    s = (RNetRbSession *)calloc(1u, sizeof(*s));
    if (s == NULL)
    {
        return NULL;
    }
    s->cfg = *cfg;
    s->vt = *vt;
    if ((s->cfg.seal_max_span == 0u) || (s->cfg.seal_max_span > RNET_RB_SEAL_MAX_SPAN))
    {
        s->cfg.seal_max_span = RNET_RB_SEAL_MAX_SPAN;
    }
    s->sealed = (RNetRbFrame *)calloc((size_t)s->cfg.seal_max_span * RNET_RB_MAX_SLOTS,
                                      sizeof(RNetRbFrame));
    if (s->sealed == NULL)
    {
        free(s);
        return NULL;
    }
    rnet_rb_session_reset(s);
    return s;
}

void rnet_rb_destroy(RNetRbSession *s)
{
    if (s == NULL)
    {
        return;
    }
    free(s->sealed);
    free(s);
}

void rnet_rb_session_reset(RNetRbSession *s)
{
    if (s == NULL)
    {
        return;
    }
    s->phase = nRNetRbPhaseLive;
    memset(&s->corr, 0, sizeof(s->corr));
    s->corr.slot = -1;
    s->sealed_span = 0u;
    s->inputs_sealed = 0u;
    memset(s->peer_seal_mask, 0, sizeof(s->peer_seal_mask));
    s->resolved_through = 0u;
    s->event_head = 0u;
    s->event_tail = 0u;
}

RNetRbPhase rnet_rb_get_phase(const RNetRbSession *s)
{
    return (s != NULL) ? s->phase : nRNetRbPhaseLive;
}

uint8_t rnet_rb_is_active(const RNetRbSession *s)
{
    return ((s != NULL) && (s->phase != nRNetRbPhaseLive)) ? 1u : 0u;
}

uint8_t rnet_rb_is_resimulating(const RNetRbSession *s)
{
    if (s == NULL)
    {
        return 0u;
    }
    return ((s->phase == nRNetRbPhaseAwaitingBaseline) || (s->phase == nRNetRbPhaseReplay) ||
            (s->phase == nRNetRbPhaseVerify))
               ? 1u
               : 0u;
}

uint32_t rnet_rb_get_epoch_id(const RNetRbSession *s) { return (s != NULL) ? s->corr.epoch_id : 0u; }
uint32_t rnet_rb_get_mismatch_tick(const RNetRbSession *s) { return (s != NULL) ? s->corr.mismatch_tick : 0u; }
uint32_t rnet_rb_get_load_tick(const RNetRbSession *s) { return (s != NULL) ? s->corr.load_tick : 0u; }
uint32_t rnet_rb_get_target_tick(const RNetRbSession *s) { return (s != NULL) ? s->corr.target_tick : 0u; }
int32_t rnet_rb_get_corrected_slot(const RNetRbSession *s) { return (s != NULL) ? s->corr.slot : -1; }
uint8_t rnet_rb_is_from_peer_notify(const RNetRbSession *s)
{
    return ((s != NULL) && (s->corr.from_peer_notify != 0u)) ? 1u : 0u;
}

void rnet_rb_begin_episode(RNetRbSession *s, const RNetRbCorrection *corr)
{
    if ((s == NULL) || (corr == NULL))
    {
        return;
    }
    s->corr = *corr;
    s->inputs_sealed = 0u;
    s->sealed_span = 0u;
    memset(s->peer_seal_mask, 0, sizeof(s->peer_seal_mask));
    s->phase = nRNetRbPhaseSealInputs;
}

void rnet_rb_set_phase(RNetRbSession *s, RNetRbPhase phase)
{
    if (s == NULL)
    {
        return;
    }
    s->phase = phase;
    if ((phase == nRNetRbPhaseCommit) || (phase == nRNetRbPhaseAbort))
    {
        /* Terminal: advance the resolved-through watermark to the target so
         * the shared frontier reflects the agreed span. */
        if (s->corr.target_tick > s->resolved_through)
        {
            s->resolved_through = s->corr.target_tick;
        }
    }
}

RNetInputContractDecision rnet_rb_decide_stick_replace(RNetRbSession *s,
                                                       const RNetInputContractFrame *published,
                                                       const RNetInputContractFrame *wire,
                                                       uint8_t completed_sim)
{
    RNetInputContractParams params;

    if (s == NULL)
    {
        return nRNetInputContractRewind;
    }
    rnet_input_contract_params_init_defaults(&params);
    return rnet_input_contract_stick_replace_decide(published, wire, completed_sim, &params,
                                                    &s->vt.stick_gates);
}

void rnet_rb_seal_inputs(RNetRbSession *s, uint32_t mismatch_tick, uint32_t target_tick,
                         int32_t correction_slot)
{
    uint32_t span;
    uint32_t offset;
    uint32_t slot;

    (void)correction_slot;
    if (s == NULL)
    {
        return;
    }
    span = (target_tick >= mismatch_tick) ? (target_tick - mismatch_tick + 1u) : 0u;
    if (span == 0u)
    {
        return;
    }
    if (span > s->cfg.seal_max_span)
    {
        span = s->cfg.seal_max_span;
    }
    /* Seal local-authority rows from the host's input history; peer-authority
     * slots arrive via apply_peer_seal_rows. */
    for (offset = 0u; offset < span; ++offset)
    {
        uint32_t tick = mismatch_tick + offset;
        for (slot = 0u; slot < RNET_RB_MAX_SLOTS; ++slot)
        {
            RNetRbFrame *dst = &s->sealed[rnet_rb_seal_index(offset, slot)];
            memset(dst, 0, sizeof(*dst));
            dst->tick = tick;
            if (slot == s->cfg.local_slot)
            {
                RNetRbFrame row;
                if (s->vt.get_input_row(s->vt.ctx, (int32_t)slot, tick, &row) != 0u)
                {
                    *dst = row;
                    dst->tick = tick;
                }
            }
        }
    }
    s->sealed_span = span;
    s->inputs_sealed = 1u;
}

uint8_t rnet_rb_inputs_sealed(const RNetRbSession *s)
{
    return ((s != NULL) && (s->inputs_sealed != 0u)) ? 1u : 0u;
}

uint8_t rnet_rb_tick_in_sealed_span(const RNetRbSession *s, uint32_t tick)
{
    if ((s == NULL) || (s->inputs_sealed == 0u))
    {
        return 0u;
    }
    return ((tick >= s->corr.mismatch_tick) &&
            (tick < s->corr.mismatch_tick + s->sealed_span))
               ? 1u
               : 0u;
}

uint8_t rnet_rb_get_sealed_frame(const RNetRbSession *s, int32_t slot, uint32_t tick,
                                 RNetRbFrame *out_frame)
{
    uint32_t offset;

    if ((s == NULL) || (out_frame == NULL) || (rnet_rb_tick_in_sealed_span(s, tick) == 0u) ||
        (rnet_rb_slot_valid(slot) == 0u))
    {
        return 0u;
    }
    offset = tick - s->corr.mismatch_tick;
    *out_frame = s->sealed[rnet_rb_seal_index(offset, (uint32_t)slot)];
    return out_frame->is_valid;
}

uint32_t rnet_rb_get_seal_span(const RNetRbSession *s)
{
    return (s != NULL) ? s->sealed_span : 0u;
}

uint8_t rnet_rb_apply_peer_seal_rows(RNetRbSession *s, uint32_t epoch_id, uint32_t mismatch_tick,
                                     uint32_t target_tick, int32_t slot, uint32_t row_begin,
                                     const RNetRbFrame *rows, uint32_t row_count)
{
    uint32_t i;

    if ((s == NULL) || (rows == NULL) || (rnet_rb_inputs_sealed(s) == 0u) ||
        (rnet_rb_slot_valid(slot) == 0u))
    {
        return 0u;
    }
    /* Tuple compatibility: only accept chunks for the live episode. */
    if ((epoch_id != s->corr.epoch_id) || (mismatch_tick != s->corr.mismatch_tick) ||
        (target_tick != s->corr.target_tick))
    {
        return 0u;
    }
    for (i = 0u; i < row_count; ++i)
    {
        uint32_t offset = row_begin + i;
        if (offset >= s->sealed_span)
        {
            break;
        }
        s->sealed[rnet_rb_seal_index(offset, (uint32_t)slot)] = rows[i];
        s->sealed[rnet_rb_seal_index(offset, (uint32_t)slot)].tick = mismatch_tick + offset;
        s->peer_seal_mask[(uint32_t)slot] |= (1ull << offset);
    }
    return 1u;
}

uint8_t rnet_rb_peer_seal_rows_complete(const RNetRbSession *s, int32_t slot)
{
    uint64_t want;

    if ((s == NULL) || (rnet_rb_inputs_sealed(s) == 0u) || (rnet_rb_slot_valid(slot) == 0u))
    {
        return 0u;
    }
    want = (s->sealed_span >= 64u) ? ~0ull : ((1ull << s->sealed_span) - 1ull);
    return ((s->peer_seal_mask[(uint32_t)slot] & want) == want) ? 1u : 0u;
}

uint8_t rnet_rb_all_peer_seal_rows_complete(const RNetRbSession *s)
{
    uint32_t slot;

    if ((s == NULL) || (rnet_rb_inputs_sealed(s) == 0u))
    {
        return 0u;
    }
    for (slot = 0u; slot < RNET_RB_MAX_SLOTS; ++slot)
    {
        if (slot == s->cfg.local_slot)
        {
            continue;
        }
        if (rnet_rb_peer_seal_rows_complete(s, (int32_t)slot) == 0u)
        {
            return 0u;
        }
    }
    return 1u;
}

uint8_t rnet_rb_export_seal_rows_chunk(const RNetRbSession *s, int32_t slot, uint32_t row_begin,
                                       uint32_t max_rows, RNetRbFrame *out_frames,
                                       uint32_t *out_row_count)
{
    uint32_t n;

    if ((s == NULL) || (out_frames == NULL) || (out_row_count == NULL) ||
        (rnet_rb_inputs_sealed(s) == 0u) || (rnet_rb_slot_valid(slot) == 0u))
    {
        return 0u;
    }
    if (row_begin >= s->sealed_span)
    {
        *out_row_count = 0u;
        return 1u;
    }
    n = s->sealed_span - row_begin;
    if (n > max_rows)
    {
        n = max_rows;
    }
    /* Chunk carries only this slot's rows. */
    {
        uint32_t i;
        for (i = 0u; i < n; ++i)
        {
            out_frames[i] = s->sealed[rnet_rb_seal_index(row_begin + i, (uint32_t)slot)];
        }
    }
    *out_row_count = n;
    return 1u;
}

uint32_t rnet_rb_resolved_through(const RNetRbSession *s)
{
    return (s != NULL) ? s->resolved_through : 0u;
}

void rnet_rb_set_peer_convergence(RNetRbSession *s, uint32_t peer_target)
{
    if ((s != NULL) && (peer_target > s->resolved_through))
    {
        s->resolved_through = peer_target;
    }
}

void rnet_rb_on_post_match(RNetRbSession *s)
{
    if (s == NULL)
    {
        return;
    }
    rnet_rb_commit_promote_sealed(s);
    rnet_rb_set_phase(s, nRNetRbPhaseCommit);
}

void rnet_rb_on_post_diverge(RNetRbSession *s)
{
    if (s == NULL)
    {
        return;
    }
    /* Stay in Verify; host decides to deepen the load or abort. */
    rnet_rb_set_phase(s, nRNetRbPhaseVerify);
}

void rnet_rb_commit_promote_sealed(RNetRbSession *s)
{
    /* Sealed rows become authoritative in the host's history on commit; the
     * host owns the actual promotion into its ledger during advance/replay.
     * Here we just mark the watermark. */
    if ((s != NULL) && (s->corr.target_tick > s->resolved_through))
    {
        s->resolved_through = s->corr.target_tick;
    }
}

void rnet_rb_enqueue_event(RNetRbSession *s, const RNetRbEvent *event)
{
    uint32_t next;

    if ((s == NULL) || (event == NULL))
    {
        return;
    }
    next = (s->event_tail + 1u) % RNET_RB_EVENT_QUEUE_MAX;
    if (next == s->event_head)
    {
        return; /* full: drop oldest discipline is host policy */
    }
    s->events[s->event_tail] = *event;
    s->event_tail = next;
}

uint8_t rnet_rb_drain_next_event(RNetRbSession *s, RNetRbEvent *out_event)
{
    if ((s == NULL) || (out_event == NULL) || (s->event_head == s->event_tail))
    {
        return 0u;
    }
    *out_event = s->events[s->event_head];
    s->event_head = (s->event_head + 1u) % RNET_RB_EVENT_QUEUE_MAX;
    return 1u;
}

uint8_t rnet_rb_has_pending_events(const RNetRbSession *s)
{
    return ((s != NULL) && (s->event_head != s->event_tail)) ? 1u : 0u;
}
