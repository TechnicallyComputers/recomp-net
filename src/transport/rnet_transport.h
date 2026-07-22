#ifndef RNET_TRANSPORT_INTERNAL_H
#define RNET_TRANSPORT_INTERNAL_H

#include "recomp_net/transport.h"
#include "recomp_net/types.h"
#include "platform/rnet_platform.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct RNetTransport RNetTransport;

struct RNetTransport
{
    RNetTransportMode mode;
    rnet_socket sock;
    struct sockaddr_in peer;
    struct sockaddr_in pending_peer;
    int peer_known;
    int pending_peer_known;
    int accept_first_peer;
    /* ICE callbacks filled by ice module when mode == ICE. */
    int (*ice_send)(void *ice_ctx, const rnet_u8 *buf, size_t len);
    int (*ice_recv)(void *ice_ctx, rnet_u8 *buf, size_t cap, size_t *out_len);
    void *ice_ctx;
};

void rnet_transport_init(RNetTransport *t);
void rnet_transport_shutdown(RNetTransport *t);
int rnet_transport_start_lan(RNetTransport *t, const char *bind_hostport, const char *peer_hostport);
int rnet_transport_send(RNetTransport *t, const rnet_u8 *buf, size_t len);
/* Returns bytes read, 0 if would-block/empty, -1 on hard error. */
int rnet_transport_recv(RNetTransport *t, rnet_u8 *buf, size_t cap);
/* Accept the source of the most recently received LAN datagram. */
void rnet_transport_accept_pending_peer(RNetTransport *t);

#ifdef __cplusplus
}
#endif

#endif /* RNET_TRANSPORT_INTERNAL_H */
