#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
runtime=26.08
sdk="org.freedesktop.Sdk/x86_64/$runtime"
platform="org.freedesktop.Platform/x86_64/$runtime"
llvm=llvm22
build_directory="$root/build/steamdeck"
game="$build_directory/Release/smbd"
[[ $(uname -s):$(uname -m) == Linux:x86_64 ]] || { echo 'Steam Deck setup requires Linux x86-64.' >&2; exit 1; }
case ${1:-} in
    prepare)
        command -v flatpak >/dev/null || { echo 'Install Flatpak first. SteamOS includes it.' >&2; exit 1; }
        flatpak remote-add --user --if-not-exists flathub https://dl.flathub.org/repo/flathub.flatpakrepo
        flatpak install --user --noninteractive -y flathub "$platform" "$sdk" \
            "org.freedesktop.Sdk.Extension.$llvm/x86_64/$runtime" "org.freedesktop.Sdk.Extension.openjdk21/x86_64/$runtime"
        ;;
    build)
        shift
        if (( $# != 1 )); then echo 'Pass a disc image path or --tools-only.' >&2; exit 1; fi
        input=$1
        if [[ $input != --tools-only ]]; then input=$(realpath -e -- "$input"); fi
        exec flatpak run --user --devel --share=network --filesystem=host \
            "--env=FLATPAK_ENABLE_SDK_EXT=$llvm,openjdk21" --command=bash "$sdk" \
            "$root/cmake/steamdeck.sh" inside-build "$input"
        ;;
    inside-build)
        shift
        [[ -f /.flatpak-info && -f /usr/lib/sdk/$llvm/enable.sh ]] || {
            echo 'Run this build through Install.sh --steamdeck.' >&2; exit 1;
        }
        set +u
        # shellcheck source=/dev/null
        source "/usr/lib/sdk/$llvm/enable.sh"
        set -u
        export JAVA_HOME=/usr/lib/sdk/openjdk21
        export PATH="$JAVA_HOME/bin:$PATH"
        # Keep build-only headers and libraries inside the user's source tree.
        spirv="$root/build/deps/spirv-headers-steamdeck"
        if [[ ! -f $spirv/include/spirv/unified1/spirv.h ]]; then
            archive="$root/build/deps/spirv-headers-steamdeck.tar.gz"
            checksum=1b220e3eec1714f0451b0e3652979bd280edf10893f617837b88e6359a804ded
            mkdir -p -- "$spirv"
            curl -fL --retry 2 https://codeload.github.com/KhronosGroup/SPIRV-Headers/tar.gz/04f10f650d514df88b76d25e83db360142c7b174 -o "$archive.download"
            printf '%s  %s\n' "$checksum" "$archive.download" | sha256sum --status -c
            mv -f -- "$archive.download" "$archive"
            tar -xzf "$archive" --strip-components=1 -C "$spirv"
        fi
        export CPATH="$spirv/include${CPATH:+:$CPATH}"
        export SMBD_BUILD_DIRECTORY="$build_directory" SMBD_DEPENDENCY_SUFFIX=-steamdeck
        exec bash "$root/Build.sh" "$@"
        ;;
    play)
        shift
        [[ -x $game ]] || { echo 'Run Install.sh --steamdeck with your disc image first.' >&2; exit 1; }
        # Keep XWayland and its abstract sockets accessible even when Wayland
        # is available. Preserve Steam's DISPLAY across Flatpak's launcher.
        unset LD_LIBRARY_PATH LD_PRELOAD
        display=()
        if [[ -n ${DISPLAY:-} ]]; then display+=("DISPLAY=$DISPLAY"); fi
        discord=(--filesystem=xdg-run/app/com.discordapp.Discord:ro)
        for slot in {0..9}; do
            if [[ -S ${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/discord-ipc-$slot ]]; then
                discord+=("--filesystem=xdg-run/discord-ipc-$slot:ro")
            fi
        done
        exec flatpak run --user --share=ipc --share=network --socket=wayland --socket=x11 \
            --socket=pulseaudio --device=all --filesystem=host \
            "${discord[@]}" \
            --unset-env=LD_LIBRARY_PATH --unset-env=LD_PRELOAD --command=env "$platform" \
            "${display[@]}" "$game" "$@"
        ;;
    *) echo 'Use Install.sh --steamdeck or Play.sh --steamdeck.' >&2; exit 1 ;;
esac
