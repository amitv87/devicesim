#ifndef NET_RA6D_H
#define NET_RA6D_H

/* In-process IPv6 Router Advertisement injector.
 *
 * Ported from the standalone `ra6d` host tool (vy_os tools/ra6d) to run on the
 * simulator's own io event loop — a repeating timer plus a registered raw
 * ICMPv6 socket — instead of as a separate daemon. The AT engine starts it when
 * pppd spawns and stops it when pppd dies, so a PPPoS client gets a SLAAC /64
 * (and a working return route) for the whole life of the link, with nothing to
 * launch by hand.
 *
 * Why it exists: macOS pppd's +ipv6 negotiates only link-local on ppp0 and never
 * sends Router Advertisements, so the client never auto-configures a global/ULA
 * address. ra6d multicasts an RA (and answers Router Solicitations) advertising
 * a /64 with the on-link + autonomous flags, and keeps a return route for the
 * prefix pointed at the interface (pppd recreates ppp0 each handshake and the
 * kernel drops the route with it, so the route is re-asserted every tick).
 *
 * For real IPv6 internet, pair the ULA prefix with NAT66 on the uplink:
 *   echo 'nat on en0 inet6 from fd00:cafe:1::/64 to any -> (en0)' | sudo pfctl -E -f -
 * (host net.inet6.ip6.forwarding must be 1).
 *
 * Needs root (raw ICMPv6 socket + route table) — the same privilege pppd needs.
 * Both calls are idempotent: start replaces any running instance, stop on an
 * idle daemon is a no-op. */

void ra6d_start(const char* ifname, const char* prefix);
void ra6d_stop(void);

#endif
