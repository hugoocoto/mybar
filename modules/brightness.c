/* brightness: screen backlight from /sys/class/backlight. Event driven through
 * kernel uevents: the backlight sends one on every change (brightness keys,
 * brightnessctl, ...), so there is no polling.
 *   { path = "brightness", format = "bri {pct}%", device = "intel_backlight" }
 * placeholders: {pct}
 * device defaults to the first backlight. With no backlight (desktop) the
 * module disables itself. */
#include "plugin.h"
#include <dirent.h>
#include <limits.h>
#include <linux/netlink.h>
#include <sys/socket.h>

struct st {
        char dir[PATH_MAX];
        char *fmt;
};

static int
find_backlight(const char *want, char *out, size_t size)
{
        const char *base = "/sys/class/backlight";
        if (want && *want) {
                snprintf(out, size, "%s/%s", base, want);
                return access(out, R_OK);
        }
        DIR *d = opendir(base);
        if (!d) return -1;
        int found = -1;
        struct dirent *e;
        while (found && (e = readdir(d))) {
                if (e->d_name[0] == '.') continue;
                snprintf(out, size, "%s/%.200s", base, e->d_name);
                found = 0;
        }
        closedir(d);
        return found;
}

MOD_API int
mod_init(struct mod *m)
{
        struct st *s = calloc(1, sizeof *s);
        if (!s) return -1;
        if (find_backlight(m->host->opt_str(m, "device", NULL), s->dir, sizeof s->dir)) {
                fprintf(stderr, "brightness: no backlight found\n");
                free(s);
                return -1;
        }
        s->fmt   = strdup(m->host->opt_str(m, "format", "bri {pct}%"));
        m->state = s;

        int fd = socket(AF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, NETLINK_KOBJECT_UEVENT);
        if (fd >= 0) {
                struct sockaddr_nl sa = { .nl_family = AF_NETLINK, .nl_groups = 1 /* kernel events */ };
                if (bind(fd, (struct sockaddr *) &sa, sizeof sa) < 0) {
                        close(fd);
                        fd = -1;
                }
        }
        m->event_fd    = fd;
        m->interval_ms = fd >= 0 ? 0 : 1000; /* no uevents: poll */
        return 0;
}

MOD_API void
mod_destroy(struct mod *m)
{
        struct st *s = m->state;
        if (m->event_fd >= 0) close(m->event_fd);
        free(s->fmt);
        free(s);
}

MOD_API int
mod_update(struct mod *m)
{
        struct st *s = m->state;
        char p[PATH_MAX + 32], cur[32], max[32];

        snprintf(p, sizeof p, "%s/actual_brightness", s->dir);
        if (mod_read_file(p, cur, sizeof cur) <= 0) {
                snprintf(p, sizeof p, "%s/brightness", s->dir);
                if (mod_read_file(p, cur, sizeof cur) <= 0) return mod_set_text(m, "%s", "");
        }
        snprintf(p, sizeof p, "%s/max_brightness", s->dir);
        if (mod_read_file(p, max, sizeof max) <= 0 || atol(max) <= 0) return mod_set_text(m, "%s", "");

        char pct[16];
        snprintf(pct, sizeof pct, "%ld", (atol(cur) * 100 + atol(max) / 2) / atol(max));
        const char *keys[] = { "pct" };
        const char *vals[] = { pct };
        char out[sizeof m->text];
        mod_format(out, sizeof out, s->fmt, keys, vals, 1);
        return mod_set_text(m, "%s", out);
}

MOD_API int
mod_event(struct mod *m)
{
        char buf[8192];
        ssize_t n;
        int relevant = 0;
        /* a uevent is "ACTION@devpath\0KEY=value\0..."; only backlight ones matter */
        while ((n = recv(m->event_fd, buf, sizeof buf, 0)) > 0)
                if (memmem(buf, (size_t) n, "SUBSYSTEM=backlight", 19)) relevant = 1;
        return relevant ? mod_update(m) : 0;
}
