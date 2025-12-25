{
  description = "Linux Bias Lighting";
  inputs = {
    nixpkgs.url = "github:nixos/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs =
    {
      self,
      nixpkgs,
      flake-utils,
    }:
    flake-utils.lib.eachDefaultSystem (
      system:
      let
        pkgs = nixpkgs.legacyPackages.${system};
      in
      {
        packages.default = pkgs.stdenv.mkDerivation {
          pname = "blight";
          version = "0.1.0";
          src = ./.;

          nativeBuildInputs = with pkgs; [
            pkg-config
            gcc
            makeWrapper
          ];

          buildInputs = with pkgs; [
            gst_all_1.gstreamer
            gst_all_1.gst-plugins-base
            gst_all_1.gst-plugins-good
            gst_all_1.gst-plugins-bad
            pipewire
            glib
            libportal
          ];

          buildPhase = ''
            gcc -o main src/main.c src/serial.c \
              $(pkg-config --cflags --libs gstreamer-1.0 gstreamer-app-1.0 libportal glib-2.0)
          '';

          installPhase = ''
            mkdir -p $out/bin
            cp main $out/bin/blight

            mkdir -p $out/share/systemd/user
            cat > $out/share/systemd/user/blight.service <<EOF
            [Unit]
            Description=Bias Lighting for Monitor
            After=graphical-session.target pipewire.service

            [Service]
            Type=simple
            ExecStart=$out/bin/blight 120 1
            Restart=on-failure
            RestartSec=5

            [Install]
            WantedBy=graphical-session.target
            EOF
          '';

          postFixup = ''
            wrapProgram $out/bin/blight \
              --prefix GST_PLUGIN_SYSTEM_PATH_1_0 : "$GST_PLUGIN_SYSTEM_PATH_1_0"
          '';
        };

        devShells.default = pkgs.mkShell {
          nativeBuildInputs = with pkgs; [
            pkg-config
            gcc
            clang-tools
          ];

          buildInputs = with pkgs; [
            gst_all_1.gstreamer
            gst_all_1.gst-plugins-base
            gst_all_1.gst-plugins-good
            gst_all_1.gst-plugins-bad
            arduino-cli
            python3
            pipewire
            glib
            libportal
          ];

          shellHook = ''
            export GST_PLUGIN_PATH="${pkgs.gst_all_1.gst-plugins-base}/lib/gstreamer-1.0:${pkgs.gst_all_1.gst-plugins-good}/lib/gstreamer-1.0:${pkgs.gst_all_1.gst-plugins-bad}/lib/gstreamer-1.0:${pkgs.pipewire}/lib/gstreamer-1.0"
            pkg-config --cflags gstreamer-1.0 gstreamer-app-1.0 libportal glib-2.0 | tr ' ' '\n' > compile_flags.txt
            export CPATH="$(pkg-config --cflags-only-I gstreamer-1.0 gstreamer-app-1.0 libportal glib-2.0 | sed 's/-I//g' | tr ' ' ':')"
            if command -v zsh &> /dev/null; then exec zsh; fi
          '';
        };
      }
    );
}
