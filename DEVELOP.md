# Nix Development

This project ships with a Nix flake to simplify development and building.

## Entering a devShell

To get a reproducible development environment with all dependencies:

```sh
nix develop
```

This provides:

* `libX11`, `libXft`, and related headers
* `pkg-config`
* A compiler toolchain

Inside the shell you can run:

```sh
make
```

## Building with Nix

To build `rootclock` directly through Nix:

```sh
nix build
```

This produces a result symlink:

```
./result/bin/rootclock
```

## Running without Installing

You can run it directly via Nix:

```sh
nix run
```

## Configuration

Like other suckless tools, all configuration is done in `config.def.h`.
When using Nix, you can override this file by passing `--override-input` or by editing the flake to point to your own `config.def.h`.
