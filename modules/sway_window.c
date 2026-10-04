/* sway_window: title of the focused window, for sway. Event driven through
 * sway's IPC (window and workspace events).
 *   { path = "sway_window", format = "{title}", max_chars = 60 }
 * placeholders: {title} {class} (app_id, or the X11 class)
 * Hidden when no window is focused (e.g. an empty workspace).
 * Disables itself when not running under sway. */
#include "sway.h"

struct st {
        char *fmt;
        int max_chars;
        struct sway_events ev;
};

struct focused {
        char title[256], class[128];
        int found;
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

static void
on_node(void *ud, struct sway_node *stack, int depth)
{
        struct focused *f   = ud;
        struct sway_node *n = &stack[depth];
        if (!n->focused || (strcmp(n->type, "con") && strcmp(n->type, "floating_con"))) return;
        snprintf(f->title, sizeof f->title, "%s", n->name);
        snprintf(f->class, sizeof f->class, "%s", n->app_id);
        f->found = 1;
}

static int
refresh(struct mod *m)
{
        struct st *s     = m->state;
        struct focused f = { 0 };
        char *tree       = sway_request(SWAY_GET_TREE, "");
        if (tree) sway_walk(tree, on_node, &f);
        free(tree);
        if (!f.found) return mod_set_text(m, "%s", "");

        truncate_utf8(f.title, s->max_chars);
        const char *keys[] = { "title", "class" };
        const char *vals[] = { f.title, f.class };
        char out[sizeof m->text];
        mod_format(out, sizeof out, s->fmt, keys, vals, 2);
        return mod_set_text(m, "%s", out);
}

MOD_API int
mod_init(struct mod *m)
{
        int fd = sway_subscribe("[\"window\",\"workspace\"]");
        if (fd < 0) {
                fprintf(stderr, "sway_window: not running under sway\n");
                return -1;
        }
        struct st *s = calloc(1, sizeof *s);
        if (!s) {
                close(fd);
                return -1;
        }
        s->fmt         = strdup(m->host->opt_str(m, "format", "{title}"));
        s->max_chars   = m->host->opt_int(m, "max_chars", 60);
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

MOD_API int
mod_update(struct mod *m)
{
        return refresh(m);
}

static void
on_event(void *ud, uint32_t type, char *payload)
{
        *(int *) ud = 1;
}

MOD_API int
mod_event(struct mod *m)
{
        struct st *s = m->state;
        int need     = 0;
        if (sway_read_events(m->event_fd, &s->ev, on_event, &need) < 0) { /* sway went away */
                close(m->event_fd);
                m->event_fd = -1;
                return mod_set_text(m, "%s", "");
        }
        return need ? refresh(m) : 0;
}
