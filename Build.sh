#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
[[ $(uname -s):$(uname -m) == Linux:x86_64 ]] || { echo 'This builder requires Linux x86-64.' >&2; exit 1; }
if [[ ${1:-} == --steamdeck ]]; then
    shift
    exec bash "$root/cmake/steamdeck.sh" build "$@"
fi
if (( $# != 1 )); then
    echo 'Usage: bash Build.sh [--steamdeck] /path/to/game.xiso.iso | --tools-only' >&2
    exit 1
fi
input=$1
if [[ $input != --tools-only ]]; then
    input=$(realpath -e -- "$input")
    [[ -f $input ]] || { echo 'Select a supported Xbox disc image.' >&2; exit 1; }
    [[ $input != *$'\n'* && $input != *$'\r'* && ${#input} -lt 2048 ]] || {
        echo 'The disc path is too long or contains a line break.' >&2; exit 1;
    }
fi
cd -- "$root"
export CC=clang CXX=clang++
if ! command -v clang++ >/dev/null || ! command -v ld.lld >/dev/null; then
    echo 'Clang 19 or newer and LLD must be on PATH. See BUILDING.txt.' >&2
    exit 1
fi
smbd_sha256() { sha256sum -- "$1" | cut -d' ' -f1; }
source cmake/cmake-linux.sh
source cmake/posix-dependencies.sh
build_directory=${SMBD_BUILD_DIRECTORY:-"$root/build/linux"}
tools_directory="$build_directory/tools"
generated="$build_directory/generated"
logs="$build_directory/logs"
mkdir -p -- "$generated" "$logs"
if [[ ! -f $tools_directory/build-Release.ninja ]]; then
    cmake --preset linux -B "$tools_directory" -DSMBD_BATCH= -DSMBD_SHADER_BANK=
fi
cmake --build "$tools_directory" --config Release --parallel 1 --target smbd-tool
tool="$tools_directory/Release/smbd-tool"
[[ $input != --tools-only ]] || exit 0
local_data="$build_directory/Release/data/local"
mkdir -p -- "$local_data"
"$tool" prepare "$input" "$local_data" --extract-shaders > "$logs/prepare.json"
bash tools/Analyze.sh "$tool" "$local_data/default.xbe" "$build_directory"
"$tool" batch "$local_data/default.xbe" "$build_directory/analysis/seeds.txt" 65536 8192 \
    "$generated/game.cpp" --replace --shards 512 --report "$generated/batch.json"
shader_bank="$generated/shader_bank.cpp"
shader_cache="$generated/shader-cache"
fingerprint=$(
    sha256sum -- "$tool" "$local_data/default.xbe"
    find "$local_data/media" -type f -name '*.xvu' -print0 | sort -z | xargs -0 -r sha256sum --
)
fingerprint=$(printf '%s' "$fingerprint" | sha256sum | cut -d' ' -f1)
if [[ ! -f $shader_bank || ! -f $shader_cache ]] ||
    [[ $(cat "$shader_cache") != "$fingerprint $(smbd_sha256 "$shader_bank")" ]]; then
    "$tool" compile-shaders "$local_data/default.xbe" "$shader_bank" > "$logs/shaders.json" || {
        cat "$logs/shaders.json" >&2; exit 1;
    }
    printf '%s %s\n' "$fingerprint" "$(smbd_sha256 "$shader_bank")" > "$shader_cache"
fi
cache="$build_directory/CMakeCache.txt"
if [[ ! -f $build_directory/build-Release.ninja ]] ||
    ! grep -Fxq "SMBD_BATCH:FILEPATH=$generated/game.cpp" "$cache" ||
    ! grep -Fxq "SMBD_SHADER_BANK:FILEPATH=$shader_bank" "$cache"; then
    cmake --preset linux -B "$build_directory" "-DSMBD_BATCH=$generated/game.cpp" "-DSMBD_SHADER_BANK=$shader_bank"
fi
cmake --build "$build_directory" --config Release --parallel 1 --target smbd-tool smbd
settings="$local_data/settings.cfg"
temporary=$(mktemp -- "$settings.XXXXXX")
trap 'rm -f -- "$temporary"' EXIT
{
    printf 'executable=data/local/default.xbe\ndisc=%s\n' "$input"
    if [[ -f $settings ]]; then
        sed '/^executable=/d; /^disc=/d' "$settings"
    elif [[ ${SMBD_DEPENDENCY_SUFFIX:-} == -steamdeck ]]; then
        printf 'fullscreen=1\nrefresh_rate=0\nresolution=1\naspect=1\n'
    fi
} > "$temporary"
mv -f -- "$temporary" "$settings"
printf '\nBuilt successfully. Launch with bash Play.sh.\n'
