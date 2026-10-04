/* submap: the active Hyprland submap (a keybind mode such as "resize"). Event
 * driven through Hyprland's event socket, like the window module.
 *   { path = "submap", format = "{name}" }
 * placeholders: {name}
 * Hidden outside of submaps. Disables itself when not running under Hyprland. */
#include "plugin.h"
#include <sys/socket.h>
#include <sys/un.h>

struct st {
        char dir[256];
        char *fmt;
        char buf[4096]; /* partial event line */
        size_t len;
};

static int
unix_connect(const char *path)
{
        int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0) return -1;
        struct sockaddr_un sa = { .sun_family = AF_UNIX };
        snprintf(sa.sun_path, sizeof sa.sun_path, "%s", path);
        if (connect(fd, (struct sockaddr *) &sa, sizeof sa) < 0) {
                close(fd);
                return -1;
        }
        return fd;
}

static int
show(struct mod *m, const char *name)
{
        struct st *s = m->state;
        if (!*name || !strcmp(name, "default")) return mod_set_text(m, "%s", "");
        const char *keys[] = { "name" };
        const char *vals[] = { name };
        char out[sizeof m->text];
        mod_format(out, sizeof out, s->fmt, keys, vals, 1);
        return mod_set_text(m, "%s", out);
}

MOD_API int
mod_init(struct mod *m)
{
        const char *xdg = getenv("XDG_RUNTIME_DIR");
        const char *sig = getenv("HYPRLAND_INSTANCE_SIGNATURE");
        if (!xdg || !sig) {
                fprintf(stderr, "submap: not running under Hyprland\n");
                return -1;
        }
        struct st *s = calloc(1, sizeof *s);
        if (!s) return -1;
        snprintf(s->dir, sizeof s->dir, "%s/hypr/%s", xdg, sig);

        char path[320];
        snprintf(path, sizeof path, "%s/.socket2.sock", s->dir);
        int fd = unix_connect(path);
        if (fd < 0) {
                fprintf(stderr, "submap: cannot connect to %s\n", path);
                free(s);
                return -1;
        }
        fcntl(fd, F_SETFL, O_NONBLOCK);

        s->fmt         = strdup(m->host->opt_str(m, "format", "{name}"));
        m->state       = s;
        m->event_fd    = fd;
        m->interval_ms = 0; /* purely event driven */
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

/* Ask Hyprland once (a config reload may happen while inside a submap). */
MOD_API int
mod_update(struct mod *m)
{
        struct st *s = m->state;
        char path[320], resp[256] = "";
        snprintf(path, sizeof path, "%s/.socket.sock", s->dir);
        int fd = unix_connect(path);
        if (fd < 0) return show(m, "");
        size_t n = 0;
        if (write(fd, "submap", 6) == 6) {
                for (;;) {
                        struct pollfd pf = { .fd = fd, .events = POLLIN };
                        if (poll(&pf, 1, 300) <= 0) break;
                        ssize_t k = read(fd, resp + n, sizeof resp - 1 - n);
                        if (k <= 0 || (n += (size_t) k) >= sizeof resp - 1) break;
                }
        }
        close(fd);
        resp[n] = 0;
        resp[strcspn(resp, "\n")] = 0;
        return show(m, resp);
}

MOD_API int
mod_event(struct mod *m)
{
        struct st *s = m->state;
        int changed  = 0;
        ssize_t n;

        while ((n = read(m->event_fd, s->buf + s->len, sizeof s->buf - 1 - s->len)) > 0) {
                s->len += (size_t) n;
                s->buf[s->len] = 0;
                char *line = s->buf, *nl;
                while ((nl = strchr(line, '\n'))) {
                        *nl = 0;
                        if (!strncmp(line, "submap>>", 8) && show(m, line + 8)) changed = 1;
                        line = nl + 1;
                }
                s->len = strlen(line);
                memmove(s->buf, line, s->len + 1);
                if (s->len >= sizeof s->buf - 1) s->len = 0; /* absurdly long line: drop it */
        }
        if (n == 0) { /* Hyprland went away */
                close(m->event_fd);
                m->event_fd = -1;
                return show(m, "") || changed;
        }
        return changed;
}
