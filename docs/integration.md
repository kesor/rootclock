# rootclock Integration Guide

This document collects tips for getting `rootclock` running comfortably, both
for Nix users (via the bundled module) and for users on other distributions who
prefer to manage the service manually with `systemd`.

## 1. Using the NixOS / Home Manager Module

The repository exposes a module under `nix/` that packages the binary and sets
up a user service. To pull it in, add the module to `imports` and enable the
service:

```nix
{ inputs, ... }:
{
  imports = [ inputs.rootclock.homeManagerModules.rootclock ];

  programs.rootclock = {
    enable = true;
    package = inputs.rootclock.packages.${system}.default;
    # Optional: override config.h values (fonts, colors, formats) here.
    # config = builtins.readFile ./my-config.def.h;
    service = {
      enable = true;
      extraConfig = {
        Environment = [ "HOME=%h" ];
      };
    };
  };
}
```

The module builds `rootclock` from the current flake revision, deploys it to
`~/.nix-profile/bin/rootclock`, and creates a user-level `systemd` unit
(`rootclock.service`) that starts it automatically on login. To customise the
fonts, colours, or formats, either point `programs.rootclock.config` at your own
`config.def.h` or patch the repo and rebuild.

### Handy Nix commands

- `nix develop` – drop into the dev shell with `clang-format`, `pkg-config`, etc.
- `nix build` – produce the default package (binaries under `result/bin`).
- `nix run` – run the package directly without installing it first.

Once the service is enabled, use `systemctl --user status rootclock.service` to
check its state and `journalctl --user -u rootclock.service` for logs.

## 2. Manual Installation (non-Nix)

1. Install dependencies: `libX11`, `libXft`, `libXrender`, `libXinerama`,
   `fontconfig`, `freetype` headers (`-dev` packages on Debian/Ubuntu,
   `-devel` on Fedora).

2. Build and install:

   ```sh
   make
   sudo make install PREFIX=/usr/local
   ```

3. Copy `config.def.h` to `config.h` and edit fonts/colours as needed before
   rebuilding.

## 3. User-level systemd Service

`rootclock` runs best as a user service so it dies and respawns with your X
session. Create `~/.config/systemd/user/rootclock.service`:

```ini
[Unit]
Description=Draw rootclock on the background
After=graphical-session.target

[Service]
ExecStart=%h/.local/bin/rootclock
Restart=on-failure
Environment=DISPLAY=%I

[Install]
WantedBy=graphical-session.target
```

Adjust `ExecStart` to the installed binary path and set `DISPLAY` appropriately
(often `:0`). Then run:

```sh
systemctl --user daemon-reload
systemctl --user enable --now rootclock.service
```

## 4. Wallpaper Compositor Notes

`rootclock` now blends with the underlying wallpaper even when a compositor
like `picom` is active. Choose the desired mode in `config.def.h`:

- `BG_MODE_SOLID`: solid fill, behaves like classic `xsetroot`.
- `BG_MODE_COPY`: copy wallpaper + draw text on top.
- `BG_MODE_INVERT`, `MULTIPLY`, `SCREEN`, `OVERLAY`, `DARKEN`, `LIGHTEN`:
  blend inside the glyphs only. These respect the `time_color` / `date_color`
  settings.

Remember to rebuild after changing the config:

```sh
make clean
make
systemctl --user restart rootclock.service
```

## 5. Troubleshooting Checklist

- **Clock not visible under a compositor**: ensure you’re running the latest
  build (the compositor-aware rendering landed in this branch) and the service
  is restarted. For `picom`, the default settings work out of the box now.
- **Font fallback issues**: add extra fonts to `time_fonts` / `date_fonts` in
  `config.def.h`. The renderer caches missing code points to avoid repeated
  fallback scans.
- **Formatting before committing**: `nix develop` sets up a pre-commit hook that
  runs `clang-format` on `rootclock.c` and `config.def.h`. Enable it with
  `git config core.hooksPath .githooks`.

With these pieces in place, `rootclock` should feel at home whether you manage
your system declaratively or prefer hand-crafted setups. Enjoy the new blend
modes!
