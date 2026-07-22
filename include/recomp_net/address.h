#ifndef RNET_ADDRESS_H
#define RNET_ADDRESS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RNET_IPV4_ADDRESS_TEXT_MAX 16
#define RNET_INTERFACE_LABEL_MAX 128

/* One usable local IPv4 unicast address and the OS-provided interface label.
 * Results are ordered for presentation: the default-route source first,
 * private LAN addresses next, then other unicast and link-local addresses. */
typedef struct RNetIpv4Address {
    char address[RNET_IPV4_ADDRESS_TEXT_MAX];
    char interface_label[RNET_INTERFACE_LABEL_MAX];
} RNetIpv4Address;

/* Enumerate usable local IPv4 addresses. Loopback is included as the final
 * choice for same-machine sessions, after all LAN-capable addresses.
 *
 * Returns the total number of available unique addresses, or -1 on an OS or
 * allocation error. Up to capacity entries are written to out. Passing NULL
 * with capacity 0 is a supported size query. Because interfaces can change
 * between calls, callers should tolerate a second result larger than the
 * queried size and retry when they require the complete list. */
int rnet_ipv4_enumerate(RNetIpv4Address *out, size_t capacity);

#ifdef __cplusplus
}
#endif

#endif /* RNET_ADDRESS_H */
