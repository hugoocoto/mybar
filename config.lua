-- mybar configuration. The bar reloads automatically when you save this file.
-- Keep the table in a global called `Config`.

Config = {
    height        = 26,                         -- pixels; also the space reserved for the bar
    padding       = 12,                         -- left/right margin
    padding_left  = 0,                          -- override one side (default: padding)
    padding_right = 18,
    spacing       = 23,                         -- gap between modules (per module: spacing = px)
    font          = "Iosevka NFP:pixelsize=18", -- fontconfig name + fcft attributes
    bg            = "#141617",
    fg            = "#d4be98",

    -- Where bare plugin names are looked up (default: <this dir>/modules, ./modules,
    -- ~/.local/lib/mybar/modules, /usr/local/lib/mybar/modules, /usr/lib/mybar/modules).
    -- modules_dir = "modules",

    -- Each entry is either a plugin
    --   "battery"                    a name   -> <modules_dir>/battery.so
    --   "modules/battery.so"         a path   -> relative to this file, then to the cwd
    -- or a table with options:
    --   { path = "battery", interval = 10, low = 20, color = "#a6e3a1" }
    -- Options understood by every module: path, interval (seconds, 0 = events only),
    -- color, spacing (gap after it, px).
    -- Everything else is read by the plugin itself (see the header of modules/NAME.c).
    --
    -- Below, every module with all its options set to their default values: a bare
    -- "name" behaves the same. Delete what you do not change, and the modules you
    -- do not want. Modules with nothing to show (no VPN, nothing playing...) are
    -- hidden and take no space.
    modules = {
        left = {
            -- Hyprland. placeholders: {id} {name} {windows} {icons} (app icons)
            { path = "workspaces", format = "{name}", pad = 6, gap = 0, persistent = 0,
              special = false, icon_size = 16, icon_gap = 4, icon_theme = "hicolor",
              -- active_fg / active_bg: default the bar's colors swapped; active_bg = "none": no box
              -- fg: the other workspaces, default the bar's fg
              -- pad_left / pad_right: override pad on one side
            },
            { path = "submap", format = "{name}" },                 -- only inside a submap
            { path = "window", format = "{title}", max_chars = 60 }, -- {title} {class}

            -- sway: the same options as the Hyprland modules above
            -- { path = "sway_workspaces", format = "{name}", pad = 6, gap = 0, persistent = 0,
            --   icon_size = 16, icon_gap = 4, icon_theme = "hicolor" },
            -- { path = "sway_mode", format = "{name}" },
            -- { path = "sway_window", format = "{title}", max_chars = 60 },
        },
        center = {},
        right = {
            -- {dynamic} {artist} {title} {album} {player} {status}; player: default any
            { path = "media", format = "{dynamic}", paused_format = "{dynamic} (paused)",
              max_chars = 50 },
            -- {iface} {count}
            { path = "vpn", format = "vpn", prefixes = "tun,tap,wg,ppp,vpn,nordlynx,proton" },
            { path = "capslock", format = "CAPS", interval = 0.25 },
            -- {cpu} {mem} {mem_used} {mem_total} {temp}; thermal: default x86_pkg_temp, TCPU...
            { path = "system", format = "cpu {cpu}%  mem {mem}%  {temp}°C", interval = 2,
              cpu_high = 80, mem_high = 80, temp_high = 85, high_color = "#f38ba8",
              only_high = false },
            -- {free} {used} {total} {pct} {free_pct} {mount}
            { path = "disk", format = "disk {free}", mount = "/", interval = 60,
              low = 10, low_color = "#f38ba8" },
            -- {pct}; device: default the first backlight
            { path = "brightness", format = "bri {pct}%" },
            -- {pct}
            { path = "volume", format = "vol {pct}%", muted_format = "vol muted" },
            -- {pct} {status} {sign}; device: default the first battery
            { path = "battery", format = "BAT {pct}%{sign}", interval = 30,
              low = 15, low_color = "#f38ba8" },
            -- {short} {long}
            { path = "language", format = "{short}" },
            -- { path = "sway_language", format = "{short}" },
            -- {name} {iface} {ssid} {signal}
            { path = "network", wifi_format = "wifi {name} {signal}%", ethernet_format = "eth {name}",
              offline_format = "offline" },
            -- strftime(3)
            { path = "datetime", format = "%a %d %b  %H:%M" },
        },
    },
}
