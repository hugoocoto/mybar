/* volume: output volume. Event driven through `pactl subscribe` (works with
 * PulseAudio and PipeWire-pulse); the value is read with pactl, or wpctl as a
 * fallback.
 *   { path = "volume", format = "vol {pct}%", muted_format = "vol muted" }
 * If the event stream is not available it falls back to polling every 2 s. */
#include "plugin.h"
#include <ctype.h>

struct st {
        pid_t pid;
        char *fmt, *muted_fmt;
};

static int
query(int *pct, int *muted)
{
        char out[512];
        char *vol[]  = { "pactl", "get-sink-volume", "@DEFAULT_SINK@", NULL };
        char *mute[] = { "pactl", "get-sink-mute", "@DEFAULT_SINK@", NULL };
        char *wp[]   = { "wpctl", "get-volume", "@DEFAULT_AUDIO_SINK@", NULL };

        if (mod_run(vol, out, sizeof out) > 0) {
                char *p = strchr(out, '%');
                if (!p) return -1;
                while (p > out && isdigit((unsigned char) p[-1])) p--;
                *pct = atoi(p);
                *muted = 0;
                if (mod_run(mute, out, sizeof out) > 0) *muted = strstr(out, "yes") != NULL;
                return 0;
        }
        if (mod_run(wp, out, sizeof out) > 0) { /* "Volume: 0.45" or "Volume: 0.45 [MUTED]" */
                char *p = strstr(out, "Volume:");
                if (!p) return -1;
                *pct   = (int) (strtod(p + 7, NULL) * 100.0 + 0.5);
                *muted = strstr(out, "MUTED") != NULL;
                return 0;
        }
        return -1;
}

MOD_API int
mod_init(struct mod *m)
{
        struct st *s = calloc(1, sizeof *s);
        if (!s) return -1;
        s->fmt       = strdup(m->host->opt_str(m, "format", "vol {pct}%"));
        s->muted_fmt = strdup(m->host->opt_str(m, "muted_format", "vol muted"));
        m->state     = s;

        char *sub[] = { "pactl", "subscribe", NULL };
        s->pid      = mod_spawn(sub, &m->event_fd);
        if (s->pid < 0) {
                m->event_fd    = -1;
                m->interval_ms = 2000;
        }
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
        free(s->muted_fmt);
        free(s);
}

MOD_API int
mod_update(struct mod *m)
{
        struct st *s = m->state;
        int pct = 0, muted = 0;
        if (query(&pct, &muted) < 0) return mod_set_text(m, "vol n/a");

        char num[16];
        snprintf(num, sizeof num, "%d", pct);
        const char *keys[] = { "pct" };
        const char *vals[] = { num };
        char out[sizeof m->text];
        mod_format(out, sizeof out, muted ? s->muted_fmt : s->fmt, keys, vals, 1);
        return mod_set_text(m, "%s", out);
}

MOD_API int
mod_event(struct mod *m)
{
        struct st *s = m->state;
        char buf[4096];
        int relevant = 0;
        ssize_t n;

        while ((n = read(m->event_fd, buf, sizeof buf - 1)) > 0) {
                buf[n] = 0;
                /* "on sink #N" and "on server" only; ignore sink-input, source, ... */
                if (strstr(buf, "on sink #") || strstr(buf, "on server")) relevant = 1;
        }
        if (n == 0) { /* pactl exited: fall back to polling */
                close(m->event_fd);
                m->event_fd = -1;
                waitpid(s->pid, NULL, 0);
                s->pid         = 0;
                m->interval_ms = 2000;
                relevant       = 1;
        }
        return relevant ? mod_update(m) : 0;
}
