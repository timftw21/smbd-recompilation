#!/usr/bin/env bash
# Builds the Linux x86-64 release installer, the counterpart of tools/Package.ps1.
set -euo pipefail
export LC_ALL=C
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
usage() {
    echo 'Usage: bash tools/Package.sh [--output DIR] /path/to/game.xiso.iso | --skip-build [--output DIR]' >&2
    exit 1
}
[[ $(uname -s):$(uname -m) == Linux:x86_64 ]] || { echo 'Linux packaging requires Linux x86-64.' >&2; exit 1; }
skip_build=0 output="$root/build/release" input=
while (( $# )); do
    case $1 in
        --skip-build) skip_build=1 ;;
        --output) (( $# > 1 )) || usage; output=$2; shift ;;
        -*) usage ;;
        *) [[ -z $input ]] || usage; input=$1 ;;
    esac
    shift
done
(( skip_build )) || [[ -n $input ]] || usage
for program in xz sha256sum ldd objdump dpkg-query realpath; do
    command -v "$program" >/dev/null || { echo "Missing $program; see BUILDING.txt." >&2; exit 1; }
done
if (( !skip_build )); then bash "$root/Build.sh" "$input"; fi
cd -- "$root"
export CC=clang CXX=clang++
smbd_sha256() { sha256sum -- "$1" | cut -d' ' -f1; }
workers=${SMBD_BUILD_WORKERS:-1}
[[ $workers =~ ^[123]$ ]] || { echo 'SMBD_BUILD_WORKERS must be 1, 2 or 3.' >&2; exit 1; }
source cmake/cmake-linux.sh
source cmake/posix-dependencies.sh
build_directory=${SMBD_BUILD_DIRECTORY:-"$root/build/linux"}
[[ -x $build_directory/Release/smbd && -x $build_directory/Release/smbd-tool ]] || {
    echo 'Build the game first with Build.sh, or omit --skip-build.' >&2; exit 1;
}
package="$root/build/package-linux"
mkdir -p -- "$package" "$output"
output=$(realpath -e -- "$output")
stage=$(mktemp -d -- "$package/stage.XXXXXXXX")
trap 'rm -rf -- "$stage"' EXIT
# CMake's Runtime component is an explicit whitelist: no media, settings,
# saves, generated source, profiles, debug maps, or build directories.
cmake --install "$build_directory" --config Release --prefix "$stage" --component Runtime --strip > "$package/install.log" || { tail -n 40 -- "$package/install.log" >&2; exit 1; }

# The payload holds regular files only: each library keeps its loader name.
declare -A versioned=()
while IFS= read -r -d '' link; do
    target=$(realpath -e -- "$link")
    [[ $target == "$stage"/* && -f $target ]] || { echo "Unexpected link in the release: $link" >&2; exit 1; }
    cp --remove-destination -- "$target" "$link"
    versioned[$target]=1
done < <(find "$stage" -type l -print0)
for target in "${!versioned[@]}"; do rm -f -- "$target"; done

# Bundle every library outside the assumed system base. Distributions disagree
# on image-library sonames; glibc, libstdc++ and Mesa's dependencies must stay
# the system's own so graphics drivers load correctly.
system='^(linux-vdso\.so\.1|ld-linux-x86-64\.so\.2|lib(c|m|dl|pthread|rt|stdc\+\+|gcc_s|z|crypto)\.so\.[0-9]+)$'
bundled=()
mkdir -p -- "$stage/lib" "$stage/licenses"
for (( pass=0; pass<8; ++pass )); do
    added=0
    while IFS= read -r -d '' elf; do
        while read -r name path; do
            [[ $name =~ $system || $path == "$stage"/* ]] && continue
            [[ $path == not ]] && { echo "$(basename -- "$elf") needs $name, which is not installed." >&2; exit 1; }
            [[ $name =~ ^lib[A-Za-z0-9_.+-]+\.so\.[0-9]+$ && ! -e $stage/lib/$name ]] || {
                echo "Cannot bundle $name from $path." >&2; exit 1;
            }
            source_file=$(realpath -e -- "$path")
            cp -- "$source_file" "$stage/lib/$name"
            owner=$(dpkg-query -S "$source_file" | head -n1 | cut -d: -f1)
            [[ -f /usr/share/doc/$owner/copyright ]] || { echo "No license text for $owner." >&2; exit 1; }
            cp -- "/usr/share/doc/$owner/copyright" "$stage/licenses/$name.txt"
            bundled+=("$name")
            added=1
        # "name => path (address)" or "name => not found"; paths may contain spaces.
        done < <(LD_LIBRARY_PATH="$stage/lib" ldd -- "$elf" | sed -n \
            -e 's/^[[:space:]]*\([^[:space:]]*\) => not found$/\1 not/p' \
            -e 's/^[[:space:]]*\([^[:space:]]*\) => \(.*\) (0x[0-9a-f]*)$/\1 \2/p')
    done < <(find "$stage" -type f \( -name smbd -o -name smbd-tool -o -name '*.so.*' \) -print0)
    (( added )) || break
done
(( added == 0 )) || { echo 'Library bundling did not converge.' >&2; exit 1; }
rmdir --ignore-fail-on-non-empty -- "$stage/licenses"

mapfile -d '' -t elves < <(find "$stage" -type f \( -name smbd -o -name smbd-tool -o -name '*.so.*' \) -print0 | LC_ALL=C sort -z)
floor() { for elf in "${elves[@]}"; do objdump -T -- "$elf"; done | { grep -o "$1" || true; } | sort -uV | tail -n1; }
glibc=$(floor 'GLIBC_2\.[0-9.]*[0-9]') libstdcxx=$(floor 'GLIBCXX_3\.4\.[0-9]*')

mapfile -d '' -t files < <(cd -- "$stage" && find . -type f -printf '%P\0' | LC_ALL=C sort -z)
(( ${#files[@]} > 0 && ${#files[@]} <= 128 )) || { echo 'The release must contain 1..128 files.' >&2; exit 1; }
manifest=$'SMBD-PAYLOAD-V1\n' payload_bytes=0
for file in "${files[@]}"; do
    if [[ $file == *[![:print:]]* || $file == *[\":]* || $file =~ ^(data|logs|captures|build)/ ]]; then
        echo "Unexpected release file: $file" >&2; exit 1
    fi
    size=$(stat -c %s -- "$stage/$file")
    hash=$(smbd_sha256 "$stage/$file")
    manifest+="$file"$'\t'"$size"$'\t'"${hash^^}"$'\n'
    payload_bytes=$(( payload_bytes + size ))
done
manifest_path="$package/payload.tsv" payload="$package/payload.xz" cache="$package/payload.sha256"
if printf '%s' "$manifest" | cmp -s -- - "$manifest_path" && [[ -f $payload && -f $cache ]] &&
    [[ $(cat -- "$cache") == "$(smbd_sha256 "$payload")" ]]; then
    echo 'Reusing the verified release payload.'
else
    echo 'Packing verified release files (one compression worker).'
    (cd -- "$stage" && cat -- "${files[@]}") | xz -9 -T1 --check=crc64 > "$payload.tmp"
    printf '%s' "$manifest" > "$manifest_path.tmp"
    mv -f -- "$payload.tmp" "$payload"
    mv -f -- "$manifest_path.tmp" "$manifest_path"
    smbd_sha256 "$payload" > "$cache"
fi

# Setup links a static SDL, so it builds in its own tree beside the game's.
setup_directory="$build_directory/setup"
cmake --preset linux -B "$setup_directory" -DSMBD_BATCH= -DSMBD_SHADER_BANK= \
    "-DSMBD_INSTALLER_PAYLOAD=$payload" "-DSMBD_INSTALLER_MANIFEST=$manifest_path" "-DSMBD_BUILD_WORKERS=$workers" > "$package/configure.log" || { tail -n 40 -- "$package/configure.log" >&2; exit 1; }
cmake --build "$setup_directory" --config Release --parallel "$workers" --target smbd-setup
version=$(sed -n 's/^project(smbd VERSION \([0-9.]*\) .*/\1/p' CMakeLists.txt)
[[ $version =~ ^[0-9]+(\.[0-9]+)+$ ]] || { echo 'Cannot determine the release version.' >&2; exit 1; }
name="smbd-$version-linux-x64-setup"
installer="$output/$name"
cp -f -- "$setup_directory/Release/smbd-setup" "$installer.tmp"
chmod 0755 -- "$installer.tmp"
mv -f -- "$installer.tmp" "$installer"
setup_glibc=$(objdump -T -- "$installer" | { grep -o 'GLIBC_2\.[0-9.]*[0-9]' || true; } | sort -uV | tail -n1)
checksum=$(smbd_sha256 "$installer")
printf '%s  %s\n' "$checksum" "$name" > "$installer.sha256"
git=(git -c "safe.directory=$root")
modified=false; [[ -z $("${git[@]}" status --porcelain) ]] || modified=true
list=; (( ${#bundled[@]} == 0 )) || { list=$(printf '"%s",' "${bundled[@]}"); list=${list%,}; }
cat > "$output/release-linux-x64.json" <<JSON
{
  "format": "smbd-release-v1",
  "version": "$version",
  "platform": "linux-x86_64",
  "installer": "$name",
  "sha256": "$checksum",
  "payload_files": ${#files[@]},
  "payload_bytes": $payload_bytes,
  "source_commit": "$("${git[@]}" rev-parse HEAD)",
  "source_modified": $modified,
  "includes_game_media": false,
  "game_requires": ["${glibc:-none}", "${libstdcxx:-none}"],
  "setup_requires": ["${setup_glibc:-none}"],
  "bundled_system_libraries": [$list]
}
JSON
echo "Installer ready: $installer (game needs ${glibc:-glibc} and ${libstdcxx:-libstdc++})"
