/* window: title of the focused window. Hyprland only; event driven through
 * Hyprland's event socket ($XDG_RUNTIME_DIR/hypr/$HYPRLAND_INSTANCE_SIGNATURE/.socket2.sock).
 *   { path = "window", format = "{title}", max_chars = 60 }
 * placeholders: {title} {class}
 * Hidden when no window is focused.
 * Disables itself when not running under Hyprland. */
#include "plugin.h"
#include <sys/socket.h>
#include <sys/un.h>

struct st {
        char dir[256];
        char *fmt;
        int max_chars;
        char buf[4096]; /* partial event line */
        size_t len;
        pid_t pid;
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

/* Keep at most max code points; end with "..." if cut. */
static void
truncate_utf8(char *s, int max)
{
        int chars = 0;
        for (char *p = s; *p; p++) {
                if (((unsigned char) *p & 0xc0) == 0x80) continue; /* continuation byte */
                if (++chars > max) {
                        strcpy(p, "\xe2\x80\xa6"); /* U+2026 */
                        return;
                }
        }
}

static int
refresh(struct mod *m)
{
        struct st *s = m->state;
        char path[320], resp[8192], title[256] = "", class[128] = "";

        snprintf(path, sizeof path, "%s/.socket.sock", s->dir);
        int fd = unix_connect(path);
        if (fd < 0) return mod_set_text(m, "%s", "");

        static const char req[] = "j/activewindow";
        size_t n = 0;
        if (write(fd, req, sizeof req - 1) == (ssize_t) sizeof req - 1) {
                for (;;) {
                        struct pollfd pf = { .fd = fd, .events = POLLIN };
                        if (poll(&pf, 1, 300) <= 0) break;
                        ssize_t k = read(fd, resp + n, sizeof resp - 1 - n);
                        if (k <= 0 || (n += (size_t) k) >= sizeof resp - 1) break;
                }
        }
        resp[n] = 0;
        close(fd);

        json_string(resp, "title", title, sizeof title);
        json_string(resp, "class", class, sizeof class);
        if (!title[0] && !class[0]) return mod_set_text(m, "%s", "");
        truncate_utf8(title, s->max_chars);

        const char *keys[] = { "title", "class" };
        const char *vals[] = { title, class };
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
                fprintf(stderr, "window: not running under Hyprland\n");
                return -1;
        }
        struct st *s = calloc(1, sizeof *s);
        if (!s) return -1;
        snprintf(s->dir, sizeof s->dir, "%s/hypr/%s", xdg, sig);

        char path[320];
        snprintf(path, sizeof path, "%s/.socket2.sock", s->dir);
        int fd = unix_connect(path);
        if (fd < 0) {
                fprintf(stderr, "window: cannot connect to %s\n", path);
                free(s);
                return -1;
        }
        fcntl(fd, F_SETFL, O_NONBLOCK);

        s->fmt       = strdup(m->host->opt_str(m, "format", "{title}"));
        s->max_chars = m->host->opt_int(m, "max_chars", 60);
        m->state     = s;
        m->event_fd  = fd;
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
                        if (!strncmp(line, "activewindow>>", 14) || !strncmp(line, "activewindowv2>>", 16) ||
                            !strncmp(line, "windowtitle>>", 13) || !strncmp(line, "windowtitlev2>>", 15) ||
                            !strncmp(line, "closewindow>>", 13) || !strncmp(line, "workspace>>", 11) ||
                            !strncmp(line, "focusedmon>>", 12))
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
                need        = 1;
        }
        return need ? refresh(m) : 0;
}
