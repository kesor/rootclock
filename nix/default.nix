{
  pkgs,
  lib,
  config,
  ...
}:

let
  cfg = config.programs.rootclock;

  packageArgs = if cfg.configFile != null then { conf = cfg.configFile; } else { };

  defaultPkg = pkgs.callPackage ./package.nix packageArgs;

  rootclockPkg = if cfg.package != null then cfg.package else defaultPkg;
in
{
  options.programs.rootclock = {
    enable = lib.mkEnableOption "rootclock (root-window clock)";

    # Option A: provide a prebuilt package (e.g. from a flake input)
    package = lib.mkOption {
      type = lib.types.nullOr lib.types.package;
      default = null;
      description = "Prebuilt rootclock package.";
    };

    # Inject a custom config.def.h into the build (choose one)
    configFile = lib.mkOption {
      type = lib.types.nullOr lib.types.path;
      default = null;
      description = "Path to a full config.def.h to inject.";
    };

    # Service knobs
    extraArgs = lib.mkOption {
      type = lib.types.listOf lib.types.str;
      default = [ ];
    };
    environment = lib.mkOption {
      type = lib.types.attrsOf lib.types.str;
      default = { };
    };
    wantedBy = lib.mkOption {
      type = lib.types.listOf lib.types.str;
      default = [ "graphical-session.target" ];
    };
    after = lib.mkOption {
      type = lib.types.listOf lib.types.str;
      default = [ "graphical-session-pre.target" ];
    };
  };

  config = lib.mkIf cfg.enable {
    home.packages = [ rootclockPkg ];

    systemd.user.services.rootclock = {
      Unit = {
        Description = "rootclock – draw clock on the root window";
        PartOf = [ "graphical-session.target" ];
        After = cfg.after; # list of strings, not a single concatenated string
      };
      Service = {
        Environment = lib.mapAttrsToList (k: v: "${k}=${v}") cfg.environment;
        ExecStart = "${rootclockPkg}/bin/rootclock ${lib.concatStringsSep " " cfg.extraArgs}";
        Restart = "on-failure";
        RestartSec = 2;
      };
      Install = {
        WantedBy = cfg.wantedBy;
      };
    };
  };
}
