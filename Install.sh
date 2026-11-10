#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
mode=--linux
if [[ ${1:-} == --steamdeck || ${1:-} == --linux ]]; then
    mode=$1;shift
elif grep -Eq '^ID="?steamos"?$' /etc/os-release; then
    mode=--steamdeck
fi
if (( $# != 1 )); then
    echo 'Usage: bash Install.sh [--linux|--steamdeck] /path/to/game.xiso.iso' >&2
    exit 1
fi
input=$(realpath -e -- "$1")
[[ -f $input ]] || { echo 'Select a supported Xbox disc image.' >&2; exit 1; }
if [[ $mode == --steamdeck ]]; then
    bash "$root/cmake/steamdeck.sh" prepare
    bash "$root/Build.sh" --steamdeck "$input"
else
    bash "$root/Build.sh" "$input"
fi
applications="${XDG_DATA_HOME:-$HOME/.local/share}/applications"
mkdir -p -- "$applications"
# Desktop entry strings unescape backslashes before parsing the Exec field.
exec_path=${root//\\/\\\\\\\\}
exec_path=${exec_path//\"/\\\\\"}
exec_path=${exec_path//\$/\\\\\$}
exec_path=${exec_path//\`/\\\\\`}
exec_path=${exec_path//%/%%}
icon_path=${root//\\/\\\\}
printf '[Desktop Entry]\nType=Application\nName=Super Monkey Ball Deluxe\nExec=/usr/bin/env bash "%s/Play.sh" %s\nIcon=%s/resources/smbd.png\nTerminal=false\nCategories=Game;\n' \
    "$exec_path" "$mode" "$icon_path" > "$applications/smbd-recompilation.desktop"
printf 'Added Super Monkey Ball Deluxe to the application menu.\n'
