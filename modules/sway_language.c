/* sway_language: keyboard layout, for sway. Event driven through sway's IPC
 * (input events).
 *   { path = "sway_language", format = "{short}" }
 * placeholders: {short} (xkb layout, e.g. "us") {long} (e.g. "English (US)")
 * sway only reports the long name; {short} is looked up in xkb's evdev.lst
 * (falls back to the long name). The first keyboard with a layout is used.
 * Disables itself when not running under sway. */
#include "sway.h"

struct st {
        char *fmt;
        struct sway_events ev;
        char long_name[128], short_name[64]; /* last lookup */
};

static void
on_input(void *ud, struct sway_node *stack, int depth)
{
        char *layout = ud;
        struct sway_node *n = &stack[depth];
        if (!layout[0] && !strcmp(n->type, "keyboard") && n->layout[0]) snprintf(layout, 128, "%s", n->layout);
}

/* "English (US)" -> "us", from the layout and variant lists of evdev.lst:
 *   ! layout                       ! variant
 *     us   English (US)              intl   us: English (US, intl., with dead keys) */
static int
xkb_short(const char *desc, char *out, size_t size)
{
        FILE *f = fopen("/usr/share/X11/xkb/rules/evdev.lst", "r");
        if (!f) return 0;
        char line[512];
        int section = 0, found = 0; /* 1 layout, 2 variant */
        while (!found && fgets(line, sizeof line, f)) {
                line[strcspn(line, "\n")] = 0;
                if (line[0] == '!') {
                        section = !strcmp(line, "! layout") ? 1 : !strcmp(line, "! variant") ? 2 : 0;
                        continue;
                }
                if (!section) continue;
                char *p = line;
                while (*p == ' ') p++;
                char *code = p;
                p += strcspn(p, " ");
                if (!*p) continue;
                *p++ = 0;
                while (*p == ' ') p++;
                if (section == 1 && !strcmp(p, desc)) {
                        snprintf(out, size, "%s", code);
                        found = 1;
                } else if (section == 2) { /* "us: English (US, intl...)" */
                        char *colon = strstr(p, ": ");
                        if (colon && !strcmp(colon + 2, desc)) {
                                snprintf(out, size, "%.*s", (int) (colon - p), p);
                                found = 1;
                        }
                }
        }
        fclose(f);
        return found;
}

static int
refresh(struct mod *m)
{
        struct st *s      = m->state;
        char layout[128]  = "";
        char *reply       = sway_request(SWAY_GET_INPUTS, "");
        if (reply) sway_walk(reply, on_input, layout);
        free(reply);
        if (!layout[0]) return mod_set_text(m, "%s", "");

        if (strcmp(layout, s->long_name)) {
                snprintf(s->long_name, sizeof s->long_name, "%s", layout);
                if (!xkb_short(layout, s->short_name, sizeof s->short_name))
                        snprintf(s->short_name, sizeof s->short_name, "%.63s", layout);
        }
        const char *keys[] = { "short", "long" };
        const char *vals[] = { s->short_name, s->long_name };
        char out[sizeof m->text];
        mod_format(out, sizeof out, s->fmt, keys, vals, 2);
        return mod_set_text(m, "%s", out);
}

MOD_API int
mod_init(struct mod *m)
{
        int fd = sway_subscribe("[\"input\"]");
        if (fd < 0) {
                fprintf(stderr, "sway_language: not running under sway\n");
                return -1;
        }
        struct st *s = calloc(1, sizeof *s);
        if (!s) {
                close(fd);
                return -1;
        }
        s->fmt         = strdup(m->host->opt_str(m, "format", "{short}"));
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

/* top-level "change" of an input event */
static void
on_event_node(void *ud, struct sway_node *stack, int depth)
{
        if (depth == 0) snprintf(ud, sizeof stack->change, "%s", stack->change);
}

static void
on_event(void *ud, uint32_t type, char *payload)
{
        char change[32] = "";
        sway_walk(payload, on_event_node, change);
        if (!strncmp(change, "xkb_", 4) || !strcmp(change, "added")) *(int *) ud = 1;
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
