/* language: keyboard layout of the main keyboard. Hyprland only; event driven
 * through Hyprland's event socket, like the window module.
 *   { path = "language", format = "{short}" }
 * placeholders: {short} (xkb layout, e.g. "us") {long} (e.g. "English (US)")
 * Disables itself when not running under Hyprland. */
#include "plugin.h"
#include <sys/socket.h>
#include <sys/un.h>

struct st {
        char dir[256];
        char *fmt;
        char buf[4096]; /* partial event line */
        size_t len;
        char resp[65536]; /* reply to j/devices */
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

/* Send req to Hyprland's command socket and read the whole reply into out.
 * Returns the length, or -1. */
static int
hypr_request(struct st *s, const char *req, char *out, size_t size)
{
        char path[320];
        snprintf(path, sizeof path, "%s/.socket.sock", s->dir);
        int fd = unix_connect(path);
        if (fd < 0) return -1;
        size_t n = 0, rl = strlen(req);
        if (write(fd, req, rl) == (ssize_t) rl) {
                for (;;) {
                        struct pollfd pf = { .fd = fd, .events = POLLIN };
                        if (poll(&pf, 1, 300) <= 0) break;
                        ssize_t k = read(fd, out + n, size - 1 - n);
                        if (k <= 0 || (n += (size_t) k) >= size - 1) break;
                }
        }
        out[n] = 0;
        close(fd);
        return (int) n;
}

/* Copy the JSON string value of "key" from json into out (escapes kept as
 * is: layout names are plain ASCII). */
static void
json_string(const char *json, const char *key, char *out, size_t size)
{
        char pat[64];
        out[0] = 0;
        snprintf(pat, sizeof pat, "\"%s\":", key);
        const char *p = strstr(json, pat);
        if (!p) return;
        p += strlen(pat);
        while (*p == ' ') p++;
        if (*p != '"') return;
        p++;
        size_t o = 0;
        while (*p && *p != '"' && o + 1 < size) {
                if (*p == '\\' && p[1]) p++;
                out[o++] = *p++;
        }
        out[o] = 0;
}

/* The JSON integer value of "key" in json, or def. */
static int
json_int(const char *json, const char *key, int def)
{
        char pat[64];
        snprintf(pat, sizeof pat, "\"%s\":", key);
        const char *p = strstr(json, pat);
        if (!p) return def;
        char *end;
        long v = strtol(p + strlen(pat), &end, 10);
        return end == p + strlen(pat) ? def : (int) v;
}

/* Find the next top-level {...}, skipping braces inside strings. Terminates
 * it in place and returns its start; *next is where to continue. */
static char *
next_object(char *p, char **next)
{
        while (*p && *p != '{' && *p != ']') p++;
        if (*p != '{') return NULL; /* end of the array */
        char *start = p;
        int depth = 0, in_str = 0;
        for (; *p; p++) {
                if (in_str) {
                        if (*p == '\\' && p[1]) p++;
                        else if (*p == '"') in_str = 0;
                } else if (*p == '"') in_str = 1;
                else if (*p == '{') depth++;
                else if (*p == '}' && --depth == 0) {
                        *p    = 0;
                        *next = p + 1;
                        return start;
                }
        }
        return NULL; /* truncated reply */
}

static int
refresh(struct mod *m)
{
        struct st *s = m->state;
        char layouts[256] = "", lng[128] = "", shrt[64] = "";

        if (hypr_request(s, "j/devices", s->resp, sizeof s->resp) <= 0) return mod_set_text(m, "%s", "");
        char *p = strstr(s->resp, "\"keyboards\"");
        if (!p) return mod_set_text(m, "%s", "");

        /* the main keyboard, else the first one */
        char *obj, *first = NULL;
        int idx = 0;
        while ((obj = next_object(p, &p))) {
                if (!first) first = obj;
                if (strstr(obj, "\"main\": true") || strstr(obj, "\"main\":true")) {
                        first = obj;
                        break;
                }
        }
        if (first) {
                json_string(first, "layout", layouts, sizeof layouts);
                json_string(first, "active_keymap", lng, sizeof lng);
                idx = json_int(first, "active_layout_index", 0);
        }

        /* "us,es" + index 1 -> "es" */
        char *l = layouts;
        for (int i = 0; i < idx && l; i++) {
                l = strchr(l, ',');
                if (l) l++;
        }
        if (l) snprintf(shrt, sizeof shrt, "%.*s", (int) strcspn(l, ","), l);

        const char *keys[] = { "short", "long" };
        const char *vals[] = { shrt, lng };
        char out[sizeof m->text];
        mod_format(out, sizeof out, s->fmt, keys, vals, 2);
        return mod_set_text(m, "%s", out);
}

MOD_API int
mod_init(struct mod *m)
{
        const char *xdg = getenv("XDG_RUNTIME_DIR");
        const char *sig = getenv("HYPRLAND_INSTANCE_SIGNATURE");
        if (!xdg || !sig) {
                fprintf(stderr, "language: not running under Hyprland\n");
                return -1;
        }
        struct st *s = calloc(1, sizeof *s);
        if (!s) return -1;
        snprintf(s->dir, sizeof s->dir, "%s/hypr/%s", xdg, sig);

        char path[320];
        snprintf(path, sizeof path, "%s/.socket2.sock", s->dir);
        int fd = unix_connect(path);
        if (fd < 0) {
                fprintf(stderr, "language: cannot connect to %s\n", path);
                free(s);
                return -1;
        }
        fcntl(fd, F_SETFL, O_NONBLOCK);

        s->fmt         = strdup(m->host->opt_str(m, "format", "{short}"));
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

MOD_API int
mod_update(struct mod *m)
{
        return refresh(m);
}

MOD_API int
mod_event(struct mod *m)
{
        struct st *s = m->state;
        int need = 0;
        ssize_t n;

        while ((n = read(m->event_fd, s->buf + s->len, sizeof s->buf - 1 - s->len)) > 0) {
                s->len += (size_t) n;
                s->buf[s->len] = 0;
                char *line = s->buf, *nl;
                while ((nl = strchr(line, '\n'))) {
                        *nl = 0;
                        if (!strncmp(line, "activelayout>>", 14) || !strncmp(line, "configreloaded", 14))
                                need = 1;
                        line = nl + 1;
                }
                s->len = strlen(line);
                memmove(s->buf, line, s->len + 1);
                if (s->len >= sizeof s->buf - 1) s->len = 0; /* absurdly long line: drop it */
        }
        if (n == 0) { /* Hyprland went away */
                close(m->event_fd);
                m->event_fd = -1;
                return mod_set_text(m, "%s", "");
        }
        return need ? refresh(m) : 0;
}
