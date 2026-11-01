{
  description = "SoundFetch - lightweight yt-dlp desktop client";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" ];
      forAllSystems = nixpkgs.lib.genAttrs systems;
    in {
      packages = forAllSystems (system:
        let
          pkgs = import nixpkgs { inherit system; };
          inherit (pkgs) lib;
        in {
          default = pkgs.stdenv.mkDerivation {
            pname = "soundfetch";
            version = "0.1.0";
            src = self;

            nativeBuildInputs = with pkgs; [ cmake go pkg-config makeWrapper ];
            buildInputs = with pkgs; [ fltk nlohmann_json yt-dlp ffmpeg ];

            cmakeFlags = [
              "-DCMAKE_BUILD_TYPE=Release"
              # FindFLTK.cmake does not consistently discover FLTK headers
              # inside the Nix store, so pass the include path explicitly.
              "-DFLTK_INCLUDE_DIR=${lib.getDev pkgs.fltk}/include"
            ];

            postInstall = ''
              wrapProgram $out/bin/soundfetch \
                --prefix PATH : ${lib.makeBinPath [ pkgs.yt-dlp pkgs.ffmpeg ]}
            '';

            meta = {
              description = "A lightweight yt-dlp downloader with a C++ GUI and Go backend";
              license = lib.licenses.mit;
              platforms = [ "x86_64-linux" "aarch64-linux" ];
              mainProgram = "soundfetch";
            };
          };
        });

      apps = forAllSystems (system: {
        default = {
          type = "app";
          program = "${self.packages.${system}.default}/bin/soundfetch";
        };
      });
    };
}
