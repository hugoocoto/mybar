/* workspaces: Hyprland workspaces, the focused one highlighted. Hyprland only;
 * event driven through Hyprland's event socket, like the window module.
 *   { path = "workspaces", format = "{name}", pad = 6, gap = 0,
 *     active_fg = "#272727", active_bg = "#888888", fg = "#888888",
 *     special = false, persistent = 0,
 *     icon_size = 16, icon_gap = 4, icon_theme = "hicolor" }
 * placeholders: {id} {name} {windows} (window count) {icons}
 * {icons} draws the app icon of every window in the workspace, looked up like
 * a taskbar does: window class -> .desktop file -> Icon= -> icon theme
 * (icon_theme, then the themes it inherits, then hicolor). Needs librsvg and
 * cairo at build time; without them {icons} draws nothing.
 * active_fg/active_bg default to the bar's colors swapped; active_bg = "none"
 * draws no box. fg is the color of the other workspaces (default: the bar's).
 * pad_left/pad_right override pad on one side. special = true also lists
 * special workspaces (scratchpads, negative ids). persistent = N always shows
 * workspaces 1..N, even when they do not exist.
 * Disables itself when not running under Hyprland. */
#include "plugin.h"
#pragma GCC diagnostic ignored "-Wformat-truncation" /* paths: cutting is harmless */
#include <ctype.h>
#include <dirent.h>
#include <limits.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/un.h>

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
        int id;
        char label[64], label2[64]; /* text before and after the icons */
        int icons[MAX_ICONS];       /* indexes into st.cache */
        int nicons;
        int width, width1, width2;  /* total, label, label2; filled in mod_width */
};

struct st {
        char dir[256];
        char *fmt, *fmt2; /* format split at {icons}; fmt2 is NULL without it */
        int pad_left, pad_right, gap, special, persistent, no_box;
        int32_t active_fg, active_bg, fg;
        int icon_size, icon_gap;
        char *icon_theme;
        struct icon *cache;
        int ncache;
        struct ws ws[MAX_WS];
        int n, active;
        char buf[4096]; /* partial event line */
        size_t len;
        char resp[262144]; /* reply to j/workspaces and j/clients */
};

static int
unix_connect(const char *path)
{
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

/* Send req to Hyprland's command socket and read the whole reply into out.
 * Returns the length, or -1. */
static int
hypr_request(struct st *s, const char *req, char *out, size_t size)
{
        char path[320];
        snprintf(path, sizeof path, "%s/.socket.sock", s->dir);
        int fd = unix_connect(path);
        if (fd < 0) return -1;
        size_t n = 0, rl = strlen(req);
        if (write(fd, req, rl) == (ssize_t) rl) {
                for (;;) {
                        struct pollfd pf = { .fd = fd, .events = POLLIN };
                        if (poll(&pf, 1, 300) <= 0) break;
                        ssize_t k = read(fd, out + n, size - 1 - n);
                        if (k <= 0 || (n += (size_t) k) >= size - 1) break;
                }
        }
        out[n] = 0;
        close(fd);
        return (int) n;
}

/* Copy the JSON string value of "key" from json into out (unescaped). */
static void
json_string(const char *json, const char *key, char *out, size_t size)
{
        char pat[64];
        out[0] = 0;
        snprintf(pat, sizeof pat, "\"%s\":", key);
        const char *p = strstr(json, pat);
        if (!p) return;
        p += strlen(pat);
        while (*p == ' ') p++;
        if (*p != '"') return;
        p++;
        size_t o = 0;
        while (*p && *p != '"' && o + 4 < size) {
                if (*p != '\\') {
                        out[o++] = *p++;
                        continue;
                }
                p++;
                switch (*p) {
                case 'n': case 't': case 'r': out[o++] = ' '; p++; break;
                case 'u': {
                        unsigned cp = (unsigned) strtoul((char[5]){ p[1], p[2], p[3], p[4], 0 }, NULL, 16);
                        p += 5;
                        if (cp >= 0xd800 && cp < 0xdc00 && p[0] == '\\' && p[1] == 'u') { /* surrogate pair */
                                unsigned lo = (unsigned) strtoul((char[5]){ p[2], p[3], p[4], p[5], 0 }, NULL, 16);
                                cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
                                p += 6;
                        }
                        if (cp < 0x80) out[o++] = (char) cp;
                        else if (cp < 0x800) { out[o++] = 0xc0 | (cp >> 6); out[o++] = 0x80 | (cp & 0x3f); }
                        else if (cp < 0x10000) { out[o++] = 0xe0 | (cp >> 12); out[o++] = 0x80 | ((cp >> 6) & 0x3f); out[o++] = 0x80 | (cp & 0x3f); }
                        else { out[o++] = 0xf0 | (cp >> 18); out[o++] = 0x80 | ((cp >> 12) & 0x3f); out[o++] = 0x80 | ((cp >> 6) & 0x3f); out[o++] = 0x80 | (cp & 0x3f); }
                        break;
                }
                default: if (*p) out[o++] = *p++; break; /* \" \\ \/ */
                }
        }
        out[o] = 0;
}

/* The JSON integer value of "key" in json, or def. */
static int
json_int(const char *json, const char *key, int def)
{
        char pat[64];
        snprintf(pat, sizeof pat, "\"%s\":", key);
        const char *p = strstr(json, pat);
        if (!p) return def;
        char *end;
        long v = strtol(p + strlen(pat), &end, 10);
        return end == p + strlen(pat) ? def : (int) v;
}

/* Find the next top-level {...} in a JSON array, skipping braces inside
 * strings (window titles can contain anything). Terminates it in place and
 * returns its start; *next is where to continue. NULL when there are no more. */
static char *
next_object(char *p, char **next)
{
        while (*p && *p != '{') p++;
        if (!*p) return NULL;
        char *start = p;
        int depth = 0, in_str = 0;
        for (; *p; p++) {
                if (in_str) {
                        if (*p == '\\' && p[1]) p++;
                        else if (*p == '"') in_str = 0;
                } else if (*p == '"') in_str = 1;
                else if (*p == '{') depth++;
                else if (*p == '}' && --depth == 0) {
                        *p    = 0;
                        *next = p + 1;
                        return start;
                }
        }
        return NULL; /* truncated reply */
}

static int
ws_cmp(const void *a, const void *b)
{
        const struct ws *x = a, *y = b;
        return (x->id > y->id) - (x->id < y->id);
}

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
find_icon(struct st *s, const char *name, char *out, size_t size)
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
load_icon(struct st *s, const char *class)
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

static void *load_icon(struct st *s, const char *class) { return NULL; }
static void free_icon(void *img) {}
static void pin_librsvg(void) {}

#endif

/* Index in the icon cache for this window class (loaded on first use), or -1
 * if the class has no icon. */
static int
icon_index(struct st *s, const char *class)
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

/* Fill the icons of every workspace in ws[0..n) from j/clients. */
static void
add_icons(struct st *s, struct ws *ws, int n)
{
        if (hypr_request(s, "j/clients", s->resp, sizeof s->resp) <= 0) return;
        char *p = s->resp, *obj;
        while ((obj = next_object(p, &p))) {
                if (strstr(obj, "\"mapped\": false") || strstr(obj, "\"mapped\":false")) continue;
                char *wsobj = strstr(obj, "\"workspace\":");
                if (!wsobj) continue;
                int id = json_int(wsobj, "id", 0);
                char class[128];
                json_string(obj, "class", class, sizeof class);
                if (!class[0]) json_string(obj, "initialClass", class, sizeof class);
                for (int i = 0; i < n; i++) {
                        if (ws[i].id != id || ws[i].nicons >= MAX_ICONS) continue;
                        int ic = class[0] ? icon_index(s, class) : -1;
                        if (ic >= 0) ws[i].icons[ws[i].nicons++] = ic;
                }
        }
}

/* ---- the module ---------------------------------------------------------- */

static void
make_labels(struct st *s, struct ws *w, const char *id, const char *name, const char *wins)
{
        const char *keys[] = { "id", "name", "windows" };
        const char *vals[] = { id, name, wins };
        mod_format(w->label, sizeof w->label, s->fmt, keys, vals, 3);
        w->label2[0] = 0;
        if (s->fmt2) mod_format(w->label2, sizeof w->label2, s->fmt2, keys, vals, 3);
}

/* Re-query Hyprland. Returns 1 if anything we show changed. */
static int
refresh(struct mod *m)
{
        struct st *s = m->state;
        struct ws ws[MAX_WS];
        int n = 0, active = s->active;
        char small[8192];

        if (hypr_request(s, "j/activeworkspace", small, sizeof small) > 0)
                active = json_int(small, "id", active);

        if (hypr_request(s, "j/workspaces", s->resp, sizeof s->resp) > 0) {
                char *p = s->resp, *obj;
                while (n < MAX_WS && (obj = next_object(p, &p))) {
                        int id = json_int(obj, "id", 0);
                        if (id == 0 || (id < 0 && !s->special)) continue;
                        char name[64], idstr[16], wins[16];
                        json_string(obj, "name", name, sizeof name);
                        snprintf(idstr, sizeof idstr, "%d", id);
                        snprintf(wins, sizeof wins, "%d", json_int(obj, "windows", 0));
                        if (!strncmp(name, "special:", 8)) memmove(name, name + 8, strlen(name + 8) + 1);

                        memset(&ws[n], 0, sizeof ws[n]);
                        ws[n].id = id;
                        make_labels(s, &ws[n], idstr, name, wins);
                        n++;
                }
        }
        for (int id = 1; id <= s->persistent && n < MAX_WS; id++) {
                int have = 0;
                for (int i = 0; i < n && !have; i++) have = ws[i].id == id;
                if (have) continue;
                char idstr[16];
                snprintf(idstr, sizeof idstr, "%d", id);
                memset(&ws[n], 0, sizeof ws[n]);
                ws[n].id = id;
                make_labels(s, &ws[n], idstr, idstr, "0");
                n++;
        }
        qsort(ws, (size_t) n, sizeof *ws, ws_cmp);

        if (s->fmt2) {
                add_icons(s, ws, n);
                /* "{name} {icons}" on an empty workspace: drop the dangling space */
                for (int i = 0; i < n; i++) {
                        if (ws[i].nicons) continue;
                        size_t l = strlen(ws[i].label);
                        while (l && ws[i].label[l - 1] == ' ') ws[i].label[--l] = 0;
                }
        }

        int changed = n != s->n || active != s->active;
        for (int i = 0; i < n && !changed; i++)
                changed = ws[i].id != s->ws[i].id || strcmp(ws[i].label, s->ws[i].label) ||
                          strcmp(ws[i].label2, s->ws[i].label2) || ws[i].nicons != s->ws[i].nicons ||
                          memcmp(ws[i].icons, s->ws[i].icons, (size_t) ws[i].nicons * sizeof *ws[i].icons);
        memcpy(s->ws, ws, (size_t) n * sizeof *ws);
        s->n      = n;
        s->active = active;
        return changed;
}

MOD_API int
mod_init(struct mod *m)
{
        const char *xdg = getenv("XDG_RUNTIME_DIR");
        const char *sig = getenv("HYPRLAND_INSTANCE_SIGNATURE");
        if (!xdg || !sig) {
                fprintf(stderr, "workspaces: not running under Hyprland\n");
                return -1;
        }
        struct st *s = calloc(1, sizeof *s);
        if (!s) return -1;
        snprintf(s->dir, sizeof s->dir, "%s/hypr/%s", xdg, sig);

        char path[320];
        snprintf(path, sizeof path, "%s/.socket2.sock", s->dir);
        int fd = unix_connect(path);
        if (fd < 0) {
                fprintf(stderr, "workspaces: cannot connect to %s\n", path);
                free(s);
                return -1;
        }
        fcntl(fd, F_SETFL, O_NONBLOCK);

        const char *fmt = m->host->opt_str(m, "format", "{name}");
        const char *ic  = strstr(fmt, "{icons}");
        s->fmt          = ic ? strndup(fmt, (size_t) (ic - fmt)) : strdup(fmt);
        s->fmt2         = ic ? strdup(ic + 7) : NULL;
        int pad         = m->host->opt_int(m, "pad", 6);
        s->pad_left     = m->host->opt_int(m, "pad_left", pad);
        s->pad_right    = m->host->opt_int(m, "pad_right", pad);
        s->gap          = m->host->opt_int(m, "gap", 0);
        s->special      = m->host->opt_bool(m, "special", 0);
        s->persistent   = m->host->opt_int(m, "persistent", 0);
        const char *bg  = m->host->opt_str(m, "active_bg", NULL);
        s->no_box       = bg && !strcmp(bg, "none");
        s->active_fg    = mod_parse_color(m->host->opt_str(m, "active_fg", NULL), -1);
        s->active_bg    = mod_parse_color(bg, -1);
        s->fg           = mod_parse_color(m->host->opt_str(m, "fg", NULL), -1);
        s->icon_size    = m->host->opt_int(m, "icon_size", 16);
        s->icon_gap     = m->host->opt_int(m, "icon_gap", 4);
        s->icon_theme   = strdup(m->host->opt_str(m, "icon_theme", "hicolor"));
        if (s->icon_size < 1) s->icon_size = 1;
        if (s->fmt2) pin_librsvg();
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
        for (int i = 0; i < s->ncache; i++) free_icon(s->cache[i].img);
        free(s->cache);
        free(s->fmt);
        free(s->fmt2);
        free(s->icon_theme);
        free(s);
}

MOD_API int
mod_update(struct mod *m)
{
        return refresh(m);
}

MOD_API int
mod_event(struct mod *m)
{
        static const char *const events[] = {
                "workspace>>", "workspacev2>>", "focusedmon>>", "focusedmonv2>>",
                "createworkspace", "destroyworkspace", "moveworkspace", "renameworkspace",
                "activespecial", "openwindow>>", "closewindow>>", "movewindow",
        };
        struct st *s = m->state;
        int need = 0;
        ssize_t n;

        while ((n = read(m->event_fd, s->buf + s->len, sizeof s->buf - 1 - s->len)) > 0) {
                s->len += (size_t) n;
                s->buf[s->len] = 0;
                char *line = s->buf, *nl;
                while ((nl = strchr(line, '\n'))) {
                        *nl = 0;
                        for (size_t i = 0; i < sizeof events / sizeof *events && !need; i++)
                                need = !strncmp(line, events[i], strlen(events[i]));
                        line = nl + 1;
                }
                s->len = strlen(line);
                memmove(s->buf, line, s->len + 1);
                if (s->len >= sizeof s->buf - 1) s->len = 0; /* absurdly long line: drop it */
        }
        if (n == 0) { /* Hyprland went away */
                close(m->event_fd);
                m->event_fd = -1;
                s->n        = 0;
                return 1;
        }
        return need ? refresh(m) : 0;
}

MOD_API int
mod_width(struct mod *m)
{
        struct st *s = m->state;
        int w = 0;
        for (int i = 0; i < s->n; i++) {
                struct ws *ws = &s->ws[i];
                ws->width1    = m->host->text_width(ws->label);
                ws->width2    = m->host->text_width(ws->label2);
                ws->width     = s->pad_left + ws->width1 + ws->width2 + s->pad_right;
                if (ws->nicons) ws->width += ws->nicons * s->icon_size + (ws->nicons - 1) * s->icon_gap;
                w += ws->width + (i ? s->gap : 0);
        }
        return w;
}

MOD_API void
mod_draw(struct mod *m, struct canvas *cv, int x)
{
        struct st *s = m->state;
        for (int i = 0; i < s->n; i++) {
                struct ws *w = &s->ws[i];
                int32_t fg   = s->fg;
                if (w->id == s->active) {
                        if (!s->no_box)
                                m->host->fill_rect(cv, x, 0, w->width, cv->h, (uint32_t) (s->active_bg >= 0 ? s->active_bg : (int32_t) cv->fg));
                        fg = s->active_fg >= 0 ? s->active_fg : (s->no_box ? -1 : (int32_t) cv->bg);
                }
                int cx = x + s->pad_left;
                m->host->draw_text(cv, cx, w->label, fg);
                cx += w->width1;
#ifdef HAVE_ICONS
                int y = (cv->h - s->icon_size) / 2;
                for (int k = 0; k < w->nicons; k++) {
                        cairo_surface_t *img = s->cache[w->icons[k]].img;
                        m->host->draw_image(cv, cx, y, s->icon_size, s->icon_size,
                                            (const uint32_t *) cairo_image_surface_get_data(img),
                                            cairo_image_surface_get_stride(img));
                        cx += s->icon_size + s->icon_gap;
                }
                if (w->nicons) cx -= s->icon_gap;
#endif
                m->host->draw_text(cv, cx, w->label2, fg);
                x += w->width + s->gap;
        }
}
