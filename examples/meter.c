/* meter: a module that draws itself. Shows memory use as a small bar.
 *   { path = "meter", width = 60, interval = 2, color = "#89b4fa" } */
#include "plugin.h"

struct st {
        int pct, width;
};

static int
mem_used_pct(void)
{
        FILE *f = fopen("/proc/meminfo", "r");
        if (!f) return 0;
        long total = 0, avail = 0;
        char line[128];
        while (fgets(line, sizeof line, f)) {
                sscanf(line, "MemTotal: %ld kB", &total);
                sscanf(line, "MemAvailable: %ld kB", &avail);
        }
        fclose(f);
        return total ? (int) (100 - avail * 100 / total) : 0;
}

MOD_API int
mod_init(struct mod *m)
{
        struct st *s = calloc(1, sizeof *s);
        if (!s) return -1;
        s->width       = m->host->opt_int(m, "width", 60);
        m->state       = s;
        m->interval_ms = 2000;
        return 0;
}

MOD_API void
mod_destroy(struct mod *m)
{
        free(m->state);
}

MOD_API int
mod_update(struct mod *m)
{
        struct st *s = m->state;
        int pct      = mem_used_pct();
        int changed  = pct != s->pct;
        s->pct       = pct;
        return changed;                 /* redraw only when it changed */
}

/* "mem " label + a bar: the total width is what the bar reserves for us */
MOD_API int
mod_width(struct mod *m)
{
        struct st *s = m->state;
        return m->host->text_width("mem ") + s->width;
}

MOD_API void
mod_draw(struct mod *m, struct canvas *cv, int x)
{
        struct st *s = m->state;
        int adv      = m->host->draw_text(cv, x, "mem ", -1);       /* -1 = default color */
        int bh       = 10, y = (cv->h - bh) / 2;
        m->host->fill_rect(cv, x + adv, y, s->width, bh, 0x45475a);   /* track */
        m->host->fill_rect(cv, x + adv, y, s->width * s->pct / 100, bh, cv->fg); /* fill */
}
