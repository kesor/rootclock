{
  lib,
  stdenv,
  pkg-config,
  libX11,
  libXft,
  libXinerama,
  fontconfig,
  freetype,
  conf ? null,
}:

stdenv.mkDerivation {
  pname = "rootclock";
  version = "unstable-2025-09-02";

  src = ./.;

  nativeBuildInputs = [ pkg-config ];
  buildInputs = [
    libX11
    libXft
    libXinerama
    fontconfig
    freetype
  ];

  # Ensure install prefix goes to $out.
  preConfigure = ''
    sed -i "s@^PREFIX = .*@PREFIX = $out@" config.mk
  '';

  # Optional: allow a custom config.def.h to be injected (string, path, or derivation).
  postPatch =
    let
      cfg =
        if lib.isDerivation conf || builtins.isPath conf then
          conf
        else if conf == null then
          null
        else
          builtins.toFile "config.def.h" conf;
    in
    lib.optionalString (conf != null) ''
      cp ${cfg} config.def.h
    '';

  makeFlags = [ "CC=${stdenv.cc.targetPrefix}cc" ];
  installFlags = [ "PREFIX=$(out)" ];

  meta = with lib; {
    description = "Minimal root-window clock (multi-monitor) drawn with Xft";
    homepage = "https://github.com/kesor/rootclock";
    license = licenses.mit;
    maintainers = [ "kesor" ];
    platforms = platforms.unix;
    mainProgram = "rootclock";
  };
}
