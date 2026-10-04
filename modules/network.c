/* network: the interface of the default route. Event driven through a netlink
 * socket (link / address / route changes); no external programs.
 *   { path = "network", wifi_format = "wifi {name} {signal}%",
 *     ethernet_format = "eth {name}", offline_format = "offline", interval = 10 }
 * placeholders: {name} (wifi SSID, else the interface) {iface} {ssid}
 *               {signal} (wifi link quality in %, else 0)
 * The interval only refreshes the wifi signal strength. */
#include "plugin.h"
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/wireless.h>
#include <sys/ioctl.h>
#include <sys/socket.h>

struct st {
        char *wifi_fmt, *eth_fmt, *off_fmt;
};

/* name of the interface carrying the default route (lowest metric) */
static int
default_iface(char *out, size_t size)
{
        FILE *f = fopen("/proc/net/route", "r");
        if (!f) return -1;
        char line[256];
        long best = -1;
        if (!fgets(line, sizeof line, f)) { /* header */
                fclose(f);
                return -1;
        }
        while (fgets(line, sizeof line, f)) {
                char name[32];
                unsigned long dest, gw, flags;
                int refcnt, use;
                long metric;
                if (sscanf(line, "%31s %lx %lx %lx %d %d %ld", name, &dest, &gw, &flags, &refcnt, &use, &metric) != 7)
                        continue;
                if (dest != 0 || !(flags & 1)) continue; /* default route, RTF_UP */
                if (best < 0 || metric < best) {
                        best = metric;
                        snprintf(out, size, "%s", name);
                }
        }
        fclose(f);
        return best < 0 ? -1 : 0;
}

static int
is_wifi(const char *iface)
{
        char p[128];
        snprintf(p, sizeof p, "/sys/class/net/%.40s/wireless", iface);
        if (access(p, F_OK) == 0) return 1;
        snprintf(p, sizeof p, "/sys/class/net/%.40s/phy80211", iface);
        return access(p, F_OK) == 0;
}

/* link quality (0..100) from /proc/net/wireless */
static int
wifi_signal(const char *iface)
{
        FILE *f = fopen("/proc/net/wireless", "r");
        if (!f) return 0;
        char line[256];
        int pct   = 0;
        size_t il = strlen(iface);
        while (fgets(line, sizeof line, f)) {
                char *p = line;
                while (*p == ' ')
                        p++;
                if (strncmp(p, iface, il) || p[il] != ':') continue;
                int status;
                double q;
                if (sscanf(p + il + 1, "%d %lf", &status, &q) == 2) pct = (int) (q * 100.0 / 70.0 + 0.5);
                break;
        }
        fclose(f);
        return pct > 100 ? 100 : pct;
}

/* SSID of the network the interface is associated with ("" if none) */
static void
wifi_ssid(const char *iface, char *out, size_t size)
{
        char essid[IW_ESSID_MAX_SIZE + 1] = { 0 };
        struct iwreq wr                   = { 0 };
        out[0]                            = 0;
        int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        if (fd < 0) return;
        snprintf(wr.ifr_name, sizeof wr.ifr_name, "%s", iface);
        wr.u.essid.pointer = essid;
        wr.u.essid.length  = IW_ESSID_MAX_SIZE;
        if (ioctl(fd, SIOCGIWESSID, &wr) == 0) snprintf(out, size, "%.*s", (int) wr.u.essid.length, essid);
        close(fd);
}

MOD_API int
mod_init(struct mod *m)
{
        struct st *s = calloc(1, sizeof *s);
        if (!s) return -1;
        s->wifi_fmt = strdup(m->host->opt_str(m, "wifi_format", "wifi {name} {signal}%"));
        s->eth_fmt  = strdup(m->host->opt_str(m, "ethernet_format", "eth {name}"));
        s->off_fmt  = strdup(m->host->opt_str(m, "offline_format", "offline"));
        m->state    = s;

        int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC | SOCK_NONBLOCK, NETLINK_ROUTE);
        if (fd >= 0) {
                struct sockaddr_nl sa = {
                        .nl_family = AF_NETLINK,
                        .nl_groups = RTMGRP_LINK | RTMGRP_IPV4_IFADDR | RTMGRP_IPV4_ROUTE | RTMGRP_IPV6_ROUTE,
                };
                if (bind(fd, (struct sockaddr *) &sa, sizeof sa) < 0) {
                        close(fd);
                        fd = -1;
                }
        }
        m->event_fd    = fd;
        m->interval_ms = fd >= 0 ? 10000 : 3000;
        return 0;
}

MOD_API void
mod_destroy(struct mod *m)
{
        struct st *s = m->state;
        if (m->event_fd >= 0) close(m->event_fd);
        free(s->wifi_fmt);
        free(s->eth_fmt);
        free(s->off_fmt);
        free(s);
}

MOD_API int
mod_update(struct mod *m)
{
        struct st *s = m->state;
        char iface[32], sig[16] = "0", ssid[IW_ESSID_MAX_SIZE + 1] = "";
        const char *fmt;

        if (default_iface(iface, sizeof iface) < 0) {
                iface[0] = 0;
                fmt      = s->off_fmt;
        } else if (is_wifi(iface)) {
                snprintf(sig, sizeof sig, "%d", wifi_signal(iface));
                wifi_ssid(iface, ssid, sizeof ssid);
                fmt = s->wifi_fmt;
        } else {
                fmt = s->eth_fmt;
        }

        const char *keys[] = { "name", "iface", "ssid", "signal" };
        const char *vals[] = { ssid[0] ? ssid : iface, iface, ssid, sig };
        char out[sizeof m->text];
        mod_format(out, sizeof out, fmt, keys, vals, 4);
        return mod_set_text(m, "%s", out);
}

MOD_API int
mod_event(struct mod *m)
{
        char buf[8192];
        ssize_t n;
        int got = 0;
        while ((n = recv(m->event_fd, buf, sizeof buf, 0)) > 0)
                got = 1; /* drain */
        return got ? mod_update(m) : 0;
}
