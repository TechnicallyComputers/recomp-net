#ifndef RECOMP_NET_H
#define RECOMP_NET_H

#include "recomp_net/config.h"
#include "recomp_net/ice.h"
#include "recomp_net/input.h"
#include "recomp_net/session.h"
#include "recomp_net/transport.h"
#include "recomp_net/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Library version: 0.1.0 */
#define RNET_VERSION_MAJOR 0
#define RNET_VERSION_MINOR 1
#define RNET_VERSION_PATCH 0

const char *rnet_version_string(void);

#ifdef __cplusplus
}
#endif

#endif /* RECOMP_NET_H */
