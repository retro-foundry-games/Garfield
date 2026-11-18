{
  description = "Garfield: Big, Fat, Hairy Deal, native and browser builds";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixpkgs-unstable";
  };

  outputs = { self, nixpkgs }:
    let
      linux = [ "x86_64-linux" "aarch64-linux" ];
      darwin = [ "x86_64-darwin" "aarch64-darwin" ];
      forSystems = systems: f: nixpkgs.lib.genAttrs systems (system: f nixpkgs.legacyPackages.${system});
      forAll = forSystems (linux ++ darwin);
      webTools = pkgs: [
        pkgs.cmake
        pkgs.ninja
        pkgs.emscripten
        pkgs.python3
      ];
      src = pkgs: pkgs.lib.cleanSourceWith {
        src = ./.;
        filter = path: type:
          let name = baseNameOf path; in
          !(builtins.elem name [ "build" "extracted" "result" ".git" ".venv" ".direnv" ]);
      };
    in {
      packages = forAll (pkgs: {
        # SDL3 compiled once for Emscripten (nixpkgs' source), so rebuilds of the game reuse it.
        sdl3-emscripten = import ./sdl3-emscripten.nix { inherit pkgs; };

        # The browser build, the folder to publish: index.html (the same as garfield.html), garfield.js and garfield.wasm
        # (reconstruct/Makefile.web). The page asks for the disk image.
        web = pkgs.stdenv.mkDerivation {
          pname = "garfield-web";
          version = "0";
          src = src pkgs;
          nativeBuildInputs = [
            pkgs.gnumake
            pkgs.python3
            pkgs.emscripten
          ];
          dontUseCmakeConfigure = true;
          buildPhase = ''
            export HOME=$TMPDIR EM_CACHE=$TMPDIR/em-cache
            make -C reconstruct -f Makefile.web GARFIELD_NO_NIX=1 SDL3_PREFIX=${self.packages.${pkgs.stdenv.hostPlatform.system}.sdl3-emscripten} W=$PWD/build/web
          '';
          installPhase = ''
            mkdir -p $out
            cp build/web/garfield.html build/web/garfield.js build/web/garfield.wasm build/web/retro-foundry.png $out/
            cp build/web/garfield.html $out/index.html
          '';
        };
        default = self.packages.${pkgs.stdenv.hostPlatform.system}.web;
      });

      # nix run: serve the browser build on http://localhost:8080/ (PORT overrides).
      apps = forAll (pkgs: {
        default = {
          type = "app";
          program = toString (pkgs.writeShellScript "garfield-serve" ''
            port=''${PORT:-8080}
            echo "Garfield on http://localhost:$port/"
            exec ${pkgs.python3}/bin/python3 -m http.server "$port" \
              --bind 127.0.0.1 --directory ${self.packages.${pkgs.stdenv.hostPlatform.system}.web}
          '');
        };
      });

      devShells = forSystems linux (pkgs: {
        default = pkgs.mkShell.override { stdenv = pkgs.overrideCC pkgs.stdenv pkgs.gcc_multi; } {
          buildInputs = [
            pkgs.pkgsi686Linux.sdl3
            pkgs.pkgsi686Linux.unicorn
          ];
          packages = webTools pkgs ++ [
            pkgs.pkg-config
            pkgs.gcc_multi
            pkgs.uv
          ];
          GARFIELD_SDL3_SOURCE_DIR = pkgs.sdl3.src;
          shellHook = ''
            export EM_CACHE="$PWD/build/emscripten-cache"
          '';
        };
      }) // forSystems darwin (pkgs: {
        # The native port needs an i686 Linux toolchain; macOS gets the browser build only.
        default = pkgs.mkShell {
          packages = webTools pkgs;
          GARFIELD_SDL3_SOURCE_DIR = pkgs.sdl3.src;
          shellHook = ''
            export EM_CACHE="$PWD/build/emscripten-cache"
          '';
        };
      });
    };
}
