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
| `loader/` | C++ (x86) | `berkutx_loader.exe` — the single shipped file. It **embeds** `hook.dll` and `berkutx_rng.exe` (RCDATA), unpacks them on launch, then launches the game and injects the hook. Traces each run to `berkutx_loader.log`. |
| `reroller/` | C# (.NET) | `berkutx_rng.exe` re-rolls the arena's content (camps / chests / gatherer) by swapping ID strings + stats in place. It parses the game's dBASE tables for the unit/item pools and embeds the arena `.sg`. Self-contained (~11 MB, .NET runtime bundled) and shipped **inside** the loader. |
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
./build.ps1        # -> dist/berkutx_loader.exe  (single self-contained file)
```

Requires 32-bit MinGW (`i686-w64-mingw32-g++` + `windres`) for the C++ side and the .NET SDK (8+) for the
C# side — both cross-build the Windows artifacts on Linux. The re-roller is built first, then the loader
embeds it + `hook.dll` as resources. CI ([`build.yml`](.github/workflows/build.yml)) runs on Ubuntu for
every push/PR; pushing a `v*` tag triggers [`release.yml`](.github/workflows/release.yml) to publish a
GitHub Release (zip + SHA-256). The loader is fully self-contained, so players copy a single file and
install nothing.

## Install

Drop the single `berkutx_loader.exe` from `dist/` into the *Disciples II* game folder and run it with no
arguments — it unpacks its embedded hook + re-roller (into `%LOCALAPPDATA%\berkutx_roulette\`), launches
the game, waits for it to come up, and injects the hook (which installs the fake template automatically).
Random scenario → **Roulette Arena** → Continue → host. Each run is traced to `berkutx_loader.log` next
to the exe.

Other modes: `berkutx_loader.exe <pid>` injects into a specific running instance, `berkutx_loader.exe -a`
(or `--attach`) attaches to the first running game without launching one, and a trailing
`path\to\hook.dll` overrides the embedded hook.

## Status

Working in hotseat on the targeted mod build. The mss32 hooks are matched against fixed RVAs + a prologue
signature; on a *different* `mss32.dll` build the guard refuses to patch (logged once in
`berkutx_roulette.log`) instead of crashing — porting needs the RVAs re-found for that build. Further
hardening (signature/RTTI resolution, an `.ini` for paths/factions) is tracked in `loader/` TODOs.
