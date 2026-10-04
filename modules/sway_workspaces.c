/* sway_workspaces: the workspaces module for sway: the focused workspace
 * highlighted, with the app icons of its windows. Event driven through sway's
 * IPC (workspace and window events).
 *   { path = "sway_workspaces", format = "{name} {icons}", ... }
 * The options, placeholders and icons are the ones of the Hyprland module:
 * see workspaces.h. {id} is the workspace number; named workspaces without a
 * number come after the numbered ones.
 * Disables itself when not running under sway. */
#include "workspaces.h"
#include "sway.h"

struct st {
        struct wsview v;
        struct sway_events ev;
};

/* state of one refresh, for the walk callbacks */
struct walk {
        struct wsview *v;
        int active, named;
        struct {
                char name[64];
                int id;
        } map[MAX_WS]; /* workspace name -> id, to place the windows */
        int nmap;
};

/* GET_WORKSPACES: an array of workspace objects (depth 0) */
static void
on_workspace(void *ud, struct sway_node *stack, int depth)
{
        struct walk *w      = ud;
        struct sway_node *n = &stack[depth];
        if (depth != 0 || !n->name[0] || w->nmap >= MAX_WS) return;
        int id = n->has_num && n->num >= 0 ? n->num : 1000 + w->named++;
        snprintf(w->map[w->nmap].name, sizeof w->map[0].name, "%s", n->name);
        w->map[w->nmap++].id = id;
        if (n->focused) w->active = id;
        ws_add(w->v, id, n->name, -1); /* -1: count the windows from the tree */
}

/* GET_TREE: every window (a con with an app_id or X11 class) goes to the
 * workspace around it */
static void
on_tree_node(void *ud, struct sway_node *stack, int depth)
{
        struct walk *w      = ud;
        struct sway_node *n = &stack[depth];
        if (strcmp(n->type, "con") && strcmp(n->type, "floating_con")) return;
        if (!n->app_id[0]) return; /* a split container, not a window */
        for (int d = depth - 1; d >= 0; d--) {
                if (strcmp(stack[d].type, "workspace")) continue;
                for (int i = 0; i < w->nmap; i++)
                        if (!strcmp(w->map[i].name, stack[d].name)) ws_add_window(w->v, w->map[i].id, n->app_id);
                return;
        }
}

static int
refresh(struct mod *m)
{
        struct st *s  = m->state;
        struct walk w = { .v = &s->v, .active = s->v.active };

        ws_begin(&s->v);
        char *reply = sway_request(SWAY_GET_WORKSPACES, "");
        if (reply) sway_walk(reply, on_workspace, &w);
        free(reply);
        if ((reply = sway_request(SWAY_GET_TREE, ""))) sway_walk(reply, on_tree_node, &w);
        free(reply);
        return ws_commit(&s->v, w.active);
}

MOD_API int
mod_init(struct mod *m)
{
        int fd = sway_subscribe("[\"workspace\",\"window\"]");
        if (fd < 0) {
                fprintf(stderr, "sway_workspaces: not running under sway\n");
                return -1;
        }
        struct st *s = calloc(1, sizeof *s);
        if (!s) {
                close(fd);
                return -1;
        }
        ws_options(m, &s->v);
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
        ws_free(&s->v);
        free(s);
}

MOD_API int
mod_update(struct mod *m)
{
        return refresh(m);
}

/* top-level "change" of an event */
static void
on_event_node(void *ud, struct sway_node *stack, int depth)
{
        if (depth == 0) snprintf(ud, sizeof stack->change, "%s", stack->change);
}

static void
on_event(void *ud, uint32_t type, char *payload)
{
        int *need = ud;
        char change[32] = "";
        sway_walk(payload, on_event_node, change);
        /* window titles and marks do not change what we show */
        if (type == SWAY_EVENT_WINDOW && (!strcmp(change, "title") || !strcmp(change, "mark"))) return;
        *need = 1;
}

MOD_API int
mod_event(struct mod *m)
{
        struct st *s = m->state;
        int need     = 0;
        if (sway_read_events(m->event_fd, &s->ev, on_event, &need) < 0) { /* sway went away */
                close(m->event_fd);
                m->event_fd = -1;
                s->v.n      = 0;
                return 1;
        }
        return need ? refresh(m) : 0;
}

MOD_API int
mod_width(struct mod *m)
{
        return ws_width(m, &((struct st *) m->state)->v);
}

MOD_API void
mod_draw(struct mod *m, struct canvas *cv, int x)
{
        ws_draw(m, &((struct st *) m->state)->v, cv, x);
}
