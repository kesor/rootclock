{
  lib,
  stdenv,
  pkg-config,
  fontconfig,
  freetype,
  libX11,
  libXft,
  libXinerama,
  libXrender,
  conf ? null,
}:

import ./nix/package.nix {
  inherit
    lib
    stdenv
    fontconfig
    freetype
    libX11
    libXft
    libXinerama
    libXrender
    conf
    ;
  pkg-config = pkg-config;
}
