/* workspaces.h - what the workspace modules share (workspaces.c for Hyprland,
 * sway_workspaces.c for sway): the options, the app icons, the labels and the
 * drawing. A backend only lists the workspaces and the class of every window:
 *
 *   ws_begin(v);
 *   ws_add(v, id, name, windows);       for every workspace (windows < 0: count
 *                                       the ws_add_window calls instead)
 *   ws_add_window(v, id, class);        for every window
 *   changed = ws_commit(v, active_id);
 *
 * and calls ws_options / ws_free / ws_width / ws_draw from its mod_* functions.
 *
 * Options (read by ws_options):
 *   format = "{name}", pad = 6, pad_left, pad_right, gap = 0,
 *   active_fg = "#272727", active_bg = "#888888" (or "none"), fg = "#888888",
 *   persistent = 0, icon_size = 16, icon_gap = 4, icon_theme = "hicolor"
 * placeholders: {id} {name} {windows} (window count) {icons}
 * {icons} draws the app icon of every window in the workspace, looked up like
 * a taskbar does: window class -> .desktop file -> Icon= -> icon theme
 * (icon_theme, then the themes it inherits, then hicolor). Needs librsvg and
 * cairo at build time (-DHAVE_ICONS); without them {icons} draws nothing.
 * active_fg/active_bg default to the bar's colors swapped; active_bg = "none"
 * draws no box. fg is the color of the other workspaces (default: the bar's).
 * pad_left/pad_right override pad on one side. persistent = N always shows
 * workspaces 1..N, even when they do not exist. */
#ifndef MYBAR_WORKSPACES_H
#define MYBAR_WORKSPACES_H

#include "plugin.h"
#pragma GCC diagnostic ignored "-Wformat-truncation" /* paths: cutting is harmless */
#include <ctype.h>
#include <dirent.h>
#include <limits.h>
#include <strings.h>

#ifdef HAVE_ICONS
#include <cairo.h>
#include <dlfcn.h>
#include <librsvg/rsvg.h>
#endif

#define MAX_WS 64
#define MAX_ICONS 16

struct icon {
        char class[128];
        void *img; /* cairo_surface_t, or NULL when the class has no icon */
};

struct ws {
        int id, windows, count_windows;
        char name[64];
        char label[64], label2[64]; /* text before and after the icons */
        int icons[MAX_ICONS];       /* indexes into wsview.cache */
        int nicons;
        int width, width1, width2;  /* total, label, label2; filled in ws_width */
};

struct wsview {
        char *fmt, *fmt2; /* format split at {icons}; fmt2 is NULL without it */
        int pad_left, pad_right, gap, persistent, no_box;
        int32_t active_fg, active_bg, fg;
        int icon_size, icon_gap;
        char *icon_theme;
        struct icon *cache;
        int ncache;
        struct ws ws[MAX_WS]; /* what is drawn */
        int n, active;
        struct ws next[MAX_WS]; /* being filled by the backend */
        int nnext;
};

/* ---- app icons ----------------------------------------------------------- */

#ifdef HAVE_ICONS

#define MAX_DIRS 16

/* XDG data dirs ($XDG_DATA_HOME first) with `sub` appended. Returns the count. */
static int
data_dirs(const char *sub, char out[][PATH_MAX], int max)
{
        const char *home = getenv("HOME"), *dh = getenv("XDG_DATA_HOME"), *dd = getenv("XDG_DATA_DIRS");
        int n = 0;
        if (dh && *dh) snprintf(out[n++], PATH_MAX, "%s/%s", dh, sub);
        else if (home) snprintf(out[n++], PATH_MAX, "%s/.local/share/%s", home, sub);
        if (!dd || !*dd) dd = "/usr/local/share:/usr/share";
        while (*dd && n < max) {
                size_t l = strcspn(dd, ":");
                if (l) snprintf(out[n++], PATH_MAX, "%.*s/%s", (int) l, dd, sub);
                dd += l + (dd[l] == ':');
        }
        return n;
}

/* Value of key in the [Desktop Entry] section of a .desktop file. */
static int
desktop_key(const char *path, const char *key, char *out, size_t size)
{
        FILE *f = fopen(path, "r");
        if (!f) return 0;
        char line[1024];
        size_t kl = strlen(key);
        int in_entry = 0, found = 0;
        while (!found && fgets(line, sizeof line, f)) {
                line[strcspn(line, "\r\n")] = 0;
                if (line[0] == '[') in_entry = !strcmp(line, "[Desktop Entry]");
                else if (in_entry && !strncmp(line, key, kl) && line[kl] == '=') {
                        snprintf(out, size, "%s", line + kl + 1);
                        found = 1;
                }
        }
        fclose(f);
        return found;
}

/* Icon= of the .desktop file of this window class. Tried in order: CLASS.desktop,
 * *.CLASS.desktop (reverse-DNS ids), StartupWMClass=CLASS (all case-insensitive). */
static int
desktop_icon(const char *class, char *out, size_t size)
{
        char dirs[MAX_DIRS][PATH_MAX];
        int nd = data_dirs("applications", dirs, MAX_DIRS);
        char want[160], suffix[160];
        snprintf(want, sizeof want, "%s.desktop", class);
        snprintf(suffix, sizeof suffix, ".%s.desktop", class);
        size_t sl = strlen(suffix);

        for (int pass = 0; pass < 3; pass++)
                for (int i = 0; i < nd; i++) {
                        DIR *d = opendir(dirs[i]);
                        if (!d) continue;
                        struct dirent *e;
                        int found = 0;
                        while (!found && (e = readdir(d))) {
                                size_t l = strlen(e->d_name);
                                if (l < 9 || strcmp(e->d_name + l - 8, ".desktop")) continue;
                                char p[PATH_MAX * 2], wm[128];
                                snprintf(p, sizeof p, "%s/%s", dirs[i], e->d_name);
                                if (pass == 0) found = !strcasecmp(e->d_name, want);
                                else if (pass == 1) found = l > sl && !strcasecmp(e->d_name + l - sl, suffix);
                                else found = desktop_key(p, "StartupWMClass", wm, sizeof wm) && !strcasecmp(wm, class);
                                if (found) found = desktop_key(p, "Icon", out, size) && *out;
                        }
                        closedir(d);
                        if (found) return 1;
                }
        return 0;
}

/* Base dirs that hold icon themes. */
static int
icon_dirs(char out[][PATH_MAX], int max)
{
        const char *home = getenv("HOME");
        int n = 0;
        if (home) snprintf(out[n++], PATH_MAX, "%s/.icons", home);
        return n + data_dirs("icons", out + n, max - n);
}

/* Look for icon `name` in one theme. Fills *inherits (comma separated) from
 * its index.theme. Returns 1 and the best file in out. */
static int
theme_lookup(const char *theme, const char *name, int want, char *inherits, size_t isize, char *out, size_t size)
{
        char bases[MAX_DIRS][PATH_MAX], p[PATH_MAX * 2];
        int nb = icon_dirs(bases, MAX_DIRS);
        FILE *f = NULL;
        for (int i = 0; i < nb && !f; i++) {
                snprintf(p, sizeof p, "%s/%s/index.theme", bases[i], theme);
                f = fopen(p, "r");
        }
        if (!f) return 0;

        /* each [section] is a directory of the theme, with its Size / Type */
        char line[512], sect[256] = "";
        int best = INT_MAX, size_px = 0, scalable = 0;
        inherits[0] = 0;
        for (int eof = 0; !eof && best;) {
                eof = !fgets(line, sizeof line, f);
                line[strcspn(line, "\r\n")] = 0;
                if (eof || line[0] == '[') {
                        /* finish the previous section */
                        for (int i = 0; sect[0] && i < nb && best; i++)
                                for (int k = 0; k < 2; k++) {
                                        const char *ext = k ? "svg" : "png";
                                        snprintf(p, sizeof p, "%s/%s/%s/%s.%s", bases[i], theme, sect, name, ext);
                                        if (access(p, R_OK)) continue;
                                        int dist = (scalable || k) ? 0 : size_px >= want ? size_px - want : 2 * (want - size_px);
                                        if (dist < best) {
                                                best = dist;
                                                snprintf(out, size, "%s", p);
                                        }
                                }
                        sect[0]  = 0;
                        size_px  = 0;
                        scalable = 0;
                        if (!eof && strcmp(line, "[Icon Theme]"))
                                snprintf(sect, sizeof sect, "%.*s", (int) strcspn(line + 1, "]"), line + 1);
                } else if (!strncmp(line, "Inherits=", 9)) snprintf(inherits, isize, "%s", line + 9);
                else if (!strncmp(line, "Size=", 5)) size_px = atoi(line + 5);
                else if (!strcmp(line, "Type=Scalable")) scalable = 1;
        }
        fclose(f);
        return best != INT_MAX;
}

/* Path of icon `name`: an absolute path, the theme chain, then loose files
 * in the icon dirs and /usr/share/pixmaps. */
static int
find_icon(struct wsview *s, const char *name, char *out, size_t size)
{
        if (name[0] == '/') {
                snprintf(out, size, "%s", name);
                return access(out, R_OK) == 0;
        }
        char themes[8][64];
        int nt = 0;
        snprintf(themes[nt++], sizeof themes[0], "%s", s->icon_theme);
        if (strcmp(s->icon_theme, "hicolor")) snprintf(themes[nt++], sizeof themes[0], "hicolor");
        for (int t = 0; t < nt; t++) {
                char inh[256];
                if (theme_lookup(themes[t], name, s->icon_size, inh, sizeof inh, out, size)) return 1;
                for (char *q = strtok(inh, ","); q && nt < 8; q = strtok(NULL, ",")) {
                        int seen = 0;
                        for (int k = 0; k < nt && !seen; k++) seen = !strcmp(themes[k], q);
                        if (!seen) snprintf(themes[nt++], sizeof themes[0], "%s", q);
                }
        }
        char bases[MAX_DIRS][PATH_MAX];
        int nb = icon_dirs(bases, MAX_DIRS - 1);
        snprintf(bases[nb++], PATH_MAX, "/usr/share/pixmaps");
        for (int i = 0; i < nb; i++)
                for (int k = 0; k < 2; k++) {
                        snprintf(out, size, "%s/%s.%s", bases[i], name, k ? "svg" : "png");
                        if (access(out, R_OK) == 0) return 1;
                }
        return 0;
}

/* Render an svg/png into a size x size ARGB32 surface, centered. */
static cairo_surface_t *
render_icon(const char *path, int size)
{
        cairo_surface_t *img = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
        cairo_t *cr          = cairo_create(img);
        size_t l             = strlen(path);
        int ok               = 0;
        if (l > 4 && !strcasecmp(path + l - 4, ".svg")) {
                RsvgHandle *h = rsvg_handle_new_from_file(path, NULL);
                if (h) {
                        RsvgRectangle vp = { 0, 0, size, size };
                        ok               = rsvg_handle_render_document(h, cr, &vp, NULL);
                        g_object_unref(h);
                }
        } else {
                cairo_surface_t *src = cairo_image_surface_create_from_png(path);
                if (cairo_surface_status(src) == CAIRO_STATUS_SUCCESS) {
                        int w = cairo_image_surface_get_width(src), h = cairo_image_surface_get_height(src);
                        double sc = (double) size / (w > h ? w : h);
                        cairo_translate(cr, (size - w * sc) / 2, (size - h * sc) / 2);
                        cairo_scale(cr, sc, sc);
                        cairo_set_source_surface(cr, src, 0, 0);
                        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
                        cairo_paint(cr);
                        ok = 1;
                }
                cairo_surface_destroy(src);
        }
        cairo_destroy(cr);
        if (!ok) {
                cairo_surface_destroy(img);
                return NULL;
        }
        cairo_surface_flush(img);
        return img;
}

static void *
load_icon(struct wsview *s, const char *class)
{
        char name[256], lower[128], path[PATH_MAX * 2];
        snprintf(lower, sizeof lower, "%s", class);
        for (char *c = lower; *c; c++) *c = (char) tolower((unsigned char) *c);

        if ((desktop_icon(class, name, sizeof name) && find_icon(s, name, path, sizeof path)) ||
            find_icon(s, class, path, sizeof path) || find_icon(s, lower, path, sizeof path))
                return render_icon(path, s->icon_size);
        return NULL;
}

static void
free_icon(void *img)
{
        if (img) cairo_surface_destroy(img);
}

/* librsvg must never be unloaded (Rust thread-locals); pin it in memory so a
 * config reload that dlclose()s us does not take it along. */
static void
pin_librsvg(void)
{
        Dl_info info;
        if (dladdr((void *) rsvg_handle_new_from_file, &info) && info.dli_fname)
                dlopen(info.dli_fname, RTLD_NOW | RTLD_NOLOAD | RTLD_NODELETE);
}

#else /* !HAVE_ICONS */

static void *load_icon(struct wsview *s, const char *class) { return NULL; }
static void free_icon(void *img) {}
static void pin_librsvg(void) {}

#endif

/* Index in the icon cache for this window class (loaded on first use), or -1
 * if the class has no icon. */
static int
icon_index(struct wsview *s, const char *class)
{
        for (int i = 0; i < s->ncache; i++)
                if (!strcmp(s->cache[i].class, class)) return s->cache[i].img ? i : -1;
        struct icon *c = realloc(s->cache, (size_t) (s->ncache + 1) * sizeof *c);
        if (!c) return -1;
        s->cache = c;
        c        = &s->cache[s->ncache++];
        snprintf(c->class, sizeof c->class, "%s", class);
        c->img = load_icon(s, class);
        return c->img ? s->ncache - 1 : -1;
}

/* ---- the shared module code ---------------------------------------------- */

static void
ws_options(struct mod *m, struct wsview *v)
{
        const char *fmt = m->host->opt_str(m, "format", "{name}");
        const char *ic  = strstr(fmt, "{icons}");
        v->fmt          = ic ? strndup(fmt, (size_t) (ic - fmt)) : strdup(fmt);
        v->fmt2         = ic ? strdup(ic + 7) : NULL;
        int pad         = m->host->opt_int(m, "pad", 6);
        v->pad_left     = m->host->opt_int(m, "pad_left", pad);
        v->pad_right    = m->host->opt_int(m, "pad_right", pad);
        v->gap          = m->host->opt_int(m, "gap", 0);
        v->persistent   = m->host->opt_int(m, "persistent", 0);
        const char *bg  = m->host->opt_str(m, "active_bg", NULL);
        v->no_box       = bg && !strcmp(bg, "none");
        v->active_fg    = mod_parse_color(m->host->opt_str(m, "active_fg", NULL), -1);
        v->active_bg    = mod_parse_color(bg, -1);
        v->fg           = mod_parse_color(m->host->opt_str(m, "fg", NULL), -1);
        v->icon_size    = m->host->opt_int(m, "icon_size", 16);
        v->icon_gap     = m->host->opt_int(m, "icon_gap", 4);
        v->icon_theme   = strdup(m->host->opt_str(m, "icon_theme", "hicolor"));
        if (v->icon_size < 1) v->icon_size = 1;
        if (v->fmt2) pin_librsvg();
}

static void
ws_free(struct wsview *v)
{
        for (int i = 0; i < v->ncache; i++) free_icon(v->cache[i].img);
        free(v->cache);
        free(v->fmt);
        free(v->fmt2);
        free(v->icon_theme);
}

static void
ws_begin(struct wsview *v)
{
        v->nnext = 0;
}

/* windows < 0: count the ws_add_window calls */
static void
ws_add(struct wsview *v, int id, const char *name, int windows)
{
        if (v->nnext >= MAX_WS) return;
        struct ws *w = &v->next[v->nnext++];
        memset(w, 0, sizeof *w);
        w->id            = id;
        w->count_windows = windows < 0;
        w->windows       = windows < 0 ? 0 : windows;
        snprintf(w->name, sizeof w->name, "%s", name);
}

static void
ws_add_window(struct wsview *v, int id, const char *class)
{
        for (int i = 0; i < v->nnext; i++) {
                struct ws *w = &v->next[i];
                if (w->id != id) continue;
                if (w->count_windows) w->windows++;
                if (!v->fmt2 || !class || !*class || w->nicons >= MAX_ICONS) return;
                int ic = icon_index(v, class);
                if (ic >= 0) w->icons[w->nicons++] = ic;
                return;
        }
}

static int
ws_cmp(const void *a, const void *b)
{
        const struct ws *x = a, *y = b;
        return (x->id > y->id) - (x->id < y->id);
}

/* Add the persistent workspaces, sort by id, format the labels and swap the
 * new list in. Returns 1 if anything we show changed. */
static int
ws_commit(struct wsview *v, int active)
{
        for (int id = 1; id <= v->persistent; id++) {
                int have = 0;
                for (int i = 0; i < v->nnext && !have; i++) have = v->next[i].id == id;
                if (have) continue;
                char idstr[16];
                snprintf(idstr, sizeof idstr, "%d", id);
                ws_add(v, id, idstr, 0);
        }
        qsort(v->next, (size_t) v->nnext, sizeof *v->next, ws_cmp);

        for (int i = 0; i < v->nnext; i++) {
                struct ws *w = &v->next[i];
                char idstr[16], wins[16];
                snprintf(idstr, sizeof idstr, "%d", w->id);
                snprintf(wins, sizeof wins, "%d", w->windows);
                const char *keys[] = { "id", "name", "windows" };
                const char *vals[] = { idstr, w->name, wins };
                mod_format(w->label, sizeof w->label, v->fmt, keys, vals, 3);
                if (v->fmt2) mod_format(w->label2, sizeof w->label2, v->fmt2, keys, vals, 3);
                /* "{name} {icons}" on an empty workspace: drop the dangling space */
                if (v->fmt2 && !w->nicons) {
                        size_t l = strlen(w->label);
                        while (l && w->label[l - 1] == ' ') w->label[--l] = 0;
                }
        }

        int changed = v->nnext != v->n || active != v->active;
        for (int i = 0; i < v->nnext && !changed; i++) {
                struct ws *a = &v->next[i], *b = &v->ws[i];
                changed = a->id != b->id || strcmp(a->label, b->label) || strcmp(a->label2, b->label2) ||
                          a->nicons != b->nicons || memcmp(a->icons, b->icons, (size_t) a->nicons * sizeof *a->icons);
        }
        memcpy(v->ws, v->next, (size_t) v->nnext * sizeof *v->next);
        v->n      = v->nnext;
        v->active = active;
        return changed;
}

static int
ws_width(struct mod *m, struct wsview *v)
{
        int w = 0;
        for (int i = 0; i < v->n; i++) {
                struct ws *ws = &v->ws[i];
                ws->width1    = m->host->text_width(ws->label);
                ws->width2    = m->host->text_width(ws->label2);
                ws->width     = v->pad_left + ws->width1 + ws->width2 + v->pad_right;
                if (ws->nicons) ws->width += ws->nicons * v->icon_size + (ws->nicons - 1) * v->icon_gap;
                w += ws->width + (i ? v->gap : 0);
        }
        return w;
}

static void
ws_draw(struct mod *m, struct wsview *v, struct canvas *cv, int x)
{
        for (int i = 0; i < v->n; i++) {
                struct ws *w = &v->ws[i];
                int32_t fg   = v->fg;
                if (w->id == v->active) {
                        if (!v->no_box)
                                m->host->fill_rect(cv, x, 0, w->width, cv->h, (uint32_t) (v->active_bg >= 0 ? v->active_bg : (int32_t) cv->fg));
                        fg = v->active_fg >= 0 ? v->active_fg : (v->no_box ? -1 : (int32_t) cv->bg);
                }
                int cx = x + v->pad_left;
                m->host->draw_text(cv, cx, w->label, fg);
                cx += w->width1;
#ifdef HAVE_ICONS
                int y = (cv->h - v->icon_size) / 2;
                for (int k = 0; k < w->nicons; k++) {
                        cairo_surface_t *img = v->cache[w->icons[k]].img;
                        m->host->draw_image(cv, cx, y, v->icon_size, v->icon_size,
                                            (const uint32_t *) cairo_image_surface_get_data(img),
                                            cairo_image_surface_get_stride(img));
                        cx += v->icon_size + v->icon_gap;
                }
                if (w->nicons) cx -= v->icon_gap;
#endif
                m->host->draw_text(cv, cx, w->label2, fg);
                x += w->width + v->gap;
        }
}

#endif /* MYBAR_WORKSPACES_H */
