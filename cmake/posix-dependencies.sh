# Sourced by Build.sh. The pinned native HLSL compiler needs a copy-propagation
# fix: stock 2.1 incorrectly moves float negation through integer bitcasts.
# Only its standalone library is built; no Python or Wine runtime is needed.
prefix="$root/build/deps/vkd3d-2.1${SMBD_DEPENDENCY_SUFFIX:-}"
export PKG_CONFIG_PATH="$prefix/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
if [[ ! -f "$prefix/lib/libvkd3d-shader.a" ]] || ! pkg-config --exact-version=2.1 smbd-vkd3d-shader ||
    ! cmp -s "$root/cmake/vkd3d-copy-propagation.patch" "$prefix/share/smbd/vkd3d-copy-propagation.patch" ||
    ! cmp -s "$root/cmake/vkd3d-json.patch" "$prefix/share/smbd/vkd3d-json.patch"; then
    for program in make flex bison perl cc patch; do
        command -v "$program" >/dev/null || { printf 'Missing %s; see BUILDING.txt.\n' "$program" >&2; exit 1; }
    done
    perl -MJSON::PP -e 1 || { printf '%s\n' 'Perl with its core JSON::PP module is required; see BUILDING.txt.' >&2; exit 1; }
    archive="$root/build/deps/vkd3d-2.1.tar.xz"
    mkdir -p -- "$root/build/deps"
    checksum=7510146aff2adfb4ae07ab890701a607e5ff7c66e57100cfc9f630ec92eeda6a
    if [[ ! -f $archive ]] || [[ $(smbd_sha256 "$archive") != "$checksum" ]]; then
        curl -fL --retry 2 https://dl.winehq.org/vkd3d/source/vkd3d-2.1.tar.xz -o "$archive.download"
        [[ $(smbd_sha256 "$archive.download") == "$checksum" ]] || { printf '%s\n' 'vkd3d checksum mismatch.' >&2; exit 1; }
        mv -f -- "$archive.download" "$archive"
    fi
    (
        # Autotools and libtool need a build directory without spaces.
        source_directory=$(mktemp -d /tmp/smbd-vkd3d.XXXXXX)
        trap 'rm -rf -- "$source_directory"' EXIT
        tar -xJf "$archive" --strip-components=1 -C "$source_directory"
        cd -- "$source_directory"
        patch -p1 < "$root/cmake/vkd3d-copy-propagation.patch"
        patch -p1 < "$root/cmake/vkd3d-json.patch"
        # The release archive includes configure/Makefile.in; our patch updates
        # configure and configure.ac together, so no Autotools regeneration is needed.
        ./configure --disable-maintainer-mode --disable-shared --enable-static --disable-tests --disable-demos --disable-doxygen-doc
        make include/private/vkd3d_version.h
        make -j1 libvkd3d-shader.la
        mkdir -p "$prefix/lib/pkgconfig" "$prefix/include/vkd3d" "$prefix/share/licenses/vkd3d" "$prefix/share/smbd"
        install -m644 .libs/libvkd3d-shader.a "$prefix/lib/"
        cat > "$prefix/lib/pkgconfig/smbd-vkd3d-shader.pc" <<'PC'
prefix=${pcfiledir}/../..
Name: smbd-vkd3d-shader
Description: Native offline HLSL compiler
Version: 2.1
Libs: -L"${prefix}/lib" -lvkd3d-shader -lm
Cflags: -I"${prefix}/include/vkd3d"
PC
        install -m644 include/vkd3d_shader.h include/vkd3d_types.h "$prefix/include/vkd3d/"
        install -m644 COPYING "$prefix/share/licenses/vkd3d/"
        install -m644 "$root/cmake/vkd3d-copy-propagation.patch" "$prefix/share/smbd/"
        install -m644 "$root/cmake/vkd3d-json.patch" "$prefix/share/smbd/"
    )
fi
