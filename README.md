# Ghost Recon Wildlands: offline ESP, aim assist and weapon mods

A personal reverse engineering project on Ghost Recon Wildlands (PC, DX11). It is an injected DLL with an
in-game ImGui menu: an ESP, a hold-to-aim assist, line of sight taken from the game's own ray queries, and
weapon and bullet mods. Each feature started as a research question about how the game works.

> **Single-player offline only.** It is for the solo campaign with Ubisoft Connect in offline mode. Never use it
> in Ghost War, in co-op or in any online session. The injector refuses to run if it sees an anti-cheat module,
> process or service. Nothing here bypasses anti-cheat, DRM or ownership checks, and the repository contains no
> game files and no decompiled code.

## Features

**ESP**
- Characters read from the game's own entity list and character update job, without any memory scanning.
- Categories (enemies, civilians and others), a dead flag and the distance; heads are found even on crouched or
  leaning characters.
- Colour by line of sight: visible, hidden or unknown.

**Aim assist** (while a key is held)
- Written into the game's view-angle accumulator, with one correction per new camera frame so it doesn't
  oscillate.
- Leaves recoil to the game: the kick is detected and not fought.
- Ballistics from the active weapon's real bullet speed, and from bullet paths learned from the player's own
  shots.
- Line of sight cast from the game's fire origin, with the same ray queries the bullets use.

**Weapon and bullet mods** (Weapon tab)
- Spread, recoil, damage and bullet speed.
- Instant, straight (no drop) and teleported bullets.
- "Magic" bullets: one per enemy, picked automatically, through walls and vehicle hulls.
- Infinite ammo through the game's own rule flag, and 1 to 32 bullets per shot.

## How it works

- **No code patching for the gameplay hooks.** The CPU debug registers act as hooks: execution breakpoints and
  a vectored exception handler, so no byte of the game's code is changed. The only inline hooks are DXGI
  `Present` / `ResizeBuffers` (MinHook), for the overlay.
- **Bullets** follow a precomputed path, one segment per simulation step. The mods rewrite that path: straight,
  in long chunks, or with a jump next to the target before the last collision test.
- **Memory reads that can't crash:** SEH-guarded copies, validated through a per-thread cache of whole memory
  regions.
- **Live telemetry:** the DLL streams JSON lines to a file or a named pipe, and `tools/tail_telemetry.py` follows
  them in real time.
- **Hot-reloaded config:** `grw.ini` is parsed all-or-nothing and reapplied within a second of being saved.

## Repository layout

| Path | Contents |
|---|---|
| `src/dll` | The injected DLL: hooks, overlay, menu, ESP, aim, hardware breakpoints |
| `src/core`, `src/math`, `src/aim`, `src/scan`, `src/telemetry`, `src/config` | Game-agnostic building blocks, unit tested |
| `src/game` | The game's structures as they were found |
| `src/injector` | Injector with an anti-cheat check and clean unload |
| `src/testhost`, `src/probe` | A DX11 test window to inject into, and a read-only probe |
| `tests` | Catch2 unit tests |
| `tools/tail_telemetry.py` | Follows the DLL's telemetry live |
| `scripts/build.ps1` | Configure, build and test in one step |

## Build

Requirements: Windows 10/11 x64, Visual Studio 2022 (MSVC, CMake) and [vcpkg](https://github.com/microsoft/vcpkg).
Dependencies (MinHook, Dear ImGui, Catch2) come from `vcpkg.json`.

```powershell
$env:VCPKG_ROOT = 'C:\path\to\vcpkg'
cmake --preset vs-x64
cmake --build --preset release
ctest --preset release
```

`scripts/build.ps1 -Test` does the same; adjust the vcpkg and CMake paths at the top of the file first.

## Usage

1. Start the game with Ubisoft Connect in offline mode, then load the solo campaign.
2. Inject: `injector.exe --proc GRW.exe --dll <path to grw.dll>`.
3. In game: `Insert` opens the menu, `F8` toggles the panel, `F6` toggles the ESP, mouse button 5 holds the aim
   and `End` unloads the DLL. All keys can be changed in `grw.ini`.
4. Unload with `End` or `injector.exe --unload` before you close the game or go online.

Back up your saves before trying it.

## Status

The project is finished. One thing is still open: the **fire rate**. It isn't stored in the weapon, the shooter
or the weapon's design data. It appears to come from the behaviour graph or the fire animation, and "bullets
per shot" takes its place.

Tested on the Steam version (TU25) on Windows 11.
