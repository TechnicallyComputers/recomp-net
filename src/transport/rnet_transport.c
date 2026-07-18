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
    t->peer_known = 0;
}

int rnet_transport_start_lan(RNetTransport *t, const char *bind_hostport, const char *peer_hostport)
{
    char host[128];
    rnet_u16 port = 0;
    struct sockaddr_in bind_addr;

    if ((t == NULL) || (bind_hostport == NULL) || (peer_hostport == NULL))
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

    if (rnet_os_parse_hostport(peer_hostport, host, sizeof(host), &port) != 0)
    {
        rnet_os_socket_destroy(&t->sock);
        return -1;
    }
    if (rnet_os_resolve_sockaddr(host, port, &t->peer) != 0)
    {
        rnet_os_socket_destroy(&t->sock);
        return -1;
    }
    t->peer_known = 1;
    t->mode = RNET_TRANSPORT_LAN_UDP;
    return 0;
}

int rnet_transport_send(RNetTransport *t, const rnet_u8 *buf, size_t len)
{
    if ((t == NULL) || (buf == NULL) || (len == 0))
    {
        return -1;
    }
    if (t->mode == RNET_TRANSPORT_LAN_UDP)
    {
        if (!t->peer_known || !rnet_os_socket_valid(t->sock))
        {
            return -1;
        }
        return rnet_os_sendto(t->sock, buf, len, &t->peer);
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
        n = rnet_os_recvfrom(t->sock, buf, cap, &src, &would_block);
        if (n < 0)
        {
            return would_block ? 0 : -1;
        }
        /* Learn peer address from first packet if desired (keep configured peer). */
        (void)src;
        return n;
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
