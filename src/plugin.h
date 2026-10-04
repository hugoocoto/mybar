/* plugin.h - the interface between mybar and its modules (plugins).
 *
 * A plugin is one .c file in modules/. The Makefile compiles every
 * modules/NAME.c into modules/NAME.so, and the bar dlopen()s the ones listed
 * in the Lua config. A plugin exports any of the functions below (all are
 * optional) using the exact names shown:
 *
 *   int  mod_init   (struct mod *m);   set up; return 0 on success, non-zero
 *                                      to disable this module (clean up
 *                                      yourself before returning non-zero)
 *   void mod_destroy(struct mod *m);   free everything you allocated
 *   int  mod_update (struct mod *m);   refresh your data. Called once after
 *                                      init, then every m->interval_ms.
 *                                      Return non-zero if what you show changed.
 *   int  mod_event  (struct mod *m);   m->event_fd became readable. Read from
 *                                      it. Return non-zero if what you show
 *                                      changed.
 *   int  mod_width  (struct mod *m);   width in pixels (only with mod_draw)
 *   void mod_draw   (struct mod *m, struct canvas *cv, int x);
 *                                      draw yourself at x, using m->host.
 *
 * If you do not export mod_draw, the bar simply draws m->text (set it with
 * mod_set_text()). Most modules only need init + update (+ event).
 *
 * RULES
 *  - Keep state in m->state. Never use static/global variables: the same
 *    plugin can be loaded twice, and during a config reload the old and new
 *    instances briefly coexist.
 *  - Read your options from the Lua config in mod_init (see host_api.opt_*).
 *    Do not read them in mod_destroy.
 *  - Do not block. Anything that can be slow (a socket, a command) must be
 *    non-blocking or very short; the whole bar waits for you.
 *  - Child processes: use mod_spawn()/mod_run() below.
 */
#ifndef MYBAR_PLUGIN_H
#define MYBAR_PLUGIN_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

#define MOD_API __attribute__((visibility("default")))

struct mod;

/* Passed to mod_draw. Colors are 0xRRGGBB. */
struct canvas {
        int w, h;          /* size of the bar being drawn */
        int baseline;      /* y of the text baseline (vertically centered) */
        uint32_t fg, bg;   /* default text and background colors */
};

/* Services the bar provides to plugins. */
struct host_api {
        /* drawing (use inside mod_draw / mod_width) */
        int  (*text_width)(const char *utf8);
        /* returns the advance in pixels. color -1 = the canvas default */
        int  (*draw_text)(struct canvas *cv, int x, const char *utf8, int32_t color);
        void (*fill_rect)(struct canvas *cv, int x, int y, int w, int h, uint32_t color);

        /* options of this module from the Lua config, e.g. { path="battery", low=20 }.
         * Returned strings stay valid until the module is destroyed. */
        const char *(*opt_str)(struct mod *m, const char *key, const char *def);
        int         (*opt_int)(struct mod *m, const char *key, int def);
        double      (*opt_num)(struct mod *m, const char *key, double def);
        int         (*opt_bool)(struct mod *m, const char *key, int def);

        /* draw a w x h image at (x, y). pixels are premultiplied ARGB32 (the
         * format of cairo's CAIRO_FORMAT_ARGB32); stride is in bytes. */
        void (*draw_image)(struct canvas *cv, int x, int y, int w, int h, const uint32_t *pixels, int stride);
};

struct mod {
        /* ---- yours ---- */
        void *state;         /* private pointer for the plugin */
        char text[256];      /* what the bar draws (when there is no mod_draw) */
        int32_t color;       /* -1 = no override; else 0xRRGGBB for this text */
        int interval_ms;     /* call mod_update every N ms. 0 = never (event only).
                                Set the default in mod_init; the user's Lua
                                `interval = seconds` overrides it afterwards. */
        int align;           /* 1 = align updates to the wall clock (so a 1000 ms
                                clock ticks exactly on the second) */
        int event_fd;        /* -1, or an fd the bar polls and reports through
                                mod_event. Set it in mod_init. */

        /* ---- the bar's ---- */
        const struct host_api *host;
};

/* ---- helpers (static inline, so every plugin gets its own copy) ---------- */

/* printf into m->text. Returns 1 if the text changed (handy as the return
 * value of mod_update), else 0. */
static inline int __attribute__((format(printf, 2, 3)))
mod_set_text(struct mod *m, const char *fmt, ...)
{
        char tmp[sizeof m->text];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(tmp, sizeof tmp, fmt, ap);
        va_end(ap);
        if (!strcmp(tmp, m->text)) return 0;
        memcpy(m->text, tmp, sizeof tmp);
        return 1;
}

/* Expand "{key}" placeholders: mod_format(out, n, "vol {pct}%", keys, vals, 1) */
static inline void
mod_format(char *out, size_t size, const char *fmt,
           const char *const keys[], const char *const vals[], int n)
{
        size_t o = 0;
        for (const char *p = fmt; *p && o + 1 < size;) {
                if (*p == '{') {
                        const char *end = strchr(p, '}');
                        if (end) {
                                size_t kl = (size_t) (end - p - 1);
                                int hit   = 0;
                                for (int i = 0; i < n && !hit; i++) {
                                        if (strlen(keys[i]) != kl || strncmp(keys[i], p + 1, kl)) continue;
                                        for (const char *v = vals[i]; *v && o + 1 < size; v++) out[o++] = *v;
                                        hit = 1;
                                }
                                if (hit) {
                                        p = end + 1;
                                        continue;
                                }
                        }
                }
                out[o++] = *p++;
        }
        out[o] = 0;
}

/* "#rrggbb" / "rrggbb" / "0xrrggbb" -> 0xRRGGBB, or def if s is NULL/invalid */
static inline int32_t
mod_parse_color(const char *s, int32_t def)
{
        if (!s) return def;
        if (*s == '#') s++;
        else if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
        if (strlen(s) != 6) return def;
        char *end;
        unsigned long v = strtoul(s, &end, 16);
        return (end == s || *end) ? def : (int32_t) v;
}

/* Read a small file (e.g. in /sys), trimming trailing whitespace.
 * Returns the length, or -1 if it cannot be read. */
static inline int
mod_read_file(const char *path, char *buf, size_t size)
{
        FILE *f = fopen(path, "r");
        if (!f) return -1;
        size_t n = fread(buf, 1, size - 1, f);
        fclose(f);
        while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r' || buf[n - 1] == ' ')) n--;
        buf[n] = 0;
        return (int) n;
}

/* Start a long-lived child (argv[0] is looked up in PATH). Its stdout is
 * returned as a non-blocking fd in *fd (use it as m->event_fd). The child is
 * killed if the bar dies. Returns the pid, or -1. When you are done:
 * kill(pid, SIGTERM); waitpid(pid, NULL, 0); close(fd). */
static inline pid_t
mod_spawn(char *const argv[], int *fd)
{
        int p[2];
        if (pipe2(p, O_CLOEXEC) < 0) return -1;
        pid_t pid = fork();
        if (pid < 0) {
                close(p[0]);
                close(p[1]);
                return -1;
        }
        if (pid == 0) {
                prctl(PR_SET_PDEATHSIG, SIGTERM);
                dup2(p[1], 1);
                int dn = open("/dev/null", O_WRONLY);
                if (dn >= 0) dup2(dn, 2);
                execvp(argv[0], argv);
                _exit(127);
        }
        close(p[1]);
        fcntl(p[0], F_SETFL, O_NONBLOCK);
        *fd = p[0];
        return pid;
}

/* Run a short command and capture its stdout into buf (NUL-terminated).
 * Gives up after ~500 ms. Returns the byte count, or -1 on failure/non-zero exit. */
static inline int
mod_run(char *const argv[], char *buf, size_t size)
{
        int fd;
        pid_t pid = mod_spawn(argv, &fd);
        if (pid < 0) return -1;
        size_t n = 0;
        for (;;) {
                struct pollfd pf = { .fd = fd, .events = POLLIN };
                int r            = poll(&pf, 1, 500);
                if (r == 0) {
                        kill(pid, SIGKILL);
                        break;
                }
                if (r < 0) continue;
                ssize_t k = read(fd, buf + n, size - 1 - n);
                if (k <= 0 || (n += (size_t) k) >= size - 1) break;
        }
        buf[n] = 0;
        close(fd);
        int st = 0;
        waitpid(pid, &st, 0);
        return (WIFEXITED(st) && WEXITSTATUS(st) == 0) ? (int) n : -1;
}

#endif /* MYBAR_PLUGIN_H */
