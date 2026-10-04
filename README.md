# mybar

A small Wayland status bar.

- One bar at the bottom of every monitor (wlr-layer-shell: Hyprland, sway, river...)
- Configured in Lua; it reloads when you save the config
- Modules are plugins: one `.c` file each, loaded as a `.so`
- Event driven: it does nothing until something changes

Shipped modules: `workspaces` (with app icons), `window`, `language`, `volume`,
`battery`, `network` and `datetime`. Several of them only work on Hyprland.

# Installation

1. Install the dependencies (Arch): `wayland wayland-protocols fcft pixman luajit`,
   plus `librsvg cairo` for the workspace app icons (optional).
2. `make`
3. `make install` (to `/usr/local`), or run it in place: `make run`.

With [pm](https://github.com/hugoocoto/pm):

```lua
ur.Fetch { user = "hugoocoto", file = "mybar/mybar.lua" },
```

# Configuration

mybar reads `~/.config/mybar/config.lua`, then `./config.lua` (or `-c FILE`).
Start from the [config.lua](config.lua) in this repo; every option is commented
there. Each module documents its own options at the top of `modules/NAME.c`.

`pkill -USR1 mybar` forces a reload.

# Writing a module

Drop a `.c` file in `modules/` and run `make`: it becomes `modules/NAME.so`, and
you add `"NAME"` to the config. The interface is [src/plugin.h](src/plugin.h);
[examples/](examples/) has a minimal text module (`uptime.c`) and one that draws
itself (`meter.c`).
