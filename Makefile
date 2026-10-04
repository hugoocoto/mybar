# mybar: Wayland status bar with Lua config and shared-library plugins.
#   make          build the bar and every modules/*.c -> modules/*.so
#   make run      build and run with ./config.lua
#   make LUA_PKG=lua5.1   pick the Lua package explicitly (default: luajit, else lua5.1)

CC       ?= cc
CFLAGS   ?= -O2 -Wall -Wextra -Wno-unused-parameter

TARGET   = mybar
GEN      = gen
LUA_PKG ?= $(shell for p in luajit lua5.1 lua-5.1 lua51; do pkg-config --exists $$p && echo $$p && break; done)
PKGS     = wayland-client fcft pixman-1 $(LUA_PKG)

ALL_CFLAGS = $(CFLAGS) -std=gnu11 -D_GNU_SOURCE -Isrc -Ivendor -I$(GEN) $(shell pkg-config --cflags $(PKGS))
ALL_LIBS   = $(shell pkg-config --libs $(PKGS)) -ldl $(LDLIBS)

# plugins: every modules/NAME.c becomes modules/NAME.so automatically
MOD_SRC    = $(wildcard modules/*.c)
MOD_SO     = $(MOD_SRC:.c=.so)
MOD_CFLAGS = $(CFLAGS) -std=gnu11 -D_GNU_SOURCE -Isrc -shared -fPIC -fvisibility=hidden
# extra flags for a plugin that needs libraries: MOD_LIBS_<name>
ICON_PKGS  = librsvg-2.0 cairo
MOD_LIBS_workspaces = $(if $(shell pkg-config --exists $(ICON_PKGS) && echo y),-DHAVE_ICONS $(shell pkg-config --cflags --libs $(ICON_PKGS)))

XDG_XML    = $(shell pkg-config --variable=pkgdatadir wayland-protocols)/stable/xdg-shell/xdg-shell.xml
LAYER_XML  = protocols/wlr-layer-shell-unstable-v1.xml
LAYER_URL  = https://raw.githubusercontent.com/swaywm/wlr-protocols/master/unstable/wlr-layer-shell-unstable-v1.xml
CONF_URL   = https://raw.githubusercontent.com/hugoocoto/conf.h/main/conf.h
CUM_URL    = https://raw.githubusercontent.com/hugoocoto/cum.h/main/cum.h

GEN_HDRS   = $(GEN)/xdg-shell-client-protocol.h $(GEN)/wlr-layer-shell-unstable-v1-client-protocol.h
GEN_SRCS   = $(GEN)/xdg-shell-protocol.c $(GEN)/wlr-layer-shell-protocol.c
OBJS       = src/main.o $(GEN_SRCS:.c=.o)

.PHONY: all modules run clean distclean install uninstall
all: $(TARGET) modules
modules: $(MOD_SO)

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(ALL_LIBS)

src/main.o: src/main.c src/plugin.h vendor/conf.h vendor/cum.h | $(GEN_HDRS)
	$(CC) $(ALL_CFLAGS) -c -o $@ $<

$(GEN)/%.o: $(GEN)/%.c
	$(CC) $(ALL_CFLAGS) -w -c -o $@ $<

modules/%.so: modules/%.c src/plugin.h
	$(CC) $(MOD_CFLAGS) -o $@ $< $(MOD_LIBS_$*)

# ---- downloaded / generated files ----
$(GEN) protocols vendor:
	mkdir -p $@

vendor/conf.h: | vendor
	curl -fsSL -o $@ $(CONF_URL)
vendor/cum.h: | vendor
	curl -fsSL -o $@ $(CUM_URL)
$(LAYER_XML): | protocols
	curl -fsSL -o $@ $(LAYER_URL)

$(GEN)/xdg-shell-client-protocol.h: | $(GEN)
	wayland-scanner client-header $(XDG_XML) $@
$(GEN)/xdg-shell-protocol.c: | $(GEN)
	wayland-scanner private-code $(XDG_XML) $@
$(GEN)/wlr-layer-shell-unstable-v1-client-protocol.h: $(LAYER_XML) | $(GEN)
	wayland-scanner client-header $< $@
$(GEN)/wlr-layer-shell-protocol.c: $(LAYER_XML) | $(GEN)
	wayland-scanner private-code $< $@

run: all
	./$(TARGET) -c config.lua

PREFIX ?= /usr/local
install: all
	install -Dm755 $(TARGET) $(DESTDIR)$(PREFIX)/bin/$(TARGET)
	install -d $(DESTDIR)$(PREFIX)/lib/mybar/modules
	install -m755 $(MOD_SO) $(DESTDIR)$(PREFIX)/lib/mybar/modules/
	install -Dm644 config.lua $(DESTDIR)$(PREFIX)/share/mybar/config.lua
uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/$(TARGET)
	rm -rf $(DESTDIR)$(PREFIX)/lib/mybar $(DESTDIR)$(PREFIX)/share/mybar

clean:
	rm -rf $(TARGET) src/*.o $(GEN) modules/*.so
# also removes the downloaded protocol XML and conf.h/cum.h
distclean: clean
	rm -rf protocols vendor
