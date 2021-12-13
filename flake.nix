{
  inputs.nixpkgs.url = "github:nixos/nixpkgs/nixos-21.11";
  inputs.flake-utils.url = "github:numtide/flake-utils";
  outputs = { self, nixpkgs, flake-utils }: flake-utils.lib.eachDefaultSystem
    (system: with import nixpkgs { system = system; }; rec {
      packages.succade = stdenv.mkDerivation {
        pname = "succade";
        version = "2.1.3";
        src = self;
        buildInputs = [ pkgs.inih ];
        buildPhase = "gcc -Wall -O3 -o succade src/succade.c -linih";
        installPhase = "mkdir -p $out/bin; install -t $out/bin succade";
      };
      defaultPackage = packages.succade;
      apps.succade = flake-utils.lib.mkApp { drv = packages.succade; };
      defaultApp = apps.succade;
    });
}
