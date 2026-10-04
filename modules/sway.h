/* sway.h - talking to sway over its IPC socket ($SWAYSOCK, the i3 protocol),
 * shared by the sway_* modules.
 *
 * A message is "i3-ipc" + u32 payload length + u32 type + JSON payload, both
 * ways. Requests use a short-lived connection (sway_request); events come on a
 * second connection that subscribed to them (sway_subscribe + sway_read_events).
 * sway_walk goes through a JSON tree (GET_TREE, GET_INPUTS...) without a JSON
 * library, handing each object's interesting fields to a callback. */
#ifndef MYBAR_SWAY_H
#define MYBAR_SWAY_H

#include "plugin.h"
#pragma GCC diagnostic ignored "-Wformat-truncation" /* long names are cut: fine */
#include <sys/socket.h>
#include <sys/un.h>

enum {
        SWAY_GET_WORKSPACES    = 1,
        SWAY_SUBSCRIBE         = 2,
        SWAY_GET_TREE          = 4,
        SWAY_GET_BINDING_STATE = 12,
        SWAY_GET_INPUTS        = 100,
};
#define SWAY_EVENT_WORKSPACE 0x80000000u
#define SWAY_EVENT_MODE      0x80000002u
#define SWAY_EVENT_WINDOW    0x80000003u
#define SWAY_EVENT_INPUT     0x80000015u

#define SWAY_MAGIC "i3-ipc"
#define SWAY_HDR   14 /* magic + length + type */

static int
sway_connect(void)
{
        const char *path = getenv("SWAYSOCK");
        if (!path || !*path) return -1;
        int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0) return -1;
        struct sockaddr_un sa = { .sun_family = AF_UNIX };
        snprintf(sa.sun_path, sizeof sa.sun_path, "%s", path);
        if (connect(fd, (struct sockaddr *) &sa, sizeof sa) < 0) {
                close(fd);
                return -1;
        }
        return fd;
}

static int
sway_send(int fd, uint32_t type, const char *payload)
{
        uint32_t len = (uint32_t) strlen(payload);
        char hdr[SWAY_HDR];
        memcpy(hdr, SWAY_MAGIC, 6);
        memcpy(hdr + 6, &len, 4);
        memcpy(hdr + 10, &type, 4);
        if (write(fd, hdr, sizeof hdr) != (ssize_t) sizeof hdr) return -1;
        return write(fd, payload, len) == (ssize_t) len ? 0 : -1;
}

/* Read exactly n bytes, waiting at most ~300 ms for each chunk. */
static int
sway_read_full(int fd, char *buf, size_t n)
{
        size_t got = 0;
        while (got < n) {
                struct pollfd pf = { .fd = fd, .events = POLLIN };
                if (poll(&pf, 1, 300) <= 0) return -1;
                ssize_t k = read(fd, buf + got, n - got);
                if (k <= 0) return -1;
                got += (size_t) k;
        }
        return 0;
}

/* Send a request and return the reply payload (malloc'd, NUL-terminated), or
 * NULL. */
static char *
sway_request(uint32_t type, const char *payload)
{
        int fd = sway_connect();
        if (fd < 0) return NULL;
        char hdr[SWAY_HDR], *reply = NULL;
        if (sway_send(fd, type, payload) == 0 && sway_read_full(fd, hdr, sizeof hdr) == 0 &&
            !memcmp(hdr, SWAY_MAGIC, 6)) {
                uint32_t len;
                memcpy(&len, hdr + 6, 4);
                reply = malloc((size_t) len + 1);
                if (reply && sway_read_full(fd, reply, len) == 0) reply[len] = 0;
                else {
                        free(reply);
                        reply = NULL;
                }
        }
        close(fd);
        return reply;
}

/* ---- events -------------------------------------------------------------- */

struct sway_events {
        char *buf; /* partial messages */
        size_t len, cap;
};

/* Connect and subscribe, e.g. events = "[\"workspace\",\"window\"]". Returns
 * a non-blocking fd for m->event_fd, or -1. */
static int
sway_subscribe(const char *events)
{
        int fd = sway_connect();
        if (fd < 0) return -1;
        char hdr[SWAY_HDR], reply[256];
        uint32_t len = 0;
        if (sway_send(fd, SWAY_SUBSCRIBE, events) || sway_read_full(fd, hdr, sizeof hdr) ||
            (memcpy(&len, hdr + 6, 4), len >= sizeof reply) || sway_read_full(fd, reply, len)) {
                close(fd);
                return -1;
        }
        reply[len] = 0;
        if (!strstr(reply, "true")) { /* {"success": true} */
                close(fd);
                return -1;
        }
        fcntl(fd, F_SETFL, O_NONBLOCK);
        return fd;
}

/* Read what is available on fd and call fn for every complete event.
 * Returns 0, or -1 when sway closed the connection. */
static int
sway_read_events(int fd, struct sway_events *ev, void (*fn)(void *ud, uint32_t type, char *payload), void *ud)
{
        for (;;) {
                if (ev->cap - ev->len < 4096) {
                        size_t cap = ev->cap ? ev->cap * 2 : 16384;
                        char *b    = realloc(ev->buf, cap);
                        if (!b) return 0;
                        ev->buf = b;
                        ev->cap = cap;
                }
                ssize_t n = read(fd, ev->buf + ev->len, ev->cap - ev->len - 1);
                if (n == 0) return -1;
                if (n < 0) break; /* EAGAIN: all read */
                ev->len += (size_t) n;
                /* hand out the complete messages */
                size_t off = 0;
                while (ev->len - off >= SWAY_HDR) {
                        uint32_t len, type;
                        memcpy(&len, ev->buf + off + 6, 4);
                        memcpy(&type, ev->buf + off + 10, 4);
                        if (ev->len - off - SWAY_HDR < len) break;
                        char *payload = ev->buf + off + SWAY_HDR;
                        char save     = payload[len];
                        payload[len]  = 0;
                        fn(ud, type, payload);
                        payload[len] = save;
                        off += SWAY_HDR + len;
                }
                memmove(ev->buf, ev->buf + off, ev->len - off);
                ev->len -= off;
        }
        return 0;
}

static void
sway_events_free(struct sway_events *ev)
{
        free(ev->buf);
}

/* ---- walking the JSON ---------------------------------------------------- */

/* The fields of one JSON object that the sway modules care about. */
struct sway_node {
        char type[24];       /* "workspace", "con", "floating_con", "keyboard", ... */
        char name[256];      /* name, i.e. the title for windows */
        char app_id[128];    /* app_id, or window_properties.class for X11 windows */
        char change[32];     /* "change" of an event */
        char layout[128];    /* xkb_active_layout_name */
        int focused, num, layout_index;
        int has_num;
};

/* Called when an object closes. stack[depth] is that object, stack[0..depth-1]
 * the objects around it (outermost first). */
typedef void (*sway_node_fn)(void *ud, struct sway_node *stack, int depth);

#define SWAY_MAX_DEPTH 64

/* Decode the JSON string at *p (just after the opening quote) into out and
 * move *p past the closing quote. */
static void
sway_json_string(const char **p, char *out, size_t size)
{
        const char *s = *p;
        size_t o      = 0;
        while (*s && *s != '"') {
                char c = *s++;
                if (c == '\\' && *s) {
                        c = *s++;
                        switch (c) {
                        case 'n': case 't': case 'r': c = ' '; break;
                        case 'u': {
                                unsigned cp = (unsigned) strtoul((char[5]){ s[0], s[1], s[2], s[3], 0 }, NULL, 16);
                                s += 4;
                                if (cp >= 0xd800 && cp < 0xdc00 && s[0] == '\\' && s[1] == 'u') { /* surrogate pair */
                                        unsigned lo = (unsigned) strtoul((char[5]){ s[2], s[3], s[4], s[5], 0 }, NULL, 16);
                                        cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
                                        s += 6;
                                }
                                char u[4];
                                int n = 0;
                                if (cp < 0x80) u[n++] = (char) cp;
                                else if (cp < 0x800) { u[n++] = 0xc0 | (cp >> 6); u[n++] = 0x80 | (cp & 0x3f); }
                                else if (cp < 0x10000) { u[n++] = 0xe0 | (cp >> 12); u[n++] = 0x80 | ((cp >> 6) & 0x3f); u[n++] = 0x80 | (cp & 0x3f); }
                                else { u[n++] = 0xf0 | (cp >> 18); u[n++] = 0x80 | ((cp >> 12) & 0x3f); u[n++] = 0x80 | ((cp >> 6) & 0x3f); u[n++] = 0x80 | (cp & 0x3f); }
                                for (int i = 0; i < n; i++)
                                        if (o + 1 < size) out[o++] = u[i];
                                continue;
                        }
                        default: break; /* \" \\ \/ */
                        }
                }
                if (o + 1 < size) out[o++] = c;
        }
        out[o] = 0;
        *p     = *s ? s + 1 : s;
}

/* Walk every object of json, calling fn as each one closes. */
static void
sway_walk(const char *json, sway_node_fn fn, void *ud)
{
        struct sway_node *stack = calloc(SWAY_MAX_DEPTH, sizeof *stack);
        if (!stack) return;
        int depth = -1;
        char key[64] = "", str[256];
        int expect_value = 0; /* just saw "key": */

        for (const char *p = json; *p;) {
                char c = *p;
                if (c == '"') {
                        p++;
                        sway_json_string(&p, str, sizeof str);
                        while (*p == ' ' || *p == '\n' || *p == '\t' || *p == '\r') p++;
                        if (*p == ':') { /* a key */
                                snprintf(key, sizeof key, "%s", str);
                                expect_value = 1;
                                p++;
                                continue;
                        }
                        if (expect_value && depth >= 0 && depth < SWAY_MAX_DEPTH) {
                                struct sway_node *n = &stack[depth];
                                if (!strcmp(key, "type")) snprintf(n->type, sizeof n->type, "%s", str);
                                else if (!strcmp(key, "name")) snprintf(n->name, sizeof n->name, "%s", str);
                                else if (!strcmp(key, "app_id") || (!strcmp(key, "class") && !n->app_id[0]))
                                        snprintf(n->app_id, sizeof n->app_id, "%s", str);
                                else if (!strcmp(key, "change")) snprintf(n->change, sizeof n->change, "%s", str);
                                else if (!strcmp(key, "xkb_active_layout_name"))
                                        snprintf(n->layout, sizeof n->layout, "%s", str);
                        }
                        expect_value = 0;
                        continue;
                }
                if (c == '{') {
                        depth++;
                        if (depth < SWAY_MAX_DEPTH) memset(&stack[depth], 0, sizeof *stack);
                        expect_value = 0;
                } else if (c == '}') {
                        if (depth >= 0 && depth < SWAY_MAX_DEPTH) {
                                struct sway_node *n = &stack[depth];
                                /* window_properties (no type): give its class to the window */
                                if (!n->type[0] && n->app_id[0] && depth > 0 && !stack[depth - 1].app_id[0])
                                        memcpy(stack[depth - 1].app_id, n->app_id, sizeof n->app_id);
                                fn(ud, stack, depth);
                        }
                        depth--;
                        expect_value = 0;
                } else if (expect_value && (c == 't' || c == 'f' || c == '-' || (c >= '0' && c <= '9'))) {
                        if (depth >= 0 && depth < SWAY_MAX_DEPTH) {
                                struct sway_node *n = &stack[depth];
                                if (!strcmp(key, "focused")) n->focused = c == 't';
                                else if (!strcmp(key, "num")) {
                                        n->num     = atoi(p);
                                        n->has_num = 1;
                                } else if (!strcmp(key, "xkb_active_layout_index")) n->layout_index = atoi(p);
                        }
                        expect_value = 0;
                } else if (c == '[' || c == ',' || c == 'n') {
                        expect_value = 0; /* arrays, null */
                }
                p++;
        }
        free(stack);
}

#endif /* MYBAR_SWAY_H */
