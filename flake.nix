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
          fontconfig = pkgs.fontconfig;
          freetype = pkgs.freetype;
          libX11 = pkgs.xorg.libX11;
          libXft = pkgs.xorg.libXft;
          libXinerama = pkgs.xorg.libXinerama;
          libXrender = pkgs.xorg.libXrender;
        };

        # Example variant with custom config:
        # packages.withConfig = pkgs.callPackage ./default.nix {
        #   conf = builtins.readFile ./my-config.def.h;
        # };

        devShells.default = pkgs.mkShell {
          nativeBuildInputs = [
            pkgs.gcc
            pkgs.gnumake
            pkgs.pkg-config
            pkgs.clang-tools
          ];
          buildInputs = [
            pkgs.fontconfig
            pkgs.freetype
            pkgs.xorg.libX11
            pkgs.xorg.libXft
            pkgs.xorg.libXinerama
            pkgs.xorg.libXrender
          ];
          shellHook = ''
            echo "rootclock dev shell: run 'make' to build, 'make clean' to clean, 'clang-format -i rootclock.c' to format."
          '';
        };
      }
    );
}
