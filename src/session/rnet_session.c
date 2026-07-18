#include "recomp_net/recomp_net.h"

#include "ice/rnet_ice_internal.h"
#include "input/rnet_rings.h"
#include "platform/rnet_platform.h"
#include "protocol/rnet_protocol.h"
#include "transport/rnet_transport.h"

#include <stdlib.h>
#include <string.h>

const char *rnet_version_string(void)
{
    return "0.1.0";
}

typedef enum RNetSessionPhase
{
    RNET_PHASE_IDLE = 0,
    RNET_PHASE_LINKING,
    RNET_PHASE_READY,
    RNET_PHASE_RUNNING
} RNetSessionPhase;

struct RNetSession
{
    RNetConfig cfg;
    RNetHostVTable host;
    RNetTransport transport;
    RNetIceAgent *ice;
    RNetSessionPhase phase;
    RNetInputRing local_ring;
    RNetInputRing remote_rings[RNET_MAX_SLOTS];
    rnet_u8 peer_ready[RNET_MAX_SLOTS];
    rnet_u8 local_ready;
    rnet_u8 start_sent;
    rnet_u8 delay;
    rnet_u32 sim_tick;
    rnet_u32 highest_remote_ack;
    rnet_u64 last_hello_ms;
    rnet_u64 last_ready_ms;
    rnet_u64 last_input_ms;
    int is_sim_authority; /* local_slot == 0 sends START */
};

static rnet_u64 session_now(RNetSession *s)
{
    if (s->host.now_ms != NULL)
    {
        return s->host.now_ms(s->host.ctx);
    }
    return rnet_os_monotonic_ms();
}

#if defined(RNET_ENABLE_ICE)
static void ice_emit_bridge(const RNetSignal *msg, void *user)
{
    RNetSession *s = (RNetSession *)user;
    if ((s != NULL) && (s->host.on_signal != NULL) && (msg != NULL))
    {
        s->host.on_signal(msg, s->host.ctx);
    }
}

static int ice_send_bridge(void *ice_ctx, const rnet_u8 *buf, size_t len)
{
    return rnet_ice_agent_send((RNetIceAgent *)ice_ctx, buf, len);
}

static int ice_recv_bridge(void *ice_ctx, rnet_u8 *buf, size_t cap, size_t *out_len)
{
    return rnet_ice_agent_recv((RNetIceAgent *)ice_ctx, buf, cap, out_len);
}
#endif /* RNET_ENABLE_ICE */

static void store_remote_frame(RNetSession *s, rnet_u8 slot, const RNetWireFrame *frame)
{
    RNetInputSample sample;
    if ((s == NULL) || (frame == NULL) || (slot >= s->cfg.slot_count) || (slot == s->cfg.local_slot))
    {
        return;
    }
    memset(&sample, 0, sizeof(sample));
    sample.tick = frame->tick;
    sample.size = frame->size;
    if (frame->size > 0)
    {
        memcpy(sample.bytes, frame->bytes, frame->size);
    }
    sample.valid = 1;
    rnet_ring_store(&s->remote_rings[slot], &sample);
}

static void handle_decoded(RNetSession *s, const RNetDecodedPacket *pkt)
{
    int i;
    if ((s == NULL) || (pkt == NULL))
    {
        return;
    }
    if (pkt->session_id != s->cfg.session_id)
    {
        return;
    }
    switch (pkt->type)
    {
    case RNET_PKT_HELLO:
        if (pkt->slot_count != s->cfg.slot_count)
        {
            break;
        }
        /* Peer is alive; move toward READY once we have exchanged HELLO. */
        if (s->phase == RNET_PHASE_LINKING)
        {
            s->phase = RNET_PHASE_READY;
        }
        break;
    case RNET_PKT_READY:
        if (pkt->local_slot < s->cfg.slot_count)
        {
            s->peer_ready[pkt->local_slot] = 1;
        }
        break;
    case RNET_PKT_START:
        if (s->phase != RNET_PHASE_RUNNING)
        {
            s->sim_tick = pkt->start_tick;
            s->phase = RNET_PHASE_RUNNING;
        }
        break;
    case RNET_PKT_INPUT:
        for (i = 0; i < pkt->frame_count; ++i)
        {
            store_remote_frame(s, pkt->local_slot, &pkt->frames[i]);
        }
        if (pkt->ack_tick > s->highest_remote_ack)
        {
            s->highest_remote_ack = pkt->ack_tick;
        }
        break;
    case RNET_PKT_DELAY_SYNC:
        if (pkt->effective_tick <= s->sim_tick || s->phase != RNET_PHASE_RUNNING)
        {
            s->delay = pkt->new_delay;
            s->cfg.input_delay = pkt->new_delay;
        }
        break;
    default:
        break;
    }
}

static void send_raw(RNetSession *s, const rnet_u8 *buf, int len)
{
    if ((s == NULL) || (buf == NULL) || (len <= 0))
    {
        return;
    }
    (void)rnet_transport_send(&s->transport, buf, (size_t)len);
}

static void pump_recv(RNetSession *s)
{
    rnet_u8 buf[RNET_MAX_PACKET];
    int n;
    RNetDecodedPacket pkt;
    int guard = 64;

    while (guard-- > 0)
    {
        n = rnet_transport_recv(&s->transport, buf, sizeof(buf));
        if (n <= 0)
        {
            break;
        }
        if (rnet_proto_decode(buf, (size_t)n, s->cfg.protocol_magic, &pkt) == 0)
        {
            handle_decoded(s, &pkt);
        }
    }
}

static void maybe_bootstrap(RNetSession *s)
{
    rnet_u8 buf[RNET_MAX_PACKET];
    int len;
    rnet_u64 now = session_now(s);
    rnet_u8 all_ready = 1;
    rnet_u8 slot;

    if (s->phase == RNET_PHASE_LINKING)
    {
        if (now - s->last_hello_ms >= 100ULL)
        {
            len = rnet_proto_encode_hello(buf, sizeof(buf), s->cfg.protocol_magic, s->cfg.session_id, s->cfg.local_slot,
                                          s->cfg.slot_count, s->delay);
            send_raw(s, buf, len);
            s->last_hello_ms = now;
        }
    }

    if (s->phase == RNET_PHASE_READY || s->phase == RNET_PHASE_LINKING)
    {
        if (!s->local_ready)
        {
            s->local_ready = 1;
        }
        if (now - s->last_ready_ms >= 100ULL)
        {
            len = rnet_proto_encode_ready(buf, sizeof(buf), s->cfg.protocol_magic, s->cfg.session_id, s->cfg.local_slot);
            send_raw(s, buf, len);
            s->last_ready_ms = now;
        }
        s->peer_ready[s->cfg.local_slot] = 1;
        for (slot = 0; slot < s->cfg.slot_count; ++slot)
        {
            if (!s->peer_ready[slot])
            {
                all_ready = 0;
                break;
            }
        }
        if (all_ready && s->is_sim_authority && !s->start_sent)
        {
            len = rnet_proto_encode_start(buf, sizeof(buf), s->cfg.protocol_magic, s->cfg.session_id, 0);
            send_raw(s, buf, len);
            s->start_sent = 1;
            s->sim_tick = 0;
            s->phase = RNET_PHASE_RUNNING;
        }
    }
}

static void send_input_bundle(RNetSession *s)
{
    rnet_u8 buf[RNET_MAX_PACKET];
    RNetWireFrame frames[RNET_MAX_BUNDLE];
    int count = 0;
    int red = (int)s->cfg.bundle_redundancy;
    rnet_u32 tip;
    rnet_u32 t;
    int len;
    rnet_u64 now = session_now(s);

    if (s->phase != RNET_PHASE_RUNNING)
    {
        return;
    }
    if (now - s->last_input_ms < 2ULL)
    {
        /* Soft rate limit; still allow every pump after a few ms. */
    }
    tip = rnet_wire_tick_from_sim(s->sim_tick, s->delay);
    if (red < 1)
    {
        red = 1;
    }
    if (red > RNET_MAX_BUNDLE)
    {
        red = RNET_MAX_BUNDLE;
    }
    for (t = (tip + 1U > (rnet_u32)red) ? (tip + 1U - (rnet_u32)red) : 0U; t <= tip; ++t)
    {
        RNetInputSample sample;
        if (!rnet_ring_get(&s->local_ring, t, &sample))
        {
            continue;
        }
        frames[count].tick = sample.tick;
        frames[count].size = sample.size;
        memcpy(frames[count].bytes, sample.bytes, sample.size);
        count++;
        if (count >= red)
        {
            break;
        }
    }
    if (count == 0)
    {
        return;
    }
    len = rnet_proto_encode_input(buf, sizeof(buf), s->cfg.protocol_magic, s->cfg.session_id, s->cfg.local_slot,
                                  rnet_ring_highest_valid(&s->remote_rings[(s->cfg.local_slot + 1) % s->cfg.slot_count]),
                                  frames, count);
    send_raw(s, buf, len);
    s->last_input_ms = now;
}

static int remotes_ready_for_sim(RNetSession *s, rnet_u32 sim_tick)
{
    rnet_u32 wire = rnet_wire_tick_from_sim(sim_tick, s->delay);
    rnet_u8 slot;
    RNetInputSample tmp;

    for (slot = 0; slot < s->cfg.slot_count; ++slot)
    {
        if (slot == s->cfg.local_slot)
        {
            continue;
        }
        if (!rnet_ring_get(&s->remote_rings[slot], wire, &tmp))
        {
            return 0;
        }
    }
    return 1;
}

RNetSession *rnet_session_create(const RNetConfig *cfg, const RNetHostVTable *host)
{
    RNetSession *s;
    rnet_u8 i;
    if ((cfg == NULL) || (host == NULL) || (host->sample_local == NULL) || (host->publish == NULL))
    {
        return NULL;
    }
    if (cfg->slot_count < 2 || cfg->slot_count > RNET_MAX_SLOTS || cfg->local_slot >= cfg->slot_count)
    {
        return NULL;
    }
    s = (RNetSession *)calloc(1, sizeof(*s));
    if (s == NULL)
    {
        return NULL;
    }
    s->cfg = *cfg;
    s->host = *host;
    s->delay = cfg->input_delay;
    s->phase = RNET_PHASE_IDLE;
    s->is_sim_authority = (cfg->local_slot == 0) ? 1 : 0;
    rnet_transport_init(&s->transport);
    rnet_ring_clear(&s->local_ring);
    for (i = 0; i < RNET_MAX_SLOTS; ++i)
    {
        rnet_ring_clear(&s->remote_rings[i]);
    }
    return s;
}

void rnet_session_destroy(RNetSession *s)
{
    if (s == NULL)
    {
        return;
    }
    rnet_transport_shutdown(&s->transport);
    rnet_ice_agent_destroy(s->ice);
    s->ice = NULL;
    free(s);
}

int rnet_session_start_lan(RNetSession *s, const char *bind_hostport, const char *peer_hostport)
{
    if (s == NULL)
    {
        return -1;
    }
    if (rnet_transport_start_lan(&s->transport, bind_hostport, peer_hostport) != 0)
    {
        return -1;
    }
    s->phase = RNET_PHASE_LINKING;
    s->last_hello_ms = 0;
    return 0;
}

int rnet_session_start_ice(RNetSession *s, const RNetIceConfig *ice)
{
#if !defined(RNET_ENABLE_ICE)
    (void)s;
    (void)ice;
    return -1;
#else
    RNetIceConfig local;
    if ((s == NULL) || (ice == NULL))
    {
        return -1;
    }
    local = *ice;
    s->ice = rnet_ice_agent_create(&local, ice_emit_bridge, s);
    if (s->ice == NULL)
    {
        return -1;
    }
    rnet_transport_shutdown(&s->transport);
    rnet_transport_init(&s->transport);
    s->transport.mode = RNET_TRANSPORT_ICE;
    s->transport.ice_send = ice_send_bridge;
    s->transport.ice_recv = ice_recv_bridge;
    s->transport.ice_ctx = s->ice;
    if (rnet_ice_agent_start_gathering(s->ice) != 0)
    {
        return -1;
    }
    /* Stay IDLE until ICE completes; pump will promote to LINKING. */
    s->phase = RNET_PHASE_IDLE;
    return 0;
#endif
}

void rnet_session_pump(RNetSession *s)
{
    if (s == NULL)
    {
        return;
    }
    if (s->ice != NULL)
    {
        rnet_ice_agent_poll(s->ice);
        if (s->phase == RNET_PHASE_IDLE && rnet_ice_agent_state(s->ice) == RNET_ICE_STATE_COMPLETED)
        {
            s->phase = RNET_PHASE_LINKING;
        }
    }
    pump_recv(s);
    maybe_bootstrap(s);
    send_input_bundle(s);
}

int rnet_session_try_admit(RNetSession *s, rnet_u32 sim_tick)
{
    RNetInputSample resolved[RNET_MAX_SLOTS];
    RNetInputSample local;
    rnet_u32 wire;
    rnet_u8 slot;

    if ((s == NULL) || (s->phase != RNET_PHASE_RUNNING))
    {
        return 0;
    }
    if (sim_tick != s->sim_tick)
    {
        /* Host must advance in lockstep with session clock. */
        return 0;
    }

    wire = rnet_wire_tick_from_sim(sim_tick, s->delay);
    memset(&local, 0, sizeof(local));
    s->host.sample_local(sim_tick, &local, s->host.ctx);
    local.tick = wire;
    local.valid = 1;
    if (local.size > RNET_INPUT_MAX)
    {
        local.size = RNET_INPUT_MAX;
    }
    rnet_ring_store(&s->local_ring, &local);

    /* Also keep a gameplay-indexed copy at sim_tick for publish clarity. */
    {
        RNetInputSample gameplay = local;
        gameplay.tick = sim_tick;
        /* Prefer publishing gameplay-indexed samples. */
        (void)gameplay;
    }

    if (!remotes_ready_for_sim(s, sim_tick))
    {
        send_input_bundle(s);
        return 0;
    }

    memset(resolved, 0, sizeof(resolved));
    for (slot = 0; slot < s->cfg.slot_count; ++slot)
    {
        if (slot == s->cfg.local_slot)
        {
            resolved[slot] = local;
            resolved[slot].tick = sim_tick;
        }
        else
        {
            RNetInputSample remote;
            if (!rnet_ring_get(&s->remote_rings[slot], wire, &remote))
            {
                return 0;
            }
            resolved[slot] = remote;
            resolved[slot].tick = sim_tick;
        }
    }

    s->host.publish(sim_tick, resolved, (int)s->cfg.slot_count, s->host.ctx);
    send_input_bundle(s);
    return 1;
}

void rnet_session_advance(RNetSession *s)
{
    if (s == NULL)
    {
        return;
    }
    s->sim_tick++;
}

void rnet_session_push_signal(RNetSession *s, const RNetSignal *msg)
{
    if ((s == NULL) || (msg == NULL) || (s->ice == NULL))
    {
        return;
    }
    rnet_ice_agent_push_signal(s->ice, msg);
}

rnet_u8 rnet_session_committed_delay(const RNetSession *s)
{
    return (s != NULL) ? s->delay : 0;
}

int rnet_session_local_slot(const RNetSession *s)
{
    return (s != NULL) ? (int)s->cfg.local_slot : -1;
}

rnet_u32 rnet_session_sim_tick(const RNetSession *s)
{
    return (s != NULL) ? s->sim_tick : 0;
}

int rnet_session_is_running(const RNetSession *s)
{
    return (s != NULL && s->phase == RNET_PHASE_RUNNING) ? 1 : 0;
}

RNetIceState rnet_session_ice_state(const RNetSession *s)
{
    if ((s == NULL) || (s->ice == NULL))
    {
        return RNET_ICE_STATE_IDLE;
    }
    return rnet_ice_agent_state(s->ice);
}
