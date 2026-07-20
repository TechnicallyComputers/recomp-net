#include "rnet_protocol.h"

#include <string.h>

static void write_u16(rnet_u8 **cursor, rnet_u16 v)
{
    (*cursor)[0] = (rnet_u8)(v & 0xFFu);
    (*cursor)[1] = (rnet_u8)((v >> 8) & 0xFFu);
    *cursor += 2;
}

static void write_u32(rnet_u8 **cursor, rnet_u32 v)
{
    (*cursor)[0] = (rnet_u8)(v & 0xFFu);
    (*cursor)[1] = (rnet_u8)((v >> 8) & 0xFFu);
    (*cursor)[2] = (rnet_u8)((v >> 16) & 0xFFu);
    (*cursor)[3] = (rnet_u8)((v >> 24) & 0xFFu);
    *cursor += 4;
}

static rnet_u16 read_u16(const rnet_u8 **cursor)
{
    rnet_u16 v = (rnet_u16)(*cursor)[0] | ((rnet_u16)(*cursor)[1] << 8);
    *cursor += 2;
    return v;
}

static rnet_u32 read_u32(const rnet_u8 **cursor)
{
    rnet_u32 v = (rnet_u32)(*cursor)[0] | ((rnet_u32)(*cursor)[1] << 8) | ((rnet_u32)(*cursor)[2] << 16) |
                 ((rnet_u32)(*cursor)[3] << 24);
    *cursor += 4;
    return v;
}

rnet_u32 rnet_proto_checksum(const rnet_u8 *data, size_t len)
{
    rnet_u32 sum = 0x811c9dc5u;
    size_t i;
    for (i = 0; i < len; ++i)
    {
        sum ^= data[i];
        sum *= 0x01000193u;
    }
    return sum;
}

static int finish_packet(rnet_u8 *out, rnet_u8 *cursor, size_t cap)
{
    size_t body = (size_t)(cursor - out);
    rnet_u32 csum;
    if (body + 4 > cap)
    {
        return -1;
    }
    csum = rnet_proto_checksum(out, body);
    write_u32(&cursor, csum);
    return (int)(cursor - out);
}

int rnet_proto_encode_hello(rnet_u8 *out, size_t cap, rnet_u32 magic, rnet_u32 session_id, rnet_u8 local_slot,
                            rnet_u8 slot_count, rnet_u8 delay)
{
    rnet_u8 *c = out;
    if (cap < 20)
    {
        return -1;
    }
    write_u32(&c, magic);
    write_u16(&c, RNET_PKT_HELLO);
    write_u32(&c, session_id);
    *c++ = local_slot;
    *c++ = slot_count;
    *c++ = delay;
    *c++ = 0;
    return finish_packet(out, c, cap);
}

int rnet_proto_encode_ready(rnet_u8 *out, size_t cap, rnet_u32 magic, rnet_u32 session_id, rnet_u8 local_slot)
{
    rnet_u8 *c = out;
    if (cap < 16)
    {
        return -1;
    }
    write_u32(&c, magic);
    write_u16(&c, RNET_PKT_READY);
    write_u32(&c, session_id);
    *c++ = local_slot;
    *c++ = 0;
    *c++ = 0;
    *c++ = 0;
    return finish_packet(out, c, cap);
}

int rnet_proto_encode_start(rnet_u8 *out, size_t cap, rnet_u32 magic, rnet_u32 session_id, rnet_u32 start_tick)
{
    rnet_u8 *c = out;
    if (cap < 20)
    {
        return -1;
    }
    write_u32(&c, magic);
    write_u16(&c, RNET_PKT_START);
    write_u32(&c, session_id);
    write_u32(&c, start_tick);
    return finish_packet(out, c, cap);
}

int rnet_proto_encode_input(rnet_u8 *out, size_t cap, rnet_u32 magic, rnet_u32 session_id, rnet_u8 local_slot,
                            rnet_u32 ack_tick, const RNetWireFrame *frames, int frame_count)
{
    rnet_u8 *c = out;
    int i;
    if (frame_count < 1 || frame_count > RNET_MAX_BUNDLE || frames == NULL)
    {
        return -1;
    }
    if (cap < 32)
    {
        return -1;
    }
    write_u32(&c, magic);
    write_u16(&c, RNET_PKT_INPUT);
    write_u32(&c, session_id);
    *c++ = local_slot;
    *c++ = (rnet_u8)frame_count;
    *c++ = 0;
    *c++ = 0;
    write_u32(&c, ack_tick);
    for (i = 0; i < frame_count; ++i)
    {
        rnet_u16 sz = frames[i].size;
        if (sz > RNET_INPUT_MAX)
        {
            return -1;
        }
        if ((size_t)(c - out) + 4 + 2 + sz + 4 > cap)
        {
            return -1;
        }
        write_u32(&c, frames[i].tick);
        write_u16(&c, sz);
        if (sz > 0)
        {
            memcpy(c, frames[i].bytes, sz);
            c += sz;
        }
    }
    return finish_packet(out, c, cap);
}

int rnet_proto_encode_delay_sync(rnet_u8 *out, size_t cap, rnet_u32 magic, rnet_u32 session_id, rnet_u8 new_delay,
                                 rnet_u32 effective_tick)
{
    rnet_u8 *c = out;
    if (cap < 24)
    {
        return -1;
    }
    write_u32(&c, magic);
    write_u16(&c, RNET_PKT_DELAY_SYNC);
    write_u32(&c, session_id);
    *c++ = new_delay;
    *c++ = 0;
    *c++ = 0;
    *c++ = 0;
    write_u32(&c, effective_tick);
    return finish_packet(out, c, cap);
}

int rnet_proto_encode_input_confirm(rnet_u8 *out, size_t cap, rnet_u32 magic, rnet_u32 session_id, rnet_u8 local_slot,
                                    rnet_u32 sim_tick, rnet_u32 input_hash)
{
    rnet_u8 *c = out;
    if (cap < 28)
    {
        return -1;
    }
    write_u32(&c, magic);
    write_u16(&c, RNET_PKT_INPUT_CONFIRM);
    write_u32(&c, session_id);
    *c++ = local_slot;
    *c++ = 0;
    *c++ = 0;
    *c++ = 0;
    write_u32(&c, sim_tick);
    write_u32(&c, input_hash);
    return finish_packet(out, c, cap);
}

int rnet_proto_encode_bye(rnet_u8 *out, size_t cap, rnet_u32 magic, rnet_u32 session_id, rnet_u8 local_slot)
{
    rnet_u8 *c = out;
    if (cap < 16)
    {
        return -1;
    }
    write_u32(&c, magic);
    write_u16(&c, RNET_PKT_BYE);
    write_u32(&c, session_id);
    *c++ = local_slot;
    *c++ = 0;
    *c++ = 0;
    *c++ = 0;
    return finish_packet(out, c, cap);
}

int rnet_proto_decode(const rnet_u8 *data, size_t len, rnet_u32 expect_magic, RNetDecodedPacket *out)
{
    const rnet_u8 *c;
    const rnet_u8 *end;
    rnet_u32 magic;
    rnet_u32 csum;
    rnet_u32 expect;
    int i;

    if ((data == NULL) || (out == NULL) || (len < 10))
    {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    end = data + len;
    c = data;
    magic = read_u32(&c);
    if (magic != expect_magic)
    {
        return -1;
    }
    out->type = read_u16(&c);
    out->session_id = read_u32(&c);

    /* Verify trailing checksum. */
    if (len < 4)
    {
        return -1;
    }
    {
        const rnet_u8 *tail = data + len - 4;
        expect = (rnet_u32)tail[0] | ((rnet_u32)tail[1] << 8) | ((rnet_u32)tail[2] << 16) | ((rnet_u32)tail[3] << 24);
        csum = rnet_proto_checksum(data, len - 4);
        if (csum != expect)
        {
            return -1;
        }
        end = tail;
    }

    switch (out->type)
    {
    case RNET_PKT_HELLO:
        if ((size_t)(end - c) < 4)
        {
            return -1;
        }
        out->local_slot = *c++;
        out->slot_count = *c++;
        out->delay = *c++;
        (void)*c++;
        break;
    case RNET_PKT_READY:
        if ((size_t)(end - c) < 4)
        {
            return -1;
        }
        out->local_slot = *c++;
        c += 3;
        break;
    case RNET_PKT_START:
        if ((size_t)(end - c) < 4)
        {
            return -1;
        }
        out->start_tick = read_u32(&c);
        break;
    case RNET_PKT_INPUT:
        if ((size_t)(end - c) < 8)
        {
            return -1;
        }
        out->local_slot = *c++;
        out->frame_count = (int)(*c++);
        c += 2;
        out->ack_tick = read_u32(&c);
        if (out->frame_count < 1 || out->frame_count > RNET_MAX_BUNDLE)
        {
            return -1;
        }
        for (i = 0; i < out->frame_count; ++i)
        {
            rnet_u16 sz;
            if ((size_t)(end - c) < 6)
            {
                return -1;
            }
            out->frames[i].tick = read_u32(&c);
            sz = read_u16(&c);
            if (sz > RNET_INPUT_MAX || (size_t)(end - c) < sz)
            {
                return -1;
            }
            out->frames[i].size = sz;
            if (sz > 0)
            {
                memcpy(out->frames[i].bytes, c, sz);
                c += sz;
            }
        }
        break;
    case RNET_PKT_DELAY_SYNC:
        if ((size_t)(end - c) < 8)
        {
            return -1;
        }
        out->new_delay = *c++;
        c += 3;
        out->effective_tick = read_u32(&c);
        break;
    case RNET_PKT_INPUT_CONFIRM:
        if ((size_t)(end - c) < 12)
        {
            return -1;
        }
        out->local_slot = *c++;
        c += 3;
        out->confirm_sim_tick = read_u32(&c);
        out->confirm_hash = read_u32(&c);
        break;
    case RNET_PKT_BYE:
        if ((size_t)(end - c) < 4)
        {
            return -1;
        }
        out->local_slot = *c++;
        c += 3;
        break;
    default:
        return -1;
    }
    return 0;
}
