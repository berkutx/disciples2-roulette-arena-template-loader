-- ============================================================================
--  Roulette Arena — PLACEHOLDER RSG template (the "fake template" hijack entry).
--
--  The real arena is NOT generated from this file. This stub only has to:
--    (a) appear in the random-scenario template list (the mod scans Templates\*.lua),
--    (b) parse cleanly (a malformed .lua can break the WHOLE picker — readSettings
--        runs per-file at list-build time with no try/catch),
--    (c) let the RSG produce a trivial throwaway 48x48 map WITHOUT erroring,
--        because generation runs synchronously on the host thread with no try/catch
--        and an uncaught rsg::TemplateException aborts the game.
--
--  The roulette loader hook then REPLACES the generated result with the authored
--  arena (RouletteRng.exe output) at the .sg-save point (sub_10267FD0). See
--  RE/30-rsg-generator-hijack.md.
--
--  Minimal valid structure (verified vs maptemplatereader.cpp + mss32.dll.c port):
--    * global table `template` (REQUIRED)
--    * `template.getContents` is a function (REQUIRED)
--    * getContents returns { zones = {...}, connections = {...} }  (both keys REQUIRED)
--    * each zone needs `type`; PlayerStart/AiStart zones need `race`
--    * maxPlayers must be >= number of starting zones (2 here)
--    * scenario size floor is 48 in this build
--    * NO `diplomacy` block (omitting it => auto neutral/war = free-for-all PvP)
--
--  Author: berkutx
-- ============================================================================

template = {
    name        = 'luckytest',
    -- Russian description (Windows-1251 \ddd escapes; ASCII literal; no "ё" — the font lacks the glyph).
    -- "Арена удачи для быстрого PvP. Каждому игроку выпадает 7 случайных отрядов и 7 сундуков. Первый ход
    --  на подготовку, на втором хост обязан выпить свои зелья непосредственно перед решающим боем.  Автор: berkutx"
    description = '\192\240\229\237\224 \243\228\224\247\232 \228\235\255 \225\251\241\242\240\238\227\238 PvP. \202\224\230\228\238\236\243 \232\227\240\238\234\243 \226\251\239\224\228\224\229\242 7 \241\235\243\247\224\233\237\251\245 \238\242\240\255\228\238\226 \232 7 \241\243\237\228\243\234\238\226. \207\229\240\226\251\233 \245\238\228 \237\224 \239\238\228\227\238\242\238\226\234\243, \237\224 \226\242\238\240\238\236 \245\238\241\242 \238\225\255\231\224\237 \226\251\239\232\242\252 \241\226\238\232 \231\229\235\252\255 \237\229\239\238\241\240\229\228\241\242\226\229\237\237\238 \239\229\240\229\228 \240\229\248\224\254\249\232\236 \225\238\229\236.\n\192\226\242\238\240: berkutx',

    minSize    = 48,   -- floor in this mss32 build; equal min=max => size fixed at 48
    maxSize    = 48,
    maxPlayers = 2,    -- must be >= starting zones (2) or generation throws

    -- In-game spin controls (SPIN_PARAMETER_1..3). The chosen values persist in the menu's
    -- GenerationSettings (menu+44); the loader hook reads them there and passes mode/chests/perks to
    -- berkutx_rng. The engine ignores these (they are not its built-in params) — harmless to generation.
    -- Labels are Windows-1251 bytes (\ddd escapes) — the Disciples II UI renders Cyrillic in cp1251.
    customParameters = {
        { name = '\208\229\230\232\236',         values = { '\209\235\243\247\224\233\237\238', '\193\224\235\224\237\241' }, default = 1 },  -- Mode:   Random / Balance
        { name = '\209\243\237\228\243\234\232', values = { '\194\241\229', '\199\229\235\252\255' },                         default = 1 },  -- Chests: Vse(All) / Potions
        { name = '\207\229\240\234\232',         values = { '\205\229\242', '\196\224' },                                     default = 1 },  -- Perks:  No / Yes
    },

    -- races = the lobby-chosen races (>= 2 for a 2-player game); scenarioSize = 48.
    getContents = function(races, scenarioSize)
        return {
            zones = {
                { id = 0, type = Zone.PlayerStart, race = races[1],             size = scenarioSize / 2 },
                { id = 1, type = Zone.PlayerStart, race = races[2] or races[1], size = scenarioSize / 2 },
            },
            connections = {
                { from = 0, to = 1 },
            },
        }
    end,
}
