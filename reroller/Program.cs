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
        foreach (var a in args)
        {
            if (int.TryParse(a, out int s)) seed = s;
            else if (a.EndsWith(".sg", StringComparison.OrdinalIgnoreCase)) outPath = a;
            else if (Directory.Exists(a)) install = a;
        }
        outPath ??= Path.Combine(install, "Exports", "Roulette Arena.sg");
        var rng = new Random(seed);

        // --- pools (and per-unit level/HP) from the game's dBASE tables ---
        var soldiers = new List<string>();
        var leaders  = new List<string>();
        var items    = new List<string>();
        var stats    = new Dictionary<string, (int lvl, int hp)>(StringComparer.OrdinalIgnoreCase);
        var gunits = new Dbf(Path.Combine(install, "Globals", "Gunits.dbf"));
        foreach (int r in gunits.Rows())
        {
            string id = gunits.Str(r, "unit_id");
            if (id.Length != 0) stats[id] = (Math.Max(1, gunits.Int(r, "level")), gunits.Int(r, "hit_point"));
            if (gunits.Bool(r, "water_only")) continue;
            int cat = gunits.Int(r, "unit_cat");
            if (cat == 0 && gunits.Int(r, "leadership") <= 0 && !id.Equals(Gargoyle, StringComparison.OrdinalIgnoreCase)) soldiers.Add(id);
            if (cat == 2 && gunits.Str(r, "leader_cat").Length > 0) leaders.Add(id);
        }
        var gitem = new Dbf(Path.Combine(install, "Globals", "GItem.dbf"));
        foreach (int r in gitem.Rows())
        {
            int c = gitem.Int(r, "item_cat");
            if (c != 10 && c != 13 && c != 14) items.Add(gitem.Str(r, "item_id"));
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

        int nCamp = 0, nChest = 0, nGath = 0;
        foreach (var b in blocks)
        {
            switch (b.Class)
            {
                case "CMidSiteMercs":                                        // camp: recruit unit + its stats + look
                {
                    string uid = Pick(rng, soldiers);
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
                case "CMidStack":                                            // gatherer: field stack leader
                {
                    if (ReadDefStr(sg, b, "INSIDE") != "000000") break;
                    string lid = ReadDefStr(sg, b, "LEADER_ID");
                    if (lid == null || !byId.TryGetValue(lid, out var u) || u.Class != "CMidUnit") break;
                    string lt = Pick(rng, leaders);
                    if (PatchLenStr(sg, u, "TYPE", lt, out int valEnd))
                    {
                        PatchInt(sg, u, "LEVEL", 1, valEnd);                  // the LEVEL right after TYPE (not DYNLEVEL)
                        int hpFrom = Find(sg, Ascii("DYNLEVEL"), u.Start, u.End);   // HP sits just past DYNLEVEL
                        PatchInt(sg, u, "HP", stats.TryGetValue(lt, out var st) ? st.hp : 65, hpFrom < 0 ? valEnd : hpFrom);
                        nGath++;
                    }
                    break;
                }
            }
        }

        File.WriteAllBytes(outPath, sg);
        foreach (var ext in new[] { ".id0", ".id1", ".nam", ".til" })        // drop stale per-scenario caches
        { var f = outPath + ext; if (File.Exists(f)) File.Delete(f); }
        var lua = Path.ChangeExtension(outPath, ".lua"); if (File.Exists(lua)) File.Delete(lua);

        Console.WriteLine($"[rng] seed={seed} camps={nCamp}(pool {soldiers.Count}) chests={nChest}(pool {items.Count}) gatherers={nGath}(pool {leaders.Count})");
        Console.WriteLine("[rng] wrote " + outPath);
        return 0;
    }

    private static string Pick(Random r, List<string> pool) => pool[r.Next(pool.Count)].ToUpperInvariant();

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
