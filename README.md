<p align="center">
  <img src="resources/smbd.png" alt="Super Monkey Ball Deluxe project icon" width="160">
</p>

<h1 align="center">Super Monkey Ball Deluxe — Static Recompilation</h1>

From the creator of the [Burnout 2 recompilation](https://github.com/timftw21/burnout2-recompilation).

An unofficial native version of the original Xbox game for Windows, built around accurate gameplay, the original presentation, and higher refresh rates.

**This is a playable development build, not a finished port.** Bugs, slowdowns, and stops in untested game modes are still possible. Linux and macOS are planned targets; the current game build requires Windows x64.

This fan project is independent of SEGA. You must provide your own US Xbox game disc image. Original game files are not distributed here.

## AI disclosure

OpenAI Codex has assisted with development and research. Changes are checked using native diagnostics, scripted gameplay, and rendering comparisons. Those checks establish specific results, rather than complete game compatibility.

## Current status

### What works

- Startup, opening movies, and the main menus.
- Story Mode gameplay, including reaching GOAL on 1-1 SIMPLE and continuing to 1-2 DIAMOND.
- Practice Mode, including AIAI on World 2, stage 3 COMPOSITION.
- Keyboard and gamepad input, pause/resume, and native audio playback.
- Creating and reopening game saves.
- Saved display, refresh-rate, audio, and game-file settings.
- Original 4:3, widescreen 16:9, and ultrawide 21:9 options; 1×–4× internal resolution.
- Fresh scene rendering above 60 Hz, with the original rule and input timing preserved.
- Frame captures, offline rendering replay, and CPU/GPU performance reports.

### What still needs work

- Testing every stage, game mode, and progression path. Party games have not been fully validated.
- Remaining original call targets and native Xbox services. The latest generation compiles 10,628 functions with three unresolved indirect call sites; that count is not a measure of overall completeness.
- Wider visual, audio, and gameplay comparisons against the original Xbox release.
- Sustained 360 Hz performance. Selecting a refresh rate does not guarantee the hardware can reach it.
- Native Linux and macOS implementation and hardware validation.

## Getting started

### Windows x64

You need a Direct3D 11-capable graphics card and an XISO image of the **US Xbox release of Super Monkey Ball Deluxe**. A full raw-disc dump must first be converted to XISO. The executable is checked against [the supported-target registry](knowledge/supported_target.json).

This repository currently uses a source build. There is no automatic installer.

Install these build prerequisites:

- Visual Studio 2026 C++ build tools and the Windows SDK, including MASM.
- LLVM 22.1 or newer, including `clang-cl`, LLD, and `llvm-lib`.
- CMake 4.2 or newer, Git, PowerShell 7 or newer, and JDK 21 or newer.
- Ninja; the build script can use the copy included with Visual Studio.

Clone the repository, then run from its folder in PowerShell:

```powershell
git clone https://github.com/timftw21/smbd-recompilation.git
cd smbd-recompilation
.\tools\Build.ps1 -Disc 'C:\Games\Super Monkey Ball Deluxe (USA).xiso.iso' -AllowPartial -Workers 3
.\Play.cmd
```

Replace the example disc path with your own. The first build downloads pinned dependencies, prepares the disc, runs offline analysis, and compiles the game. Python is not required. `-AllowPartial` is currently necessary for the prototype: unimplemented paths stop with an error instead of silently pretending to work.

In **Game Files**, select your disc image and the prepared `data/local/default.xbe`, validate the files, then choose **Play game**. Keep the source folder, build output, and disc image available. See [BUILDING.txt](BUILDING.txt) for the build stages and developer commands.

### Linux and macOS

These remain development targets, not supported installation options. The intended graphics backends are DXVK Native/Vulkan on Linux and DXMT/Metal on macOS, without Wine. The current CMake game-batch build explicitly requires Windows x64; no native Linux or Mac gameplay result is claimed.

## Game settings

Press **F1** or **Esc**, or the gamepad **Back** button, to open settings. Preferences save automatically.

- **Game Files:** Select and validate the XISO and executable.
- **General:** Choose a refresh limit or Unlimited, and toggle the frame-rate display. V-sync can impose an additional limit.
- **Audio:** Adjust master volume, mute, or mute when the window loses focus.
- **Graphics:** Set aspect ratio, fullscreen, V-sync, and 1×–4× internal resolution. Restart the game to apply resolution or aspect changes.
- **Diagnostics:** Capture a frame or record a slowdown.

Higher rendering rates produce new scene updates. Game rules and input retain their original 60 Hz clock; audio keeps its own sample clock and movies retain their source cadence.

## Controls

Connected gamepads use the left stick for stage tilt, **A** to confirm, **B** to go back, and **Start** to pause.

| Action | Keyboard |
| --- | --- |
| Tilt the stage | W / A / S / D |
| Navigate menus | Arrow keys |
| Confirm / A | Space |
| Back / B | Left Shift |
| Start / Pause | Enter |
| Open settings | F1 / Esc |
| Capture a frame | F8 |
| Start / save performance recording | F9 |

Movement keys apply full tilt. Short taps allow finer control. Game input is held neutral while settings are open.

## Saves and updates

Game saves and the local storage identity are in `data/user`. **Back up that entire folder, including `storage.key`.** Preferences are in `data/local/settings.cfg`.

Close the game before updating the source. Preserve your `data` and `build` folders, rebuild, then launch `Play.cmd` again. When moving the project or disc image, update the paths in Game Files.

## Development and credits

Game code is translated ahead of time into native C++23. Xbox services use native implementations; the game does not run a CPU interpreter. Extraction, analysis, rendering inspection, and diagnostics share the project's native core.

- [Burnout 2 recompilation](https://github.com/timftw21/burnout2-recompilation): reusable extraction, CPU, and recompilation foundations.
- SDL3: windows, controllers, and audio.
- Dear ImGui: settings and diagnostic controls.
- Zydis and Ghidra: offline instruction analysis and generation.
- Direct3D 11 / HLSL: the current Windows renderer.
- XboxDev, xemu, and Cxbx-Reloaded: documented system and graphics behavior used as research references.

See [DESIGN.txt](DESIGN.txt), the [research notes](knowledge/), and [THIRD_PARTY_NOTICES.txt](THIRD_PARTY_NOTICES.txt) for requirements, evidence, and dependency notices. Disc images, extracted game assets, generated game sources, saves, and local diagnostics are intentionally excluded from this repository.
