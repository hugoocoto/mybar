/* battery: charge level from /sys/class/power_supply.
 *   { path = "battery", format = "BAT {pct}%{sign}", device = "BAT0",
 *     low = 15, low_color = "#f38ba8", interval = 30 }
 * (base = "/sys/class/power_supply" is only useful for testing)
 * placeholders: {pct} {status} (charging/discharging/full/...) {sign} ("+" while charging)
 * With no battery (desktop) the module disables itself. */
#include "plugin.h"
#include <dirent.h>
#include <limits.h>

struct st {
        char dir[PATH_MAX];
        char *fmt;
        int low;
        int32_t low_color;
};

static int
find_battery(const char *base, const char *want, char *out, size_t size)
{
        if (want && *want) {
                snprintf(out, size, "%s/%s", base, want);
                return access(out, R_OK);
        }
        DIR *d = opendir(base);
        if (!d) return -1;
        int found = -1;
        struct dirent *e;
        while (found && (e = readdir(d))) {
                char p[PATH_MAX], type[32];
                if (e->d_name[0] == '.') continue;
                snprintf(p, sizeof p, "%s/%.200s/type", base, e->d_name);
                if (mod_read_file(p, type, sizeof type) > 0 && !strcmp(type, "Battery")) {
                        snprintf(out, size, "%s/%.200s", base, e->d_name);
                        found = 0;
                }
        }
        closedir(d);
        return found;
}

MOD_API int
mod_init(struct mod *m)
{
        struct st *s = calloc(1, sizeof *s);
        if (!s) return -1;
        if (find_battery(m->host->opt_str(m, "base", "/sys/class/power_supply"),
                         m->host->opt_str(m, "device", NULL), s->dir, sizeof s->dir)) {
                fprintf(stderr, "battery: no battery found\n");
                free(s);
                return -1;
        }
        s->fmt       = strdup(m->host->opt_str(m, "format", "BAT {pct}%{sign}"));
        s->low       = m->host->opt_int(m, "low", 15);
        s->low_color = mod_parse_color(m->host->opt_str(m, "low_color", NULL), 0xf38ba8);
        m->state     = s;
        m->interval_ms = 30000;
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
        char p[PATH_MAX + 16], cap[16] = "", status[32] = "";

        snprintf(p, sizeof p, "%s/capacity", s->dir);
        if (mod_read_file(p, cap, sizeof cap) <= 0) return mod_set_text(m, "%s", "");
        snprintf(p, sizeof p, "%s/status", s->dir);
        mod_read_file(p, status, sizeof status);

        int pct      = atoi(cap);
        int charging = !strcmp(status, "Charging");
        int full     = !strcmp(status, "Full");

        for (char *c = status; *c; c++) if (*c >= 'A' && *c <= 'Z') *c += 32;

        const char *keys[] = { "pct", "status", "sign" };
        const char *vals[] = { cap, status, charging ? "+" : "" };
        char out[sizeof m->text];
        mod_format(out, sizeof out, s->fmt, keys, vals, 3);

        int32_t col = (pct <= s->low && !charging && !full) ? s->low_color : -1;
        int changed = (col != m->color);
        m->color    = col;
        return mod_set_text(m, "%s", out) || changed;
}
