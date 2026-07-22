# LAN address discovery

LAN hosts should bind their UDP socket to `0.0.0.0:port`, but advertise a
concrete local address that the other player can reach. `rnet_ipv4_enumerate`
provides the address choices and their OS interface labels:

```c
int count = rnet_ipv4_enumerate(NULL, 0);
RNetIpv4Address *addresses = NULL;
if (count > 0) {
    addresses = calloc((size_t)count, sizeof(*addresses));
    count = rnet_ipv4_enumerate(addresses, (size_t)count);
}
```

The function uses `GetAdaptersAddresses` on Windows and `getifaddrs` on POSIX.
It excludes unspecified, multicast, down interfaces, and Windows addresses that
have not completed duplicate-address detection. Duplicate addresses are
removed. Loopback remains available for two instances on one machine.

Results have a deterministic, useful presentation order: the source address
selected by the default IPv4 route, RFC 1918 private addresses, RFC 6598 shared
space, other unicast, RFC 3927 link-local, and finally loopback. Numeric address
and interface label break ties. The default-route probe only connects a UDP
socket; it does not send a packet. A host UI should still let the player choose,
because VPNs and virtual adapters can be either intentional or unrelated to the
peer's LAN.

The return value is the total available count even when the output capacity is
smaller. Interface state can change between a size query and the fill call, so
retry if a complete list is required and the second result exceeds capacity.

This API discovers local interface addresses only. A public NAT address and its
UDP port mapping require STUN/ICE, explicit port forwarding, or a trusted lobby
service that reports the observed endpoint.
