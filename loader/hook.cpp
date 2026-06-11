// ============================================================================
//  berkutx — RSG-generator hijack (mss32.dll + Discipl2.exe, x86).
//  When the host picks the fake template "Roulette Arena", the mod's random
//  scenario plays our authored arena instead of an rsg map. See docs/how-it-works.md.
// ============================================================================
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include "template_lua.h"   // TEMPLATE_LUA[] / TEMPLATE_LUA_LEN — the fake-template stub (generated from template.lua)

// mss32 sub_10267FD0 = rsg .sg serializer; the ONLY thing we gate on. __thiscall(this, wchar_t* path).
static const uint32_t RVA_SAVE = 0x00267FD0;                            // 0x10267FD0 - mss32 base 0x10000000
static const unsigned char SIG[5] = { 0x55, 0x8B, 0xEC, 0x6A, 0xFF };   // push ebp; mov ebp,esp; push -1
static const char* MARKER = "Roulette Arena";                          // our template name, embedded in the saved scenario desc

// Discipl2 builders (base 0x400000). sub_433B0B = CMidServerBuilderFull(this@ecx, char* Source@[esp+4], …)
// — the single file-build chokepoint for the played map; we rewrite Source to our rolled arena.
// sub_4036D0 / sub_403798 are log-only. (sub_403617 in-memory build is dead for hotseat-random.)
static const uint32_t RVA_4036D0 = 0x000036D0;
static const uint32_t RVA_403798 = 0x00003798;
static const uint32_t RVA_433B0B = 0x00033B0B;

// Resolved at load by init_paths(): berkutx_rng.exe sits next to this DLL; Exports\ / Templates\ hang off the game exe.
static wchar_t g_rngExe[MAX_PATH]       = {0};
static wchar_t g_templatesLua[MAX_PATH] = {0};
static wchar_t g_exportsDir[MAX_PATH]   = {0};
static char    OUR_PATH_A[MAX_PATH]     = {0};   // current roll's seed file: ADDRESS baked into the sub_433B0B stub, CONTENT set per roll
static wchar_t g_prev_arena_w[MAX_PATH] = {0};   // previous roll's file — deleted before the next
static volatile unsigned char g_ourArena = 0;    // armed by the save-hook when our template's marker is seen
static unsigned char* g_tramp_36D0 = 0;
static unsigned char* g_tramp_3798 = 0;

static unsigned char* g_tramp_433b0b = 0;
static const char LBL_36D0[] = "sub_4036D0(path-build)";
static const char LBL_3798[] = "sub_403798(path-build)";
static wchar_t g_logPath[MAX_PATH] = {0};   // <gameDir>\berkutx_roulette.log (init_paths)

// mss32 sub_101D3CB0(menu@ecx, char a2): host commit; copies the lobby race vector (menu+68..72,
// std::vector<int>) into scenInfo. We overwrite it with our arena's races. __fastcall, prologue 55 8B EC 6A FF.
static const uint32_t RVA_3CB0 = 0x001D3CB0;   // 0x101D3CB0 - mss32 base
typedef int (__attribute__((fastcall)) *fn_3cb0)(int a1, int a2);
static fn_3cb0        g_orig_3cb0 = 0;
static unsigned char* g_tramp_3cb0 = 0;

// mss32 sub_101D21C0(menu@ecx): generation-complete timer cb. On status(menu+276)==3 it builds the
// DLG_GENERATION_RESULT dialog (sub_101AFDD0). For our template we skip the dialog: replay its pre-dialog
// bookkeeping (#1 stop poll-timer (__thiscall)dword_103B0DE0[4*sel](menu+12); #2 if(menu+40) join
// sub_1005BAC0(menu+36)) then proceed via sub_101D20D0(menu). Our template name is the gen-descriptor
// std::string at menu+80 (len @+96, cap @+100; MSVC SSO).
static const uint32_t RVA_21C0  = 0x001D21C0;  // generation-complete cb (prologue 55 8B EC 6A FF)
static const uint32_t RVA_20D0  = 0x001D20D0;  // proceed-to-lobby, __cdecl(menu)
static const uint32_t RVA_5BAC0 = 0x0005BAC0;  // std::thread::join, __thiscall
static const uint32_t RVA_T_103B0DE0 = 0x003B0DE0; // timer-manager dispatch table (stop = [4*sel])
static const uint32_t RVA_SEL_103B1138 = 0x003B1138; // UI-manager selector
typedef void (__cdecl *fn_20d0)(int menu);
static fn_20d0        g_sub_20d0   = 0;         // resolved sub_101D20D0 (proceed-to-lobby)
static unsigned char* g_tramp_21c0 = 0;         // trampoline -> original sub_101D21C0
static void*          g_handle_ptr = 0;         // = &handle_21c0 (stub's indirect call target)
static HMODULE        g_mss32      = 0;         // cached mss32 base (for runtime dispatch in handle_21c0)

// ---- tiny append logger (visibility for the prototype) ---------------------
static void logf(const char* fmt, ...)
{
    char buf[1024];
    va_list ap; va_start(ap, fmt);
    int n = _vsnprintf(buf, sizeof(buf) - 2, fmt, ap);
    va_end(ap);
    if (n < 0) n = (int)sizeof(buf) - 2;
    buf[n++] = '\n';
    HANDLE h = CreateFileW(g_logPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD wr = 0; SetFilePointer(h, 0, NULL, FILE_END);
        WriteFile(h, buf, (DWORD)n, &wr, NULL);
        CloseHandle(h);
    }
}

// ---- hook state ------------------------------------------------------------
typedef void (__attribute__((fastcall)) *save_fn)(void* thisp, void* edx, const void* pathWStr);
static save_fn        g_orig  = NULL;   // -> trampoline (original prologue + jmp back)
static unsigned char* g_tramp = NULL;

// The save target is passed as a std::wstring* (MSVC layout), NOT a raw wchar_t*.
// sub_10267FD0 does: cap = *(uint32_t*)(a2+20); chars = (cap < 8) ? (wchar_t*)a2 : *(wchar_t**)a2;
// (16-byte SSO buffer union at +0, _Mysize at +16, _Myres/capacity at +20.)
static const wchar_t* wstr_data(const void* ws)
{
    if (!ws) return NULL;
    const unsigned char* p = (const unsigned char*)ws;
    uint32_t cap = *(const uint32_t*)(p + 20);
    return (cap < 8) ? (const wchar_t*)p : *(const wchar_t* const*)p;
}

// ---- helpers ---------------------------------------------------------------
static bool file_contains(const wchar_t* path, const char* needle)
{
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD size = GetFileSize(h, NULL);
    bool found = false;
    if (size != INVALID_FILE_SIZE && size > 0 && size < 8u * 1024u * 1024u) {
        char* buf = (char*)HeapAlloc(GetProcessHeap(), 0, size);
        if (buf) {
            DWORD got = 0;
            if (ReadFile(h, buf, size, &got, NULL) && got) {
                size_t nl = strlen(needle);
                if (nl && got >= nl) {
                    for (size_t i = 0; i + nl <= got && !found; ++i)
                        if (memcmp(buf + i, needle, nl) == 0) found = true;
                }
            }
            HeapFree(GetProcessHeap(), 0, buf);
        }
    }
    CloseHandle(h);
    return found;
}

// Roll a fresh arena into `outPath` via berkutx_rng.exe <seed> <out.sg> (the template is embedded in the exe).
static bool run_roulette(unsigned seed, const wchar_t* outPath)
{
    wchar_t cmd[2048];
    _snwprintf(cmd, 2048, L"\"%s\" %u \"%s\"", g_rngExe, seed, outPath);
    cmd[2047] = 0;
    STARTUPINFOW si; ZeroMemory(&si, sizeof(si)); si.cb = sizeof(si);
    PROCESS_INFORMATION pi; ZeroMemory(&pi, sizeof(pi));
    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        logf("[hk] run_roulette: CreateProcessW FAILED err=%lu", GetLastError());
        return false;
    }
    DWORD w = WaitForSingleObject(pi.hProcess, 60000);
    DWORD ec = (DWORD)-1; GetExitCodeProcess(pi.hProcess, &ec);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    HANDLE hf = CreateFileW(outPath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    DWORD sz = (hf != INVALID_HANDLE_VALUE) ? GetFileSize(hf, NULL) : 0xFFFFFFFF;
    if (hf != INVALID_HANDLE_VALUE) CloseHandle(hf);
    logf("[hk] run_roulette: wait=%lu exit=%lu outSize=%lu", w, ec, sz);   // exit!=0 => roll FAILED, file still rsg
    return (w == WAIT_OBJECT_0 && ec == 0);
}

// ---- the detour ------------------------------------------------------------
static void __attribute__((fastcall)) hk_save(void* thisp, void* edx, const void* pathWStr)
{
    g_orig(thisp, edx, pathWStr);   // normal rsg .sg save (arg = std::wstring*)
    // (no SEH wrapper: mingw g++ lacks MSVC __try/__except; all calls below are
    //  null/size-checked WinAPI, so they won't fault on normal input.)
    const wchar_t* path = wstr_data(pathWStr);     // extract chars from the std::wstring
    bool mark = path && file_contains(path, MARKER);   // our template's marker in the just-saved rsg scenario
    logf("[hk] save fired path='%ls' (ourTemplate=%d)", path ? path : L"(null)", (int)mark);
    // Arm/disarm the game-side hijack: only when THIS host-start used our template.
    g_ourArena = mark ? 1 : 0;
    if (mark) {
        // Roll a FRESH SEED-NAMED arena into OUR OWN file (the game's rsg "Random scenario.sg" is
        // left untouched; sub_433B0B then redirects the played-map build to this file). The unique
        // per-roll name keeps the game's companion cache fresh.
        unsigned seed = GetTickCount();
        wchar_t outW[MAX_PATH]; char outA[MAX_PATH];
        _snwprintf(outW, MAX_PATH, L"%sRoulette Arena %u.sg", g_exportsDir, seed); outW[MAX_PATH - 1] = 0;
        WideCharToMultiByte(CP_ACP, 0, outW, -1, outA, MAX_PATH, NULL, NULL);
        if (g_prev_arena_w[0]) DeleteFileW(g_prev_arena_w);   // drop the previous roll (best-effort)
        bool ok = run_roulette(seed, outW);
        if (ok) {
            lstrcpynA(OUR_PATH_A, outA, MAX_PATH);        // sub_433B0B redirect target (content; its address is baked into the stub)
            lstrcpynW(g_prev_arena_w, outW, MAX_PATH);
        }
        logf("[hk] %s (seed=%u) -> %ls", ok ? "rolled arena; g_ourArena armed"
                                            : "berkutx_rng FAILED (arena NOT rolled)", seed, outW);
    }
}

// ---- install ---------------------------------------------------------------
static bool install_hook()
{
    HMODULE m = GetModuleHandleA("mss32.dll");
    if (!m) return false;
    unsigned char* target = (unsigned char*)m + RVA_SAVE;

    if (memcmp(target, SIG, 5) != 0) {                // build guard: refuse unknown prologue
        logf("[hk] SIGNATURE MISMATCH at mss32+0x%X (got %02X %02X %02X %02X %02X) — not patching",
             RVA_SAVE, target[0], target[1], target[2], target[3], target[4]);
        return false;
    }

    g_tramp = (unsigned char*)VirtualAlloc(NULL, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!g_tramp) return false;
    memcpy(g_tramp, target, 5);                       // stolen prologue (55 8B EC 6A FF)
    g_tramp[5] = 0xE9;                                // jmp back to target+5
    *(int32_t*)(g_tramp + 6) = (int32_t)((target + 5) - (g_tramp + 10));
    g_orig = (save_fn)g_tramp;

    DWORD oldp = 0;
    if (!VirtualProtect(target, 5, PAGE_EXECUTE_READWRITE, &oldp)) return false;
    target[0] = 0xE9;                                 // jmp hk_save
    *(int32_t*)(target + 1) = (int32_t)((unsigned char*)&hk_save - (target + 5));
    VirtualProtect(target, 5, oldp, &oldp);
    FlushInstructionCache(GetCurrentProcess(), target, 5);
    logf("[hk] installed: mss32=%p target=%p tramp=%p detour=%p", m, target, g_tramp, (void*)&hk_save);
    return true;
}

// log-only: report a path-based server-build call + its arg0 (the scenario path it loads)
static void __cdecl on_path_hook(const char* label, const char* path)
{
    if (path && !IsBadReadPtr(path, 4))
        logf("[hk] %s FIRED arg0(path)='%.255s'", label, path);
    else
        logf("[hk] %s FIRED arg0=%p (unreadable)", label, (const void*)path);
}

// sub_433B0B (the played-map builder) reports the EXACT .sg the host map is built from.
static void __cdecl on_433b0b(const char* src, void* ret)
{
    if (src && !IsBadReadPtr(src, 4))
        logf("[hk] sub_433B0B(map-build) Source='%.255s' ret=%p ourArena=%u%s",
             src, ret, (unsigned)g_ourArena, g_ourArena ? " -> REWRITE to our arena" : "");
    else
        logf("[hk] sub_433B0B(map-build) Source=%p (unreadable) ret=%p", (const void*)src, ret);
}

// THE REAL FIX. Hook sub_433B0B (CMidServerBuilderFull path-build). Stub logs the Source path
// (ground truth for which .sg the played map is built from), then — when g_ourArena is set —
// rewrites the Source arg ([esp+4]) to OUR arena so the game parses OUR file as the played map.
// Register/stack-transparent: pushad/log/popad, then conditional [esp+4] rewrite, then trampoline.
static bool install_433b0b()
{
    HMODULE exe = GetModuleHandleW(NULL);
    if (!exe) return false;
    unsigned char* t = (unsigned char*)exe + RVA_433B0B;
    if (t[0] != 0xB8) { logf("[hk] 433B0B sig mismatch (%02X) — not patching", t[0]); return false; }
    // trampoline: stolen 5 bytes (mov eax,imm32 of the EH-prolog) + jmp to t+5 (the call _EH_prolog)
    g_tramp_433b0b = (unsigned char*)VirtualAlloc(NULL, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!g_tramp_433b0b) return false;
    memcpy(g_tramp_433b0b, t, 5);
    g_tramp_433b0b[5] = 0xE9;
    *(int32_t*)(g_tramp_433b0b + 6) = (int32_t)((t + 5) - (g_tramp_433b0b + 10));

    unsigned char* stub = (unsigned char*)VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!stub) return false;
    int o = 0;
    stub[o++] = 0x60;                                                                    // pushad (esp-=0x20)
    stub[o++] = 0xFF; stub[o++] = 0x74; stub[o++] = 0x24; stub[o++] = 0x20;              // push [esp+0x20]  (orig retaddr)
    stub[o++] = 0xFF; stub[o++] = 0x74; stub[o++] = 0x24; stub[o++] = 0x28;              // push [esp+0x28]  (orig Source = [esp+4])
    stub[o++] = 0xB8; *(uint32_t*)(stub + o) = (uint32_t)(uintptr_t)&on_433b0b; o += 4;  // mov eax, on_433b0b
    stub[o++] = 0xFF; stub[o++] = 0xD0;                                                  // call eax  -> on_433b0b(Source, retaddr)
    stub[o++] = 0x83; stub[o++] = 0xC4; stub[o++] = 0x08;                                // add esp, 8 (2 cdecl args)
    stub[o++] = 0x61;                                                                    // popad
    stub[o++] = 0x80; stub[o++] = 0x3D; *(uint32_t*)(stub + o) = (uint32_t)(uintptr_t)&g_ourArena; o += 4; stub[o++] = 0x00; // cmp byte[g_ourArena],0
    stub[o++] = 0x74; int je = o++;                                                      // je .jmp (skip rewrite)
    stub[o++] = 0xC7; stub[o++] = 0x44; stub[o++] = 0x24; stub[o++] = 0x04; *(uint32_t*)(stub + o) = (uint32_t)(uintptr_t)OUR_PATH_A; o += 4; // mov [esp+4], OUR_PATH_A
    int jmp = o;
    stub[o++] = 0xFF; stub[o++] = 0x25; *(uint32_t*)(stub + o) = (uint32_t)(uintptr_t)&g_tramp_433b0b; o += 4; // jmp [g_tramp_433b0b]
    stub[je] = (unsigned char)(jmp - (je + 1));

    DWORD op = 0;
    if (!VirtualProtect(t, 5, PAGE_EXECUTE_READWRITE, &op)) return false;
    t[0] = 0xE9; *(int32_t*)(t + 1) = (int32_t)(stub - (t + 5));
    VirtualProtect(t, 5, op, &op);
    FlushInstructionCache(GetCurrentProcess(), t, 5);
    logf("[hk] 433B0B map-build hook installed: t=%p tramp=%p stub=%p redirect='%s'", t, g_tramp_433b0b, stub, OUR_PATH_A);
    return true;
}

// log-only hook on a __stdcall(char* path, ...) function: logs arg0 then passes through.
static bool install_log_hook(uint32_t rva, unsigned char** trampOut, const char* label)
{
    HMODULE exe = GetModuleHandleW(NULL);
    if (!exe) return false;
    unsigned char* t = (unsigned char*)exe + rva;
    if (t[0] != 0xB8) { logf("[hk] log-hook @0x%X sig mismatch (%02X)", rva, t[0]); return false; }
    unsigned char* tr = (unsigned char*)VirtualAlloc(NULL, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tr) return false;
    memcpy(tr, t, 5); tr[5] = 0xE9; *(int32_t*)(tr + 6) = (int32_t)((t + 5) - (tr + 10));
    *trampOut = tr;
    unsigned char* stub = (unsigned char*)VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!stub) return false;
    int o = 0;
    stub[o++] = 0x60;                                                                    // pushad
    stub[o++] = 0xFF; stub[o++] = 0x74; stub[o++] = 0x24; stub[o++] = 0x24;              // push [esp+0x24]  (arg0/path)
    stub[o++] = 0x68; *(uint32_t*)(stub + o) = (uint32_t)(uintptr_t)label; o += 4;       // push label
    stub[o++] = 0xB8; *(uint32_t*)(stub + o) = (uint32_t)(uintptr_t)&on_path_hook; o += 4; // mov eax, on_path_hook
    stub[o++] = 0xFF; stub[o++] = 0xD0;                                                  // call eax
    stub[o++] = 0x83; stub[o++] = 0xC4; stub[o++] = 0x08;                                // add esp, 8 (cdecl, 2 args)
    stub[o++] = 0x61;                                                                    // popad
    stub[o++] = 0xFF; stub[o++] = 0x25; *(uint32_t*)(stub + o) = (uint32_t)(uintptr_t)trampOut; o += 4; // jmp [*trampOut]
    DWORD op = 0;
    if (!VirtualProtect(t, 5, PAGE_EXECUTE_READWRITE, &op)) return false;
    t[0] = 0xE9; *(int32_t*)(t + 1) = (int32_t)(stub - (t + 5));
    VirtualProtect(t, 5, op, &op);
    FlushInstructionCache(GetCurrentProcess(), t, 5);
    logf("[hk] log-hook installed @0x%X (%s) stub=%p", rva, label, stub);
    return true;
}

// log the lobby RACE VECTOR (menu+68..+72) that sub_101D3CB0 copies into scenInfo, then pass through.
static bool        str_eq(const char* a, const char* b);   // fwd (defined in DIALOG EXCLUSION section)
static const char* tmpl_name(int menu);                     // fwd

// Our arena's two HUMAN players' race ids (Empire=0, Elf=5; neutral=4 is map-side, not a lobby slot).
static const int ARENA_RACES[2] = { 0, 5 };

static int __attribute__((fastcall)) hk_3cb0(int a1, int a2)
{
    int* begin = 0; int* end = 0;
    if (a1) { begin = *(int**)(a1 + 68); end = *(int**)(a1 + 72); }
    int cnt = (begin && end && end >= begin) ? (int)(end - begin) : -1;
    const char* nm = a1 ? tmpl_name(a1) : 0;
    bool ours = nm && str_eq(nm, MARKER);
    char b[256]; int o = 0;
    o += _snprintf(b + o, sizeof(b) - o, "[hk] sub_101D3CB0 menu=%p tmpl='%s' race-vec(+68) count=%d:", (void*)a1, nm ? nm : "(null)", cnt);
    for (int i = 0; i < cnt && i < 12 && o < 200; i++) o += _snprintf(b + o, sizeof(b) - o, " %d", begin[i]);
    b[sizeof(b) - 1] = 0;
    logf("%s", b);
    // For OUR arena, overwrite the rsg-generated race vector with our arena's actual human races
    // BEFORE the original copies it into the game scenInfo. The rsg races (random, e.g. [3,1]) don't
    // exist in our loaded map (Empire+Elf), so host player/race resolution stalls -> the load HANG
    // and the "wrong lobby races". With [0,5] the scenInfo matches the map.
    if (ours && begin && cnt >= 2) {
        begin[0] = ARENA_RACES[0];   // Empire
        begin[1] = ARENA_RACES[1];   // Elf
        logf("[hk] sub_101D3CB0 OUR arena -> race-vec overridden to [%d=Empire,%d=Elf]", ARENA_RACES[0], ARENA_RACES[1]);
    }
    return g_orig_3cb0(a1, a2);
}

static bool install_3cb0()
{
    HMODULE m = GetModuleHandleA("mss32.dll");
    if (!m) return false;
    unsigned char* t = (unsigned char*)m + RVA_3CB0;
    if (memcmp(t, SIG, 5) != 0) { logf("[hk] 3cb0 sig mismatch %02X %02X %02X %02X %02X", t[0], t[1], t[2], t[3], t[4]); return false; }
    g_tramp_3cb0 = (unsigned char*)VirtualAlloc(NULL, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!g_tramp_3cb0) return false;
    memcpy(g_tramp_3cb0, t, 5);
    g_tramp_3cb0[5] = 0xE9;
    *(int32_t*)(g_tramp_3cb0 + 6) = (int32_t)((t + 5) - (g_tramp_3cb0 + 10));
    g_orig_3cb0 = (fn_3cb0)g_tramp_3cb0;
    DWORD op = 0;
    if (!VirtualProtect(t, 5, PAGE_EXECUTE_READWRITE, &op)) return false;
    t[0] = 0xE9; *(int32_t*)(t + 1) = (int32_t)((unsigned char*)&hk_3cb0 - (t + 5));
    VirtualProtect(t, 5, op, &op);
    FlushInstructionCache(GetCurrentProcess(), t, 5);
    logf("[hk] 3cb0 log-hook installed: mss32=%p t=%p tramp=%p detour=%p", m, t, g_tramp_3cb0, (void*)&hk_3cb0);
    return true;
}

// ---- DIALOG EXCLUSION (sub_101D21C0) --------------------------------------
static bool str_eq(const char* a, const char* b)
{
    while (*a && *b) { if (*a != *b) return false; ++a; ++b; }
    return *a == *b;
}

// read the gen-descriptor template name (std::string @menu+80, MSVC SSO)
static const char* tmpl_name(int menu)
{
    int cap = *(int*)(menu + 100);
    return (cap >= 16) ? *(const char**)(menu + 80) : (const char*)(menu + 80);
}

// Called by the detour stub when status(menu+276)==3. Returns 1 if WE handled the tick (our
// template -> dialog excluded, proceeded to lobby) so the stub returns without running the
// original; returns 0 to let the original run normally (other templates / no result).
// We replicate ONLY the original's pre-dialog bookkeeping, then go straight to the lobby —
// the dialog builder sub_101AFDD0 is never called.
extern "C" int __cdecl handle_21c0(int menu)
{
    if (!*(volatile int*)(menu + 232)) return 0;        // no rsg result -> let original show "BUG!" path
    const char* nm = tmpl_name(menu);
    if (!(nm && str_eq(nm, MARKER))) {
        logf("[hk] sub_101D21C0 status=3 tmpl='%s' -> not ours, original dialog", nm ? nm : "(null)");
        return 0;                                       // other template -> normal dialog
    }
    // OUR template + generation success: exclude DLG_GENERATION_RESULT entirely.
    // #1 stop the 50ms poll-timer at menu+12 : ((__thiscall)dword_103B0DE0[4*sel])(menu+12)
    int sel = *(int*)((char*)g_mss32 + RVA_SEL_103B1138);
    void* stopTimer = *(void**)((char*)g_mss32 + RVA_T_103B0DE0 + 16 * sel);
    ((void(__attribute__((fastcall)) *)(int, int))stopTimer)(menu + 12, 0);
    // #2 join the gen worker thread (guarded on _Id @menu+40, like the original)
    if (*(int*)(menu + 40))
        ((void(__attribute__((fastcall)) *)(int, int))((char*)g_mss32 + RVA_5BAC0))(menu + 36, 0);
    logf("[hk] sub_101D21C0 status=3 tmpl='%s' -> DIALOG EXCLUDED (sub_101AFDD0 not called), proceed to lobby", nm);
    // proceed: sub_101D20D0 closes the wait-dialog (sub_101D1200 @menu+272) and runs menu+280 -> lobby
    if (g_sub_20d0) g_sub_20d0(menu);
    return 1;
}

// Install the dialog-exclusion detour. Stub (entered with ecx=menu, st0=a2 from the 5-byte JMP):
//   mov eax,[ecx+276]; cmp eax,3; jne .orig          ; only the generation-success tick
//   push ecx; push ecx; call [handle_21c0]; add esp,4; pop ecx; test eax,eax; je .orig
//   ret                                              ; handled -> dialog excluded, return
//  .orig: jmp [tramp]                                ; not status3 OR not ours -> original
// st0 is never touched before a passthrough, and the only status reaching the original via
// the je is 3 (whose code path doesn't use a2@st0), so the FPU arg stays correct.
static bool install_21c0()
{
    HMODULE m = GetModuleHandleA("mss32.dll");
    if (!m) return false;
    unsigned char* t = (unsigned char*)m + RVA_21C0;
    if (memcmp(t, SIG, 5) != 0) { logf("[hk] 21c0 sig mismatch %02X %02X %02X %02X %02X", t[0], t[1], t[2], t[3], t[4]); return false; }
    g_mss32      = m;
    g_sub_20d0   = (fn_20d0)((unsigned char*)m + RVA_20D0);
    g_handle_ptr = (void*)&handle_21c0;

    g_tramp_21c0 = (unsigned char*)VirtualAlloc(NULL, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!g_tramp_21c0) return false;
    memcpy(g_tramp_21c0, t, 5);
    g_tramp_21c0[5] = 0xE9;
    *(int32_t*)(g_tramp_21c0 + 6) = (int32_t)((t + 5) - (g_tramp_21c0 + 10));

    unsigned char* stub = (unsigned char*)VirtualAlloc(NULL, 48, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!stub) return false;
    int o = 0;
    stub[o++] = 0x8B; stub[o++] = 0x81; *(uint32_t*)(stub + o) = 276; o += 4;                          // mov eax,[ecx+276] (status)
    stub[o++] = 0x83; stub[o++] = 0xF8; stub[o++] = 0x03;                                              // cmp eax,3
    stub[o++] = 0x75; int jne_orig = o++;                                                              // jne .orig (rel8, patched)
    stub[o++] = 0x51;                                                                                  // push ecx (save menu)
    stub[o++] = 0x51;                                                                                  // push ecx (arg)
    stub[o++] = 0xFF; stub[o++] = 0x15; *(uint32_t*)(stub + o) = (uint32_t)(uintptr_t)&g_handle_ptr; o += 4; // call [handle_21c0]
    stub[o++] = 0x83; stub[o++] = 0xC4; stub[o++] = 0x04;                                              // add esp,4
    stub[o++] = 0x59;                                                                                  // pop ecx (restore menu; eax preserved)
    stub[o++] = 0x85; stub[o++] = 0xC0;                                                                // test eax,eax
    stub[o++] = 0x74; int je_orig = o++;                                                               // je .orig (rel8, patched)
    stub[o++] = 0xC3;                                                                                  // ret (handled -> dialog excluded)
    int orig = o;
    stub[o++] = 0xFF; stub[o++] = 0x25; *(uint32_t*)(stub + o) = (uint32_t)(uintptr_t)&g_tramp_21c0; o += 4; // jmp [g_tramp_21c0]
    stub[jne_orig] = (unsigned char)(orig - (jne_orig + 1));
    stub[je_orig]  = (unsigned char)(orig - (je_orig + 1));

    DWORD op = 0;
    if (!VirtualProtect(t, 5, PAGE_EXECUTE_READWRITE, &op)) return false;
    t[0] = 0xE9; *(int32_t*)(t + 1) = (int32_t)(stub - (t + 5));
    VirtualProtect(t, 5, op, &op);
    FlushInstructionCache(GetCurrentProcess(), t, 5);
    logf("[hk] 21c0 dialog-exclude hook installed: t=%p tramp=%p stub=%p proceed=%p sel=%d", t, g_tramp_21c0, stub, (void*)g_sub_20d0, *(int*)((char*)m + RVA_SEL_103B1138));
    return true;
}

// directory of a loaded module (with trailing backslash); module NULL -> the game exe's dir.
static void dir_of(HMODULE m, wchar_t* out)
{
    if (!GetModuleFileNameW(m, out, MAX_PATH)) { out[0] = 0; return; }
    wchar_t* slash = wcsrchr(out, L'\\');
    if (slash) slash[1] = 0; else out[0] = 0;
}

// Resolve every path from where the DLL and the game actually live — nothing hardcoded.
static void init_paths(HMODULE self)
{
    wchar_t hookDir[MAX_PATH], gameDir[MAX_PATH];
    dir_of(self, hookDir);   // berkutx_rng.exe sits next to this DLL
    dir_of(NULL, gameDir);   // Exports\ / Templates\ hang off the game exe
    _snwprintf(g_rngExe,       MAX_PATH, L"%sberkutx_rng.exe", hookDir);
    _snwprintf(g_exportsDir,   MAX_PATH, L"%sExports\\", gameDir);
    _snwprintf(g_templatesLua, MAX_PATH, L"%sTemplates\\Roulette Arena.lua", gameDir);
    _snwprintf(g_logPath,      MAX_PATH, L"%sberkutx_roulette.log", gameDir);
}

// Write the fake-template stub (embedded in this DLL) into the game's Templates\ so "Roulette Arena"
// appears in the random-scenario list — no separate file is shipped, no manual install step.
static void ensure_stub_template()
{
    HANDLE h = CreateFileW(g_templatesLua, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) { logf("[hk] template write FAILED: %ls", g_templatesLua); return; }
    DWORD wr = 0; WriteFile(h, TEMPLATE_LUA, TEMPLATE_LUA_LEN, &wr, NULL);
    CloseHandle(h);
    logf("[hk] template ensured: %ls (%lu b)", g_templatesLua, wr);
}

static DWORD WINAPI worker(LPVOID)
{
    ensure_stub_template();   // fake template must exist before the game scans Templates\
    // mss32.dll is already loaded when we inject into the running game; retry a
    // few times just in case of timing.
    bool mss = false, l1 = false, l2 = false, l3 = false, skp = false, bld = false;
    for (int i = 0; i < 50 && !(mss && l1 && l2 && l3 && skp && bld); ++i) {
        if (!mss)  mss  = install_hook();
        if (!l1)   l1   = install_log_hook(RVA_4036D0, &g_tramp_36D0, LBL_36D0);
        if (!l2)   l2   = install_log_hook(RVA_403798, &g_tramp_3798, LBL_3798);
        if (!l3)   l3   = install_3cb0();
        if (!skp)  skp  = install_21c0();
        if (!bld)  bld  = install_433b0b();
        if (mss && l1 && l2 && l3 && skp && bld) break;
        Sleep(100);
    }
    logf("[hk] worker done: mss32=%d log4036D0=%d log403798=%d log3cb0=%d skip21c0=%d build433B0B=%d", (int)mss, (int)l1, (int)l2, (int)l3, (int)skp, (int)bld);
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hinst);
        init_paths(hinst);
        CreateThread(NULL, 0, worker, NULL, 0, NULL);
    }
    return TRUE;
}
