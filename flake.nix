{
  description = "C++ runtime reflection framework — dev environment (GCC 16 + Meson + Ninja)";

  inputs = {
    nixpkgs.url = "github:nixos/nixpkgs/nixpkgs-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = import nixpkgs { inherit system; };

        # GCC 16 is the first release with mainline C++26 reflection
        # (P2996, enabled with -std=c++26 -freflection).  Building the whole
        # shell on gcc16Stdenv makes gcc/g++ on PATH resolve to GCC 16.
        gcc16Stdenv = pkgs.gcc16Stdenv;
      in {
        devShells.default = (pkgs.mkShell.override { stdenv = gcc16Stdenv; }) {
          packages = [
            # Build system
            pkgs.meson
            pkgs.ninja

            # GCC 16 toolchain (also pulled in by the stdenv, but listed so the
            # compiler is explicitly on PATH and its gcov/gprof sit alongside).
            pkgs.gcc16

            # Static / dynamic checking — clang-free.
            # cppcheck: independent static analyzer, complements -fanalyzer.
            # valgrind: runtime memory / UB detection.
            pkgs.cppcheck
            pkgs.valgrind

            # Documentation — MkDocs Material (markdown-based, modern output).
            pkgs.python3Packages.mkdocs
            pkgs.python3Packages.mkdocs-material
          ];

          # Pin the compiler for meson so it never falls back to a different gcc.
          CC = "${pkgs.gcc16}/bin/gcc";
          CXX = "${pkgs.gcc16}/bin/g++";

        };
      }
    );
}
