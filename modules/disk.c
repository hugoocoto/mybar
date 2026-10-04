/* disk: space on a filesystem, with statvfs(3), every 60 s.
 *   { path = "disk", format = "disk {free}", mount = "/",
 *     low = 10, low_color = "#f38ba8", interval = 60 }
 * placeholders: {free} {used} {total} (e.g. "338G") {pct} (% used)
 *               {free_pct} {mount}
 * The text turns low_color when the free space drops to low % or less.
 * (The option is `mount` because `path` is the plugin itself.) */
#include "plugin.h"
#include <sys/statvfs.h>

struct st {
        char *fmt, *mount;
        int low;
        int32_t low_color;
};

/* 338G, 4.5G, 1.2T, 512M */
static void
human(unsigned long long bytes, char *out, size_t size)
{
        const char *units = "KMGTP";
        double v          = (double) bytes / 1024.0;
        int u             = 0;
        while (v >= 1024.0 && units[u + 1]) {
                v /= 1024.0;
                u++;
        }
        snprintf(out, size, v < 10.0 ? "%.1f%c" : "%.0f%c", v, units[u]);
}

MOD_API int
mod_init(struct mod *m)
{
        struct st *s = calloc(1, sizeof *s);
        if (!s) return -1;
        s->fmt       = strdup(m->host->opt_str(m, "format", "disk {free}"));
        s->mount     = strdup(m->host->opt_str(m, "mount", "/"));
        s->low       = m->host->opt_int(m, "low", 10);
        s->low_color = mod_parse_color(m->host->opt_str(m, "low_color", NULL), 0xf38ba8);
        m->state       = s;
        m->interval_ms = 60000;
        return 0;
}

MOD_API void
mod_destroy(struct mod *m)
{
        struct st *s = m->state;
        free(s->fmt);
        free(s->mount);
        free(s);
}

MOD_API int
mod_update(struct mod *m)
{
        struct st *s = m->state;
        struct statvfs v;
        if (statvfs(s->mount, &v) < 0 || !v.f_blocks) return mod_set_text(m, "%s", "");

        /* f_bavail: what a normal user can still write (excludes root's reserve) */
        unsigned long long total = (unsigned long long) v.f_blocks * v.f_frsize;
        unsigned long long free_ = (unsigned long long) v.f_bavail * v.f_frsize;
        unsigned long long used  = total - (unsigned long long) v.f_bfree * v.f_frsize;
        int free_pct             = (int) (free_ * 100 / total);

        char fr[16], us[16], to[16], pct[16], fp[16];
        human(free_, fr, sizeof fr);
        human(used, us, sizeof us);
        human(total, to, sizeof to);
        snprintf(pct, sizeof pct, "%d", 100 - free_pct);
        snprintf(fp, sizeof fp, "%d", free_pct);

        const char *keys[] = { "free", "used", "total", "pct", "free_pct", "mount" };
        const char *vals[] = { fr, us, to, pct, fp, s->mount };
        char out[sizeof m->text];
        mod_format(out, sizeof out, s->fmt, keys, vals, 6);

        int32_t col = free_pct <= s->low ? s->low_color : -1;
        int changed = col != m->color;
        m->color    = col;
        return mod_set_text(m, "%s", out) || changed;
}
