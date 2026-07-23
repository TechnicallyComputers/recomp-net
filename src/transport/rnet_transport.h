#ifndef RNET_TRANSPORT_INTERNAL_H
#define RNET_TRANSPORT_INTERNAL_H

#include "recomp_net/config.h"
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
    /* Known LAN peers (guest→host: one entry; host hub: up to RNET_MAX_SLOTS). */
    struct sockaddr_in peers[RNET_MAX_SLOTS];
    int peer_count;
    struct sockaddr_in pending_peer;
    int pending_peer_known;
    /* Host waiting for guests: accept unknown src into pending/peers. */
    int accept_any_peer;
    /* Host-relay fanout: rebroadcast recv from peer i to all other peers. */
    int relay_hub;
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
/* Promote the pending LAN source into peers[] after a valid decoded packet. */
void rnet_transport_accept_pending_peer(RNetTransport *t);
/* Enable host-relay fanout (slot_count > 2, local_slot == 0). */
void rnet_transport_set_relay_hub(RNetTransport *t, int enabled);

#ifdef __cplusplus
}
#endif

#endif /* RNET_TRANSPORT_INTERNAL_H */
