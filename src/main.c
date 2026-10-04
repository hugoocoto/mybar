/* mybar - a small Wayland status bar.
 *
 *   - bottom of every monitor, reserves its space (wlr-layer-shell)
 *   - text rendered with fcft, composited with pixman
 *   - configured in Lua (conf.h); the file is watched with inotify and the
 *     bar reloads when it changes
 *   - modules are plugins: modules/NAME.c -> NAME.so, loaded with dlopen()
 *     (see plugin.h for the interface)
 *
 * Sections: 1 types and globals | 2 config | 3 plugins | 4 drawing |
 *           5 wayland | 6 reload and inotify | 7 main loop
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <fcft/fcft.h>
#include <pixman.h>
#include <wayland-client.h>
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

#define INCLUDE_CONF_IMPLEMENTATION
#include "conf.h"
#include "cum.h"
#include "plugin.h"

/* ========================================================================== */
/* 1. types and globals                                                       */
/* ========================================================================== */

struct config {
        int height, padding_left, padding_right, spacing;
        char *font;
        char *modules_dir; /* may be NULL */
        uint32_t bg, fg;
};

/* One loaded plugin. `pub` must stay first: plugins get a struct mod * and
 * we cast it back. */
struct mod_inst {
        struct mod pub;
        void *dl;
        char *path;
        char cfgpath[64]; /* "Config.modules.left.2" */
        int (*init)(struct mod *);
        void (*destroy)(struct mod *);
        int (*update)(struct mod *);
        int (*event)(struct mod *);
        int (*width_fn)(struct mod *);
        void (*draw_fn)(struct mod *, struct canvas *, int);
        int64_t next_ms;     /* next mod_update deadline (monotonic ms) */
        int sched_interval;  /* interval next_ms was computed for */
        int32_t cfg_color;   /* `color = "#rrggbb"` from the config, or -1 */
        int spacing;         /* `spacing = px` after this module, or -1 = Config.spacing */
        int width;           /* measured at draw time */
        Da(char *) owned;    /* strings handed out by opt_str */
};

typedef Da(struct mod_inst *) ModList;
enum { LEFT, CENTER, RIGHT, NGROUPS };
static const char *const group_names[NGROUPS] = { "left", "center", "right" };

struct bar;
struct buf {
        struct wl_buffer *wl;
        void *data;
        int busy; /* the compositor may still be reading it */
        struct bar *bar;
};

struct bar { /* one per monitor */
        struct wl_output *output;
        uint32_t global_name;
        struct wl_surface *surface;
        struct zwlr_layer_surface_v1 *ls;
        struct buf bufs[2];
        void *map;
        size_t map_size;
        int w, h;
        int configured;
        int needs_redraw; /* wanted to draw but both buffers were busy */
        struct bar *next;
};

static struct config cfg;
static Conf conf;
static char *cfg_path, *cfg_dir;
static ModList groups[NGROUPS];
static struct fcft_font *font;

static struct wl_display *display;
static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct zwlr_layer_shell_v1 *layer_shell;
static struct bar *bars;
static int ready;

static volatile sig_atomic_t running = 1;
static int sigpipe[2];
static int ifd = -1; /* inotify */
static struct { int wd; char name[NAME_MAX + 1]; } watches[2];
static int nwatch;

static int64_t
now_ms(void)
{
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}

static int64_t
wall_ms(void)
{
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}

static char *
dir_of(const char *p)
{
        const char *s = strrchr(p, '/');
        if (!s) return strdup(".");
        if (s == p) return strdup("/");
        return strndup(p, (size_t) (s - p));
}

/* ========================================================================== */
/* 2. config (Lua, through conf.h)                                            */
/* ========================================================================== */

static int
cfg_int(const char *path, int def)
{
        int v;
        return Conf_get_int(conf, &v, "%s", path) == CONF_OK ? v : def;
}

/* The returned string is a copy: conf.h strings die on the next Lua call. */
static char *
cfg_strdup(const char *path, const char *def)
{
        const char *v;
        if (Conf_get_str(conf, &v, "%s", path) == CONF_OK) return strdup(v);
        return def ? strdup(def) : NULL;
}

static uint32_t
cfg_color(const char *path, uint32_t def)
{
        const char *v;
        if (Conf_get_str(conf, &v, "%s", path) != CONF_OK) return def;
        return (uint32_t) mod_parse_color(v, (int32_t) def);
}

static void
read_config(struct config *c)
{
        c->height      = cfg_int("Config.height", 32);
        int padding      = cfg_int("Config.padding", 12);
        c->padding_left  = cfg_int("Config.padding_left", padding);
        c->padding_right = cfg_int("Config.padding_right", padding);
        c->spacing     = cfg_int("Config.spacing", 16);
        c->font        = cfg_strdup("Config.font", "monospace:size=14");
        c->modules_dir = cfg_strdup("Config.modules_dir", NULL);
        c->bg          = cfg_color("Config.bg", 0x1e1e2e);
        c->fg          = cfg_color("Config.fg", 0xcdd6f4);
        if (c->height < 8) c->height = 8;
        if (c->height > 512) c->height = 512;
}

static void
free_config(struct config *c)
{
        free(c->font);
        free(c->modules_dir);
}

static char *
find_config(void)
{
        const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
        char p[PATH_MAX] = "";
        if (xdg && *xdg) snprintf(p, sizeof p, "%s/mybar/config.lua", xdg);
        else if (home) snprintf(p, sizeof p, "%s/.config/mybar/config.lua", home);
        if (p[0] && access(p, R_OK) == 0) return strdup(p);
        if (access("config.lua", R_OK) == 0) return strdup("config.lua");
        return NULL;
}

/* ========================================================================== */
/* 3. plugins                                                                 */
/* ========================================================================== */

/* ---- the services plugins call (host_api) ---- */

static struct mod_inst *
inst_of(struct mod *m)
{
        return (struct mod_inst *) m;
}

static const char *
api_opt_str(struct mod *m, const char *key, const char *def)
{
        struct mod_inst *mi = inst_of(m);
        const char *v;
        if (Conf_get_str(conf, &v, "%s.%s", mi->cfgpath, key) != CONF_OK) return def;
        char *copy = strdup(v);
        Da_append(&mi->owned, copy); /* freed with the module */
        return copy;
}

static int
api_opt_int(struct mod *m, const char *key, int def)
{
        int v;
        return Conf_get_int(conf, &v, "%s.%s", inst_of(m)->cfgpath, key) == CONF_OK ? v : def;
}

static double
api_opt_num(struct mod *m, const char *key, double def)
{
        double v;
        return Conf_get_num(conf, &v, "%s.%s", inst_of(m)->cfgpath, key) == CONF_OK ? v : def;
}

static int
api_opt_bool(struct mod *m, const char *key, int def)
{
        int v;
        return Conf_get_bool(conf, &v, "%s.%s", inst_of(m)->cfgpath, key) == CONF_OK ? v : def;
}

static int api_text_width(const char *text);
static int api_draw_text(struct canvas *cv, int x, const char *text, int32_t color);
static void api_fill_rect(struct canvas *cv, int x, int y, int w, int h, uint32_t color);
static void api_draw_image(struct canvas *cv, int x, int y, int w, int h, const uint32_t *pixels, int stride);

static const struct host_api host_api = {
        .text_width = api_text_width,
        .draw_text  = api_draw_text,
        .fill_rect  = api_fill_rect,
        .opt_str    = api_opt_str,
        .opt_int    = api_opt_int,
        .opt_num    = api_opt_num,
        .opt_bool   = api_opt_bool,
        .draw_image = api_draw_image,
};

/* ---- finding the .so ---- */

static int
file_ok(const char *p)
{
        return access(p, R_OK) == 0;
}

/* "<a>/<b>" as a malloc'd string */
static char *
join(const char *a, const char *b)
{
        char *r = NULL;
        if (asprintf(&r, "%s/%s", a, b) < 0) return NULL;
        return r;
}

/* If dir/file exists return it (malloc'd), else NULL. */
static char *
try_dir(const char *dir, const char *file)
{
        if (!dir || !*dir) return NULL;
        char *p = join(dir, file);
        if (p && file_ok(p)) return p;
        free(p);
        return NULL;
}

/* spec: "battery", "battery.so", "modules/battery.so", "/abs/battery.so", "~/x.so"
 * Names without '/' are searched in modules_dir, <config dir>/modules,
 * ./modules, ~/.local/lib/mybar/modules and the system install dirs. Returns a malloc'd path or NULL. */
static char *
resolve_plugin(const char *spec)
{
        size_t l   = strlen(spec);
        int has_so = l > 3 && !strcmp(spec + l - 3, ".so");
        char *file = NULL, *found = NULL;
        const char *home = getenv("HOME");

        if (spec[0] == '~' && spec[1] == '/' && home) {
                if (asprintf(&file, "%s/%s%s", home, spec + 2, has_so ? "" : ".so") < 0) return NULL;
        } else {
                if (asprintf(&file, "%s%s", spec, has_so ? "" : ".so") < 0) return NULL;
        }

        if (strchr(file, '/')) {
                if (file[0] == '/') found = file_ok(file) ? strdup(file) : NULL;
                else if (!(found = try_dir(cfg_dir, file))) /* relative to the config... */
                        found = file_ok(file) ? strdup(file) : NULL; /* ...or to the cwd */
        } else {
                char *mdir = NULL, *cdir = join(cfg_dir, "modules");
                char *ldir = home ? join(home, ".local/lib/mybar/modules") : NULL;
                if (cfg.modules_dir)
                        mdir = cfg.modules_dir[0] == '/' ? strdup(cfg.modules_dir) : join(cfg_dir, cfg.modules_dir);
                const char *dirs[] = { mdir, cdir, "./modules", ldir, "/usr/local/lib/mybar/modules", "/usr/lib/mybar/modules" };
                for (size_t i = 0; i < sizeof dirs / sizeof *dirs && !found; i++) found = try_dir(dirs[i], file);
                free(mdir);
                free(cdir);
                free(ldir);
        }
        free(file);
        return found;
}

/* ---- lifecycle ---- */

static void
mod_schedule(struct mod_inst *m, int64_t now)
{
        m->sched_interval = m->pub.interval_ms;
        if (m->pub.interval_ms <= 0) {
                m->next_ms = INT64_MAX;
                return;
        }
        if (m->pub.align) { /* land just after the next multiple of the interval */
                int64_t iv = m->pub.interval_ms;
                m->next_ms = now + (iv - wall_ms() % iv) + 2;
        } else {
                m->next_ms = now + m->pub.interval_ms;
        }
}

static void
mod_free(struct mod_inst *m)
{
        if (m->destroy) m->destroy(&m->pub);
        if (m->dl) dlclose(m->dl);
        for (int i = 0; i < m->owned.count; i++) free(m->owned.items[i]);
        Da_destroy(&m->owned);
        free(m->path);
        free(m);
}

/* Load entry `idx` (1-based) of Config.modules.<group>. NULL on failure. */
static struct mod_inst *
mod_load(const char *group, int idx)
{
        char entry[64];
        snprintf(entry, sizeof entry, "Config.modules.%s.%d", group, idx);

        /* an entry is "name" or { path = "name", ...options } */
        const char *spec = NULL;
        char *name = NULL;
        if (Conf_get_str(conf, &spec, "%s", entry) == CONF_OK) name = strdup(spec);
        else if (Conf_get_str(conf, &spec, "%s.path", entry) == CONF_OK) name = strdup(spec);
        if (!name) {
                fprintf(stderr, "mybar: modules.%s[%d]: expected a string or a table with 'path'\n", group, idx);
                return NULL;
        }

        char *path = resolve_plugin(name);
        if (!path) {
                fprintf(stderr, "mybar: modules.%s[%d]: plugin '%s' not found (did you run make?)\n", group, idx, name);
                free(name);
                return NULL;
        }
        free(name);

        struct mod_inst *m = calloc(1, sizeof *m);
        m->path            = path;
        m->cfg_color       = -1;
        m->spacing         = -1;
        m->pub.color       = -1;
        m->pub.event_fd    = -1;
        m->pub.host        = &host_api;
        snprintf(m->cfgpath, sizeof m->cfgpath, "%s", entry);

        m->dl = dlopen(path, RTLD_NOW | RTLD_LOCAL);
        if (!m->dl) {
                fprintf(stderr, "mybar: %s\n", dlerror());
                mod_free(m);
                return NULL;
        }
#define SYM(field, sym) (*(void **) &m->field = dlsym(m->dl, sym))
        SYM(init, "mod_init");
        SYM(destroy, "mod_destroy");
        SYM(update, "mod_update");
        SYM(event, "mod_event");
        SYM(width_fn, "mod_width");
        SYM(draw_fn, "mod_draw");
#undef SYM

        if (m->init && m->init(&m->pub) != 0) {
                fprintf(stderr, "mybar: %s: disabled (init failed)\n", path);
                m->destroy = NULL; /* a failed init cleans up after itself */
                mod_free(m);
                return NULL;
        }

        /* per-module options understood by the bar itself */
        double secs;
        if (Conf_get_num(conf, &secs, "%s.interval", entry) == CONF_OK)
                m->pub.interval_ms = secs > 0 ? (int) (secs * 1000.0) : 0;
        const char *col;
        if (Conf_get_str(conf, &col, "%s.color", entry) == CONF_OK)
                m->cfg_color = mod_parse_color(col, -1);
        m->spacing = api_opt_int(&m->pub, "spacing", -1);

        if (m->pub.event_fd >= 0 && !m->event) {
                fprintf(stderr, "mybar: %s sets event_fd but has no mod_event\n", path);
                m->pub.event_fd = -1;
        }

        if (m->update) m->update(&m->pub);
        mod_schedule(m, now_ms());
        fprintf(stderr, "mybar: loaded %s\n", path);
        return m;
}

static void
load_groups(ModList out[NGROUPS])
{
        for (int g = 0; g < NGROUPS; g++) {
                int n = 0;
                Conf_get_len(conf, &n, "Config.modules.%s", group_names[g]);
                for (int i = 1; i <= n; i++) {
                        struct mod_inst *m = mod_load(group_names[g], i);
                        if (m) Da_append(&out[g], m);
                }
        }
}

/* ========================================================================== */
/* 4. drawing                                                                 */
/* ========================================================================== */

struct canvas_impl {
        struct canvas pub; /* first */
        pixman_image_t *dst;
};

static pixman_color_t
color_from_rgb(uint32_t rgb)
{
        return (pixman_color_t){
                .red   = ((rgb >> 16) & 0xff) * 0x101,
                .green = ((rgb >> 8) & 0xff) * 0x101,
                .blue  = (rgb & 0xff) * 0x101,
                .alpha = 0xffff,
        };
}

/* Decode one UTF-8 code point; advances *p. Invalid bytes become U+FFFD. */
static uint32_t
utf8_next(const char **p)
{
        const unsigned char *s = (const unsigned char *) *p;
        uint32_t cp;
        int extra;
        if (s[0] < 0x80) { cp = s[0]; extra = 0; }
        else if (s[0] >= 0xf0) { cp = s[0] & 0x07; extra = 3; }
        else if (s[0] >= 0xe0) { cp = s[0] & 0x0f; extra = 2; }
        else if (s[0] >= 0xc0) { cp = s[0] & 0x1f; extra = 1; }
        else { *p += 1; return 0xfffd; }
        for (int i = 1; i <= extra; i++) {
                if ((s[i] & 0xc0) != 0x80) { *p += i; return 0xfffd; }
                cp = (cp << 6) | (s[i] & 0x3f);
        }
        *p += extra + 1;
        return cp;
}

static int
api_text_width(const char *text)
{
        int w = 0;
        while (*text) {
                const struct fcft_glyph *g = fcft_rasterize_char_utf32(font, utf8_next(&text), FCFT_SUBPIXEL_NONE);
                if (g) w += g->advance.x;
        }
        return w;
}

static int
api_draw_text(struct canvas *pcv, int x, const char *text, int32_t color)
{
        struct canvas_impl *cv = (struct canvas_impl *) pcv;
        pixman_color_t c       = color_from_rgb(color >= 0 ? (uint32_t) color : cv->pub.fg);
        pixman_image_t *fg     = pixman_image_create_solid_fill(&c);
        int x0                 = x;

        while (*text) {
                const struct fcft_glyph *g = fcft_rasterize_char_utf32(font, utf8_next(&text), FCFT_SUBPIXEL_NONE);
                if (!g) continue;
                if (pixman_image_get_format(g->pix) == PIXMAN_a8r8g8b8) /* color glyph (emoji) */
                        pixman_image_composite32(PIXMAN_OP_OVER, g->pix, NULL, cv->dst, 0, 0, 0, 0,
                                                 x + g->x, cv->pub.baseline - g->y, g->width, g->height);
                else /* alpha mask over the text color */
                        pixman_image_composite32(PIXMAN_OP_OVER, fg, g->pix, cv->dst, 0, 0, 0, 0,
                                                 x + g->x, cv->pub.baseline - g->y, g->width, g->height);
                x += g->advance.x;
        }
        pixman_image_unref(fg);
        return x - x0;
}

static void
api_fill_rect(struct canvas *pcv, int x, int y, int w, int h, uint32_t color)
{
        struct canvas_impl *cv = (struct canvas_impl *) pcv;
        pixman_color_t c       = color_from_rgb(color);
        pixman_box32_t box     = { x, y, x + w, y + h };
        pixman_image_fill_boxes(PIXMAN_OP_OVER, cv->dst, &c, 1, &box);
}

static void
api_draw_image(struct canvas *pcv, int x, int y, int w, int h, const uint32_t *pixels, int stride)
{
        struct canvas_impl *cv = (struct canvas_impl *) pcv;
        pixman_image_t *src    = pixman_image_create_bits(PIXMAN_a8r8g8b8, w, h, (uint32_t *) pixels, stride);
        if (!src) return;
        pixman_image_composite32(PIXMAN_OP_OVER, src, NULL, cv->dst, 0, 0, 0, 0, x, y, w, h);
        pixman_image_unref(src);
}

static void
layout_widths(void)
{
        for (int g = 0; g < NGROUPS; g++)
                Da_foreach(mp, groups[g]) {
                        struct mod_inst *m = *mp;
                        m->width = m->width_fn ? m->width_fn(&m->pub) : api_text_width(m->pub.text);
                        if (m->width < 0) m->width = 0;
                }
}

static int
spacing_after(struct mod_inst *m)
{
        return m->spacing >= 0 ? m->spacing : cfg.spacing;
}

/* Modules with width 0 (empty text) are hidden and take no spacing. */
static int
group_width(int g)
{
        int total = 0, last = 0;
        Da_foreach(mp, groups[g]) {
                if ((*mp)->width <= 0) continue;
                last   = spacing_after(*mp);
                total += (*mp)->width + last;
        }
        return total - last; /* no spacing after the last one */
}

static void
draw_group(struct canvas_impl *cv, int g, int x)
{
        Da_foreach(mp, groups[g]) {
                struct mod_inst *m = *mp;
                if (m->width <= 0) continue;
                int32_t c = m->pub.color >= 0 ? m->pub.color : (m->cfg_color >= 0 ? m->cfg_color : (int32_t) cfg.fg);
                cv->pub.fg = (uint32_t) c;
                if (m->draw_fn) m->draw_fn(&m->pub, &cv->pub, x);
                else api_draw_text(&cv->pub, x, m->pub.text, -1);
                x += m->width + spacing_after(m);
        }
        cv->pub.fg = cfg.fg;
}

static void
bar_draw(struct bar *b)
{
        if (!b->configured || !b->map) return;

        struct buf *buf = NULL;
        for (int i = 0; i < 2; i++)
                if (!b->bufs[i].busy) { buf = &b->bufs[i]; break; }
        if (!buf) { /* both in use: buf_release will call us again */
                b->needs_redraw = 1;
                return;
        }
        b->needs_redraw = 0;

        layout_widths();

        pixman_image_t *dst = pixman_image_create_bits_no_clear(PIXMAN_a8r8g8b8, b->w, b->h, buf->data, b->w * 4);
        pixman_color_t bg   = color_from_rgb(cfg.bg);
        pixman_box32_t full = { 0, 0, b->w, b->h };
        pixman_image_fill_boxes(PIXMAN_OP_SRC, dst, &bg, 1, &full);

        struct canvas_impl cv = {
                .pub = { .w = b->w, .h = b->h, .fg = cfg.fg, .bg = cfg.bg,
                         .baseline = (b->h - (font->ascent + font->descent)) / 2 + font->ascent },
                .dst = dst,
        };
        draw_group(&cv, LEFT, cfg.padding_left);
        draw_group(&cv, CENTER, (b->w - group_width(CENTER)) / 2);
        draw_group(&cv, RIGHT, b->w - cfg.padding_right - group_width(RIGHT));
        pixman_image_unref(dst);

        wl_surface_attach(b->surface, buf->wl, 0, 0);
        wl_surface_damage_buffer(b->surface, 0, 0, b->w, b->h);
        wl_surface_commit(b->surface);
        buf->busy = 1;
}

static void
redraw_all(void)
{
        for (struct bar *b = bars; b; b = b->next) bar_draw(b);
}

/* ========================================================================== */
/* 5. wayland                                                                 */
/* ========================================================================== */

static void
buf_release(void *data, struct wl_buffer *wl)
{
        struct buf *buf = data;
        buf->busy       = 0;
        if (buf->bar->needs_redraw) bar_draw(buf->bar);
}
static const struct wl_buffer_listener buf_listener = { .release = buf_release };

static void
free_buffers(struct bar *b)
{
        for (int i = 0; i < 2; i++) {
                if (b->bufs[i].wl) wl_buffer_destroy(b->bufs[i].wl);
                b->bufs[i] = (struct buf){ 0 };
        }
        if (b->map) munmap(b->map, b->map_size);
        b->map      = NULL;
        b->map_size = 0;
}

static int
alloc_buffers(struct bar *b)
{
        int stride   = b->w * 4;
        size_t one   = (size_t) stride * b->h;
        size_t total = one * 2;

        int fd = memfd_create("mybar-shm", MFD_CLOEXEC);
        if (fd < 0) return -1;
        if (ftruncate(fd, (off_t) total) < 0) { close(fd); return -1; }
        void *map = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (map == MAP_FAILED) { close(fd); return -1; }

        struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, (int32_t) total);
        for (int i = 0; i < 2; i++) {
                b->bufs[i].wl   = wl_shm_pool_create_buffer(pool, (int32_t) (i * one), b->w, b->h, stride, WL_SHM_FORMAT_ARGB8888);
                b->bufs[i].data = (char *) map + i * one;
                b->bufs[i].bar  = b;
                wl_buffer_add_listener(b->bufs[i].wl, &buf_listener, &b->bufs[i]);
        }
        wl_shm_pool_destroy(pool);
        close(fd);
        b->map      = map;
        b->map_size = total;
        return 0;
}

static void bar_destroy(struct bar *b);

static void
layer_configure(void *data, struct zwlr_layer_surface_v1 *ls, uint32_t serial, uint32_t w, uint32_t h)
{
        struct bar *b = data;
        zwlr_layer_surface_v1_ack_configure(ls, serial);
        if (w == 0) w = 1;
        if (h == 0) h = (uint32_t) cfg.height;

        if (!b->configured || (int) w != b->w || (int) h != b->h) {
                free_buffers(b);
                b->w = (int) w;
                b->h = (int) h;
                if (alloc_buffers(b) < 0) {
                        fprintf(stderr, "mybar: buffer allocation failed\n");
                        b->configured = 0;
                        return;
                }
        }
        b->configured = 1;
        bar_draw(b);
}

/* The compositor closed this surface (its output went away): only remove this bar. */
static void
layer_closed(void *data, struct zwlr_layer_surface_v1 *ls)
{
        bar_destroy(data);
}

static const struct zwlr_layer_surface_v1_listener layer_listener = {
        .configure = layer_configure,
        .closed    = layer_closed,
};

static void
bar_apply_config(struct bar *b)
{
        if (!b->ls) return;
        zwlr_layer_surface_v1_set_size(b->ls, 0, (uint32_t) cfg.height);
        zwlr_layer_surface_v1_set_exclusive_zone(b->ls, cfg.height);
        wl_surface_commit(b->surface);
}

static void
bar_init_surface(struct bar *b)
{
        b->surface = wl_compositor_create_surface(compositor);
        b->ls      = zwlr_layer_shell_v1_get_layer_surface(layer_shell, b->surface, b->output,
                                                           ZWLR_LAYER_SHELL_V1_LAYER_TOP, "mybar");
        zwlr_layer_surface_v1_set_anchor(b->ls, ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
                                                ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
                                                ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
        zwlr_layer_surface_v1_set_size(b->ls, 0, (uint32_t) cfg.height);
        zwlr_layer_surface_v1_set_exclusive_zone(b->ls, cfg.height);
        zwlr_layer_surface_v1_add_listener(b->ls, &layer_listener, b);
        wl_surface_commit(b->surface); /* empty commit -> configure */
}

static struct bar *
bar_add(struct wl_output *out, uint32_t name)
{
        struct bar *b = calloc(1, sizeof *b);
        b->output      = out;
        b->global_name = name;
        b->next        = bars;
        bars           = b;
        return b;
}

static void
bar_destroy(struct bar *b)
{
        for (struct bar **p = &bars; *p; p = &(*p)->next)
                if (*p == b) { *p = b->next; break; }
        free_buffers(b);
        if (b->ls) zwlr_layer_surface_v1_destroy(b->ls);
        if (b->surface) wl_surface_destroy(b->surface);
        if (b->output) {
                if (wl_output_get_version(b->output) >= 3) wl_output_release(b->output);
                else wl_output_destroy(b->output);
        }
        free(b);
}

static void
reg_global(void *data, struct wl_registry *reg, uint32_t name, const char *iface, uint32_t version)
{
        if (!strcmp(iface, wl_compositor_interface.name))
                compositor = wl_registry_bind(reg, name, &wl_compositor_interface, 4);
        else if (!strcmp(iface, wl_shm_interface.name))
                shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
        else if (!strcmp(iface, zwlr_layer_shell_v1_interface.name))
                layer_shell = wl_registry_bind(reg, name, &zwlr_layer_shell_v1_interface, 1);
        else if (!strcmp(iface, wl_output_interface.name)) {
                struct wl_output *out = wl_registry_bind(reg, name, &wl_output_interface, version < 3 ? version : 3);
                struct bar *b         = bar_add(out, name);
                if (ready) bar_init_surface(b); /* hotplug; at startup surfaces are made after the roundtrip */
        }
}

static void
reg_remove(void *data, struct wl_registry *reg, uint32_t name)
{
        for (struct bar *b = bars; b; b = b->next)
                if (b->global_name == name) { bar_destroy(b); return; }
}

static const struct wl_registry_listener reg_listener = {
        .global        = reg_global,
        .global_remove = reg_remove,
};

/* ========================================================================== */
/* 6. loading, reloading, inotify                                             */
/* ========================================================================== */

static struct fcft_font *
load_font(const char *name)
{
        const char *names[] = { name };
        return fcft_from_name(1, names, NULL);
}

/* Load (or reload) the config and all modules. On any error the previous
 * state is kept. Returns 0 on success. */
static int
config_load(int initial)
{
        Conf nc = NULL;
        if (Conf_open(&nc, cfg_path) != CONF_OK) {
                fprintf(stderr, "mybar: cannot load '%s'%s\n", cfg_path, initial ? "" : " (keeping the previous config)");
                return -1;
        }
        Conf old = conf;
        conf     = nc; /* from here on the cfg_* helpers read the new file */

        struct config ncfg;
        read_config(&ncfg);
        struct fcft_font *nfont = load_font(ncfg.font);
        if (!nfont) {
                fprintf(stderr, "mybar: cannot load font '%s'%s\n", ncfg.font, initial ? "" : " (keeping the previous config)");
                conf = old;
                Conf_close(nc);
                free_config(&ncfg);
                return -1;
        }

        struct config ocfg      = cfg;
        struct fcft_font *ofont = font;
        cfg                     = ncfg;
        font                    = nfont;

        /* Unload the old modules *before* loading the new ones: while the old
         * copy is still loaded dlopen() would hand it back, and a recompiled
         * .so would not be picked up. (Config errors were already ruled out
         * above, so the old modules are only dropped when we can go ahead.) */
        for (int g = 0; g < NGROUPS; g++) {
                Da_foreach(mp, groups[g]) mod_free(*mp);
                Da_destroy(&groups[g]);
        }
        load_groups(groups);
        if (ofont) fcft_destroy(ofont);
        if (old) Conf_close(old);

        if (!initial) {
                if (ocfg.height != cfg.height)
                        for (struct bar *b = bars; b; b = b->next) bar_apply_config(b);
                fprintf(stderr, "mybar: config reloaded\n");
        }
        free_config(&ocfg);
        return 0;
}

static void
watch_add(const char *path)
{
        if (nwatch >= 2) return;
        char *d = dir_of(path);
        /* Watch the directory: editors replace files by renaming, which would
         * kill a watch on the file itself. */
        int wd = inotify_add_watch(ifd, d, IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE);
        free(d);
        if (wd < 0) return;
        const char *base = strrchr(path, '/');
        base             = base ? base + 1 : path;
        watches[nwatch].wd = wd;
        snprintf(watches[nwatch].name, sizeof watches[nwatch].name, "%s", base);
        nwatch++;
}

static void
setup_inotify(void)
{
        ifd = inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
        if (ifd < 0) {
                perror("mybar: inotify");
                return;
        }
        watch_add(cfg_path);
        char *real = realpath(cfg_path, NULL); /* config may be a symlink (dotfile managers) */
        if (real && strcmp(real, cfg_path)) watch_add(real);
        free(real);
}

static void
drain_inotify(int64_t now, int64_t *reload_at)
{
        char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
        ssize_t n;
        while ((n = read(ifd, buf, sizeof buf)) > 0) {
                for (char *p = buf; p < buf + n;) {
                        struct inotify_event *ev = (struct inotify_event *) p;
                        if (ev->len)
                                for (int i = 0; i < nwatch; i++)
                                        if (watches[i].wd == ev->wd && !strcmp(watches[i].name, ev->name))
                                                *reload_at = now + 150; /* debounce: editors write in bursts */
                        p += sizeof *ev + ev->len;
                }
        }
}

/* ========================================================================== */
/* 7. main loop                                                               */
/* ========================================================================== */

/* signal handlers only write a byte; the loop reads it (no races with poll) */
static void
on_signal(int sig)
{
        int e           = errno;
        unsigned char c = (unsigned char) sig;
        (void) !write(sigpipe[1], &c, 1);
        errno = e;
}

static void
drain_signals(int64_t now, int64_t *reload_at)
{
        unsigned char c[32];
        ssize_t n;
        while ((n = read(sigpipe[0], c, sizeof c)) > 0)
                for (ssize_t i = 0; i < n; i++) {
                        if (c[i] == SIGUSR1) *reload_at = now;
                        else running = 0;
                }
}

int
main(int argc, char **argv)
{
        for (int i = 1; i < argc; i++) {
                if (!strcmp(argv[i], "-c") && i + 1 < argc) cfg_path = strdup(argv[++i]);
                else {
                        fprintf(stderr, "usage: %s [-c config.lua]\n"
                                        "  default config: $XDG_CONFIG_HOME/mybar/config.lua, then ./config.lua\n"
                                        "  SIGUSR1 forces a reload\n", argv[0]);
                        return !strcmp(argv[i], "-h");
                }
        }
        if (!cfg_path) cfg_path = find_config();
        if (!cfg_path) {
                fprintf(stderr, "mybar: no config found (try -c config.lua)\n");
                return 1;
        }
        cfg_dir = dir_of(cfg_path);

        if (pipe2(sigpipe, O_CLOEXEC | O_NONBLOCK) < 0) return 1;
        struct sigaction sa = { .sa_handler = on_signal };
        sigemptyset(&sa.sa_mask);
        sigaction(SIGINT, &sa, NULL);
        sigaction(SIGTERM, &sa, NULL);
        sigaction(SIGUSR1, &sa, NULL);
        signal(SIGPIPE, SIG_IGN);

        fcft_init(FCFT_LOG_COLORIZE_NEVER, false, FCFT_LOG_CLASS_ERROR);
        if (config_load(1) < 0) return 1;
        setup_inotify();

        display = wl_display_connect(NULL);
        if (!display) { fprintf(stderr, "mybar: cannot connect to Wayland\n"); return 1; }
        struct wl_registry *reg = wl_display_get_registry(display);
        wl_registry_add_listener(reg, &reg_listener, NULL);
        wl_display_roundtrip(display);
        if (!compositor || !shm || !layer_shell) {
                fprintf(stderr, "mybar: compositor lacks wl_compositor/wl_shm/wlr-layer-shell\n");
                return 1;
        }
        ready = 1;
        for (struct bar *b = bars; b; b = b->next) bar_init_surface(b);

        Da(struct pollfd) pfds        = { 0 };
        Da(struct mod_inst *) pmods   = { 0 };
        int64_t reload_at             = 0;
        int dirty                     = 0;
        enum { P_WAYLAND, P_INOTIFY, P_SIGNAL, P_MODS };

        while (running) {
                int64_t now      = now_ms();
                int64_t deadline = INT64_MAX;

                /* timers: a plugin may have changed its interval */
                for (int g = 0; g < NGROUPS; g++)
                        Da_foreach(mp, groups[g]) {
                                struct mod_inst *m = *mp;
                                if (m->pub.interval_ms != m->sched_interval) mod_schedule(m, now);
                                if (m->next_ms < deadline) deadline = m->next_ms;
                        }
                if (reload_at && reload_at < deadline) deadline = reload_at;

                /* fds: wayland, inotify, signals, then every module's event_fd */
                pfds.count = pmods.count = 0;
                Da_append(&pfds, ((struct pollfd){ .fd = wl_display_get_fd(display), .events = POLLIN }));
                Da_append(&pfds, ((struct pollfd){ .fd = ifd, .events = POLLIN }));
                Da_append(&pfds, ((struct pollfd){ .fd = sigpipe[0], .events = POLLIN }));
                for (int g = 0; g < NGROUPS; g++)
                        Da_foreach(mp, groups[g]) {
                                struct mod_inst *m = *mp;
                                if (m->pub.event_fd < 0) continue;
                                Da_append(&pfds, ((struct pollfd){ .fd = m->pub.event_fd, .events = POLLIN }));
                                Da_append(&pmods, m);
                        }

                int timeout = -1;
                if (deadline != INT64_MAX) {
                        int64_t t = deadline - now;
                        timeout   = t < 0 ? 0 : (t > 3600000 ? 3600000 : (int) t);
                }

                while (wl_display_prepare_read(display) != 0)
                        if (wl_display_dispatch_pending(display) < 0) goto out;
                if (wl_display_flush(display) < 0 && errno != EAGAIN) {
                        wl_display_cancel_read(display);
                        break;
                }

                int r = poll(pfds.items, (nfds_t) pfds.count, timeout);

                short wre = r > 0 ? pfds.items[P_WAYLAND].revents : 0;
                if (wre & POLLIN) {
                        if (wl_display_read_events(display) < 0) break;
                } else {
                        wl_display_cancel_read(display);
                        if (wre & (POLLERR | POLLHUP | POLLNVAL)) break; /* compositor gone */
                }
                if (wl_display_dispatch_pending(display) < 0) break;

                now = now_ms();
                if (r > 0) {
                        if (pfds.items[P_INOTIFY].revents & POLLIN) drain_inotify(now, &reload_at);
                        if (pfds.items[P_SIGNAL].revents & POLLIN) drain_signals(now, &reload_at);
                        for (int i = 0; i < pmods.count; i++) {
                                struct pollfd *p   = &pfds.items[P_MODS + i];
                                struct mod_inst *m = pmods.items[i];
                                /* let the plugin see data and also end-of-stream (HUP) */
                                if (p->revents & (POLLIN | POLLERR | POLLHUP)) {
                                        if (m->event && m->event(&m->pub)) dirty = 1;
                                }
                                /* a plugin that keeps a dead fd would spin the loop: stop polling it */
                                if ((p->revents & (POLLERR | POLLHUP | POLLNVAL)) && m->pub.event_fd == p->fd) {
                                        fprintf(stderr, "mybar: %s: event fd closed\n", m->path);
                                        m->pub.event_fd = -1;
                                }
                        }
                }

                /* periodic updates */
                for (int g = 0; g < NGROUPS; g++)
                        Da_foreach(mp, groups[g]) {
                                struct mod_inst *m = *mp;
                                if (m->pub.interval_ms <= 0 || now < m->next_ms) continue;
                                if (m->update && m->update(&m->pub)) dirty = 1;
                                mod_schedule(m, now);
                        }

                if (reload_at && now >= reload_at) {
                        reload_at = 0;
                        if (config_load(0) == 0) dirty = 1;
                }
                if (dirty) {
                        dirty = 0;
                        redraw_all();
                }
        }

out:
        for (int g = 0; g < NGROUPS; g++) {
                Da_foreach(mp, groups[g]) mod_free(*mp);
                Da_destroy(&groups[g]);
        }
        while (bars) bar_destroy(bars);
        wl_registry_destroy(reg);
        wl_display_flush(display);
        wl_display_disconnect(display);
        if (font) fcft_destroy(font);
        fcft_fini();
        if (conf) Conf_close(conf);
        Da_destroy(&pfds);
        Da_destroy(&pmods);
        return 0;
}
