#include "rnet_transport.h"

#include <string.h>

void rnet_transport_init(RNetTransport *t)
{
    if (t == NULL)
    {
        return;
    }
    memset(t, 0, sizeof(*t));
    t->sock = RNET_SOCKET_INVALID;
    t->mode = RNET_TRANSPORT_NONE;
}

void rnet_transport_shutdown(RNetTransport *t)
{
    if (t == NULL)
    {
        return;
    }
    if (t->mode == RNET_TRANSPORT_LAN_UDP)
    {
        rnet_os_socket_destroy(&t->sock);
    }
    t->mode = RNET_TRANSPORT_NONE;
    t->ice_send = NULL;
    t->ice_recv = NULL;
    t->ice_ctx = NULL;
    t->peer_count = 0;
    t->pending_peer_known = 0;
    t->accept_any_peer = 0;
    t->relay_hub = 0;
}

void rnet_transport_set_relay_hub(RNetTransport *t, int enabled)
{
    if (t == NULL)
    {
        return;
    }
    t->relay_hub = enabled ? 1 : 0;
}

static int sockaddr_equal(const struct sockaddr_in *a, const struct sockaddr_in *b)
{
    return a->sin_family == b->sin_family && a->sin_port == b->sin_port &&
           a->sin_addr.s_addr == b->sin_addr.s_addr;
}

static int find_peer_index(const RNetTransport *t, const struct sockaddr_in *addr)
{
    int i;
    if ((t == NULL) || (addr == NULL))
    {
        return -1;
    }
    for (i = 0; i < t->peer_count; ++i)
    {
        if (sockaddr_equal(&t->peers[i], addr))
        {
            return i;
        }
    }
    return -1;
}

static int add_peer(RNetTransport *t, const struct sockaddr_in *addr)
{
    if ((t == NULL) || (addr == NULL))
    {
        return -1;
    }
    if (find_peer_index(t, addr) >= 0)
    {
        return 0;
    }
    if (t->peer_count >= RNET_MAX_SLOTS)
    {
        return -1;
    }
    t->peers[t->peer_count] = *addr;
    t->peer_count += 1;
    return 0;
}

static void relay_to_others(RNetTransport *t, int src_idx, const rnet_u8 *buf, size_t len)
{
    int i;
    if ((t == NULL) || (buf == NULL) || (len == 0) || !t->relay_hub)
    {
        return;
    }
    if (!rnet_os_socket_valid(t->sock))
    {
        return;
    }
    for (i = 0; i < t->peer_count; ++i)
    {
        if (i == src_idx)
        {
            continue;
        }
        (void)rnet_os_sendto(t->sock, buf, len, &t->peers[i]);
    }
}

int rnet_transport_start_lan(RNetTransport *t, const char *bind_hostport, const char *peer_hostport)
{
    char host[128];
    rnet_u16 port = 0;
    struct sockaddr_in bind_addr;
    struct sockaddr_in peer_addr;

    if ((t == NULL) || (bind_hostport == NULL))
    {
        return -1;
    }
    rnet_transport_shutdown(t);
    rnet_os_startup();

    if (rnet_os_parse_hostport(bind_hostport, host, sizeof(host), &port) != 0)
    {
        return -1;
    }
    if (rnet_os_resolve_sockaddr(host, port, &bind_addr) != 0)
    {
        return -1;
    }

    t->sock = rnet_os_socket_create_dgram();
    if (!rnet_os_socket_valid(t->sock))
    {
        return -1;
    }
    (void)rnet_os_setsockopt_reuseaddr(t->sock, 1);
    (void)rnet_os_setsockopt_recvbuf(t->sock, 256 * 1024);
    if (rnet_os_bind(t->sock, &bind_addr) != 0)
    {
        rnet_os_socket_destroy(&t->sock);
        return -1;
    }
    if (rnet_os_set_nonblocking(t->sock) != 0)
    {
        rnet_os_socket_destroy(&t->sock);
        return -1;
    }

    t->peer_count = 0;
    t->pending_peer_known = 0;
    t->relay_hub = 0;
    if (peer_hostport != NULL && peer_hostport[0] != '\0')
    {
        if (rnet_os_parse_hostport(peer_hostport, host, sizeof(host), &port) != 0 || port == 0)
        {
            rnet_os_socket_destroy(&t->sock);
            return -1;
        }
        if (rnet_os_resolve_sockaddr(host, port, &peer_addr) != 0)
        {
            rnet_os_socket_destroy(&t->sock);
            return -1;
        }
        t->peers[0] = peer_addr;
        t->peer_count = 1;
        t->accept_any_peer = 0;
    }
    else
    {
        t->accept_any_peer = 1;
    }
    t->mode = RNET_TRANSPORT_LAN_UDP;
    return 0;
}

int rnet_transport_send(RNetTransport *t, const rnet_u8 *buf, size_t len)
{
    int i;
    int any_ok = 0;

    if ((t == NULL) || (buf == NULL) || (len == 0))
    {
        return -1;
    }
    if (t->mode == RNET_TRANSPORT_LAN_UDP)
    {
        if (t->peer_count <= 0 || !rnet_os_socket_valid(t->sock))
        {
            return -1;
        }
        for (i = 0; i < t->peer_count; ++i)
        {
            if (rnet_os_sendto(t->sock, buf, len, &t->peers[i]) >= 0)
            {
                any_ok = 1;
            }
        }
        return any_ok ? 0 : -1;
    }
    if (t->mode == RNET_TRANSPORT_ICE)
    {
        if (t->ice_send == NULL)
        {
            return -1;
        }
        return t->ice_send(t->ice_ctx, buf, len);
    }
    return -1;
}

int rnet_transport_recv(RNetTransport *t, rnet_u8 *buf, size_t cap)
{
    int would_block = 0;
    int n;
    int src_idx;
    struct sockaddr_in src;

    if ((t == NULL) || (buf == NULL) || (cap == 0))
    {
        return -1;
    }
    if (t->mode == RNET_TRANSPORT_LAN_UDP)
    {
        if (!rnet_os_socket_valid(t->sock))
        {
            return -1;
        }
        for (;;)
        {
            n = rnet_os_recvfrom(t->sock, buf, cap, &src, &would_block);
            if (n < 0)
            {
                return would_block ? 0 : -1;
            }
            src_idx = find_peer_index(t, &src);
            if (src_idx >= 0)
            {
                /* Fanout only socket-received datagrams; never relay a relay. */
                relay_to_others(t, src_idx, buf, (size_t)n);
                return n;
            }
            if (t->accept_any_peer && t->peer_count < RNET_MAX_SLOTS)
            {
                t->pending_peer = src;
                t->pending_peer_known = 1;
                return n;
            }
            /* Unknown source and not accepting — drop and keep draining. */
        }
    }
    if (t->mode == RNET_TRANSPORT_ICE)
    {
        size_t out_len = 0;
        if (t->ice_recv == NULL)
        {
            return -1;
        }
        if (t->ice_recv(t->ice_ctx, buf, cap, &out_len) != 0)
        {
            return 0;
        }
        return (int)out_len;
    }
    return 0;
}

void rnet_transport_accept_pending_peer(RNetTransport *t)
{
    if (t == NULL || !t->accept_any_peer || !t->pending_peer_known)
    {
        return;
    }
    if (add_peer(t, &t->pending_peer) == 0)
    {
        t->pending_peer_known = 0;
    }
}
