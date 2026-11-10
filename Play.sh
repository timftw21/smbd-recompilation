#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
case ${1:-} in
    --steamdeck) shift; exec bash "$root/cmake/steamdeck.sh" play "$@" ;;
    --linux) shift ;;
    *) if [[ -x $root/build/steamdeck/Release/smbd ]]; then
           exec bash "$root/cmake/steamdeck.sh" play "$@"
       fi ;;
esac
game="$root/build/linux/Release/smbd"
[[ -x $game ]] || { echo 'Build the game with Build.sh first.' >&2; exit 1; }
exec "$game" "$@"
