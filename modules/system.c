/* system: CPU load, memory use and temperature, read every 2 s from /proc and
 * /sys/class/thermal.
 *   { path = "system", format = "cpu {cpu}%  mem {mem}%  {temp}°C",
 *     cpu_high = 80, mem_high = 80, temp_high = 85, high_color = "#f38ba8",
 *     only_high = false, thermal = "x86_pkg_temp", interval = 2 }
 * placeholders: {cpu} (% busy since the last update) {mem} (% used)
 *               {mem_used} {mem_total} (GiB, one decimal) {temp} (°C, "?" if unknown)
 * The text turns high_color when any value reaches its *_high; only_high = true
 * hides the module until then.
 * thermal is the type of the thermal zone to read (the `type` file of each
 * /sys/class/thermal/thermal_zoneN). Default: x86_pkg_temp, TCPU, cpu-thermal,
 * acpitz, else the first zone. */
#include "plugin.h"
#include <dirent.h>
#include <limits.h>

struct st {
        char *fmt;
        char temp_path[PATH_MAX]; /* "" when there is no thermal zone */
        int cpu_high, mem_high, temp_high, only_high;
        int32_t high_color;
        unsigned long long prev_busy, prev_total;
};

/* .../thermal_zoneN/temp of the zone whose type is `want`, else of the first
 * zone with a type from the default list, else of the first zone. */
static void
find_thermal(const char *want, char *out, size_t size)
{
        static const char *const pref[] = { "x86_pkg_temp", "TCPU", "cpu-thermal", "acpitz" };
        const char *base = "/sys/class/thermal";
        int best = 99;
        out[0]   = 0;
        DIR *d   = opendir(base);
        if (!d) return;
        struct dirent *e;
        while ((e = readdir(d))) {
                if (strncmp(e->d_name, "thermal_zone", 12)) continue;
                char p[PATH_MAX], type[64];
                snprintf(p, sizeof p, "%s/%.200s/type", base, e->d_name);
                if (mod_read_file(p, type, sizeof type) <= 0) continue;
                int rank = 50; /* any zone */
                if (want && *want) rank = strcmp(type, want) ? 50 : 0;
                else
                        for (int i = 0; i < (int) (sizeof pref / sizeof *pref); i++)
                                if (!strcmp(type, pref[i])) rank = i;
                /* readdir order is arbitrary: on a tie keep the lowest zone number */
                if (rank < best || (rank == best && strcmp(e->d_name, out + strlen(base) + 1) < 0)) {
                        best = rank;
                        snprintf(out, size, "%s/%.200s/temp", base, e->d_name);
                }
        }
        closedir(d);
        if (want && *want && best) fprintf(stderr, "system: no thermal zone of type '%s'\n", want);
}

static int
cpu_pct(struct st *s)
{
        FILE *f = fopen("/proc/stat", "r");
        if (!f) return 0;
        unsigned long long v[8] = { 0 };
        int n = fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
                       &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]);
        fclose(f);
        if (n < 4) return 0;
        unsigned long long total = 0;
        for (int i = 0; i < 8; i++) total += v[i];
        unsigned long long busy = total - v[3] - v[4]; /* minus idle and iowait */
        unsigned long long dt = total - s->prev_total, db = busy - s->prev_busy;
        s->prev_total = total; /* the first call: the average since boot */
        s->prev_busy  = busy;
        if (!dt) return 0;
        return (int) ((db * 100 + dt / 2) / dt);
}

/* % of memory used; *used and *total in kB */
static int
mem_pct(long *used, long *total)
{
        FILE *f = fopen("/proc/meminfo", "r");
        long avail = 0;
        *used = *total = 0;
        if (!f) return 0;
        char line[128];
        while (fgets(line, sizeof line, f)) {
                sscanf(line, "MemTotal: %ld kB", total);
                sscanf(line, "MemAvailable: %ld kB", &avail);
        }
        fclose(f);
        *used = *total - avail;
        return *total ? (int) ((*used * 100 + *total / 2) / *total) : 0;
}

MOD_API int
mod_init(struct mod *m)
{
        struct st *s = calloc(1, sizeof *s);
        if (!s) return -1;
        s->fmt        = strdup(m->host->opt_str(m, "format", "cpu {cpu}%  mem {mem}%  {temp}°C"));
        s->cpu_high   = m->host->opt_int(m, "cpu_high", 80);
        s->mem_high   = m->host->opt_int(m, "mem_high", 80);
        s->temp_high  = m->host->opt_int(m, "temp_high", 85);
        s->only_high  = m->host->opt_bool(m, "only_high", 0);
        s->high_color = mod_parse_color(m->host->opt_str(m, "high_color", NULL), 0xf38ba8);
        find_thermal(m->host->opt_str(m, "thermal", NULL), s->temp_path, sizeof s->temp_path);
        m->state       = s;
        m->interval_ms = 2000;
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
        long used, total;
        int cpu = cpu_pct(s), mem = mem_pct(&used, &total), temp = -1;
        char buf[32];
        if (s->temp_path[0] && mod_read_file(s->temp_path, buf, sizeof buf) > 0)
                temp = (atoi(buf) + 500) / 1000; /* millidegrees */

        int high = cpu >= s->cpu_high || mem >= s->mem_high || (temp >= 0 && temp >= s->temp_high);
        if (s->only_high && !high) {
                m->color = -1;
                return mod_set_text(m, "%s", "");
        }

        char c[16], mp[16], mu[16], mt[16], t[16];
        snprintf(c, sizeof c, "%d", cpu);
        snprintf(mp, sizeof mp, "%d", mem);
        snprintf(mu, sizeof mu, "%.1f", used / 1048576.0);
        snprintf(mt, sizeof mt, "%.1f", total / 1048576.0);
        if (temp >= 0) snprintf(t, sizeof t, "%d", temp);
        else snprintf(t, sizeof t, "?");

        const char *keys[] = { "cpu", "mem", "mem_used", "mem_total", "temp" };
        const char *vals[] = { c, mp, mu, mt, t };
        char out[sizeof m->text];
        mod_format(out, sizeof out, s->fmt, keys, vals, 5);

        int32_t col = high ? s->high_color : -1;
        int changed = col != m->color;
        m->color    = col;
        return mod_set_text(m, "%s", out) || changed;
}
