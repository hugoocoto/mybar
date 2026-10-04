/* capslock: shows while Caps Lock is on, from the keyboard LEDs in
 * /sys/class/leds (any keyboard, plugged in later too). The kernel does not
 * notify LED changes, so it reads them every 0.25 s: a few tiny reads, and the
 * bar only redraws when the state changes.
 *   { path = "capslock", format = "CAPS", interval = 0.25 }
 * Hidden while Caps Lock is off. */
#include "plugin.h"
#include <dirent.h>

struct st {
        char *fmt;
};

MOD_API int
mod_init(struct mod *m)
{
        struct st *s = calloc(1, sizeof *s);
        if (!s) return -1;
        s->fmt         = strdup(m->host->opt_str(m, "format", "CAPS"));
        m->state       = s;
        m->interval_ms = 250;
        return 0;
}

MOD_API void
mod_destroy(struct mod *m)
{
        struct st *s = m->state;
        free(s->fmt);
        free(s);
}

MOD_API int
mod_update(struct mod *m)
{
        struct st *s = m->state;
        int on       = 0;
        DIR *d       = opendir("/sys/class/leds");
        if (d) {
                struct dirent *e;
                while (!on && (e = readdir(d))) {
                        size_t l = strlen(e->d_name);
                        if (l < 10 || strcmp(e->d_name + l - 10, "::capslock")) continue;
                        char p[320], v[16];
                        snprintf(p, sizeof p, "/sys/class/leds/%.200s/brightness", e->d_name);
                        on = mod_read_file(p, v, sizeof v) > 0 && atoi(v) > 0;
                }
                closedir(d);
        }
        return mod_set_text(m, "%s", on ? s->fmt : "");
}
