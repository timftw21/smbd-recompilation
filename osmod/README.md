# Mod support (OSMod)

This folder makes the recompilation moddable with
[OSMod](https://github.com/irpina/osmod), an open-source mod framework for
decompiled and statically recompiled games. Mods name **events** (such as
"every frame" or "after the controller is read") instead of patching
addresses, so several mods can use the same function, and the builder
refuses incompatible mod sets before anything compiles.

**The normal build is unchanged.** Without mods, `tools\Build.ps1` builds
the same game as before, the settings menu shows an empty **Mods** page,
and no Python is involved. Python is only needed to *add* mods, because
the OSMod builder is a small Python program.

| File | What it is |
|---|---|
| `target.json` | How OSMod sees this project: the executable's identity, the generated `guest_XXXXXXXX` functions, and the build command. |
| `target.h` | `osmod::Ctx`, the handler context for the x86 guest ABI: stack arguments, `eax`, guest memory. |
| `core/` | Core 0.1: the events mods can use, and helpers for them. |
| `mods/hello-frames/` | A small example mod that proves the hooks run. |
| `../src/osmod_host.{h,cpp}` | The OSMod reference host: the registry of built-in mods, switches and file changes. |

## Events in core 0.1

| Event | Function | When | What a handler can do |
|---|---|---|---|
| `frame` | `0x00320190` (`D3DDevice_Swap`) | pre | Runs once per presented frame. `core::frame_count()` is the number of this frame. |
| `pad_read` | `0x003422A4` (`XInputGetState`) | post | `core::pad(ctx)` is the controller state the game is about to see; edit it to change input. |

New events are welcome: when a mod needs a function the core doesn't
cover, add it to `core/mod.json` (address, `when`, a `doc` line) and bump
the core's minor version.

## Building with mods

You need a working build from `tools\Build.ps1` first, and Python 3.10+
with a checkout of OSMod beside this repository.

```powershell
git clone https://github.com/irpina/osmod ..\osmod
$env:PYTHONPATH = "..\osmod"

# see where every handler would run (changes nothing)
python -m osmod plan --target osmod osmod/core osmod/mods/hello-frames

# write the mods into build/generated and compile the game (about 20 s)
python -m osmod build --target osmod --tree . --compile osmod/core osmod/mods/hello-frames

# back to stock
python -m osmod restore --target osmod --tree .
```

`build` edits only what it records: it renames the hooked functions in
two generated shards, and adds `build/generated/game.cpp.900.cpp` and up
(CMake already compiles any numbered shard). `restore` puts every file
back byte for byte.

**Run `restore` before `tools\Build.ps1`**, and `build` again afterwards:
`Build.ps1` regenerates every shard. If you forget, `restore` notices the
regenerated files and keeps them.

## In the game

Press **F1** or **Esc** and open **Mods**. The page lists every built-in mod
with its preview, version, authors, licence, description, the events it
hooks and the files it changes. The tick box switches a mod on or off:
code changes apply at once, and file changes apply after **Restart
game**. Switches are saved in `data/local/osmod.cfg`.

## File mods

Mods can redirect one disc file to another (`files.redirect`) or supply
replacement files (`files.replace`), including files a mod's generator
script builds on your computer from game data you already own. The host
serves them in `src/xbox.cpp`, where the game opens disc files.

The game reads each file at its original length, into a buffer of that
size. `target.json` therefore sets `"fixed_size": true`: the builder
refuses a replacement that is larger than the original and pads smaller
ones to the original length. Replacement files are written to
`data/local/osmod-files/`.

See OSMod's [MODDING.md](https://github.com/irpina/osmod/blob/main/docs/MODDING.md)
for how to write mods.
