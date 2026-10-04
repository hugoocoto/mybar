/* vpn: shows while a VPN tunnel is up (snx, OpenVPN, WireGuard, ...): a
 * network interface whose name starts with one of `prefixes`, up and running.
 * Event driven through netlink link changes, like the network module.
 *   { path = "vpn", format = "vpn", prefixes = "tun,tap,wg,ppp,vpn,nordlynx,proton" }
 * placeholders: {iface} (the first one up) {count}
 * Hidden when no VPN is up. */
#include "plugin.h"
#include <dirent.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <sys/socket.h>

struct st {
        char *fmt, *prefixes;
};

static int
has_prefix(const char *name, const char *prefixes)
{
        for (const char *p = prefixes; *p;) {
                size_t l = strcspn(p, ",");
                while (l && *p == ' ') p++, l--;
                if (l && !strncmp(name, p, l)) return 1;
                p += l + (p[l] == ',');
        }
        return 0;
}

MOD_API int
mod_init(struct mod *m)
{
        struct st *s = calloc(1, sizeof *s);
        if (!s) return -1;
        s->fmt      = strdup(m->host->opt_str(m, "format", "vpn"));
        s->prefixes = strdup(m->host->opt_str(m, "prefixes", "tun,tap,wg,ppp,vpn,nordlynx,proton"));
        m->state    = s;

        int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC | SOCK_NONBLOCK, NETLINK_ROUTE);
        if (fd >= 0) {
                struct sockaddr_nl sa = { .nl_family = AF_NETLINK, .nl_groups = RTMGRP_LINK };
                if (bind(fd, (struct sockaddr *) &sa, sizeof sa) < 0) {
                        close(fd);
                        fd = -1;
                }
        }
        m->event_fd    = fd;
        m->interval_ms = fd >= 0 ? 0 : 3000; /* no netlink: poll */
        return 0;
}

MOD_API void
mod_destroy(struct mod *m)
{
        struct st *s = m->state;
        if (m->event_fd >= 0) close(m->event_fd);
        free(s->fmt);
        free(s->prefixes);
        free(s);
}

MOD_API int
mod_update(struct mod *m)
{
        struct st *s = m->state;
        char first[64] = "";
        int count      = 0;

        DIR *d = opendir("/sys/class/net");
        if (d) {
                struct dirent *e;
                while ((e = readdir(d))) {
                        if (e->d_name[0] == '.' || !has_prefix(e->d_name, s->prefixes)) continue;
                        /* up, and not without carrier: operstate is "up", or "unknown"
                         * for tun devices (sysfs flags never show IFF_RUNNING) */
                        char p[300], flags[32], state[32] = "";
                        snprintf(p, sizeof p, "/sys/class/net/%.200s/flags", e->d_name);
                        if (mod_read_file(p, flags, sizeof flags) <= 0) continue;
                        if (!(strtoul(flags, NULL, 16) & IFF_UP)) continue;
                        snprintf(p, sizeof p, "/sys/class/net/%.200s/operstate", e->d_name);
                        mod_read_file(p, state, sizeof state);
                        if (strcmp(state, "up") && strcmp(state, "unknown")) continue;
                        if (!count++) snprintf(first, sizeof first, "%.63s", e->d_name);
                }
                closedir(d);
        }
        if (!count) return mod_set_text(m, "%s", "");

        char cnt[16];
        snprintf(cnt, sizeof cnt, "%d", count);
        const char *keys[] = { "iface", "count" };
        const char *vals[] = { first, cnt };
        char out[sizeof m->text];
        mod_format(out, sizeof out, s->fmt, keys, vals, 2);
        return mod_set_text(m, "%s", out);
}

MOD_API int
mod_event(struct mod *m)
{
        char buf[8192];
        int got = 0;
        while (recv(m->event_fd, buf, sizeof buf, 0) > 0) got = 1; /* drain */
        return got ? mod_update(m) : 0;
}
