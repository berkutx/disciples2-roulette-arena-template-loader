# How it works

The modded game (`mss32.dll` = D2ModdingToolset) adds a **random scenario generator**: pick a
template, the engine builds a map, you host it. We make one template — **"Roulette Arena"** — yield
our authored arena instead, entirely from an injected DLL.

Two binaries, both x86: `mss32.dll` (the mod, preferred base `0x10000000`) and `Discipl2.exe` (the
game, base `0x400000`). All offsets below are at those bases.

## The pipeline, and the bridge

```
pick template ──▶ generate (worker thread)         sub_101D1C20 ─▶ sub_1026AA50  (rsg core)
                       │ rsg map object ──────────▶ menu+232
                       ▼
              result dialog (3 buttons)             sub_101D21C0 ─▶ sub_101AFDD0
                       │ accept
                       ▼
              host commit                           sub_101D3CB0
                       │  • serialize menu+232 ─────▶  "Exports\Random scenario.sg"   (sub_10267FD0)
                       │  • stamp scenInfo: id, name, race list (menu+68..72)
                       ▼
              host start ─▶ build the played map    Discipl2 sub_4E0A8C ─▶ sub_403798 ─▶ sub_433B0B
                                                     = CMidServerBuilderFull(path)  reads a .sg FILE
```

The rsg map object (`menu+232`) is **never handed to the game as a pointer** — it has exactly two
consumers: a UI-only preview copy, and the serializer `sub_10267FD0` that writes it to a `.sg`.
**So the only bridge from generator to game is a `.sg` file**, which the game then parses with its
own reader (`CMidServerBuilderFull` = `sub_433B0B`). Whoever controls that file controls the map.

## The four hooks

All hooks are 5-byte JMP detours with a trampoline; a prologue-byte signature guard refuses to patch
an unrecognized build.

| # | target | what we do |
|---|---|---|
| 1 | `mss32 sub_10267FD0` (rsg serializer) | After the save, if the scenario carries our marker `"Roulette Arena"`, run `berkutx_rng.exe` to roll a fresh arena into our **own** seed-named `.sg`. We never touch the engine's `Random scenario.sg`. |
| 2 | `mss32 sub_101D21C0` (generation-complete cb) | For our template, skip `DLG_GENERATION_RESULT`: replay its pre-dialog bookkeeping and proceed straight to the lobby via `sub_101D20D0`. |
| 3 | `Discipl2 sub_433B0B` (`CMidServerBuilderFull(this@ecx, char* Source@[esp+4], …)`) | Rewrite the `Source` path to our rolled arena. This is the **single chokepoint** every file map-build funnels through, so the played map is ours. |
| 4 | `mss32 sub_101D3CB0` (host commit) | Overwrite the race vector at `menu+68..72` with the arena's factions `[0 = Empire, 5 = Elf]` so the lobby races match the map (also prevents a load hang from a race/player mismatch). |

The fake template itself is a minimal two-zone Lua stub, embedded in `hook.dll` and written into the
game's `Templates\` on load.

## Why the obvious approaches miss

- **Replacing `Exports\Random scenario.sg`** — the game does not load that file for the played map.
  The lobby/header phase makes a separate temp copy and stores *its* path in the scenInfo container;
  the host build reads the temp copy. A file you replace is off the load path. Fix: redirect the
  builder's path argument (hook #3) rather than fight the file.
- **The in-memory builder `sub_403617`** — for hotseat-random it is never called; the host reaches the
  map through the file-only wrapper `sub_403798 → sub_433B0B`. Redirecting a function that never runs
  changes nothing. (Verified: zero runtime hits.)

## Key addresses

mss32 (`-0x10000000` for RVA): `sub_10267FD0`, `sub_101D21C0`, `sub_101D20D0`, `sub_101D3CB0`,
`sub_101D1C20`, `sub_1026AA50`.
Discipl2 (`-0x400000` for RVA): `sub_433B0B` (played-map builder), `sub_403798` / `sub_4036D0`
(its wrappers), `sub_4E0A8C` (host start), `sub_5E3DB7` (header reader — still rsg; cosmetic TODO).
