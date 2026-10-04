# mybar

A small Wayland status bar.

![mybar](screenshot.png)

- One bar at the bottom of every monitor (wlr-layer-shell: Hyprland, sway, river...)
- Configured in Lua; it reloads when you save the config
- Modules are plugins: one `.c` file each, loaded as a `.so`
- Event driven: most modules wait for the kernel or Hyprland to tell them
  something changed; the rest (cpu, disk...) read a file every few seconds

Shipped modules:

- Hyprland: `workspaces` (with app icons), `window`, `submap`, `language`
- Sway: `sway_workspaces`, `sway_window`, `sway_mode`, `sway_language` (same
  options as the Hyprland ones)
- Hardware: `battery`, `brightness`, `volume`, `capslock`
- System: `system` (cpu, memory, temperature), `disk`
- Network: `network`, `vpn`
- Other: `media` (anything playing, through playerctl), `datetime`

Modules with nothing to show (no VPN up, nothing playing...) take no space.

# Installation

1. Install the dependencies. The second line of each is optional: app icons in
   the workspaces (librsvg, cairo), `media` (playerctl) and `volume` (pactl).

   Arch:

   ```sh
   sudo pacman -S --needed base-devel wayland wayland-protocols fcft pixman luajit
   sudo pacman -S --needed librsvg cairo playerctl libpulse
   ```

   Debian / Ubuntu:

   ```sh
   sudo apt install build-essential pkg-config libwayland-dev libwayland-bin wayland-protocols libfcft-dev libpixman-1-dev libluajit-5.1-dev
   sudo apt install librsvg2-dev libcairo2-dev playerctl pulseaudio-utils
   ```

   Fedora:

   ```sh
   sudo dnf install gcc make pkgconf-pkg-config wayland-devel wayland-protocols-devel fcft-devel pixman-devel luajit-devel
   sudo dnf install librsvg2-devel cairo-devel playerctl pulseaudio-utils
   ```

   No LuaJIT? Lua 5.1 works too (`liblua5.1-0-dev`, `lua5.1`...): `make LUA_PKG=lua5.1`.
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

The bar reloads by itself when you save the config, no restart needed (it also
follows a config that is a symlink, as dotfile managers make them). If the new
config has a Lua error or a font that can't be loaded, it keeps running with
the old one. `pkill -USR1 mybar`
forces a reload.

# Writing a module

Drop a `.c` file in `modules/` and run `make`: it becomes `modules/NAME.so`, and
you add `"NAME"` to the config. The interface is [src/plugin.h](src/plugin.h).
Start from [modules/datetime.c](modules/datetime.c) for a plain text module, or
[modules/workspaces.h](modules/workspaces.h) for one that draws itself (boxes,
colors, images).
