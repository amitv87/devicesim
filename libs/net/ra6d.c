#include "ra6d.h"
#include "../common.h"
#include "../util/io.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/icmp6.h>
#include <arpa/inet.h>

#define RA6D_INTERVAL_MS   3000
#define RA6D_RTR_LIFETIME  1800

/* RA = router-advert header + a Prefix Information option + an MTU option */
struct ra_msg {
  struct nd_router_advert   ra;
  struct nd_opt_prefix_info pi;
  struct nd_opt_mtu         mtu;
} __attribute__((packed));

/* One PPP link at a time (the modem dials a single pppd), so a single instance
 * suffices. The timer + handle live here so io_rem_timer/io_dereg_handle on
 * stop unhook the exact nodes we registered. */
static struct {
  bool        running;
  int         fd;
  unsigned    ifidx;                 /* last resolved index; 0 => iface down */
  char        ifname[IFNAMSIZ];
  char        prefix[64];            /* string form, for route + logging */
  struct sockaddr_in6 all_nodes;     /* ff02::1, scope set once iface resolves */
  struct ra_msg ra;
  io_handle_t handle;
  io_timer_t  timer;
} g;

static void build_ra(const struct in6_addr* prefix, uint16_t rtr_life){
  struct ra_msg* m = &g.ra;
  memset(m, 0, sizeof *m);

  m->ra.nd_ra_type            = ND_ROUTER_ADVERT;
  m->ra.nd_ra_code            = 0;
  m->ra.nd_ra_curhoplimit     = 64;
  m->ra.nd_ra_flags_reserved  = 0;                 /* not managed/other (pure SLAAC) */
  m->ra.nd_ra_router_lifetime = htons(rtr_life);   /* >0 => we are a default router */
  m->ra.nd_ra_reachable       = 0;
  m->ra.nd_ra_retransmit      = 0;

  m->pi.nd_opt_pi_type           = ND_OPT_PREFIX_INFORMATION;
  m->pi.nd_opt_pi_len            = 4;              /* units of 8 bytes => 32 */
  m->pi.nd_opt_pi_prefix_len     = 64;
  m->pi.nd_opt_pi_flags_reserved = ND_OPT_PI_FLAG_ONLINK | ND_OPT_PI_FLAG_AUTO;
  m->pi.nd_opt_pi_valid_time     = htonl(86400);
  m->pi.nd_opt_pi_preferred_time = htonl(14400);
  m->pi.nd_opt_pi_prefix         = *prefix;

  m->mtu.nd_opt_mtu_type = ND_OPT_MTU;
  m->mtu.nd_opt_mtu_len  = 1;
  m->mtu.nd_opt_mtu_mtu  = htonl(1500);
}

/* True if a route to `dst` currently egresses interface `ifn`. We ask the
 * kernel (route get) rather than tracking our own install state, because macOS
 * destroys+recreates the PPP interface on every handshake — often reusing the
 * same index — and the kernel drops the route with it. Polling the real route
 * table is the only reliable way to notice it vanished. */
static int route_via(const char* dst, const char* ifn){
  char cmd[256];
  snprintf(cmd, sizeof cmd, "/sbin/route -n get -inet6 %s 2>/dev/null", dst);
  FILE* f = popen(cmd, "r");
  if(!f) return 0;
  char line[256]; int ok = 0;
  while(fgets(line, sizeof line, f)){
    char* p = strstr(line, "interface:");
    if(p && strstr(p, ifn)){ ok = 1; break; }
  }
  pclose(f);
  return ok;
}

/* The per-tick body, shared by the timer and by Router Solicitation arrivals:
 * (re)resolve the interface, ensure the return route, multicast one RA. */
static void advertise(void){
  /* The interface may not exist yet, or its index may change across PPP
   * sessions. Resolve it each round. */
  unsigned now = if_nametoindex(g.ifname);
  if(now == 0){
    /* Interface gone (between PPP sessions). Reset so the re-attach + route
     * re-install fires when it returns. Log only on the down transition. */
    if(g.ifidx != 0){ LOG("ra6d: %s went down — waiting", g.ifname); g.ifidx = 0; }
    return;
  }
  if(now != g.ifidx){
    g.ifidx = now;
    setsockopt(g.fd, IPPROTO_IPV6, IPV6_MULTICAST_IF, &g.ifidx, sizeof g.ifidx);
    g.all_nodes.sin6_scope_id = g.ifidx;
    LOG("ra6d: %s up (idx %u) — advertising", g.ifname, g.ifidx);
  }

  /* Ensure the return route EVERY tick, not just on index change. macOS
   * destroys+recreates ppp0 on each PPP handshake (often reusing the index) and
   * the kernel drops this route with the interface — so an edge-triggered
   * install keyed on the index would miss same-index recreation and the route
   * would silently stay gone. Without it the host un-NATs replies to the ULA but
   * has no route back to the PPP client, misrouting them to the default gateway
   * (which rejects the ULA). Re-assert idempotently: (re)install only when the
   * prefix doesn't currently egress our interface — no churn, and a log line
   * each time it actually had to heal. */
  if(!route_via(g.prefix, g.ifname)){
    char cmd[256];
    snprintf(cmd, sizeof cmd,
             "/sbin/route -q -n delete -inet6 %s/%d -interface %s 2>/dev/null; "
             "/sbin/route -q -n add -inet6 %s/%d -interface %s",
             g.prefix, 64, g.ifname, g.prefix, 64, g.ifname);
    if(system(cmd) == 0){ LOG("ra6d: route %s/64 -> %s installed", g.prefix, g.ifname); }
    else{ LOG("ra6d: WARN route add for %s/64 failed (add it manually)", g.prefix); }
  }

  if(sendto(g.fd, &g.ra, sizeof g.ra, 0,
            (struct sockaddr*)&g.all_nodes, sizeof g.all_nodes) < 0)
    LOG("ra6d: sendto RA failed: %s", strerror(errno));
}

static void ra6d_tick(io_timer_t* t){ (void)t; advertise(); }

static void ra6d_on_fd(io_handle_t* h, uint8_t fd_mode_mask){
  (void)h;
  if(!(fd_mode_mask & FD_READ)) return;
  /* The socket is filtered to Router Solicitations (type 133); on one, answer
   * promptly by re-advertising rather than waiting for the next timer tick. */
  uint8_t buf[1500];
  struct sockaddr_in6 from; socklen_t fl = sizeof from;
  ssize_t n = recvfrom(g.fd, buf, sizeof buf, 0, (struct sockaddr*)&from, &fl);
  if(n > 0){
    char fs[64]; inet_ntop(AF_INET6, &from.sin6_addr, fs, sizeof fs);
    LOG("ra6d: Router Solicitation from %s -> replying", fs);
    advertise();
  } else if(n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR){
    LOG("ra6d: recvfrom failed: %s", strerror(errno));
  }
}

void ra6d_start(const char* ifname, const char* prefix){
  ra6d_stop();   /* replace any running instance */

  struct in6_addr pfx;
  if(inet_pton(AF_INET6, prefix, &pfx) != 1){ LOG("ra6d: bad prefix: %s", prefix); return; }

  int s = socket(AF_INET6, SOCK_RAW, IPPROTO_ICMPV6);
  if(s < 0){ LOG("ra6d: socket failed (need root): %s", strerror(errno)); return; }

  /* RAs MUST go out with hop limit 255 (receivers verify it). */
  int hops = 255;
  setsockopt(s, IPPROTO_IPV6, IPV6_MULTICAST_HOPS, &hops, sizeof hops);
  setsockopt(s, IPPROTO_IPV6, IPV6_UNICAST_HOPS,   &hops, sizeof hops);
  /* let the kernel compute the ICMPv6 checksum (offset 2 in the message) */
  int off = 2;
  setsockopt(s, IPPROTO_IPV6, IPV6_CHECKSUM, &off, sizeof off);
  /* only wake for Router Solicitations (type 133) */
  struct icmp6_filter filt;
  ICMP6_FILTER_SETBLOCKALL(&filt);
  ICMP6_FILTER_SETPASS(ND_ROUTER_SOLICIT, &filt);
  setsockopt(s, IPPROTO_ICMPV6, ICMP6_FILTER, &filt, sizeof filt);
  /* non-blocking: the loop only calls our cb when the fd is readable */
  fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);

  g.fd    = s;
  g.ifidx = 0;
  snprintf(g.ifname, sizeof g.ifname, "%s", ifname);
  snprintf(g.prefix, sizeof g.prefix, "%s", prefix);

  g.all_nodes = (struct sockaddr_in6){ .sin6_family = AF_INET6 };
  inet_pton(AF_INET6, "ff02::1", &g.all_nodes.sin6_addr);

  build_ra(&pfx, RA6D_RTR_LIFETIME);

  g.handle = (io_handle_t){ .fd = s, .cb = ra6d_on_fd, .fd_mode_mask = FD_READ };
  io_reg_handle(&g.handle);

  /* repeat + run_now => first RA/route fires immediately, then every interval */
  g.timer = (io_timer_t){ .repeat = true, .run_now = true,
                          .interval_ms = RA6D_INTERVAL_MS, .cb = ra6d_tick };
  io_add_timer(&g.timer);

  g.running = true;
  LOG("ra6d: advertising %s/64 on %s every %dms (started by pppd spawn)",
      g.prefix, g.ifname, RA6D_INTERVAL_MS);
}

void ra6d_stop(void){
  if(!g.running) return;
  io_rem_timer(&g.timer);
  io_dereg_handle(&g.handle);
  if(g.fd >= 0) close(g.fd);
  g.fd      = -1;
  g.ifidx   = 0;
  g.running = false;
  LOG("ra6d: stopped (pppd gone)");
}
