// ============================================================================
//  berkutx_loader — self-contained injector for the Disciples II battle-roulette.
//
//  hook.dll and berkutx_rng.exe are EMBEDDED in this exe (RCDATA resources) and
//  unpacked on launch, so only this single file is shipped / copied.
//
//  No args     : unpack the payload, launch Discipl2.exe (next to this exe), wait
//                for it to come up, and inject the hook. Turn-key mode.
//  <pid>       : inject into an already-running instance with that PID.
//  -a|--attach : inject into the first running Discipl2.exe (no launch).
//  <path.dll>  : use this hook DLL instead of the embedded one (skips unpack).
//  Tokens may be combined in any order. See loader/build.ps1.
//
//  Every run appends a trace to berkutx_loader.log next to this exe (or %TEMP%
//  if the install dir is read-only) so failures are visible even on a double-click.
// ============================================================================

#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>

static wchar_t g_logPath[MAX_PATH];

// Pick a writable log path: next to the exe, else %TEMP%.
static void init_log()
{
    GetModuleFileNameW(NULL, g_logPath, MAX_PATH);
    wchar_t* slash = wcsrchr(g_logPath, L'\\');
    if (slash) wcscpy(slash + 1, L"berkutx_loader.log"); else wcscpy(g_logPath, L"berkutx_loader.log");
    HANDLE h = CreateFileW(g_logPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) { CloseHandle(h); return; }
    wchar_t tmp[MAX_PATH];
    DWORD n = GetTempPathW(MAX_PATH, tmp);
    if (n && n < MAX_PATH) { _snwprintf(g_logPath, MAX_PATH, L"%sberkutx_loader.log", tmp); g_logPath[MAX_PATH - 1] = 0; }
}

// Append a line to the log (UTF-8, Unicode-safe) and echo to stdout.
static void LOG(const wchar_t* fmt, ...)
{
    wchar_t buf[2048];
    va_list ap; va_start(ap, fmt);
    _vsnwprintf(buf, 2047, fmt, ap);
    va_end(ap);
    buf[2047] = 0;
    fputws(buf, stdout);
    HANDLE h = CreateFileW(g_logPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    int n = WideCharToMultiByte(CP_UTF8, 0, buf, -1, NULL, 0, NULL, NULL);
    if (n > 1) {
        char* mb = (char*)malloc(n);
        if (mb) {
            WideCharToMultiByte(CP_UTF8, 0, buf, -1, mb, n, NULL, NULL);
            DWORD wr; WriteFile(h, mb, (DWORD)(n - 1), &wr, NULL);   // n-1: drop the trailing NUL
            free(mb);
        }
    }
    CloseHandle(h);
}

// ---- embedded payload (RCDATA: hook.dll + berkutx_rng.exe) ------------------
static bool write_resource(const wchar_t* resName, const wchar_t* outPath)
{
    HRSRC r = FindResourceW(NULL, resName, RT_RCDATA);
    if (!r) { LOG(L"[extract] embedded resource %ls not found\n", resName); return false; }
    DWORD sz = SizeofResource(NULL, r);
    HGLOBAL g = LoadResource(NULL, r);
    const void* p = g ? LockResource(g) : NULL;
    if (!p || !sz) { LOG(L"[extract] embedded resource %ls is empty\n", resName); return false; }
    HANDLE h = CreateFileW(outPath, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        if (GetFileAttributesW(outPath) != INVALID_FILE_ATTRIBUTES) {     // in use by a running game -> keep it
            LOG(L"[extract] %ls is in use (err %lu); keeping the existing copy\n", outPath, e);
            return true;
        }
        LOG(L"[extract] cannot create %ls: %lu\n", outPath, e);
        return false;
    }
    DWORD wr = 0; BOOL ok = WriteFile(h, p, sz, &wr, NULL);
    CloseHandle(h);
    if (!ok || wr != sz) { LOG(L"[extract] write failed %ls (%lu/%lu)\n", outPath, wr, sz); return false; }
    LOG(L"[extract] unpacked %ls (%lu b)\n", outPath, sz);
    return true;
}

// Unpack hook.dll + berkutx_rng.exe into `dir` (the hook execs the rng from its own folder).
static bool extract_to(const wchar_t* dir, wchar_t* outHookPath)
{
    wchar_t hookP[MAX_PATH], rngP[MAX_PATH];
    _snwprintf(hookP, MAX_PATH, L"%shook.dll", dir);        hookP[MAX_PATH - 1] = 0;
    _snwprintf(rngP,  MAX_PATH, L"%sberkutx_rng.exe", dir); rngP[MAX_PATH - 1] = 0;
    if (!write_resource(L"HOOKDLL", hookP)) return false;
    if (!write_resource(L"RNGEXE",  rngP))  return false;
    wcsncpy(outHookPath, hookP, MAX_PATH); outHookPath[MAX_PATH - 1] = 0;
    return true;
}

// Unpack into %LOCALAPPDATA%\berkutx_roulette\ (falls back to %TEMP%) so the game folder stays clean.
static bool extract_payload(wchar_t* outHookPath)
{
    wchar_t base[MAX_PATH] = {0};
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH) && base[0]) {
        size_t n = wcslen(base);
        if (n && base[n - 1] != L'\\' && n + 1 < MAX_PATH) { base[n] = L'\\'; base[n + 1] = 0; }
    } else if (!GetTempPathW(MAX_PATH, base)) {
        base[0] = 0;
    }
    if (!base[0]) { LOG(L"[extract] no writable base dir (LOCALAPPDATA/TEMP)\n"); return false; }
    wchar_t dir[MAX_PATH];
    _snwprintf(dir, MAX_PATH, L"%sberkutx_roulette\\", base); dir[MAX_PATH - 1] = 0;
    CreateDirectoryW(dir, NULL);
    LOG(L"[extract] unpack dir: %ls\n", dir);
    return extract_to(dir, outHookPath);
}

static DWORD find_pid(const wchar_t* name)
{
    DWORD pid = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) { LOG(L"[warn] process snapshot failed: %lu\n", GetLastError()); return 0; }
    PROCESSENTRY32W pe; pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, name) == 0) { pid = pe.th32ProcessID; break; }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return pid;
}

// Launch exePath with its own folder as the working directory. Returns the PID
// (0 on failure) and hands the process handle back so the caller can wait on it.
static DWORD launch_game(const wchar_t* exePath, HANDLE* outProc)
{
    wchar_t dir[MAX_PATH]; wcsncpy(dir, exePath, MAX_PATH); dir[MAX_PATH - 1] = 0;
    wchar_t* slash = wcsrchr(dir, L'\\'); if (slash) *slash = 0;

    wchar_t cmd[MAX_PATH + 2];
    _snwprintf(cmd, MAX_PATH + 2, L"\"%ls\"", exePath);   // argv[0], quoted

    STARTUPINFOW si; ZeroMemory(&si, sizeof(si)); si.cb = sizeof(si);
    PROCESS_INFORMATION pi; ZeroMemory(&pi, sizeof(pi));
    if (!CreateProcessW(exePath, cmd, NULL, NULL, FALSE, 0, NULL,
                        dir[0] ? dir : NULL, &si, &pi))
        return 0;
    CloseHandle(pi.hThread);
    if (outProc) *outProc = pi.hProcess; else CloseHandle(pi.hProcess);
    return pi.dwProcessId;
}

// Poll until `mod` is loaded in the target (so the hook can patch it), or timeout.
static bool wait_for_module(DWORD pid, const wchar_t* mod, DWORD timeoutMs)
{
    DWORD start = GetTickCount();
    for (;;) {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
        if (snap != INVALID_HANDLE_VALUE) {
            MODULEENTRY32W me; me.dwSize = sizeof(me);
            bool found = false;
            if (Module32FirstW(snap, &me)) {
                do { if (!_wcsicmp(me.szModule, mod)) { found = true; break; } }
                while (Module32NextW(snap, &me));
            }
            CloseHandle(snap);
            if (found) return true;
        }
        if (GetTickCount() - start > timeoutMs) return false;
        Sleep(200);
    }
}

static int inject(DWORD pid, const wchar_t* dll)
{
    HANDLE hp = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                            PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ, FALSE, pid);
    if (!hp) { LOG(L"[error] OpenProcess(pid %lu) failed: %lu (still running / same bitness / admin?)\n", pid, GetLastError()); return 6; }

    SIZE_T bytes = (wcslen(dll) + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(hp, NULL, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) { LOG(L"[error] VirtualAllocEx failed: %lu\n", GetLastError()); CloseHandle(hp); return 7; }
    if (!WriteProcessMemory(hp, remote, dll, bytes, NULL)) { LOG(L"[error] WriteProcessMemory failed: %lu\n", GetLastError()); VirtualFreeEx(hp, remote, 0, MEM_RELEASE); CloseHandle(hp); return 8; }

    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    LPTHREAD_START_ROUTINE loadlib = (LPTHREAD_START_ROUTINE)GetProcAddress(k32, "LoadLibraryW");
    HANDLE th = CreateRemoteThread(hp, NULL, 0, loadlib, remote, 0, NULL);
    if (!th) { LOG(L"[error] CreateRemoteThread failed: %lu\n", GetLastError()); VirtualFreeEx(hp, remote, 0, MEM_RELEASE); CloseHandle(hp); return 9; }

    DWORD w = WaitForSingleObject(th, 15000);
    DWORD exitCode = 0; GetExitCodeThread(th, &exitCode);   // = HMODULE of hook.dll (low 32 bits) or 0 on failure
    VirtualFreeEx(hp, remote, 0, MEM_RELEASE);
    CloseHandle(th);
    CloseHandle(hp);

    if (w == WAIT_TIMEOUT) LOG(L"[warn] LoadLibrary thread did not finish within 15s\n");
    LOG(L"[inject] injected %ls into pid %lu (LoadLibraryW ret=0x%08lX)\n", dll, pid, exitCode);
    if (!exitCode) {
        LOG(L"[error] LoadLibraryW returned 0 - hook.dll failed to load in the target.\n");
        return 10;
    }
    return 0;
}

int wmain(int argc, wchar_t** argv)
{
    init_log();
    SYSTEMTIME st; GetLocalTime(&st);
    LOG(L"\n=== berkutx_loader run %04d-%02d-%02d %02d:%02d:%02d (argc=%d) ===\n",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, argc);
    for (int i = 1; i < argc; ++i) LOG(L"  argv[%d]='%ls'\n", i, argv[i]);

    wchar_t dll[MAX_PATH]; dll[0] = 0;
    DWORD wantPid = 0;
    bool attachRunning = false;
    for (int i = 1; i < argc; ++i) {
        wchar_t* a = argv[i];
        if (a[0] == L'-') {                                       // flag
            if (!_wcsicmp(a, L"-a") || !_wcsicmp(a, L"--attach")) attachRunning = true;
            else LOG(L"[args] ignoring unknown flag '%ls'\n", a);
            continue;
        }
        wchar_t* end = NULL;
        unsigned long v = wcstoul(a, &end, 10);
        if (end && *end == 0 && a[0]) wantPid = (DWORD)v;          // pure number => PID
        else { wcsncpy(dll, a, MAX_PATH); dll[MAX_PATH - 1] = 0; } // else => hook DLL override path
    }

    // hook DLL: unpack the embedded payload by default, or honour an explicit override path.
    if (!dll[0]) {
        if (!extract_payload(dll)) { LOG(L"[error] could not unpack the embedded hook.dll / berkutx_rng.exe\n"); return 2; }
    } else {
        wchar_t full[MAX_PATH];
        if (GetFullPathNameW(dll, MAX_PATH, full, NULL)) wcscpy(dll, full);
        if (GetFileAttributesW(dll) == INVALID_FILE_ATTRIBUTES) { LOG(L"[error] override hook dll NOT FOUND: %ls\n", dll); return 2; }
    }
    LOG(L"[cfg] hook dll = %ls\n", dll);

    DWORD pid = 0;
    if (wantPid) {                                                 // explicit instance
        pid = wantPid;
        LOG(L"[mode] explicit pid %lu\n", pid);
    } else if (attachRunning) {                                    // first running instance
        pid = find_pid(L"Discipl2.exe");
        if (!pid) { LOG(L"[error] -a/--attach: no running Discipl2.exe found\n"); return 3; }
        LOG(L"[mode] attach to first running Discipl2.exe, pid %lu\n", pid);
    } else {                                                       // no args: launch + attach
        wchar_t game[MAX_PATH];
        GetModuleFileNameW(NULL, game, MAX_PATH);
        wchar_t* slash = wcsrchr(game, L'\\');
        if (slash) wcscpy(slash + 1, L"Discipl2.exe"); else wcscpy(game, L"Discipl2.exe");
        LOG(L"[mode] no args -> launch %ls\n", game);
        if (GetFileAttributesW(game) == INVALID_FILE_ATTRIBUTES) {
            LOG(L"[error] game exe NOT FOUND next to loader: %ls\n", game);
            return 4;
        }
        HANDLE hGame = NULL;
        pid = launch_game(game, &hGame);
        if (!pid) { LOG(L"[error] CreateProcess failed: %lu\n", GetLastError()); return 5; }
        LOG(L"[launch] started pid %lu; waiting for it to come up...\n", pid);
        DWORD wi = WaitForInputIdle(hGame, 30000);
        LOG(L"[launch] WaitForInputIdle -> %lu (0=ready, 0x102=timeout)\n", wi);
        if (wait_for_module(pid, L"mss32.dll", 30000)) LOG(L"[launch] mss32.dll present in target\n");
        else                                           LOG(L"[warn] mss32.dll not seen within 30s, injecting anyway\n");
        if (hGame) CloseHandle(hGame);
    }

    int rc = inject(pid, dll);
    LOG(L"[done] exit code %d\n", rc);
    return rc;
}
