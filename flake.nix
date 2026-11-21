{
  description = "gander - an offline previewer for a local git repository";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }:
    let
      systems = [
        "x86_64-linux"
        "aarch64-linux"
        "armv7l-linux"
        "aarch64-darwin"
      ];

      forAllSystems = f:
        nixpkgs.lib.genAttrs systems (system: f nixpkgs.legacyPackages.${system});

      version = "0.2.0";

      mkGander = pkgs: pkgs.stdenv.mkDerivation {
        pname = "gander";
        inherit version;
        src = ./.;

        # Plain Makefile project: no configure step.
        dontConfigure = true;

        # Local `make` leaves objects and a binary in the tree; path: flakes copy
        # those in, so force a fresh compile (a foreign binary has the wrong ELF
        # interpreter on NixOS).
        preBuild = ''
          make clean
        '';

        makeFlags = [ "PREFIX=${placeholder "out"}" ];
        # Keep the install phase from writing a default config into HOME.
        installFlags = [
          "PREFIX=${placeholder "out"}"
          "GANDER_CONFIG=/dev/null"
        ];

        meta = with nixpkgs.lib; {
          description = "An offline previewer for a local git repository";
          homepage = "https://github.com/campbell-nick/gander";
          license = licenses.mit;
          platforms = platforms.unix;
          mainProgram = "gander";
        };
      };
    in
    {
      packages = forAllSystems (pkgs: {
        default = mkGander pkgs;
      });

      devShells = forAllSystems (pkgs: {
        default = pkgs.mkShell {
          packages = with pkgs; [
            gcc
            gnumake
          ];
        };
      });

      formatter = forAllSystems (pkgs: pkgs.nixpkgs-fmt);
    };
}
