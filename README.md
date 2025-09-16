# rootclock

rootclock is a simple X program that draws a configurable clock (and optionally date) directly onto the root window of all monitors.
It is designed to run alongside dwm or other window managers, without interfering with their behavior.

## Features

* Shows a large clock centered on each monitor.
* Optional second line with the date.
* Customizable fonts, colors, and time/date formats via `config.def.h`.
* Lightweight, no dependencies beyond Xlib and Xft.
* Compatible with compositors like picom by properly setting wallpaper properties.

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

## Configuration

All configuration is done by editing `config.def.h` and recompiling.
You can set:

* **Fonts** for clock and date
* **Colors** for both lines
* **Formats** (strftime strings, e.g. `%H:%M`, `%a %d.%m.%Y`)
* Whether to show the date line

See the file for details.

## License

MIT/X Consortium License
