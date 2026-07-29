#include "recomp_net/rollback.h"

#include <stdio.h>
#include <string.h>

static int g_failures;

static void expect_true(int cond, const char *msg)
{
    if (!cond)
    {
        fprintf(stderr, "FAIL: %s\n", msg);
        g_failures++;
    }
}

/* --- minimal host stub: deterministic input history + digest --- */

typedef struct TestHost
{
    uint32_t digest_at[256];
    uint8_t loaded_tick_valid;
    uint32_t loaded_tick;
} TestHost;

static int host_save_state(void *ctx, uint32_t tick)
{
    (void)ctx;
    (void)tick;
    return 0;
}

static int host_load_state(void *ctx, uint32_t tick)
{
    TestHost *h = (TestHost *)ctx;
    h->loaded_tick = tick;
    h->loaded_tick_valid = 1u;
    return 0;
}

static int host_advance_sim(void *ctx, uint32_t tick)
{
    (void)ctx;
    (void)tick;
    return 0;
}

static uint32_t host_state_digest(void *ctx, uint32_t tick, uint32_t partition)
{
    TestHost *h = (TestHost *)ctx;
    (void)partition;
    return (tick < 256u) ? h->digest_at[tick] : 0u;
}

static uint8_t host_hash_confirm_through(void *ctx, uint32_t tick)
{
    (void)ctx;
    (void)tick;
    return 0u;
}

static uint8_t host_get_input_row(void *ctx, int32_t slot, uint32_t tick, RNetRbFrame *out)
{
    (void)ctx;
    out->tick = tick;
    out->buttons = (uint16_t)(0x100u + (uint16_t)slot);
    out->stick_x = (int8_t)(10 + slot);
    out->stick_y = 0;
    out->is_predicted = 0u;
    out->is_valid = 1u;
    return 1u;
}

int main(void)
{
    RNetRbConfig cfg;
    RNetRollbackVTable vt;
    RNetRbSession *s;
    RNetRbCorrection corr;
    RNetRbFrame rows[8];
    RNetRbFrame got;
    uint32_t count;
    TestHost host;
    RNetRbEvent ev;

    memset(&host, 0, sizeof(host));
    memset(&cfg, 0, sizeof(cfg));
    cfg.local_slot = 0u;
    cfg.delay = 3u;

    memset(&vt, 0, sizeof(vt));
    vt.ctx = &host;
    vt.save_state = host_save_state;
    vt.load_state = host_load_state;
    vt.advance_sim = host_advance_sim;
    vt.state_digest = host_state_digest;
    vt.hash_confirm_through = host_hash_confirm_through;
    vt.get_input_row = host_get_input_row;

    s = rnet_rb_create(&cfg, &vt);
    expect_true(s != NULL, "create session");
    expect_true(rnet_rb_get_phase(s) == nRNetRbPhaseLive, "starts live");
    expect_true(!rnet_rb_is_active(s), "live not active");

    /* Begin episode */
    memset(&corr, 0, sizeof(corr));
    corr.epoch_id = 7u;
    corr.mismatch_tick = 50u;
    corr.load_tick = 48u;
    corr.target_tick = 56u;
    corr.slot = 1;
    corr.initiator = 1u;
    rnet_rb_begin_episode(s, &corr);
    expect_true(rnet_rb_is_active(s), "episode active after begin");
    expect_true(rnet_rb_get_phase(s) == nRNetRbPhaseSealInputs, "phase seal_inputs");
    expect_true(rnet_rb_get_mismatch_tick(s) == 50u, "mismatch tick");
    expect_true(rnet_rb_get_target_tick(s) == 56u, "target tick");
    expect_true(rnet_rb_get_corrected_slot(s) == 1, "corrected slot");

    /* Seal local rows (slot 0 = local authority). */
    rnet_rb_seal_inputs(s, corr.mismatch_tick, corr.target_tick, corr.slot);
    expect_true(rnet_rb_inputs_sealed(s), "inputs sealed");
    expect_true(rnet_rb_get_seal_span(s) == 7u, "seal span = 50..56 inclusive");
    expect_true(rnet_rb_tick_in_sealed_span(s, 53u), "tick 53 in span");
    expect_true(!rnet_rb_tick_in_sealed_span(s, 57u), "tick 57 outside span");
    expect_true(rnet_rb_get_sealed_frame(s, 0, 52u, &got), "local sealed row valid");
    expect_true(got.buttons == 0x100u, "local row buttons from history");
    expect_true(!rnet_rb_all_peer_seal_rows_complete(s), "peer rows not complete yet");

    /* Export local chunk for slot 0. */
    expect_true(rnet_rb_export_seal_rows_chunk(s, 0, 0u, 8u, rows, &count), "export local chunk");
    expect_true(count == 7u, "export full span");

    /* Apply peer rows for slot 1 (peer authority) across the span. */
    {
        uint32_t i;
        for (i = 0u; i < 7u; ++i)
        {
            rows[i].tick = corr.mismatch_tick + i;
            rows[i].buttons = 0x200u;
            rows[i].is_valid = 1u;
        }
    }
    /* Wrong epoch rejected. */
    expect_true(!rnet_rb_apply_peer_seal_rows(s, 99u, corr.mismatch_tick, corr.target_tick, 1, 0u,
                                              rows, 7u),
                "wrong epoch rejected");
    expect_true(rnet_rb_apply_peer_seal_rows(s, corr.epoch_id, corr.mismatch_tick, corr.target_tick,
                                             1, 0u, rows, 7u),
                "peer rows applied");
    expect_true(rnet_rb_peer_seal_rows_complete(s, 1), "slot 1 complete");
    /* Slot 2 (unused, no peer authority required in this minimal model). */
    expect_true(!rnet_rb_peer_seal_rows_complete(s, 2), "slot 2 incomplete (no rows)");
    expect_true(rnet_rb_get_sealed_frame(s, 1, 54u, &got), "peer sealed row retrievable");
    expect_true(got.buttons == 0x200u, "peer row buttons");

    /* FSM drive: baseline -> replay -> verify -> commit. */
    rnet_rb_set_phase(s, nRNetRbPhaseAwaitingBaseline);
    expect_true(rnet_rb_is_resimulating(s), "awaiting baseline counts as resim");
    rnet_rb_set_phase(s, nRNetRbPhaseReplay);
    expect_true(rnet_rb_is_resimulating(s), "replay resim");
    rnet_rb_set_phase(s, nRNetRbPhaseVerify);
    rnet_rb_on_post_match(s);
    expect_true(rnet_rb_get_phase(s) == nRNetRbPhaseCommit, "commit on match");
    expect_true(rnet_rb_resolved_through(s) == corr.target_tick, "frontier advanced to target");

    /* Events. */
    memset(&ev, 0, sizeof(ev));
    ev.type = nRNetRbEventPeerSymmetric;
    ev.epoch_id = 7u;
    rnet_rb_enqueue_event(s, &ev);
    expect_true(rnet_rb_has_pending_events(s), "event queued");
    memset(&ev, 0, sizeof(ev));
    expect_true(rnet_rb_drain_next_event(s, &ev), "drain event");
    expect_true(ev.type == nRNetRbEventPeerSymmetric, "event type preserved");
    expect_true(!rnet_rb_has_pending_events(s), "queue drained");

    rnet_rb_session_reset(s);
    expect_true(rnet_rb_get_phase(s) == nRNetRbPhaseLive, "reset to live");
    expect_true(!rnet_rb_inputs_sealed(s), "reset clears seal");

    rnet_rb_destroy(s);

    if (g_failures == 0)
    {
        printf("rollback_episode_test: ok\n");
        return 0;
    }
    fprintf(stderr, "rollback_episode_test: %d failure(s)\n", g_failures);
    return 1;
}
