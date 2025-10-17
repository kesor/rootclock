{
  description = "rootclock – draw a centered clock/date on each monitor's root window";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-25.05";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs =
    { nixpkgs, flake-utils, ... }:
    let
      rootclockModule = import ./nix/default.nix;
    in
    flake-utils.lib.eachDefaultSystem (
      system:
      let
        pkgs = import nixpkgs { inherit system; };
        package = pkgs.callPackage ./nix/package.nix { };
      in
      {
        packages.default = package;

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
    )
    // {
      nixosModules.rootclock = import ./nix/default.nix;
      homeManagerModules.rootclock = import ./nix/default.nix;
    };
}
