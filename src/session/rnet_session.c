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

rnet_u32 rnet_checksum(const void *data, size_t len)
{
    return rnet_proto_checksum((const rnet_u8 *)data, len);
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
    rnet_u32 last_input_tip;
    int last_input_tip_valid;
    int is_sim_authority; /* local_slot == 0 sends START */
    /* Resolved hashes and peer confirmations are prepared up
     * to D ticks ahead, so strict agreement normally completes before admit. */
    rnet_u32 published_tick[RNET_HISTORY_LENGTH];
    rnet_u32 published_hash[RNET_HISTORY_LENGTH];
    rnet_u8 published_valid[RNET_HISTORY_LENGTH];
    rnet_u32 peer_history_tick[RNET_HISTORY_LENGTH][RNET_MAX_SLOTS];
    rnet_u32 peer_history_hash[RNET_HISTORY_LENGTH][RNET_MAX_SLOTS];
    rnet_u8 peer_history_valid[RNET_HISTORY_LENGTH][RNET_MAX_SLOTS];
    rnet_u64 confirm_last_sent_ms[RNET_HISTORY_LENGTH];
    int input_desync;
    rnet_u32 desync_tick;
    rnet_u32 desync_local_hash;
    rnet_u32 desync_remote_hash;
    /* Peer liveness: any valid packet stamps last_peer_rx_ms; BYE sets peer_gone. */
    rnet_u64 last_peer_rx_ms;
    rnet_u64 session_start_ms;
    int peer_gone;
    /* Host→guest savestate / SRAM transfer (chunked + ACK). */
    int state_active;
    int state_sender;
    int state_ready;
    int state_stall_sim; /* probe + all transfers stall admit until finished */
    rnet_u8 state_op;
    rnet_u8 state_slot;
    rnet_u32 state_xfer_id;
    rnet_u32 state_next_xfer_id;
    rnet_u32 state_finished_xfer_id;
    rnet_u32 state_total;
    rnet_u32 state_crc;
    rnet_u32 state_contiguity; /* receiver: bytes from 0 received; sender: unused */
    rnet_u32 state_peer_ack;   /* sender: peer contiguous ACK */
    rnet_u32 state_send_cursor;
    rnet_u8 *state_buf;
    rnet_u8 state_rx_bits[(RNET_STATE_MAX_CHUNKS + 7u) / 8u];
    rnet_u64 state_last_tx_ms;
    rnet_u64 state_last_ack_ms;
    rnet_u64 state_last_begin_ms;
    /* Hash probe before transfer (host announce → guest reply). */
    int state_probe_active;
    int state_probe_sender;
    int state_probe_reply_ready; /* host: guest answered */
    int state_probe_pending;     /* guest: awaiting app reply */
    int state_probe_match;
    rnet_u8 state_probe_op;
    rnet_u8 state_probe_slot;
    rnet_u32 state_probe_size;
    rnet_u32 state_probe_crc;
    rnet_u64 state_probe_last_tx_ms;
    /* When set, pump must not emit INPUT bundles. Used across LOAD apply/ready
     * so pre-resync tip rows cannot clobber the post-hard_resync epoch
     * (tick % RNET_HISTORY_LENGTH collisions). Cleared by prime_delay_inputs. */
    int input_send_suppress;
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
static void send_input_bundle(RNetSession *s);
static void seed_delay_prefix(RNetSession *s);
static void state_clear(RNetSession *s);
static void state_probe_clear(RNetSession *s);
static void state_send_ack(RNetSession *s);
static void state_drive_sender(RNetSession *s);
static void state_drive_probe(RNetSession *s);
static void state_on_begin(RNetSession *s, const RNetDecodedPacket *pkt);
static void state_on_chunk(RNetSession *s, const RNetDecodedPacket *pkt);
static void state_on_ack(RNetSession *s, const RNetDecodedPacket *pkt);
static void state_on_probe(RNetSession *s, const RNetDecodedPacket *pkt);
static void state_on_probe_reply(RNetSession *s, const RNetDecodedPacket *pkt);

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

static int remote_tick_in_live_window(const RNetSession *s, rnet_u32 tick)
{
    rnet_u32 tip;
    rnet_u32 slop;
    rnet_u32 lo;
    rnet_u32 hi;
    if (s == NULL || s->phase != RNET_PHASE_RUNNING)
    {
        return 1;
    }
    tip = rnet_wire_tick_from_sim(s->sim_tick, s->delay);
    slop = (rnet_u32)s->cfg.bundle_redundancy + 8u;
    if (slop < 8u)
    {
        slop = 8u;
    }
    lo = (s->sim_tick > slop) ? (s->sim_tick - slop) : 0u;
    hi = tip + slop;
    return (tick >= lo && tick <= hi) ? 1 : 0;
}

static void store_remote_frame(RNetSession *s, rnet_u8 slot, const RNetWireFrame *frame)
{
    RNetInputSample sample;
    RNetInputSample existing;
    if ((s == NULL) || (frame == NULL) || (slot >= s->cfg.slot_count) || (slot == s->cfg.local_slot))
    {
        return;
    }
    /* Drop previous-epoch residue after hard_resync (sim_tick→0). Those ticks
     * share ring slots with the new tip via tick%HISTORY and first-wins would
     * otherwise keep remotes_ready_for_sim failing until the peer stops. */
    if (!remote_tick_in_live_window(s, frame->tick))
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

static void send_input_confirm_tick(RNetSession *s, rnet_u32 tick,
                                    rnet_u32 hash)
{
    rnet_u8 buf[RNET_MAX_PACKET];
    int len;
    rnet_u64 now;

    if (s == NULL) return;
    len = rnet_proto_encode_input_confirm(buf, sizeof(buf), s->cfg.protocol_magic, s->cfg.session_id, s->cfg.local_slot,
                                          tick, hash);
    send_raw(s, buf, len);
    now = session_now(s);
    s->confirm_last_sent_ms[tick % RNET_HISTORY_LENGTH] = now;
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
            seed_delay_prefix(s);
            send_input_bundle(s);
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
        if (pkt->local_slot < s->cfg.slot_count)
        {
            rnet_u32 index = pkt->confirm_sim_tick % RNET_HISTORY_LENGTH;
            s->peer_history_tick[index][pkt->local_slot] = pkt->confirm_sim_tick;
            s->peer_history_hash[index][pkt->local_slot] = pkt->confirm_hash;
            s->peer_history_valid[index][pkt->local_slot] = 1;
            if (s->published_valid[index] &&
                s->published_tick[index] == pkt->confirm_sim_tick &&
                s->published_hash[index] != pkt->confirm_hash)
            {
                s->input_desync = 1;
                s->desync_tick = pkt->confirm_sim_tick;
                s->desync_local_hash = s->published_hash[index];
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
    case RNET_PKT_STATE_BEGIN:
        state_on_begin(s, pkt);
        break;
    case RNET_PKT_STATE_CHUNK:
        state_on_chunk(s, pkt);
        break;
    case RNET_PKT_STATE_ACK:
        state_on_ack(s, pkt);
        break;
    case RNET_PKT_STATE_PROBE:
        state_on_probe(s, pkt);
        break;
    case RNET_PKT_STATE_PROBE_REPLY:
        state_on_probe_reply(s, pkt);
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
    int guard = s->state_active ? 512 : 64;

    while (guard-- > 0)
    {
        n = rnet_transport_recv(&s->transport, buf, sizeof(buf));
        if (n <= 0)
        {
            break;
        }
        if (rnet_proto_decode(buf, (size_t)n, s->cfg.protocol_magic, &pkt) == 0 &&
            pkt.session_id == s->cfg.session_id)
        {
            rnet_transport_accept_pending_peer(&s->transport);
            s->last_peer_rx_ms = session_now(s);
            handle_decoded(s, &pkt);
        }
    }
}

static void state_probe_clear(RNetSession *s)
{
    if (s == NULL)
    {
        return;
    }
    s->state_probe_active = 0;
    s->state_probe_sender = 0;
    s->state_probe_reply_ready = 0;
    s->state_probe_pending = 0;
    s->state_probe_match = 0;
    s->state_probe_op = 0;
    s->state_probe_slot = 0;
    s->state_probe_size = 0;
    s->state_probe_crc = 0;
    s->state_probe_last_tx_ms = 0;
    if (!s->state_active)
    {
        s->state_stall_sim = 0;
    }
}

static void state_clear(RNetSession *s)
{
    if (s == NULL)
    {
        return;
    }
    free(s->state_buf);
    s->state_buf = NULL;
    s->state_active = 0;
    s->state_sender = 0;
    s->state_ready = 0;
    s->state_stall_sim = s->state_probe_active ? 1 : 0;
    s->state_op = 0;
    s->state_slot = 0;
    s->state_xfer_id = 0;
    s->state_total = 0;
    s->state_crc = 0;
    s->state_contiguity = 0;
    s->state_peer_ack = 0;
    s->state_send_cursor = 0;
    memset(s->state_rx_bits, 0, sizeof(s->state_rx_bits));
    s->state_last_tx_ms = 0;
    s->state_last_ack_ms = 0;
    s->state_last_begin_ms = 0;
}

static void state_rx_set_chunk(RNetSession *s, rnet_u32 chunk_index)
{
    if (chunk_index >= RNET_STATE_MAX_CHUNKS)
    {
        return;
    }
    s->state_rx_bits[chunk_index >> 3] |= (rnet_u8)(1u << (chunk_index & 7u));
}

static int state_rx_has_chunk(const RNetSession *s, rnet_u32 chunk_index)
{
    if (chunk_index >= RNET_STATE_MAX_CHUNKS)
    {
        return 0;
    }
    return (s->state_rx_bits[chunk_index >> 3] >> (chunk_index & 7u)) & 1;
}

static void state_rx_advance_contiguity(RNetSession *s)
{
    rnet_u32 chunks = (s->state_total + RNET_STATE_CHUNK_MAX - 1u) / RNET_STATE_CHUNK_MAX;
    rnet_u32 i = s->state_contiguity / RNET_STATE_CHUNK_MAX;
    while (i < chunks && state_rx_has_chunk(s, i))
    {
        rnet_u32 end = (i + 1u) * RNET_STATE_CHUNK_MAX;
        if (end > s->state_total)
        {
            end = s->state_total;
        }
        s->state_contiguity = end;
        i++;
    }
}

static void state_send_ack(RNetSession *s)
{
    rnet_u8 buf[64];
    int n;
    n = rnet_proto_encode_state_ack(buf, sizeof(buf), s->cfg.protocol_magic, s->cfg.session_id, s->cfg.local_slot,
                                    s->state_xfer_id, s->state_contiguity);
    if (n > 0)
    {
        send_raw(s, buf, n);
        s->state_last_ack_ms = session_now(s);
    }
}

static void state_mark_ready_if_complete(RNetSession *s)
{
    rnet_u32 crc;
    if (!s->state_active || s->state_ready || s->state_buf == NULL)
    {
        return;
    }
    if (s->state_sender)
    {
        if (s->state_peer_ack >= s->state_total)
        {
            s->state_ready = 1;
        }
        return;
    }
    if (s->state_contiguity < s->state_total)
    {
        return;
    }
    crc = rnet_proto_checksum(s->state_buf, s->state_total);
    if (crc != s->state_crc)
    {
        /* Restart receive — ask host to resend from 0 by ACKing 0 after clear. */
        s->state_contiguity = 0;
        state_send_ack(s);
        return;
    }
    s->state_ready = 1;
    state_send_ack(s);
}

static void state_drive_sender(RNetSession *s)
{
    rnet_u8 buf[RNET_MAX_PACKET];
    int n;
    rnet_u64 now;
    rnet_u32 window_end;
    enum { kWindow = 48u * 1024u };

    if (!s->state_active || !s->state_sender || s->state_ready || s->state_buf == NULL)
    {
        return;
    }
    now = session_now(s);
    /* Retransmit BEGIN until the peer has ACKed past 0 (saw BEGIN). */
    if (s->state_peer_ack == 0 && (s->state_last_begin_ms == 0 || now - s->state_last_begin_ms >= 40ULL))
    {
        n = rnet_proto_encode_state_begin(buf, sizeof(buf), s->cfg.protocol_magic, s->cfg.session_id, s->cfg.local_slot,
                                          s->state_op, s->state_slot, s->state_xfer_id, s->state_total, s->state_crc);
        if (n > 0)
        {
            send_raw(s, buf, n);
        }
        s->state_last_begin_ms = now;
    }

    if (s->state_peer_ack >= s->state_total)
    {
        state_mark_ready_if_complete(s);
        return;
    }

    /* On ACK timeout, rewind send cursor to peer_ack for retransmission. */
    if (s->state_last_ack_ms != 0 && now - s->state_last_ack_ms >= 50ULL)
    {
        s->state_send_cursor = s->state_peer_ack;
        s->state_last_ack_ms = now; /* avoid spinning every pump */
    }

    if (s->state_send_cursor < s->state_peer_ack)
    {
        s->state_send_cursor = s->state_peer_ack;
    }
    window_end = s->state_peer_ack + kWindow;
    if (window_end > s->state_total)
    {
        window_end = s->state_total;
    }

    while (s->state_send_cursor < window_end)
    {
        rnet_u32 off = s->state_send_cursor;
        rnet_u32 left = s->state_total - off;
        rnet_u16 chunk = (left > RNET_STATE_CHUNK_MAX) ? (rnet_u16)RNET_STATE_CHUNK_MAX : (rnet_u16)left;
        n = rnet_proto_encode_state_chunk(buf, sizeof(buf), s->cfg.protocol_magic, s->cfg.session_id, s->cfg.local_slot,
                                          s->state_xfer_id, off, s->state_buf + off, chunk);
        if (n <= 0)
        {
            break;
        }
        send_raw(s, buf, n);
        s->state_send_cursor += chunk;
    }
    s->state_last_tx_ms = now;
    state_mark_ready_if_complete(s);
}

static void state_on_begin(RNetSession *s, const RNetDecodedPacket *pkt)
{
    if (pkt->local_slot == s->cfg.local_slot)
    {
        return; /* ignore echo */
    }
    if (s->cfg.local_slot == 0)
    {
        return; /* host never receives BEGIN */
    }
    if (pkt->state_total_size == 0 || pkt->state_total_size > RNET_STATE_MAX)
    {
        return;
    }
    if (pkt->state_xfer_id == s->state_finished_xfer_id)
    {
        /* A delayed/retransmitted BEGIN may survive the app's LOAD apply and
         * hard resync. Re-ACK it instead of reopening a completed transfer. */
        rnet_u8 buf[64];
        int n = rnet_proto_encode_state_ack(buf, sizeof(buf), s->cfg.protocol_magic,
                                            s->cfg.session_id, s->cfg.local_slot,
                                            pkt->state_xfer_id, pkt->state_total_size);
        if (n > 0) send_raw(s, buf, n);
        return;
    }
    if (s->state_active && s->state_xfer_id == pkt->state_xfer_id && s->state_buf != NULL)
    {
        state_send_ack(s);
        return;
    }
    state_probe_clear(s); /* hash-miss path: transfer replaces probe */
    state_clear(s);
    s->state_buf = (rnet_u8 *)malloc(pkt->state_total_size);
    if (s->state_buf == NULL)
    {
        return;
    }
    memset(s->state_buf, 0, pkt->state_total_size);
    s->state_active = 1;
    s->state_sender = 0;
    s->state_stall_sim = 1;
    s->state_op = pkt->state_op;
    s->state_slot = pkt->state_slot;
    s->state_xfer_id = pkt->state_xfer_id;
    s->state_total = pkt->state_total_size;
    s->state_crc = pkt->state_payload_crc;
    s->state_contiguity = 0;
    state_send_ack(s);
}

static void state_on_chunk(RNetSession *s, const RNetDecodedPacket *pkt)
{
    rnet_u32 end;
    if (!s->state_active || s->state_sender || s->state_buf == NULL)
    {
        return;
    }
    if (pkt->state_xfer_id != s->state_xfer_id)
    {
        return;
    }
    if (pkt->state_offset > s->state_total || pkt->state_chunk_size == 0)
    {
        return;
    }
    end = pkt->state_offset + (rnet_u32)pkt->state_chunk_size;
    if (end > s->state_total)
    {
        return;
    }
    memcpy(s->state_buf + pkt->state_offset, pkt->state_chunk, pkt->state_chunk_size);
    {
        rnet_u32 chunk_index = pkt->state_offset / RNET_STATE_CHUNK_MAX;
        state_rx_set_chunk(s, chunk_index);
        (void)end;
        state_rx_advance_contiguity(s);
    }
    {
        rnet_u64 now = session_now(s);
        if (now - s->state_last_ack_ms >= 4ULL || s->state_contiguity >= s->state_total)
        {
            state_send_ack(s);
        }
    }
    state_mark_ready_if_complete(s);
}

static void state_on_ack(RNetSession *s, const RNetDecodedPacket *pkt)
{
    if (!s->state_active || !s->state_sender)
    {
        return;
    }
    if (pkt->state_xfer_id != s->state_xfer_id)
    {
        return;
    }
    if (pkt->state_ack_bytes > s->state_peer_ack)
    {
        s->state_peer_ack = pkt->state_ack_bytes;
        if (s->state_peer_ack > s->state_total)
        {
            s->state_peer_ack = s->state_total;
        }
        s->state_last_ack_ms = session_now(s);
        s->state_last_tx_ms = 0; /* send next chunk immediately */
    }
    state_mark_ready_if_complete(s);
}

static void state_drive_probe(RNetSession *s)
{
    rnet_u8 buf[64];
    int n;
    rnet_u64 now;

    if (!s->state_probe_active || !s->state_probe_sender || s->state_probe_reply_ready)
    {
        return;
    }
    now = session_now(s);
    /* Ready/hash probes: snappy retransmit — 40ms left a visible post-load hitch. */
    if (s->state_probe_last_tx_ms != 0 && now - s->state_probe_last_tx_ms < 8ULL)
    {
        return;
    }
    n = rnet_proto_encode_state_probe(buf, sizeof(buf), s->cfg.protocol_magic, s->cfg.session_id,
                                      s->cfg.local_slot, s->state_probe_op, s->state_probe_slot,
                                      s->state_probe_size, s->state_probe_crc);
    if (n > 0)
    {
        send_raw(s, buf, n);
        s->state_probe_last_tx_ms = now;
    }
}

static void state_on_probe(RNetSession *s, const RNetDecodedPacket *pkt)
{
    if (pkt->local_slot == s->cfg.local_slot)
    {
        return;
    }
    if (s->cfg.local_slot == 0)
    {
        return; /* host never receives PROBE */
    }
    if (s->state_active)
    {
        return; /* transfer in flight takes precedence */
    }
    /* Retransmit of a probe we already answered — resend REPLY, do not re-arm. */
    if (s->state_probe_active && !s->state_probe_sender && !s->state_probe_pending &&
        s->state_probe_op == pkt->state_op && s->state_probe_slot == pkt->state_slot &&
        s->state_probe_size == pkt->state_total_size && s->state_probe_crc == pkt->state_payload_crc)
    {
        rnet_u8 buf[64];
        int n = rnet_proto_encode_state_probe_reply(buf, sizeof(buf), s->cfg.protocol_magic, s->cfg.session_id,
                                                    s->cfg.local_slot, s->state_probe_op, s->state_probe_slot,
                                                    s->state_probe_match ? 1u : 0u);
        if (n > 0)
        {
            send_raw(s, buf, n);
        }
        return;
    }
    /* Fresh probe — surface to the app. size==0 (coord save) must not stall
     * admit or deferred savestate_poll never runs (deadlock). */
    s->state_probe_active = 1;
    s->state_probe_sender = 0;
    s->state_probe_pending = 1;
    s->state_probe_reply_ready = 0;
    s->state_probe_match = 0;
    s->state_probe_op = pkt->state_op;
    s->state_probe_slot = pkt->state_slot;
    s->state_probe_size = pkt->state_total_size;
    s->state_probe_crc = pkt->state_payload_crc;
    s->state_stall_sim = (pkt->state_total_size != 0) ? 1 : 0;
}

static void state_on_probe_reply(RNetSession *s, const RNetDecodedPacket *pkt)
{
    if (!s->state_probe_active || !s->state_probe_sender)
    {
        return;
    }
    if (pkt->local_slot == s->cfg.local_slot)
    {
        return;
    }
    if (pkt->state_op != s->state_probe_op || pkt->state_slot != s->state_probe_slot)
    {
        return;
    }
    s->state_probe_match = pkt->state_probe_match ? 1 : 0;
    s->state_probe_reply_ready = 1;
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
            seed_delay_prefix(s);
            send_input_bundle(s);
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
    /* Stall or explicit suppress: do not emit pre-resync tips during load. */
    if (s->state_stall_sim || s->input_send_suppress)
    {
        return;
    }
    tip = rnet_wire_tick_from_sim(s->sim_tick, s->delay);
    /* A new future tip is latency-sensitive and sends immediately. Repeated
     * pumps for the same tip are reliability retransmits, not new data. */
    if (s->last_input_tip_valid && s->last_input_tip == tip &&
        now - s->last_input_ms < 8ULL)
    {
        return;
    }
    if (red < 1)
    {
        red = 1;
    }
    /* Startup must carry the complete neutral delay prefix. Otherwise delays
     * larger than the ordinary redundancy window can never admit tick zero. */
    if (red < (int)s->delay + 1)
    {
        red = (int)s->delay + 1;
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
    s->last_input_tip = tip;
    s->last_input_tip_valid = 1;
}

static int remotes_ready_for_play_wire(RNetSession *s, rnet_u32 play_wire)
{
    rnet_u8 slot;
    RNetInputSample tmp;

    for (slot = 0; slot < s->cfg.slot_count; ++slot)
    {
        if (slot == s->cfg.local_slot)
        {
            continue;
        }
        if (!rnet_ring_get(&s->remote_rings[slot], play_wire, &tmp))
        {
            return 0;
        }
    }
    return 1;
}

/* Gameplay ticks 0..D-1 have no prior human sample. Seed them with a
 * deterministic neutral row; fresh input sampled at sim T is stored at T+D. */
static void seed_delay_prefix(RNetSession *s)
{
    rnet_u32 t;
    if (s == NULL) return;
    for (t = 0; t < (rnet_u32)s->delay; ++t)
    {
        RNetInputSample sample;
        if (rnet_ring_get(&s->local_ring, t, &sample)) continue;
        memset(&sample, 0, sizeof(sample));
        sample.tick = t;
        sample.valid = 1;
        rnet_ring_store(&s->local_ring, &sample);
    }
}

static int collect_wire_inputs(RNetSession *s, rnet_u32 wire,
                               RNetInputSample *resolved)
{
    rnet_u8 slot;
    if (s == NULL || resolved == NULL) return 0;
    memset(resolved, 0, sizeof(RNetInputSample) * RNET_MAX_SLOTS);
    for (slot = 0; slot < s->cfg.slot_count; ++slot)
    {
        RNetInputSample sample;
        int found = slot == s->cfg.local_slot
            ? rnet_ring_get(&s->local_ring, wire, &sample)
            : rnet_ring_get(&s->remote_rings[slot], wire, &sample);
        if (!found) return 0;
        resolved[slot] = sample;
        resolved[slot].tick = wire;
    }
    return 1;
}

/* Resolve and advertise a wire row as soon as both peers' inputs exist. The
 * row may be D frames in the future; that lead time absorbs confirmation RTT. */
static int prepare_wire_confirm(RNetSession *s, rnet_u32 wire, int force_send)
{
    RNetInputSample resolved[RNET_MAX_SLOTS];
    rnet_u32 index = wire % RNET_HISTORY_LENGTH;
    rnet_u32 hash;
    rnet_u64 now;
    rnet_u8 slot;

    if (!collect_wire_inputs(s, wire, resolved)) return 0;
    hash = hash_resolved_inputs(wire, resolved, (int)s->cfg.slot_count);
    s->published_tick[index] = wire;
    s->published_hash[index] = hash;
    s->published_valid[index] = 1;

    for (slot = 0; slot < s->cfg.slot_count; ++slot)
    {
        if (slot == s->cfg.local_slot) continue;
        if (s->peer_history_valid[index][slot] &&
            s->peer_history_tick[index][slot] == wire &&
            s->peer_history_hash[index][slot] != hash)
        {
            s->input_desync = 1;
            s->desync_tick = wire;
            s->desync_local_hash = hash;
            s->desync_remote_hash = s->peer_history_hash[index][slot];
            return 0;
        }
    }

    now = session_now(s);
    if (force_send || s->confirm_last_sent_ms[index] == 0 ||
        now - s->confirm_last_sent_ms[index] >= 4ULL)
        send_input_confirm_tick(s, wire, hash);
    return 1;
}

static int wire_confirmations_agree(RNetSession *s, rnet_u32 wire)
{
    rnet_u32 index = wire % RNET_HISTORY_LENGTH;
    rnet_u8 slot;
    if (!s->published_valid[index] || s->published_tick[index] != wire)
        return 0;
    for (slot = 0; slot < s->cfg.slot_count; ++slot)
    {
        if (slot == s->cfg.local_slot) continue;
        if (!s->peer_history_valid[index][slot] ||
            s->peer_history_tick[index][slot] != wire)
            return 0;
        if (s->peer_history_hash[index][slot] != s->published_hash[index])
            return 0;
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
    state_clear(s);
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
    if (s->state_probe_active)
    {
        state_drive_probe(s);
    }
    if (s->state_active)
    {
        state_drive_sender(s);
    }
    /* LOAD apply/ready suppresses INPUT; emit HELLO so peers keep stamping
     * last_peer_rx_ms (hash-match apply of a multi‑MB .pst is otherwise silent). */
    if (s->phase == RNET_PHASE_RUNNING && (s->input_send_suppress || s->state_stall_sim))
    {
        rnet_u64 now = session_now(s);
        if (now - s->last_hello_ms >= 250ULL)
        {
            rnet_u8 buf[RNET_MAX_PACKET];
            int len = rnet_proto_encode_hello(buf, sizeof(buf), s->cfg.protocol_magic, s->cfg.session_id,
                                              s->cfg.local_slot, s->cfg.slot_count, s->delay);
            send_raw(s, buf, len);
            s->last_hello_ms = now;
        }
    }
    send_input_bundle(s);
}

int rnet_session_wait_recv(RNetSession *s, int timeout_ms)
{
    if (s == NULL)
    {
        return 0;
    }
    if (timeout_ms < 0)
    {
        timeout_ms = 0;
    }
    /* LAN UDP: block in poll so the peer can run without us busy-spinning. */
    if (s->transport.mode == RNET_TRANSPORT_LAN_UDP && rnet_os_socket_valid(s->transport.sock))
    {
        int r = rnet_os_poll_recv(s->transport.sock, timeout_ms);
        return (r > 0) ? 1 : 0;
    }
    /* ICE / no sock: coarse sleep only — still better than a busy spin. */
    if (timeout_ms > 0)
    {
        rnet_os_sleep_micros((unsigned)timeout_ms * 1000U);
    }
    return 0;
}

int rnet_session_try_admit(RNetSession *s, rnet_u32 sim_tick)
{
    RNetInputSample resolved[RNET_MAX_SLOTS];
    RNetInputSample local_play;
    RNetInputSample local_future;
    rnet_u32 play_wire;
    rnet_u32 sample_wire;
    rnet_u32 confirm_wire;
    rnet_u32 hash;
    rnet_u8 slot;

    if ((s == NULL) || (s->phase != RNET_PHASE_RUNNING))
    {
        return 0;
    }
    if (s->state_stall_sim && (s->state_active || s->state_probe_active))
    {
        /* Stall while probe or chunked transfer is in flight. */
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

    /* Simulate wire T while sampling local input for wire T+D. Once the
     * neutral prefix is consumed, peer input normally arrived D frames ago. */
    play_wire = sim_tick;
    sample_wire = rnet_wire_tick_from_sim(sim_tick, s->delay);
    if (!rnet_ring_get(&s->local_ring, sample_wire, &local_future))
    {
        memset(&local_future, 0, sizeof(local_future));
        s->host.sample_local(sim_tick, &local_future, s->host.ctx);
        local_future.tick = sample_wire;
        local_future.valid = 1;
        if (local_future.size > RNET_INPUT_MAX)
        {
            local_future.size = RNET_INPUT_MAX;
        }
        rnet_ring_store(&s->local_ring, &local_future);
    }

    send_input_bundle(s);
    for (confirm_wire = play_wire; confirm_wire <= sample_wire; ++confirm_wire)
    {
        (void)prepare_wire_confirm(s, confirm_wire, 0);
        if (confirm_wire == 0xffffffffu) break;
    }

    if (!rnet_ring_get(&s->local_ring, play_wire, &local_play))
    {
        if (play_wire == sample_wire)
            local_play = local_future;
        else
        {
            send_input_bundle(s);
            return 0;
        }
    }

    if (!remotes_ready_for_play_wire(s, play_wire))
    {
        send_input_bundle(s);
        return 0;
    }

    memset(resolved, 0, sizeof(resolved));
    for (slot = 0; slot < s->cfg.slot_count; ++slot)
    {
        if (slot == s->cfg.local_slot)
        {
            resolved[slot] = local_play;
            resolved[slot].tick = sim_tick;
        }
        else
        {
            RNetInputSample remote;
            if (!rnet_ring_get(&s->remote_rings[slot], play_wire, &remote))
            {
                return 0;
            }
            resolved[slot] = remote;
            resolved[slot].tick = sim_tick;
        }
    }

    hash = hash_resolved_inputs(sim_tick, resolved, (int)s->cfg.slot_count);

    if (!prepare_wire_confirm(s, play_wire, 0) || s->input_desync)
        return 0;
    if (s->published_hash[play_wire % RNET_HISTORY_LENGTH] != hash)
    {
        s->input_desync = 1;
        s->desync_tick = play_wire;
        s->desync_local_hash = hash;
        s->desync_remote_hash =
            s->published_hash[play_wire % RNET_HISTORY_LENGTH];
        return 0;
    }
    if (!wire_confirmations_agree(s, play_wire))
    {
        (void)prepare_wire_confirm(s, play_wire, 0);
        send_input_bundle(s);
        return 0;
    }
    s->host.publish(sim_tick, resolved, (int)s->cfg.slot_count, s->host.ctx);
    return 1;

#if 0 /* Legacy strict confirmation barrier; superseded by delay pipeline. */

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
        /* Peer CONFIRM may already be in saved_seen (arrived before we
         * activated). Admit immediately when everyone already agrees. */
        if (confirms_agree(s))
        {
            s->host.publish(sim_tick, s->confirm_resolved, (int)s->cfg.slot_count, s->host.ctx);
            s->confirm_active = 0;
            return 1;
        }
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
    if (now - s->last_confirm_ms >= 4ULL)
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
#endif
}

void rnet_session_advance(RNetSession *s)
{
    if (s == NULL)
    {
        return;
    }
    s->sim_tick++;
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

void rnet_session_get_stats(const RNetSession *s, RNetSessionStats *out)
{
    rnet_u8 slot;
    rnet_u32 highest_remote = 0;
    rnet_u64 now;
    int have_remote = 0;

    if (out == NULL)
        return;
    memset(out, 0, sizeof(*out));
    if (s == NULL)
        return;

    out->sim_tick = s->sim_tick;
    out->delay = s->delay;
    out->local_slot = s->cfg.local_slot;
    out->slot_count = s->cfg.slot_count;
    out->is_running = (s->phase == RNET_PHASE_RUNNING) ? 1 : 0;
    out->peer_gone = s->peer_gone;
    out->input_desync = s->input_desync;
    out->desync_tick = s->desync_tick;
    out->ice_state = rnet_session_ice_state(s);

    now = session_now((RNetSession *)s);
    if (s->last_peer_rx_ms != 0)
        out->last_peer_rx_age_ms = now - s->last_peer_rx_ms;

    for (slot = 0; slot < s->cfg.slot_count; ++slot) {
        rnet_u32 tip;
        if (slot == s->cfg.local_slot)
            continue;
        tip = rnet_ring_highest_valid(&s->remote_rings[slot]);
        if (!have_remote || tip > highest_remote)
            highest_remote = tip;
        have_remote = 1;
    }
    out->highest_remote_wire = highest_remote;
    out->remote_lead = have_remote ? (int)highest_remote - (int)s->sim_tick : 0;
}

int rnet_session_state_probe(RNetSession *s, rnet_u8 op, rnet_u8 slot, rnet_u32 total_size, rnet_u32 payload_crc)
{
    if ((s == NULL) || s->cfg.local_slot != 0 || s->phase != RNET_PHASE_RUNNING)
    {
        return -1;
    }
    if (s->state_active || (s->state_probe_active && s->state_probe_sender && !s->state_probe_reply_ready))
    {
        return -1;
    }
    if (op != RNET_STATE_OP_SAVE && op != RNET_STATE_OP_LOAD && op != RNET_STATE_OP_SRAM)
    {
        return -1;
    }
    if (total_size > RNET_STATE_MAX)
    {
        return -1;
    }

    state_probe_clear(s);
    s->state_probe_active = 1;
    s->state_probe_sender = 1;
    s->state_probe_reply_ready = 0;
    s->state_probe_pending = 0;
    s->state_probe_match = 0;
    s->state_probe_op = op;
    s->state_probe_slot = slot;
    s->state_probe_size = total_size;
    s->state_probe_crc = payload_crc;
    s->state_probe_last_tx_ms = 0;
    /* SAVE coord (size==0): keep sim running so deferred savestate_poll can run.
     * LOAD size==0: post-load ready rendezvous — host stalls until guest ACKs.
     * Hash probe (size!=0): stall until agree or transfer. */
    if (total_size != 0)
        s->state_stall_sim = 1;
    else if (op == RNET_STATE_OP_LOAD)
        s->state_stall_sim = 1;
    else
        s->state_stall_sim = 0;
    state_drive_probe(s);
    return 0;
}

int rnet_session_state_probe_take_reply(RNetSession *s, int *match_out)
{
    if ((s == NULL) || !s->state_probe_active || !s->state_probe_sender || !s->state_probe_reply_ready)
    {
        return 0;
    }
    if (match_out)
    {
        *match_out = s->state_probe_match;
    }
    return 1;
}

int rnet_session_state_probe_pending(const RNetSession *s, rnet_u8 *op_out, rnet_u8 *slot_out, rnet_u32 *size_out,
                                     rnet_u32 *crc_out)
{
    if ((s == NULL) || !s->state_probe_active || !s->state_probe_pending)
    {
        return 0;
    }
    if (op_out)
    {
        *op_out = s->state_probe_op;
    }
    if (slot_out)
    {
        *slot_out = s->state_probe_slot;
    }
    if (size_out)
    {
        *size_out = s->state_probe_size;
    }
    if (crc_out)
    {
        *crc_out = s->state_probe_crc;
    }
    return 1;
}

int rnet_session_state_probe_reply(RNetSession *s, int match)
{
    rnet_u8 buf[64];
    int n;

    if ((s == NULL) || !s->state_probe_active || s->state_probe_sender || !s->state_probe_pending)
    {
        return -1;
    }
    n = rnet_proto_encode_state_probe_reply(buf, sizeof(buf), s->cfg.protocol_magic, s->cfg.session_id,
                                            s->cfg.local_slot, s->state_probe_op, s->state_probe_slot,
                                            match ? 1u : 0u);
    if (n <= 0)
    {
        return -1;
    }
    send_raw(s, buf, n);
    s->state_probe_pending = 0;
    s->state_probe_match = match ? 1 : 0;
    if (s->state_probe_size == 0)
    {
        s->state_stall_sim = 0;
        if (s->state_probe_op == RNET_STATE_OP_LOAD)
        {
            /* Post-load ready ACK — done; do not keep probe for retransmit. */
            state_probe_clear(s);
            return 0;
        }
        /* SAVE coord ACK: leave probe active for retransmit replies. */
        return 0;
    }
    if (match)
    {
        /* Real hash agree — host finishes probe; guest unstalls. */
        state_probe_clear(s);
    }
    /* Hash miss: keep stall until STATE_BEGIN (or a new probe). */
    return 0;
}

void rnet_session_state_probe_finish(RNetSession *s)
{
    state_probe_clear(s);
}

int rnet_session_state_begin(RNetSession *s, rnet_u8 op, rnet_u8 slot, const void *data, size_t size)
{
    rnet_u8 buf[64];
    int n;

    if ((s == NULL) || (data == NULL) || (size == 0) || (size > RNET_STATE_MAX))
    {
        return -1;
    }
    if (s->cfg.local_slot != 0 || s->phase != RNET_PHASE_RUNNING || s->state_active)
    {
        return -1;
    }
    if (op != RNET_STATE_OP_SAVE && op != RNET_STATE_OP_LOAD && op != RNET_STATE_OP_SRAM)
    {
        return -1;
    }

    /* Drop any open probe — transfer is the authority path after a hash miss. */
    state_probe_clear(s);

    s->state_buf = (rnet_u8 *)malloc(size);
    if (s->state_buf == NULL)
    {
        return -1;
    }
    memcpy(s->state_buf, data, size);
    s->state_active = 1;
    s->state_sender = 1;
    s->state_ready = 0;
    s->state_stall_sim = 1;
    s->state_op = op;
    s->state_slot = slot;
    s->state_next_xfer_id++;
    if (s->state_next_xfer_id == 0)
    {
        s->state_next_xfer_id = 1;
    }
    s->state_xfer_id = s->state_next_xfer_id;
    s->state_total = (rnet_u32)size;
    s->state_crc = rnet_proto_checksum(s->state_buf, s->state_total);
    s->state_contiguity = 0;
    s->state_peer_ack = 0;
    s->state_send_cursor = 0;
    s->state_last_tx_ms = 0;
    s->state_last_ack_ms = 0;
    s->state_last_begin_ms = 0;

    n = rnet_proto_encode_state_begin(buf, sizeof(buf), s->cfg.protocol_magic, s->cfg.session_id, s->cfg.local_slot,
                                      s->state_op, s->state_slot, s->state_xfer_id, s->state_total, s->state_crc);
    if (n > 0)
    {
        send_raw(s, buf, n);
        s->state_last_begin_ms = session_now(s);
    }
    state_drive_sender(s);
    return 0;
}

int rnet_session_state_busy(const RNetSession *s)
{
    if (s == NULL)
    {
        return 0;
    }
    if (s->state_probe_active && s->state_probe_sender && !s->state_probe_reply_ready)
    {
        return 1;
    }
    if (s->state_probe_active && s->state_probe_pending)
    {
        return 1;
    }
    return (s->state_active && !s->state_ready) ? 1 : 0;
}

int rnet_session_state_take_ready(RNetSession *s, rnet_u8 *op_out, rnet_u8 *slot_out, const void **data_out,
                                  size_t *size_out)
{
    if ((s == NULL) || !s->state_active || !s->state_ready || s->state_buf == NULL)
    {
        return 0;
    }
    if (op_out)
    {
        *op_out = s->state_op;
    }
    if (slot_out)
    {
        *slot_out = s->state_slot;
    }
    if (data_out)
    {
        *data_out = s->state_buf;
    }
    if (size_out)
    {
        *size_out = s->state_total;
    }
    return 1;
}

void rnet_session_hard_resync(RNetSession *s)
{
    rnet_u8 i;
    if (s == NULL)
    {
        return;
    }
    rnet_ring_clear(&s->local_ring);
    /* Clear remotes too: leftover tip rows from a prior post-load epoch are
     * first-wins and can let one peer admit on stale wire=D inputs. Both peers
     * re-prime after mutual ready and wait for a fresh tip exchange. */
    for (i = 0; i < RNET_MAX_SLOTS; ++i)
    {
        rnet_ring_clear(&s->remote_rings[i]);
    }
    memset(s->published_valid, 0, sizeof(s->published_valid));
    memset(s->peer_history_valid, 0, sizeof(s->peer_history_valid));
    memset(s->confirm_last_sent_ms, 0, sizeof(s->confirm_last_sent_ms));
    s->input_desync = 0;
    s->desync_tick = 0;
    s->desync_local_hash = 0;
    s->desync_remote_hash = 0;
    s->highest_remote_ack = 0;
    s->last_input_tip_valid = 0;
    /* Peers may have applied a load on different sim ticks; restart together. */
    s->sim_tick = 0;
    /* Keep suppress until prime_delay_inputs — avoids emitting an empty tip. */
    s->input_send_suppress = 1;
}

void rnet_session_set_input_send_suppress(RNetSession *s, int suppress)
{
    if (s == NULL)
    {
        return;
    }
    s->input_send_suppress = suppress ? 1 : 0;
}

void rnet_session_prime_delay_inputs(RNetSession *s, const rnet_u8 *bytes, rnet_u16 size)
{
    rnet_u32 tip;
    rnet_u32 t;
    if (s == NULL || bytes == NULL || size == 0 || size > RNET_INPUT_MAX)
    {
        return;
    }
    if (s->phase != RNET_PHASE_RUNNING)
    {
        return;
    }
    tip = rnet_wire_tick_from_sim(s->sim_tick, s->delay);
    for (t = s->sim_tick; t < tip; ++t)
    {
        RNetInputSample sample;
        memset(&sample, 0, sizeof(sample));
        sample.tick = t;
        sample.size = size;
        memcpy(sample.bytes, bytes, size);
        sample.valid = 1;
        rnet_ring_store(&s->local_ring, &sample);
    }
    s->last_input_ms = 0;
    s->last_input_tip_valid = 0;
    s->input_send_suppress = 0;
    /* Prime must emit even if a LOAD ready probe still has state_stall_sim
     * (host commits sync before probe_finish). */
    {
        int saved_stall = s->state_stall_sim;
        s->state_stall_sim = 0;
        send_input_bundle(s);
        s->state_stall_sim = saved_stall;
    }
}

void rnet_session_state_finish(RNetSession *s, int hard_resync)
{
    rnet_u32 finished_xfer_id;
    if (s == NULL)
    {
        return;
    }
    finished_xfer_id = s->state_xfer_id;
    if (hard_resync)
    {
        rnet_session_hard_resync(s);
    }
    state_clear(s);
    s->state_finished_xfer_id = finished_xfer_id;
}
