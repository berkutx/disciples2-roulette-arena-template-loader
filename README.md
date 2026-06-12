# luckytest — Disciples II PvP battle-roulette

*(Сначала по-русски: что это, установка, опции. Дальше — English technical docs.)*

**luckytest** — это карта-«рулетка» для быстрого PvP в *Disciples II* с модом **D2ModdingToolset**
(`mss32.dll`). Хост выбирает фейковый шаблон случайного сценария **luckytest** — и вместо обычной
сгенерированной карты играется готовая авторская арена со **свежими случайными армиями, сундуками и
наёмниками**. Идея: «бой на второй день» — каждый набирает 7 случайных отрядов из лагерей и 7 сундуков,
пропускает первый ход, на втором хост раздаёт зелья, дальше — бой.

Всё работает **снаружи**: без правки файлов игры и без исходников мода — инъектируемый хук
перехватывает генерацию, маленький реролл собирает карту, а нужные функции мода найдены реверсом (IDA).

## Установка

1. Скинуть **`berkutx_loader.exe`** (один файл) в папку с игрой *Disciples II*.
2. Запустить его **без аргументов** — он сам поднимет игру, дождётся загрузки и подцепится.
3. В игре: **Random scenario → luckytest →** выставить спины (см. ниже) **→ Continue → хост**.

Больше ничего ставить не нужно — `hook.dll` и реролл вшиты в лоадер и распаковываются сами.

## Опции (спины в диалоге генерации)

У шаблона **luckytest** три выпадающих спина:

| Спин | Значения | Что делает |
|---|---|---|
| **Режим** | Случайно / Баланс | *Случайно* — полностью случайные армии в лагерях. *Баланс* — тоже случайные, но суммарная сила обоих игроков (по XP за убийство, 7 лагерей каждому) ≈ равна; в лагерях нет юнитов с HP ≥ 900. |
| **Сундуки** | Всё / Зелья | *Всё* — любые предметы. *Зелья* — в сундуках только зелья. |
| **Перки** | Нет / Да | *Да* — сборщику (лидеру отряда) выдаётся знание артефактов, реликвий, знамён, сфер и свитков — может пользоваться ими сразу. |

Сборщик-лидер всегда в пределах HP 200–899.

---

## Files created (after first run)

The loader installs nothing system-wide. On first run it writes, next to / off the game:

- `%LOCALAPPDATA%\berkutx_roulette\` — the unpacked `hook.dll` + `berkutx_rng.exe`.
- `<game>\Templates\luckytest.lua` — the fake template stub (so it appears in the picker; an old
  `Roulette Arena.lua` from earlier builds is auto-removed).
- `<game>\Exports\luckytest <seed>.sg` — the rolled arena that is actually played.
- `<game>\berkutx_loader.log` (injector trace) and `<game>\berkutx_roulette.log` (hook trace).

## Antivirus

`berkutx_loader.exe` is a **DLL injector** (it launches the game and writes `hook.dll` into it via
`CreateRemoteThread` + `LoadLibrary`) **and** a self-extracting bundle. Both are classic heuristic
triggers, and the exe is **unsigned**, so Windows Defender / SmartScreen may flag it (a generic
`Wacatac` / PUA detection) — a **false positive**. If it gets quarantined, allow the file in Windows
Security, or build it yourself from source (below).

## How it works (technical)

When the host picks **luckytest** and continues, the hook:

1. reads the chosen spin values from the menu's `GenerationSettings` (`menu+44`),
2. **rolls** a fresh arena into its own seed-named `.sg` (`berkutx_rng.exe`), honouring the options,
3. **skips** the generation-result dialog and goes straight to the lobby,
4. **redirects** the game's map builder (`CMidServerBuilderFull`) to load *our* `.sg`,
5. **overrides** the lobby race list to the arena's factions (Empire + Elf).

The engine's random generator still runs (the pipeline needs it) but its output never reaches the
player. The mss32 targets are **resolved at runtime from stable anchors** (unique strings + fixed
in-function offsets), with a PE-TimeDateStamp table as a fallback, so a re-released mod build is
handled without a rebuild. Addresses, the rsg → played-scenario bridge and the dead-ends are in
[docs/how-it-works.md](docs/how-it-works.md).

### Components

| | | |
|---|---|---|
| `loader/` | C++ (x86) | `berkutx_loader.exe` — the single shipped file. **Embeds** `hook.dll` + `berkutx_rng.exe` (RCDATA), unpacks them on launch, then launches the game and injects the hook. |
| `reroller/` | C# (.NET) | `berkutx_rng.exe` re-rolls camps / chests / gatherer by swapping ID strings + stats in place (and, for perks, appends modifier IDs to the gatherer's unit). Parses the game's dBASE tables for the pools, embeds the arena `.sg`. Self-contained (~11 MB), shipped **inside** the loader. |
| `docs/` | | The reverse-engineering write-up. |

### Build

```powershell
./build.ps1        # -> dist/berkutx_loader.exe  (single self-contained file)
```

Needs 32-bit MinGW (`i686-w64-mingw32-g++` + `windres`) for the C++ and the .NET SDK (8+) for the C#
— both cross-build the Windows artifacts on Linux. The re-roller is built first, then the loader
embeds it + `hook.dll`. CI ([`build.yml`](.github/workflows/build.yml)) runs on Ubuntu for every
push/PR; a `v*` tag triggers [`release.yml`](.github/workflows/release.yml) to publish a GitHub
Release (zip + SHA-256).

Other loader modes: `berkutx_loader.exe <pid>` injects into a specific instance; `-a` / `--attach`
attaches to the first running game without launching one; a trailing `path\to\hook.dll` overrides the
embedded hook.
