#ifndef RNET_ICE_INTERNAL_H
#define RNET_ICE_INTERNAL_H

#include "recomp_net/ice.h"
#include "recomp_net/types.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct RNetIceAgent RNetIceAgent;

typedef void (*RNetIceSignalEmitFn)(const RNetSignal *msg, void *user);

#if defined(RNET_ENABLE_ICE)

RNetIceAgent *rnet_ice_agent_create(const RNetIceConfig *cfg, RNetIceSignalEmitFn emit, void *user);
void rnet_ice_agent_destroy(RNetIceAgent *agent);
int rnet_ice_agent_start_gathering(RNetIceAgent *agent);
void rnet_ice_agent_poll(RNetIceAgent *agent);
void rnet_ice_agent_push_signal(RNetIceAgent *agent, const RNetSignal *msg);
RNetIceState rnet_ice_agent_state(const RNetIceAgent *agent);
int rnet_ice_agent_send(RNetIceAgent *agent, const rnet_u8 *buf, size_t len);
int rnet_ice_agent_recv(RNetIceAgent *agent, rnet_u8 *buf, size_t cap, size_t *out_len);

#else

static inline RNetIceAgent *rnet_ice_agent_create(const RNetIceConfig *cfg, RNetIceSignalEmitFn emit, void *user)
{
    (void)cfg;
    (void)emit;
    (void)user;
    return NULL;
}
static inline void rnet_ice_agent_destroy(RNetIceAgent *agent)
{
    (void)agent;
}
static inline int rnet_ice_agent_start_gathering(RNetIceAgent *agent)
{
    (void)agent;
    return -1;
}
static inline void rnet_ice_agent_poll(RNetIceAgent *agent)
{
    (void)agent;
}
static inline void rnet_ice_agent_push_signal(RNetIceAgent *agent, const RNetSignal *msg)
{
    (void)agent;
    (void)msg;
}
static inline RNetIceState rnet_ice_agent_state(const RNetIceAgent *agent)
{
    (void)agent;
    return RNET_ICE_STATE_IDLE;
}
static inline int rnet_ice_agent_send(RNetIceAgent *agent, const rnet_u8 *buf, size_t len)
{
    (void)agent;
    (void)buf;
    (void)len;
    return -1;
}
static inline int rnet_ice_agent_recv(RNetIceAgent *agent, rnet_u8 *buf, size_t cap, size_t *out_len)
{
    (void)agent;
    (void)buf;
    (void)cap;
    (void)out_len;
    return -1;
}

#endif

#ifdef __cplusplus
}
#endif

#endif /* RNET_ICE_INTERNAL_H */
