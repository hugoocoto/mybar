/* media: what is playing in any MPRIS player (spotify, mpv, browsers...).
 * Event driven through `playerctl --follow`, which prints a line on every change.
 *   { path = "media", format = "{dynamic}", paused_format = "{dynamic} (paused)",
 *     player = "spotify", max_chars = 50 }
 * placeholders: {artist} {title} {album} {player} {status} (playing/paused)
 *               {dynamic} ("artist - title", or only the title if there is no artist)
 * player follows only that player (playerctl -p); default: the one playerctl picks.
 * Hidden when nothing is playing or paused. Needs playerctl. */
#include "plugin.h"

#define SEP "\x1f" /* field separator in playerctl's output */

struct st {
        pid_t pid;
        char *fmt, *paused_fmt;
        int max_chars;
        char buf[4096]; /* partial line */
        size_t len;
};

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

/* One line from playerctl: status SEP player SEP artist SEP title SEP album */
static int
show_line(struct mod *m, char *line)
{
        struct st *s = m->state;
        char *f[5]   = { "", "", "", "", "" };
        int n        = 0;
        for (char *p = line; n < 5;) {
                f[n++]  = p;
                char *e = strstr(p, SEP);
                if (!e) break;
                *e = 0;
                p  = e + 1;
        }
        char *status = f[0];
        for (char *c = status; *c; c++) *c = (char) (*c >= 'A' && *c <= 'Z' ? *c + 32 : *c);
        if (strcmp(status, "playing") && strcmp(status, "paused")) return mod_set_text(m, "%s", "");

        char dynamic[512];
        if (*f[2]) snprintf(dynamic, sizeof dynamic, "%s - %s", f[2], f[3]);
        else snprintf(dynamic, sizeof dynamic, "%s", f[3]);

        const char *keys[] = { "status", "player", "artist", "title", "album", "dynamic" };
        const char *vals[] = { status, f[1], f[2], f[3], f[4], dynamic };
        char out[sizeof m->text];
        mod_format(out, sizeof out, strcmp(status, "paused") ? s->fmt : s->paused_fmt, keys, vals, 6);
        if (s->max_chars > 0) truncate_utf8(out, s->max_chars);
        return mod_set_text(m, "%s", out);
}

MOD_API int
mod_init(struct mod *m)
{
        struct st *s = calloc(1, sizeof *s);
        if (!s) return -1;
        s->fmt        = strdup(m->host->opt_str(m, "format", "{dynamic}"));
        s->paused_fmt = strdup(m->host->opt_str(m, "paused_format", "{dynamic} (paused)"));
        s->max_chars  = m->host->opt_int(m, "max_chars", 50);
        m->state      = s;

        const char *player = m->host->opt_str(m, "player", NULL);
        char *argv[9];
        int a     = 0;
        argv[a++] = "playerctl";
        if (player) {
                argv[a++] = "-p";
                argv[a++] = (char *) player;
        }
        argv[a++] = "--follow";
        argv[a++] = "metadata";
        argv[a++] = "--format";
        argv[a++] = "{{status}}" SEP "{{playerName}}" SEP "{{artist}}" SEP "{{title}}" SEP "{{album}}";
        argv[a]   = NULL;
        s->pid    = mod_spawn(argv, &m->event_fd);
        if (s->pid < 0) {
                fprintf(stderr, "media: cannot run playerctl\n");
                free(s->fmt);
                free(s->paused_fmt);
                free(s);
                return -1;
        }
        m->interval_ms = 0; /* purely event driven */
        return 0;
}

MOD_API void
mod_destroy(struct mod *m)
{
        struct st *s = m->state;
        if (s->pid > 0) {
                kill(s->pid, SIGTERM);
                waitpid(s->pid, NULL, 0);
        }
        if (m->event_fd >= 0) close(m->event_fd);
        free(s->fmt);
        free(s->paused_fmt);
        free(s);
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
                        if (show_line(m, line)) changed = 1;
                        line = nl + 1;
                }
                s->len = strlen(line);
                memmove(s->buf, line, s->len + 1);
                if (s->len >= sizeof s->buf - 1) s->len = 0; /* absurdly long line: drop it */
        }
        if (n == 0) { /* playerctl exited (not installed?) */
                fprintf(stderr, "media: playerctl exited\n");
                close(m->event_fd);
                m->event_fd = -1;
                waitpid(s->pid, NULL, 0);
                s->pid = 0;
                return mod_set_text(m, "%s", "") || changed;
        }
        return changed;
}
