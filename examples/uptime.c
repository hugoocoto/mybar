/* uptime: a minimal text module.   { path = "uptime", interval = 60 } */
#include "plugin.h"

MOD_API int
mod_init(struct mod *m)
{
        m->interval_ms = 60000;      /* refresh once a minute */
        return 0;
}

MOD_API int
mod_update(struct mod *m)
{
        char buf[64];
        if (mod_read_file("/proc/uptime", buf, sizeof buf) <= 0) return mod_set_text(m, "%s", "");
        long secs = (long) atof(buf);
        return mod_set_text(m, "up %ldh%02ldm", secs / 3600, secs / 60 % 60);
}
