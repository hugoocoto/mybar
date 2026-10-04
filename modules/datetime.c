/* datetime: the clock.
 *   { path = "datetime", format = "%a %d %b  %H:%M", interval = 1 }
 * format is strftime(3). By default it refreshes once per second if the format
 * shows seconds, otherwise once per minute, always on the wall-clock boundary.
 */
#include "plugin.h"
#include <time.h>

struct st {
        char *fmt;
};

MOD_API int
mod_init(struct mod *m)
{
        struct st *s = calloc(1, sizeof *s);
        if (!s)
                return -1;
        s->fmt   = strdup(m->host->opt_str(m, "format", "%a %d %b  %H:%M"));
        m->state = s;

        int secs       = strstr(s->fmt, "%S") || strstr(s->fmt, "%T") ||
                         strstr(s->fmt, "%X") || strstr(s->fmt, "%c") ||
                         strstr(s->fmt, "%s") || strstr(s->fmt, "%r");
        m->interval_ms = secs ? 1000 : 60000;
        m->align       = 1;
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
        char buf[sizeof m->text];
        time_t t = time(NULL);
        struct tm tm;
        localtime_r(&t, &tm);
        if (!strftime(buf, sizeof buf, s->fmt, &tm))
                buf[0] = 0;
        return mod_set_text(m, "%s", buf);
}
