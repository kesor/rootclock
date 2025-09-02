{
  description = "rootclock – draw a centered clock/date on each monitor's root window";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-25.05";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs =
    { nixpkgs, flake-utils, ... }:
    flake-utils.lib.eachDefaultSystem (
      system:
      let
        pkgs = import nixpkgs { inherit system; };
      in
      {
        packages.default = pkgs.callPackage ./default.nix {
          libX11 = pkgs.xorg.libX11;
          libXft = pkgs.xorg.libXft;
          libXinerama = pkgs.xorg.libXinerama;
          fontconfig = pkgs.fontconfig;
          freetype = pkgs.freetype;
        };

        # Example variant with custom config:
        # packages.withConfig = pkgs.callPackage ./default.nix {
        #   conf = builtins.readFile ./my-config.def.h;
        # };

        devShells.default = pkgs.mkShell {
          nativeBuildInputs = [
            pkgs.pkg-config
            pkgs.gnumake
            pkgs.gcc
          ];
          buildInputs = [
            pkgs.xorg.libX11
            pkgs.xorg.libXft
            pkgs.xorg.libXinerama
            pkgs.fontconfig
            pkgs.freetype
          ];
          shellHook = ''
            echo "rootclock dev shell: run 'make' to build, 'make clean' to clean."
          '';
        };
      }
    );
}
