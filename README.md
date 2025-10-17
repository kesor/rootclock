# rootclock

rootclock is a simple X program that draws a configurable clock (and optionally date) directly onto the root window of all monitors.
It is designed to run alongside dwm or other window managers, without interfering with their behavior.

## Features

* Shows a large clock centered on each monitor.
* Optional second line with the date.
* Customizable fonts, colors, and time/date formats via `config.def.h`.
* Lightweight, no dependencies beyond Xlib and Xft.

## Requirements

In order to build rootclock you need the Xlib and Xft header files.
On Debian/Ubuntu:

```
sudo apt install libx11-dev libxft-dev
```

On Fedora:

```
sudo dnf install libX11-devel libXft-devel
```

On Nix/NixOS, see the provided flake.

## Installation

Edit `config.def.h` to suit your preferences, then run:

```
make clean install
```

## Running rootclock

Add the following to your `.xinitrc` or WM startup script:

```
rootclock &
```

It will run in the background and continuously update the clock.

## Compositors

rootclock automatically detects EWMH compositing managers such as picom. When a compositor is active it draws to an unmanaged `_NET_WM_WINDOW_TYPE_DESKTOP` layer instead of the real root window, so the clock remains visible even when the compositor's overlay is in use. No extra configuration is required; if the compositor exits, rootclock falls back to painting on the root window.

## Configuration

All configuration is done by editing `config.def.h` and recompiling.
You can set:

* **Fonts** for clock and date
* **Colors** for both lines
* **Formats** (strftime strings, e.g. `%H:%M`, `%a %d.%m.%Y`)
* **Background mode** (`BG_MODE_SOLID`, `BG_MODE_COPY`, `BG_MODE_INVERT`) to control whether the wallpaper shows through or is filtered under the text. Copy/invert sample the underlying root pixmap if `_XROOTPMAP_ID` is available; otherwise they gracefully fall back to the solid colour.
* **Block padding** (`block_padding_x`, `block_padding_y`) to adjust how much wallpaper around the text is sampled for the overlay
* Whether to show the date line

See the file for details.

## License

MIT/X Consortium License
