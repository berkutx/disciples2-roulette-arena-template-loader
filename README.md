# berkutx — Disciples II battle-roulette arena

Turns the **random-scenario** feature of a modded *Disciples II* (D2ModdingToolset / `mss32.dll`)
into a **PvP battle roulette**: the host picks one fake template — **"Roulette Arena"** — and the
game plays a hand-authored arena with freshly randomized armies, artifacts and mercenaries instead
of an engine-generated random map.

It does this from the outside, with **no game-file edits and no source** — an injected hook
intercepts the mod's generation pipeline, a small re-roller produces the map, and runtime
reverse-engineering (IDA) found the exact functions to redirect. See
[docs/how-it-works.md](docs/how-it-works.md).

## Components

| | | |
|---|---|---|
| `loader/` | C++ (x86) | `berkutx_loader.exe` injects `hook.dll`; the hook installs the inline hooks and embeds the fake-template stub. |
| `reroller/` | C# (.NET) | `berkutx_rng.exe` re-rolls the arena's content (camps / chests / gatherer) by swapping ID strings + stats in place. It parses the game's dBASE tables for the unit/item pools and embeds the arena `.sg` — a single self-contained ~12 MB exe (the .NET runtime is bundled, nothing to install). |
| `docs/` | | The reverse-engineering write-up: the rsg → played-scenario bridge and why the obvious approaches miss. |

## How it works (short)

When the host picks "Roulette Arena" and continues, the hook:

1. **rolls** a fresh arena into its own seed-named `.sg` (`berkutx_rng.exe`),
2. **skips** the generation-result dialog and goes straight to the lobby,
3. **redirects** the game's map builder (`CMidServerBuilderFull`) to load *our* `.sg`,
4. **overrides** the lobby race list to the arena's factions (Empire + Elf).

The engine's random generator still runs (the pipeline needs it) but its output never reaches the
player — every player-visible surface is sourced from the authored arena. Details, addresses and
the dead-ends in [docs/how-it-works.md](docs/how-it-works.md).

## Build

```powershell
./build.ps1        # -> dist/  (berkutx_loader.exe, hook.dll, berkutx_rng.exe)
```

Requires 32-bit MinGW (`i686-w64-mingw32-g++`) for the C++ side and the .NET SDK (8+) for the C# side
— both cross-build the Windows artifacts on Linux, so CI ([`.github/workflows/build.yml`](.github/workflows/build.yml))
runs on Ubuntu and publishes a GitHub Release (with a SHA-256 checksum) on each `v*` tag.
`berkutx_rng.exe` is self-contained (the .NET runtime is bundled), so players install nothing.

## Install

Drop the three files from `dist/` into the *Disciples II* game folder, launch the game, then run
`berkutx_loader.exe` (no arguments — it finds the running game and injects the hook; the hook installs
the fake template automatically). Random scenario → **Roulette Arena** → Continue → host.

## Status

Working in hotseat. Targets are matched against fixed RVAs + a prologue signature for one mod build;
production hardening (signature/RTTI resolution, an `.ini` for paths/factions, auto-load at game
start) is tracked in `loader/` TODOs.
