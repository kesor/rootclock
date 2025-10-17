{ stdenv
, lib
, pkg-config
, fontconfig
, freetype
, libX11
, libXft
, libXinerama
, libXrender
, conf ? null
}:

stdenv.mkDerivation rec {
  pname = "rootclock";
  version = "unstable";

  src = ../.;

  dontConfigure = true;
  enableParallelBuilding = true;

  nativeBuildInputs = [ pkg-config ];
  buildInputs = [ fontconfig freetype libX11 libXft libXinerama libXrender ];

  postPatch = lib.optionalString (conf != null) ''
    cp ${conf} config.def.h
  '';

  buildPhase = ''
    runHook preBuild
    make
    runHook postBuild
  '';

  installPhase = ''
    runHook preInstall
    make install PREFIX=$out
    runHook postInstall
  '';

  meta = with lib; {
    description = "Draw a configurable clock/date on the X root window";
    homepage = "https://github.com/kesor/rootclock";
    license = licenses.mit;
    platforms = platforms.unix;
    maintainers = [ ];
  };
}
