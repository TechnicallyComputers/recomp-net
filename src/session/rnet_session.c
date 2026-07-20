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
    rnet_u64 last_confirm_ms;
    int is_sim_authority; /* local_slot == 0 sends START */
    /* Resolved-input confirmation (hash agree before publish). */
    int confirm_active;
    rnet_u32 confirm_sim_tick;
    rnet_u32 confirm_hash;
    rnet_u8 confirm_seen[RNET_MAX_SLOTS];
    rnet_u32 peer_confirm_hash[RNET_MAX_SLOTS];
    RNetInputSample confirm_resolved[RNET_MAX_SLOTS];
    int input_desync;
    rnet_u32 desync_tick;
    rnet_u32 desync_local_hash;
    rnet_u32 desync_remote_hash;
    /* Peer liveness: any valid packet stamps last_peer_rx_ms; BYE sets peer_gone. */
    rnet_u64 last_peer_rx_ms;
    rnet_u64 session_start_ms;
    int peer_gone;
};

static rnet_u64 session_now(RNetSession *s)
{
    if (s->host.now_ms != NULL)
    {
        return s->host.now_ms(s->host.ctx);
    }
    return rnet_os_monotonic_ms();
}

static void send_raw(RNetSession *s, const rnet_u8 *buf, int len);

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
    RNetInputSample existing;
    if ((s == NULL) || (frame == NULL) || (slot >= s->cfg.slot_count) || (slot == s->cfg.local_slot))
    {
        return;
    }
    /* First-wins: later retransmits must not overwrite a latched wire row. */
    if (rnet_ring_get(&s->remote_rings[slot], frame->tick, &existing))
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

static rnet_u32 hash_resolved_inputs(rnet_u32 sim_tick, const RNetInputSample *by_slot, int slots)
{
    rnet_u8 buf[4 + RNET_MAX_SLOTS * (2 + RNET_INPUT_MAX)];
    size_t n = 0;
    int i;

    buf[n++] = (rnet_u8)(sim_tick & 0xFFu);
    buf[n++] = (rnet_u8)((sim_tick >> 8) & 0xFFu);
    buf[n++] = (rnet_u8)((sim_tick >> 16) & 0xFFu);
    buf[n++] = (rnet_u8)((sim_tick >> 24) & 0xFFu);
    for (i = 0; i < slots; ++i)
    {
        rnet_u16 sz = by_slot[i].size;
        if (sz > RNET_INPUT_MAX)
        {
            sz = RNET_INPUT_MAX;
        }
        buf[n++] = (rnet_u8)(sz & 0xFFu);
        buf[n++] = (rnet_u8)((sz >> 8) & 0xFFu);
        if (sz > 0)
        {
            memcpy(buf + n, by_slot[i].bytes, sz);
            n += sz;
        }
    }
    return rnet_proto_checksum(buf, n);
}

static void send_input_confirm(RNetSession *s)
{
    rnet_u8 buf[RNET_MAX_PACKET];
    int len;
    rnet_u64 now;

    if ((s == NULL) || !s->confirm_active)
    {
        return;
    }
    len = rnet_proto_encode_input_confirm(buf, sizeof(buf), s->cfg.protocol_magic, s->cfg.session_id, s->cfg.local_slot,
                                          s->confirm_sim_tick, s->confirm_hash);
    send_raw(s, buf, len);
    now = session_now(s);
    s->last_confirm_ms = now;
}

static int confirms_agree(const RNetSession *s)
{
    rnet_u8 slot;
    if ((s == NULL) || !s->confirm_active)
    {
        return 0;
    }
    for (slot = 0; slot < s->cfg.slot_count; ++slot)
    {
        if (!s->confirm_seen[slot])
        {
            return 0;
        }
        if (s->peer_confirm_hash[slot] != s->confirm_hash)
        {
            return 0;
        }
    }
    return 1;
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
    case RNET_PKT_INPUT_CONFIRM:
        if (pkt->local_slot < s->cfg.slot_count && pkt->confirm_sim_tick == s->sim_tick)
        {
            s->confirm_seen[pkt->local_slot] = 1;
            s->peer_confirm_hash[pkt->local_slot] = pkt->confirm_hash;
            if (s->confirm_active && pkt->confirm_hash != s->confirm_hash)
            {
                s->input_desync = 1;
                s->desync_tick = s->sim_tick;
                s->desync_local_hash = s->confirm_hash;
                s->desync_remote_hash = pkt->confirm_hash;
            }
        }
        break;
    case RNET_PKT_BYE:
        if (pkt->local_slot != s->cfg.local_slot)
        {
            s->peer_gone = 1;
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
            s->last_peer_rx_ms = session_now(s);
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
    s->session_start_ms = rnet_os_monotonic_ms();
    s->last_peer_rx_ms = 0;
    s->peer_gone = 0;
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
    rnet_u32 hash;
    rnet_u8 slot;
    rnet_u64 now;

    if ((s == NULL) || (s->phase != RNET_PHASE_RUNNING))
    {
        return 0;
    }
    if (sim_tick != s->sim_tick)
    {
        /* Host must advance in lockstep with session clock. */
        return 0;
    }
    if (s->input_desync)
    {
        return 0;
    }

    wire = rnet_wire_tick_from_sim(sim_tick, s->delay);
    /* Latch local once per wire tick; re-admits reuse the stored sample. */
    if (!rnet_ring_get(&s->local_ring, wire, &local))
    {
        memset(&local, 0, sizeof(local));
        s->host.sample_local(sim_tick, &local, s->host.ctx);
        local.tick = wire;
        local.valid = 1;
        if (local.size > RNET_INPUT_MAX)
        {
            local.size = RNET_INPUT_MAX;
        }
        rnet_ring_store(&s->local_ring, &local);
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

    hash = hash_resolved_inputs(sim_tick, resolved, (int)s->cfg.slot_count);

    if (!s->confirm_active || s->confirm_sim_tick != sim_tick)
    {
        /* Peer INPUT_CONFIRM may arrive before we activate. Preserve those
         * same-tick sightings — wiping them races the slower peer into a
         * permanent stall once the faster peer admits and stops retransmit. */
        rnet_u8 saved_seen[RNET_MAX_SLOTS];
        rnet_u32 saved_hash[RNET_MAX_SLOTS];
        memcpy(saved_seen, s->confirm_seen, sizeof(saved_seen));
        memcpy(saved_hash, s->peer_confirm_hash, sizeof(saved_hash));

        s->confirm_active = 1;
        s->confirm_sim_tick = sim_tick;
        s->confirm_hash = hash;
        memcpy(s->confirm_resolved, resolved, sizeof(resolved));
        memset(s->confirm_seen, 0, sizeof(s->confirm_seen));
        memset(s->peer_confirm_hash, 0, sizeof(s->peer_confirm_hash));
        for (slot = 0; slot < s->cfg.slot_count; ++slot)
        {
            if (slot == s->cfg.local_slot)
            {
                continue;
            }
            if (saved_seen[slot])
            {
                s->confirm_seen[slot] = 1;
                s->peer_confirm_hash[slot] = saved_hash[slot];
                if (saved_hash[slot] != hash)
                {
                    s->input_desync = 1;
                    s->desync_tick = sim_tick;
                    s->desync_local_hash = hash;
                    s->desync_remote_hash = saved_hash[slot];
                    return 0;
                }
            }
        }
        s->confirm_seen[s->cfg.local_slot] = 1;
        s->peer_confirm_hash[s->cfg.local_slot] = hash;
        send_input_confirm(s);
        send_input_bundle(s);
        return 0;
    }

    if (hash != s->confirm_hash)
    {
        s->input_desync = 1;
        s->desync_tick = sim_tick;
        s->desync_local_hash = hash;
        s->desync_remote_hash = s->confirm_hash;
        return 0;
    }

    now = session_now(s);
    if (now - s->last_confirm_ms >= 16ULL)
    {
        send_input_confirm(s);
    }

    if (!confirms_agree(s))
    {
        send_input_bundle(s);
        return 0;
    }

    s->host.publish(sim_tick, s->confirm_resolved, (int)s->cfg.slot_count, s->host.ctx);
    s->confirm_active = 0;
    return 1;
}

void rnet_session_advance(RNetSession *s)
{
    if (s == NULL)
    {
        return;
    }
    s->sim_tick++;
    s->confirm_active = 0;
    memset(s->confirm_seen, 0, sizeof(s->confirm_seen));
}

int rnet_session_input_desync(const RNetSession *s, rnet_u32 *tick, rnet_u32 *local_hash, rnet_u32 *remote_hash)
{
    if ((s == NULL) || !s->input_desync)
    {
        return 0;
    }
    if (tick != NULL)
    {
        *tick = s->desync_tick;
    }
    if (local_hash != NULL)
    {
        *local_hash = s->desync_local_hash;
    }
    if (remote_hash != NULL)
    {
        *remote_hash = s->desync_remote_hash;
    }
    return 1;
}

int rnet_session_send_bye(RNetSession *s)
{
    rnet_u8 buf[64];
    int n;

    if ((s == NULL) || (s->transport.mode == RNET_TRANSPORT_NONE))
    {
        return -1;
    }
    n = rnet_proto_encode_bye(buf, sizeof(buf), s->cfg.protocol_magic, s->cfg.session_id, s->cfg.local_slot);
    if (n <= 0)
    {
        return -1;
    }
    /* Best-effort: send a few times so a single loss doesn't leave peer hanging. */
    (void)rnet_transport_send(&s->transport, buf, (size_t)n);
    (void)rnet_transport_send(&s->transport, buf, (size_t)n);
    (void)rnet_transport_send(&s->transport, buf, (size_t)n);
    return 0;
}

int rnet_session_peer_disconnected(const RNetSession *s, rnet_u64 timeout_ms)
{
    rnet_u64 now;
    rnet_u64 last;

    if (s == NULL)
    {
        return 0;
    }
    if (s->peer_gone)
    {
        return 1;
    }
    if (timeout_ms == 0)
    {
        return 0;
    }
    now = rnet_os_monotonic_ms();
    if (s->last_peer_rx_ms == 0)
    {
        /* No peer traffic yet — only after we expected packets (linking/running).
         * Generous window so slow HELLO exchange isn't a false disconnect. */
        if (s->phase != RNET_PHASE_RUNNING && s->phase != RNET_PHASE_LINKING)
        {
            return 0;
        }
        if (s->session_start_ms != 0 && (now - s->session_start_ms) > (timeout_ms * 10u))
        {
            return 1;
        }
        return 0;
    }
    last = s->last_peer_rx_ms;
    return (now > last && (now - last) >= timeout_ms) ? 1 : 0;
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
