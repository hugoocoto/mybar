/* workspaces: Hyprland workspaces, the focused one highlighted, with the app
 * icons of their windows. Event driven through Hyprland's event socket, like
 * the window module.
 *   { path = "workspaces", format = "{name} {icons}", special = false, ... }
 * The options, placeholders and icons are shared with sway_workspaces: see
 * workspaces.h. special = true also lists special workspaces (scratchpads,
 * negative ids).
 * Disables itself when not running under Hyprland. */
#include "workspaces.h"
#include <sys/socket.h>
#include <sys/un.h>

struct st {
        char dir[256];
        int special;
        struct wsview v;
        char buf[4096]; /* partial event line */
        size_t len;
        char resp[262144]; /* reply to j/workspaces and j/clients */
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

/* Copy the JSON string value of "key" from json into out (unescaped). */
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
        while (*p && *p != '"' && o + 4 < size) {
                if (*p != '\\') {
                        out[o++] = *p++;
                        continue;
                }
                p++;
                switch (*p) {
                case 'n': case 't': case 'r': out[o++] = ' '; p++; break;
                case 'u': {
                        unsigned cp = (unsigned) strtoul((char[5]){ p[1], p[2], p[3], p[4], 0 }, NULL, 16);
                        p += 5;
                        if (cp >= 0xd800 && cp < 0xdc00 && p[0] == '\\' && p[1] == 'u') { /* surrogate pair */
                                unsigned lo = (unsigned) strtoul((char[5]){ p[2], p[3], p[4], p[5], 0 }, NULL, 16);
                                cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
                                p += 6;
                        }
                        if (cp < 0x80) out[o++] = (char) cp;
                        else if (cp < 0x800) { out[o++] = 0xc0 | (cp >> 6); out[o++] = 0x80 | (cp & 0x3f); }
                        else if (cp < 0x10000) { out[o++] = 0xe0 | (cp >> 12); out[o++] = 0x80 | ((cp >> 6) & 0x3f); out[o++] = 0x80 | (cp & 0x3f); }
                        else { out[o++] = 0xf0 | (cp >> 18); out[o++] = 0x80 | ((cp >> 12) & 0x3f); out[o++] = 0x80 | ((cp >> 6) & 0x3f); out[o++] = 0x80 | (cp & 0x3f); }
                        break;
                }
                default: if (*p) out[o++] = *p++; break; /* \" \\ \/ */
                }
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

/* Find the next top-level {...} in a JSON array, skipping braces inside
 * strings (window titles can contain anything). Terminates it in place and
 * returns its start; *next is where to continue. NULL when there are no more. */
static char *
next_object(char *p, char **next)
{
        while (*p && *p != '{') p++;
        if (!*p) return NULL;
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

/* Re-query Hyprland. Returns 1 if anything we show changed. */
static int
refresh(struct mod *m)
{
        struct st *s = m->state;
        int active   = s->v.active;
        char small[8192];

        if (hypr_request(s, "j/activeworkspace", small, sizeof small) > 0)
                active = json_int(small, "id", active);

        ws_begin(&s->v);
        if (hypr_request(s, "j/workspaces", s->resp, sizeof s->resp) > 0) {
                char *p = s->resp, *obj;
                while ((obj = next_object(p, &p))) {
                        int id = json_int(obj, "id", 0);
                        if (id == 0 || (id < 0 && !s->special)) continue;
                        char name[64];
                        json_string(obj, "name", name, sizeof name);
                        if (!strncmp(name, "special:", 8)) memmove(name, name + 8, strlen(name + 8) + 1);
                        ws_add(&s->v, id, name, json_int(obj, "windows", 0));
                }
        }
        if (s->v.fmt2 && hypr_request(s, "j/clients", s->resp, sizeof s->resp) > 0) {
                char *p = s->resp, *obj;
                while ((obj = next_object(p, &p))) {
                        if (strstr(obj, "\"mapped\": false") || strstr(obj, "\"mapped\":false")) continue;
                        char *wsobj = strstr(obj, "\"workspace\":");
                        if (!wsobj) continue;
                        char class[128];
                        json_string(obj, "class", class, sizeof class);
                        if (!class[0]) json_string(obj, "initialClass", class, sizeof class);
                        ws_add_window(&s->v, json_int(wsobj, "id", 0), class);
                }
        }
        return ws_commit(&s->v, active);
}

MOD_API int
mod_init(struct mod *m)
{
        const char *xdg = getenv("XDG_RUNTIME_DIR");
        const char *sig = getenv("HYPRLAND_INSTANCE_SIGNATURE");
        if (!xdg || !sig) {
                fprintf(stderr, "workspaces: not running under Hyprland\n");
                return -1;
        }
        struct st *s = calloc(1, sizeof *s);
        if (!s) return -1;
        snprintf(s->dir, sizeof s->dir, "%s/hypr/%s", xdg, sig);

        char path[320];
        snprintf(path, sizeof path, "%s/.socket2.sock", s->dir);
        int fd = unix_connect(path);
        if (fd < 0) {
                fprintf(stderr, "workspaces: cannot connect to %s\n", path);
                free(s);
                return -1;
        }
        fcntl(fd, F_SETFL, O_NONBLOCK);

        ws_options(m, &s->v);
        s->special     = m->host->opt_bool(m, "special", 0);
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
        ws_free(&s->v);
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
        static const char *const events[] = {
                "workspace>>", "workspacev2>>", "focusedmon>>", "focusedmonv2>>",
                "createworkspace", "destroyworkspace", "moveworkspace", "renameworkspace",
                "activespecial", "openwindow>>", "closewindow>>", "movewindow",
        };
        struct st *s = m->state;
        int need = 0;
        ssize_t n;

        while ((n = read(m->event_fd, s->buf + s->len, sizeof s->buf - 1 - s->len)) > 0) {
                s->len += (size_t) n;
                s->buf[s->len] = 0;
                char *line = s->buf, *nl;
                while ((nl = strchr(line, '\n'))) {
                        *nl = 0;
                        for (size_t i = 0; i < sizeof events / sizeof *events && !need; i++)
                                need = !strncmp(line, events[i], strlen(events[i]));
                        line = nl + 1;
                }
                s->len = strlen(line);
                memmove(s->buf, line, s->len + 1);
                if (s->len >= sizeof s->buf - 1) s->len = 0; /* absurdly long line: drop it */
        }
        if (n == 0) { /* Hyprland went away */
                close(m->event_fd);
                m->event_fd = -1;
                s->v.n      = 0;
                return 1;
        }
        return need ? refresh(m) : 0;
}

MOD_API int
mod_width(struct mod *m)
{
        return ws_width(m, &((struct st *) m->state)->v);
}

MOD_API void
mod_draw(struct mod *m, struct canvas *cv, int x)
{
        ws_draw(m, &((struct st *) m->state)->v, cv, x);
}
