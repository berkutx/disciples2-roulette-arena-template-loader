using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Text;

// berkutx_rng — zero-dependency re-roller for the PvP battle-roulette arena (.NET).
//
// The arena is authored once in ScenEdit and embedded here (arena.sg). This tool re-randomizes only
// the "roulette" content by editing fields IN PLACE — events, plan, terrain and capitals stay
// byte-for-byte identical:
//     * merc camps  (CMidSiteMercs):  UNIT_ID -> random soldier, + its UNIT_LEVEL, UNIT_UNIQ;
//                                      first two per lane also get a fresh IMG_ISO look.
//     * chest items (CMidBag->CMidItem): ITEM_TYPE -> random artifact.
//     * gatherers   (CMidStack INSIDE=000000 -> CMidUnit): TYPE -> random hero, + LEVEL=1 and HP
//                                      set to the new type's max (mods kept). HP is the hook for a
//                                      future hard mode that randomizes it.
// Pools and per-unit level/HP come from the game's dBASE tables (Globals\Gunits.dbf, GItem.dbf).
//
// It also stamps the scenario name with the generation time (date + hour:minute) and sets the
// designer to "berkutx" — in the header (fixed-width, in place) and in CScenarioInfo (NAME/CREATOR
// length-strings, spliced) — so freshly rolled arenas are distinguishable in the scenario/save list.
//
// .sg layout: header + blocks. A block = "WHAT"<size:4>".?AV<Class>@@\0" <fields> "ENDOBJECT\0".
// A field is <ascii-tag><value>: length-string = tag + int32-LE length(incl null) + bytes; int =
// tag + int32-LE; bool = tag + 1 byte; the 6-char id = tag + 8 + 6 bytes. Every patched value keeps
// its width, so no offset shifts and no re-serialize.
//
// Usage: berkutx_rng <seed> <out.sg> [gameDir]
internal static class Program
{
    private const string Gargoyle = "g000uu0167";   // half-finished in the mod -> crashes recruit display; excluded

    private static int Main(string[] args)
    {
        try { return Run(args); }
        catch (Exception e) { Console.WriteLine("[rng ERR] " + e); return 1; }
    }

    private static int Run(string[] args)
    {
        int seed = Environment.TickCount;
        string outPath = null, install = AppContext.BaseDirectory.TrimEnd('\\', '/');
        string mode = "random";      // random | balance
        string chests = "all";       // all | potions
        bool perks = false;          // grant the gatherer item-category knowledge
        foreach (var a in args)
        {
            if (a.StartsWith("mode=", StringComparison.OrdinalIgnoreCase)) mode = a.Substring(5).ToLowerInvariant();
            else if (a.StartsWith("chests=", StringComparison.OrdinalIgnoreCase)) chests = a.Substring(7).ToLowerInvariant();
            else if (a.StartsWith("perks=", StringComparison.OrdinalIgnoreCase)) perks = a.Substring(6) is "1" or "true" or "on";
            else if (int.TryParse(a, out int s)) seed = s;
            else if (a.EndsWith(".sg", StringComparison.OrdinalIgnoreCase)) outPath = a;
            else if (Directory.Exists(a)) install = a;
        }
        outPath ??= Path.Combine(install, "Exports", "Roulette Arena.sg");
        var rng = new Random(seed);

        // --- pools (and per-unit level/HP) from the game's dBASE tables ---
        var soldiers = new List<string>();
        var leaders  = new List<string>();
        var items    = new List<string>();
        var stats    = new Dictionary<string, (int lvl, int hp, int xp)>(StringComparer.OrdinalIgnoreCase);
        var gunits = new Dbf(Path.Combine(install, "Globals", "Gunits.dbf"));
        foreach (int r in gunits.Rows())
        {
            string id = gunits.Str(r, "unit_id");
            int hp = gunits.Int(r, "hit_point");
            if (id.Length != 0) stats[id] = (Math.Max(1, gunits.Int(r, "level")), hp, gunits.Int(r, "xp_killed"));
            if (gunits.Bool(r, "water_only")) continue;
            int cat = gunits.Int(r, "unit_cat");
            if (cat == 0 && gunits.Int(r, "leadership") <= 0 && !id.Equals(Gargoyle, StringComparison.OrdinalIgnoreCase)) soldiers.Add(id);
            // gatherer leader: must be a leader class, with HP in [200, 899] (no glass cannons, no end-bosses)
            if (cat == 2 && gunits.Str(r, "leader_cat").Length > 0 && hp >= 200 && hp <= 899) leaders.Add(id);
        }
        // item categories (= GItem.dbf item_cat, 0-based, per D2RSG enums.h):
        //   0 Armor 1 Jewel 2 Weapon 3 Banner 4 PotionBoost 5 PotionHeal 6 PotionRevive
        //   7 PotionPermanent 8 Scroll 9 Wand 10 Valuable 11 Orb 12 Talisman 13 TravelItem 14 Special
        var gitem = new Dbf(Path.Combine(install, "Globals", "GItem.dbf"));
        foreach (int r in gitem.Rows())
        {
            int c = gitem.Int(r, "item_cat");
            bool keep = chests == "potions"
                ? (c >= 4 && c <= 7)              // potions only: Boost/Heal/Revive/Permanent
                : (c != 10 && c != 13 && c != 14); // all: drop Valuable/TravelItem/Special
            if (keep) items.Add(gitem.Str(r, "item_id"));
        }
        if (soldiers.Count == 0 || leaders.Count == 0 || items.Count == 0)
        { Console.WriteLine($"[rng ERR] empty pool (game data not found at {install})"); return 1; }

        // --- load the embedded arena, index its blocks ---
        byte[] sg = Resource("arena.sg");
        var blocks = IndexBlocks(sg);
        var byId = new Dictionary<string, Block>(StringComparer.OrdinalIgnoreCase);
        foreach (var b in blocks) if (b.ObjId != null) byId.TryAdd(b.ObjId, b);

        // first two camps of each lane (grouped by row, ordered by column) get a fresh appearance
        var camps = blocks.Where(b => b.Class == "CMidSiteMercs").ToList();
        var freshLook = new HashSet<Block>();
        foreach (var lane in camps.GroupBy(c => ReadInt(sg, c, "POS_Y")))
            foreach (var c in lane.OrderBy(c => ReadInt(sg, c, "POS_X")).Take(2)) freshLook.Add(c);

        // balance mode: all 14 camps get INDEPENDENT random HP-limited soldiers (no pairing between the two
        // players' camps). We only balance the TOTALS: brute-force a random draw of 14 into a 7/7 split whose
        // two XP_KILLED side-sums are as equal as possible, then drop each half onto a side in random order.
        var campAssign = new Dictionary<Block, string>();
        if (mode == "balance")
        {
            var balPool = soldiers.Where(id => stats.TryGetValue(id, out var s) && s.hp < 900).ToList();   // camps: only the < 900 cap (weak units allowed)
            var sideCamps = camps.GroupBy(c => ReadInt(sg, c, "POS_Y")).Select(g => g.ToList()).ToList();
            if (balPool.Count >= 2 && sideCamps.Count == 2 && sideCamps[0].Count == sideCamps[1].Count)
            {
                int per = sideCamps[0].Count, total = per * 2;
                var masks = new List<int>();
                for (int m = 0; m < (1 << total); m++) if (PopCount(m) == per) masks.Add(m);   // all 7/7 splits
                string[] best = null; int bestMask = 0; long bestDiff = long.MaxValue;
                var sw = System.Diagnostics.Stopwatch.StartNew();
                for (int it = 0; it < 2000 && bestDiff > 0 && sw.ElapsedMilliseconds < 2500; it++)
                {
                    var pk = new string[total]; var xp = new long[total]; long tot = 0;
                    for (int i = 0; i < total; i++) { pk[i] = balPool[rng.Next(balPool.Count)]; xp[i] = stats[pk[i]].xp; tot += xp[i]; }
                    foreach (int m in masks)
                    {
                        long sa = 0;
                        for (int i = 0; i < total; i++) if ((m & (1 << i)) != 0) sa += xp[i];
                        long d = Math.Abs(2 * sa - tot);                            // |sumA - sumB|
                        if (d < bestDiff) { bestDiff = d; best = pk; bestMask = m; if (d == 0) break; }
                    }
                }
                if (best != null)
                {
                    var a = new List<string>(); var b2 = new List<string>();
                    for (int i = 0; i < total; i++) (((bestMask >> i) & 1) != 0 ? a : b2).Add(best[i]);
                    Shuffle(a, rng); Shuffle(b2, rng);
                    for (int i = 0; i < per; i++) { campAssign[sideCamps[0][i]] = a[i].ToUpperInvariant(); campAssign[sideCamps[1][i]] = b2[i].ToUpperInvariant(); }
                }
            }
        }

        // perks: modifier IDs granting the gatherer item-category use (Equip-* family, unconditional)
        string[] perkMods = { "G000UM9023", "G000UM9024", "G000UM9025", "G000UM9027", "G000UM9029" }; // arts, relics, banners, orbs, scrolls
        var gatherUnits = new List<string>();   // gatherer CMidUnit OBJ_IDs (post-loop perks + name)
        var castleUnits = new List<string>();   // capital-hero CMidUnit OBJ_IDs (post-loop name)

        // object-placement grid: the engine reads CMidgardPlan for tile occupancy, so a moved stack must be
        // re-pinned here too (otherwise the icon moves but the unit stays logically on its old tile).
        var plan = blocks.FirstOrDefault(x => x.Class == "CMidgardPlan");

        int nCamp = 0, nChest = 0, nGath = 0;
        foreach (var b in blocks)
        {
            switch (b.Class)
            {
                case "CMidSiteMercs":                                        // camp: recruit unit + its stats + look
                {
                    string uid = campAssign.TryGetValue(b, out var pre) ? pre : Pick(rng, soldiers);   // balance pre-assigns; random picks fresh
                    if (PatchLenStr(sg, b, "UNIT_ID", uid, out int valEnd))
                    {
                        PatchInt(sg, b, "UNIT_LEVEL", stats.TryGetValue(uid, out var st) ? st.lvl : 1, valEnd);
                        PatchBool(sg, b, "UNIT_UNIQ", true, valEnd);
                        if (freshLook.Contains(b)) PatchInt(sg, b, "IMG_ISO", rng.Next(1, 5));
                        nCamp++;
                    }
                    break;
                }
                case "CMidBag":                                              // chest: each held item's type
                    foreach (string iid in ReadAllDefStr(sg, b, "ITEM_ID"))
                        if (byId.TryGetValue(iid, out var it) && it.Class == "CMidItem"
                            && PatchLenStr(sg, it, "ITEM_TYPE", Pick(rng, items), out _)) nChest++;
                    break;
                case "CMidStack":
                {
                    string inside = ReadDefStr(sg, b, "INSIDE");
                    string lid = ReadDefStr(sg, b, "LEADER_ID");
                    if (inside != "000000")                                  // stack inside a fort = a capital hero
                    {
                        if (lid != null) castleUnits.Add(lid);               // name it "use the other hero" later
                        break;
                    }
                    int newX = ReadInt(sg, b, "POS_X") + 3;                  // nudge the gatherer 3 cells toward the camps (along the road)
                    PatchInt(sg, b, "POS_X", newX);                          // stack position (the icon)
                    PatchPlanPos(sg, plan, b.ObjId, newX, ReadInt(sg, b, "POS_Y"));  // and the occupancy grid (the physical tile) — must match
                    if (lid == null || !byId.TryGetValue(lid, out var u) || u.Class != "CMidUnit") break;
                    string lt = Pick(rng, leaders);
                    if (PatchLenStr(sg, u, "TYPE", lt, out int valEnd))
                    {
                        PatchInt(sg, u, "LEVEL", 1, valEnd);                  // the LEVEL right after TYPE (not DYNLEVEL)
                        int hpFrom = Find(sg, Ascii("DYNLEVEL"), u.Start, u.End);   // HP sits just past DYNLEVEL
                        PatchInt(sg, u, "HP", stats.TryGetValue(lt, out var st) ? st.hp : 65, hpFrom < 0 ? valEnd : hpFrom);
                        if (u.ObjId != null) gatherUnits.Add(u.ObjId);            // remember for the post-loop perk/name pass
                        nGath++;
                    }
                    break;
                }
            }
        }

        // perks: append the item-use modifier IDs to each gatherer's CMidUnit (variable-length -> after the in-place pass)
        if (perks)
            foreach (var uid in gatherUnits)
                sg = AddPerks(sg, uid, perkMods);

        // hero names (cp1251): gatherer -> "Сборщик"; capital hero -> "ходи другим героем" (hint: move the gatherer)
        foreach (var uid in gatherUnits) sg = SetUnitName(sg, uid, NameGatherer);
        foreach (var uid in castleUnits) sg = SetUnitName(sg, uid, NameCastle);

        // --- set the scenario name to "luckytest <date>" + the designer to berkutx ---
        const string Designer = "berkutx";
        const string MapName  = "luckytest";   // map name everywhere (overrides the embedded arena's name)
        string fullName = MapName + " " + DateTime.Now.ToString("yyyy-MM-dd HH:mm");
        PatchFixed(sg, 321, 64, fullName);     // header _Name @321/64 (drives the list display)
        PatchFixed(sg, 299, 21, Designer);     // header _Author @299/21
        // CScenarioInfo copy (length-strings): re-locate the block after each splice shifts the tail
        var (cs, ce) = ScenInfo(sg);
        if (cs >= 0)
        {
            sg = ReplaceLenStr(sg, cs, ce, "NAME", fullName);
            (cs, ce) = ScenInfo(sg);
            sg = ReplaceLenStr(sg, cs, ce, "CREATOR", Designer);
        }
        Console.WriteLine($"[rng] name=\"{fullName}\" designer={Designer}");

        File.WriteAllBytes(outPath, sg);
        foreach (var ext in new[] { ".id0", ".id1", ".nam", ".til" })        // drop stale per-scenario caches
        { var f = outPath + ext; if (File.Exists(f)) File.Delete(f); }
        var lua = Path.ChangeExtension(outPath, ".lua"); if (File.Exists(lua)) File.Delete(lua);

        Console.WriteLine($"[rng] mode={mode} chests={chests} perks={perks} | seed={seed} camps={nCamp}(pool {soldiers.Count}) chests={nChest}(pool {items.Count}) gatherers={nGath}(pool {leaders.Count})");
        Console.WriteLine("[rng] wrote " + outPath);
        return 0;
    }

    private static string Pick(Random r, List<string> pool) => pool[r.Next(pool.Count)].ToUpperInvariant();

    private static int PopCount(int x) { int c = 0; while (x != 0) { c += x & 1; x >>= 1; } return c; }
    private static void Shuffle(List<string> l, Random r) { for (int i = l.Count - 1; i > 0; i--) { int j = r.Next(i + 1); (l[i], l[j]) = (l[j], l[i]); } }

    // ---- .sg block model -------------------------------------------------------
    private sealed class Block { public string Class; public int Start; public int End; public string ObjId; }

    private static readonly byte[] AV  = Ascii(".?AV");
    private static readonly byte[] END = Ascii("ENDOBJECT\0");

    private static List<Block> IndexBlocks(byte[] a)
    {
        var list = new List<Block>();
        int i = 0;
        while ((i = Find(a, AV, i)) >= 0)
        {
            int at = i + AV.Length, cend = Find(a, Ascii("@@"), at);
            if (cend < 0) break;
            var b = new Block { Class = Str(a, at, cend - at), Start = i };
            int e = Find(a, END, cend);
            b.End = e < 0 ? a.Length : e + END.Length;
            b.ObjId = ReadDefStr(a, b, "OBJ_ID");
            list.Add(b);
            i = b.End;
        }
        return list;
    }

    // length-string value (tag + int32 len + bytes). Overwrites only when the existing value matches
    // the replacement's width (i.e. both are 10-char ids), so the layout is untouched. valEnd = the
    // byte just after the value (where the next field begins).
    private static bool PatchLenStr(byte[] a, Block b, string tag, string val, out int valEnd)
    {
        valEnd = -1;
        int p = Find(a, Ascii(tag), b.Start, b.End);
        if (p < 0) return false;
        p += tag.Length;
        int len = BitConverter.ToInt32(a, p) - 1;
        if (len != val.Length) return false;
        byte[] vb = Ascii(val);
        Array.Copy(vb, 0, a, p + 4, vb.Length);
        valEnd = p + 4 + len;
        return true;
    }

    private static void PatchInt(byte[] a, Block b, string tag, int val, int from = -1)
    {
        int p = Find(a, Ascii(tag), from < 0 ? b.Start : from, b.End);
        if (p < 0) return;
        BitConverter.GetBytes(val).CopyTo(a, p + tag.Length);
    }

    // Re-pin a CMidgardPlan element to a new tile. The plan is a flat list of {POS_X:int, POS_Y:int,
    // ELEMENT:id} triples (one per occupied tile); the layout is POS_X(5+4) POS_Y(5+4) ELEMENT(7+8+6+1),
    // so for the element whose id matches, POS_X's int is at ELEMENT-13 and POS_Y's at ELEMENT-4.
    private static void PatchPlanPos(byte[] a, Block plan, string objId, int newX, int newY)
    {
        if (plan == null || objId == null) return;
        byte[] tag = Ascii("ELEMENT");
        int p = plan.Start;
        while ((p = Find(a, tag, p, plan.End)) >= 0)
        {
            if (string.Equals(Str(a, p + 15, 6).TrimEnd('\0', ' '), objId, StringComparison.OrdinalIgnoreCase))
            {
                BitConverter.GetBytes(newX).CopyTo(a, p - 13);   // POS_X int
                BitConverter.GetBytes(newY).CopyTo(a, p - 4);    // POS_Y int
                return;
            }
            p += tag.Length;
        }
    }

    private static void PatchBool(byte[] a, Block b, string tag, bool val, int from = -1)
    {
        int p = Find(a, Ascii(tag), from < 0 ? b.Start : from, b.End);
        if (p < 0) return;
        a[p + tag.Length] = (byte)(val ? 1 : 0);
    }

    private static int ReadInt(byte[] a, Block b, string tag)
    {
        int p = Find(a, Ascii(tag), b.Start, b.End);
        return p < 0 ? 0 : BitConverter.ToInt32(a, p + tag.Length);
    }

    // Overwrite a fixed-width, null-padded header slot (clears the slot, keeps a trailing null).
    private static void PatchFixed(byte[] a, int off, int slot, string val)
    {
        Array.Clear(a, off, slot);
        byte[] vb = Ascii(val);
        Array.Copy(vb, 0, a, off, Math.Min(vb.Length, slot - 1));
    }

    // CScenarioInfo block bounds (it holds the length-string NAME / CREATOR).
    private static (int start, int end) ScenInfo(byte[] a)
    {
        int s = Find(a, Ascii(".?AVCScenarioInfo@@"), 0);
        if (s < 0) return (-1, -1);
        int e = Find(a, END, s);
        return (s, e < 0 ? a.Length : e + END.Length);
    }

    private static string ReadLenStr(byte[] a, int start, int end, string tag)
    {
        int p = Find(a, Ascii(tag), start, end);
        if (p < 0) return null;
        int lp = p + tag.Length;
        return Str(a, lp + 4, BitConverter.ToInt32(a, lp) - 1);
    }

    // Replace a length-string value (tag + int32 len + bytes + null), reallocating since the new
    // value may differ in length. Sequential, self-delimiting format -> shifting the tail is safe.
    private static byte[] ReplaceLenStr(byte[] a, int start, int end, string tag, string val)
    {
        int p = Find(a, Ascii(tag), start, end);
        if (p < 0) return a;
        int lp = p + tag.Length, vp = lp + 4;
        int tail = vp + (BitConverter.ToInt32(a, lp) - 1) + 1;     // just past the old value + null
        byte[] vb = Ascii(val);
        var outp = new byte[a.Length - tail + vp + vb.Length + 1];
        Array.Copy(a, 0, outp, 0, lp);
        BitConverter.GetBytes(vb.Length + 1).CopyTo(outp, lp);
        Array.Copy(vb, 0, outp, vp, vb.Length);
        outp[vp + vb.Length] = 0;
        Array.Copy(a, tail, outp, vp + vb.Length + 1, a.Length - tail);
        return outp;
    }

    // Append MODIF_ID entries to the CMidUnit with this OBJ_ID (and bump its modifier count). Reallocs, so
    // call after the in-place pass. The count int sits right after LEVEL: "LEVEL"(5)+lvlInt(4)+countTag(10).
    private static byte[] AddPerks(byte[] a, string unitObjId, string[] mods)
    {
        Block u = IndexBlocks(a).FirstOrDefault(b => b.Class == "CMidUnit"
            && string.Equals(b.ObjId, unitObjId, StringComparison.OrdinalIgnoreCase));
        if (u == null) return a;
        int lv = Find(a, Ascii("LEVEL"), u.Start, u.End);
        if (lv < 0) return a;
        int cntVal = lv + 5 + 4 + 10;
        int cur = BitConverter.ToInt32(a, cntVal);
        int insAt = cntVal + 4;
        var ins = new List<byte>();
        foreach (var m in mods)
        {
            ins.AddRange(Ascii("MODIF_ID"));
            ins.AddRange(BitConverter.GetBytes(m.Length + 1));
            ins.AddRange(Ascii(m));
            ins.Add(0);
        }
        var outp = new byte[a.Length + ins.Count];
        Array.Copy(a, 0, outp, 0, insAt);
        BitConverter.GetBytes(cur + mods.Length).CopyTo(outp, cntVal);
        ins.CopyTo(outp, insAt);
        Array.Copy(a, insAt, outp, insAt + ins.Count, a.Length - insAt);
        return outp;
    }

    // Cyrillic hero names in Windows-1251 (the game renders the unit's NAME_TXT in cp1251).
    private static readonly byte[] NameGatherer = { 0xD1, 0xE1, 0xEE, 0xF0, 0xF9, 0xE8, 0xEA };  // "Сборщик"
    private static readonly byte[] NameCastle   = { 0xF5, 0xEE, 0xE4, 0xE8, 0x20, 0xE4, 0xF0, 0xF3, 0xE3, 0xE8, 0xEC, 0x20, 0xE3, 0xE5, 0xF0, 0xEE, 0xE5, 0xEC };  // "ходи другим героем"

    // Set a unit's NAME_TXT to raw bytes (re-locates the unit by OBJ_ID; reallocs, so call after the in-place pass).
    private static byte[] SetUnitName(byte[] a, string unitObjId, byte[] name)
    {
        Block u = IndexBlocks(a).FirstOrDefault(b => b.Class == "CMidUnit"
            && string.Equals(b.ObjId, unitObjId, StringComparison.OrdinalIgnoreCase));
        return u == null ? a : ReplaceLenBytes(a, u.Start, u.End, "NAME_TXT", name);
    }

    // Replace a length-string value with raw bytes (variable length -> reallocs). Like ReplaceLenStr but byte[].
    private static byte[] ReplaceLenBytes(byte[] a, int start, int end, string tag, byte[] val)
    {
        int p = Find(a, Ascii(tag), start, end);
        if (p < 0) return a;
        int lp = p + tag.Length, vp = lp + 4;
        int tail = vp + (BitConverter.ToInt32(a, lp) - 1) + 1;
        var outp = new byte[a.Length - tail + vp + val.Length + 1];
        Array.Copy(a, 0, outp, 0, lp);
        BitConverter.GetBytes(val.Length + 1).CopyTo(outp, lp);
        Array.Copy(val, 0, outp, vp, val.Length);
        outp[vp + val.Length] = 0;
        Array.Copy(a, tail, outp, vp + val.Length + 1, a.Length - tail);
        return outp;
    }

    // 6-char "default" id field: <tag> + 8 bytes + 6 ascii chars.
    private static string ReadDefStr(byte[] a, Block b, string tag)
    {
        int p = Find(a, Ascii(tag), b.Start, b.End);
        return p < 0 ? null : Str(a, p + tag.Length + 8, 6).TrimEnd('\0', ' ');
    }

    private static List<string> ReadAllDefStr(byte[] a, Block b, string tag)
    {
        var res = new List<string>();
        byte[] t = Ascii(tag);
        int p = b.Start;
        while ((p = Find(a, t, p, b.End)) >= 0)
        {
            res.Add(Str(a, p + t.Length + 8, 6).TrimEnd('\0', ' '));
            p += t.Length;
        }
        return res;
    }

    // ---- bytes ----------------------------------------------------------------
    private static byte[] Ascii(string s) => Encoding.ASCII.GetBytes(s);
    private static string Str(byte[] a, int off, int len) => Encoding.ASCII.GetString(a, off, len);

    private static int Find(byte[] hay, byte[] needle, int start, int end = -1)
    {
        if (end < 0 || end > hay.Length) end = hay.Length;
        int last = end - needle.Length;
        for (int i = Math.Max(0, start); i <= last; i++)
        {
            int j = 0;
            while (j < needle.Length && hay[i + j] == needle[j]) j++;
            if (j == needle.Length) return i;
        }
        return -1;
    }

    private static byte[] Resource(string name)
    {
        using var rs = Assembly.GetExecutingAssembly().GetManifestResourceStream(name);
        var buf = new byte[rs.Length];
        rs.ReadExactly(buf);
        return buf;
    }

    // ---- minimal dBASE III reader (C/N/L fields, ascii values) -----------------
    private sealed class Dbf
    {
        private readonly byte[] d;
        private readonly int recCount, headerLen, recLen;
        private readonly Dictionary<string, (int off, int len)> fields = new(StringComparer.OrdinalIgnoreCase);

        public Dbf(string path)
        {
            d = File.ReadAllBytes(path);
            recCount  = BitConverter.ToInt32(d, 4);
            headerLen = BitConverter.ToUInt16(d, 8);
            recLen    = BitConverter.ToUInt16(d, 10);
            int pos = 32, off = 1;                                // each record starts with a 1-byte delete flag
            while (pos < d.Length && d[pos] != 0x0D)
            {
                string name = Encoding.ASCII.GetString(d, pos, 11);
                int z = name.IndexOf('\0'); if (z >= 0) name = name[..z];
                fields[name] = (off, d[pos + 16]);
                off += d[pos + 16]; pos += 32;
            }
        }

        public IEnumerable<int> Rows()
        {
            for (int i = 0; i < recCount; i++)
            {
                int rp = headerLen + i * recLen;
                if (rp + recLen <= d.Length && d[rp] != 0x2A) yield return rp;   // skip deleted (0x2A)
            }
        }

        private string Raw(int rp, string col) =>
            fields.TryGetValue(col, out var f) ? Encoding.ASCII.GetString(d, rp + f.off, f.len) : "";
        public string Str(int rp, string col) => Raw(rp, col).Trim();
        public int Int(int rp, string col) => int.TryParse(Raw(rp, col).Trim(), out int v) ? v : 0;
        public bool Bool(int rp, string col) { var c = Raw(rp, col).Trim(); return c is "T" or "t" or "Y" or "y" or "1"; }
    }
}
