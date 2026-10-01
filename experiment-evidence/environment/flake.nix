{
  description = "Local Noxim C++ build environment";
  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  inputs.dbg = { url = "github:sharkdp/dbg-macro/v0.5.1"; flake = false; };
  outputs = { self, nixpkgs, dbg }: {
    devShells.x86_64-linux.default = let
      pkgs = import nixpkgs { system = "x86_64-linux"; };
    in pkgs.mkShell {
      packages = [ pkgs.cmake pkgs.ninja pkgs.gcc pkgs.pkg-config pkgs.ripgrep ];
      buildInputs = [ pkgs.systemc pkgs.yaml-cpp ];
      NOXIM_SYSTEMC = "${pkgs.systemc}";
      NOXIM_YAML_CPP = "${pkgs.yaml-cpp}";
      NIX_CFLAGS_COMPILE = "-I${dbg}";
    };
  };
}
