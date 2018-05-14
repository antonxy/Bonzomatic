let
    pkgs = import <nixpkgs> {};
    stdenv = pkgs.stdenv;
in stdenv.mkDerivation rec {
    name = "Bonzomatic";
    src = ./.;
    nativeBuildInputs = [ pkgs.cmake ];
    buildInputs = [
        pkgs.xorg.libX11
        pkgs.xorg.libXrandr
        pkgs.xorg.libXinerama
        pkgs.xorg.libXcursor
        pkgs.mesa_glu
        pkgs.libpulseaudio
        pkgs.fftw
        pkgs.SDL2
    ];
}
