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
    name        = 'Roulette Arena',
    description = 'PvP battle-roulette arena.\nThe generated map is replaced by the berkutx loader.\nAuthor: berkutx',

    minSize    = 48,   -- floor in this mss32 build; equal min=max => size fixed at 48
    maxSize    = 48,
    maxPlayers = 2,    -- must be >= starting zones (2) or generation throws

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
