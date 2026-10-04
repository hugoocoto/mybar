/* sway_mode: the active sway binding mode (e.g. "resize"); what Hyprland calls
 * a submap. Event driven through sway's IPC (mode events).
 *   { path = "sway_mode", format = "{name}" }
 * placeholders: {name}
 * Hidden in the default mode. Disables itself when not running under sway. */
#include "sway.h"

struct st {
        char *fmt;
        struct sway_events ev;
        int changed;
};

static int
show(struct mod *m, const char *name)
{
        struct st *s = m->state;
        if (!*name || !strcmp(name, "default")) return mod_set_text(m, "%s", "");
        const char *keys[] = { "name" };
        const char *vals[] = { name };
        char out[sizeof m->text];
        mod_format(out, sizeof out, s->fmt, keys, vals, 1);
        return mod_set_text(m, "%s", out);
}

/* the top-level object: {"name": ...} for GET_BINDING_STATE, {"change": ...} for events */
static void
on_top(void *ud, struct sway_node *stack, int depth)
{
        if (depth == 0) *(struct sway_node *) ud = stack[0];
}

MOD_API int
mod_init(struct mod *m)
{
        int fd = sway_subscribe("[\"mode\"]");
        if (fd < 0) {
                fprintf(stderr, "sway_mode: not running under sway\n");
                return -1;
        }
        struct st *s = calloc(1, sizeof *s);
        if (!s) {
                close(fd);
                return -1;
        }
        s->fmt         = strdup(m->host->opt_str(m, "format", "{name}"));
        m->state       = s;
        m->event_fd    = fd;
        m->interval_ms = 0; /* purely event driven */
        return 0;
}

MOD_API void
mod_destroy(struct mod *m)
{
        struct st *s = m->state;
        if (m->event_fd >= 0) close(m->event_fd);
        sway_events_free(&s->ev);
        free(s->fmt);
        free(s);
}

/* Ask sway once (a config reload may happen while inside a mode). */
MOD_API int
mod_update(struct mod *m)
{
        struct sway_node top = { 0 };
        char *reply          = sway_request(SWAY_GET_BINDING_STATE, "");
        if (reply) sway_walk(reply, on_top, &top);
        free(reply);
        return show(m, top.name);
}

static void
on_event(void *ud, uint32_t type, char *payload)
{
        struct mod *m        = ud;
        struct sway_node top = { 0 };
        sway_walk(payload, on_top, &top);
        if (show(m, top.change)) ((struct st *) m->state)->changed = 1;
}

MOD_API int
mod_event(struct mod *m)
{
        struct st *s = m->state;
        s->changed   = 0;
        if (sway_read_events(m->event_fd, &s->ev, on_event, m) < 0) { /* sway went away */
                close(m->event_fd);
                m->event_fd = -1;
                return show(m, "") || s->changed;
        }
        return s->changed;
}
