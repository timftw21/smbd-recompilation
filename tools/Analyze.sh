#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
tool=$1 image=$2 build_directory=$3
output="$build_directory/analysis"
logs="$build_directory/logs"
analysis_cache() {
    cmake "-DROOT=$root" "-DIMAGE=$image" "-DDIRECTORY=$1" "-DRECORD=$2" -P "$root/tools/analysis-cache.cmake"
}
if [[ -s $output/seeds.txt && -s $output/instructions.tsv ]] && analysis_cache "$output" OFF 2>/dev/null; then
    exit 0
fi
mkdir -p -- "$output"
shared="$root/data/local/analysis"
if [[ -s $shared/seeds.txt && -s $shared/instructions.tsv ]] && analysis_cache "$shared" OFF 2>/dev/null; then
    cp -- "$shared/"{seeds.txt,instructions.tsv,summary.json} "$output/"
else
    command -v java >/dev/null || { echo 'A JDK 21 or newer is required for first-time analysis.' >&2; exit 1; }
    deps="$root/build/deps"
    ghidra="$deps/ghidra_12.1.4_PUBLIC"
    download() {
        local url=$1 checksum=$2 archive="$deps/${1##*/}"
        if [[ ! -f $archive ]] || ! printf '%s  %s\n' "$checksum" "$archive" | sha256sum --status -c; then
            curl -fL --retry 2 "$url" -o "$archive.download"
            printf '%s  %s\n' "$checksum" "$archive.download" | sha256sum --status -c
            mv -f -- "$archive.download" "$archive"
        fi
        printf '%s\n' "$archive"
    }
    mkdir -p -- "$deps"
    if [[ ! -f $ghidra/support/analyzeHeadless ]]; then
        archive=$(download https://github.com/NationalSecurityAgency/ghidra/releases/download/Ghidra_12.1.4_build/ghidra_12.1.4_PUBLIC_20260921.zip ddac49f903da9d5bac833e5cc79395098b9c33cfd3279be5f31bd00387d2d4db)
        (cd -- "$deps"; cmake -E tar xf "$archive")
    fi
    extensions="$ghidra/Ghidra/Extensions"
    if [[ ! -f $extensions/ghidra-xbe/extension.properties ]]; then
        archive=$(download https://github.com/XboxDev/ghidra-xbe/releases/download/build-202610161023/ghidra_12.1.4_PUBLIC_20261016_ghidra-xbe.zip 339b2f67563bbb8b5217740319b7f3eae9e9bbacf1f3a8aa29151f7ecb7d5152)
        mkdir -p -- "$extensions"
        (cd -- "$extensions"; cmake -E tar xf "$archive")
    fi
    project="$build_directory/ghidra"
    mkdir -p -- "$project"
    # A fresh marker is required even when Ghidra returns zero after a script error.
    rm -f -- "$output/summary.json"
    expected=$(sha256sum -- "$image" | cut -d' ' -f1 | tr '[:lower:]' '[:upper:]')
    GHIDRA_HEADLESS_MAXMEM=3G bash "$ghidra/support/analyzeHeadless" "$project" smbd \
        -import "$image" -overwrite -loader XbeLoader -scriptPath "$root/tools/ghidra" \
        -postScript ExportProgram.java "$output" "$expected" "$root/knowledge/non_code_entries.json" \
        -analysisTimeoutPerFile 1800 -max-cpu 1 -log "$logs/ghidra.log" -scriptlog "$logs/ghidra-scripts.log" \
        > "$logs/ghidra-stdout.log" 2>&1 || { echo "Ghidra failed. See $logs/ghidra-stdout.log." >&2; exit 1; }
    analysis_cache "$output" ON
fi
if ! "$tool" verify-analysis "$image" "$output/instructions.tsv" > "$logs/analysis-check.json"; then
    rm -f -- "$output/summary.json"
    echo "Native analysis comparison failed. See $logs/analysis-check.json." >&2
    exit 1
fi
