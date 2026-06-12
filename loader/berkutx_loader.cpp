// ============================================================================
//  berkutx_loader — LoadLibrary injector for hook.dll (x86, matches the game).
//
//  No args     : launch Discipl2.exe (next to this exe), wait for it to come up,
//                and inject hook.dll into that instance. Turn-key mode.
//  <pid>       : inject into an already-running instance with that PID.
//  -a|--attach : inject into the first running Discipl2.exe (no launch).
//  <path.dll>  : override the hook DLL (default: hook.dll next to this exe).
//  Tokens may be combined in any order. See loader/build.ps1.
// ============================================================================

#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>

static DWORD find_pid(const wchar_t* name)
{
    DWORD pid = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
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
                            PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
                            FALSE, pid);
    if (!hp) { wprintf(L"[inject] OpenProcess failed: %lu\n", GetLastError()); return 4; }

    SIZE_T bytes = (wcslen(dll) + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(hp, NULL, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) { wprintf(L"[inject] VirtualAllocEx failed\n"); CloseHandle(hp); return 5; }
    WriteProcessMemory(hp, remote, dll, bytes, NULL);

    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    LPTHREAD_START_ROUTINE loadlib = (LPTHREAD_START_ROUTINE)GetProcAddress(k32, "LoadLibraryW");
    HANDLE th = CreateRemoteThread(hp, NULL, 0, loadlib, remote, 0, NULL);
    if (!th) { wprintf(L"[inject] CreateRemoteThread failed: %lu\n", GetLastError()); VirtualFreeEx(hp, remote, 0, MEM_RELEASE); CloseHandle(hp); return 6; }

    WaitForSingleObject(th, 10000);
    DWORD exitCode = 0; GetExitCodeThread(th, &exitCode);  // = HMODULE of hook.dll (low 32 bits) or 0 on failure
    VirtualFreeEx(hp, remote, 0, MEM_RELEASE);
    CloseHandle(th);
    CloseHandle(hp);

    wprintf(L"[inject] injected %ls into pid %lu (LoadLibrary ret=0x%08lX)\n", dll, pid, exitCode);
    return exitCode ? 0 : 7;
}

int wmain(int argc, wchar_t** argv)
{
    wchar_t dll[MAX_PATH]; dll[0] = 0;
    DWORD wantPid = 0;
    bool attachRunning = false;
    for (int i = 1; i < argc; ++i) {
        wchar_t* a = argv[i];
        if (a[0] == L'-') {                                       // flag
            if (!_wcsicmp(a, L"-a") || !_wcsicmp(a, L"--attach")) attachRunning = true;
            continue;
        }
        wchar_t* end = NULL;
        unsigned long v = wcstoul(a, &end, 10);
        if (end && *end == 0 && a[0]) wantPid = (DWORD)v;          // pure number => PID
        else { wcsncpy(dll, a, MAX_PATH); dll[MAX_PATH - 1] = 0; } // else => hook DLL path
    }

    // resolve the hook DLL (default: hook.dll next to this exe) to an absolute path
    if (!dll[0]) {
        GetModuleFileNameW(NULL, dll, MAX_PATH);
        wchar_t* slash = wcsrchr(dll, L'\\');
        if (slash) wcscpy(slash + 1, L"hook.dll"); else wcscpy(dll, L"hook.dll");
    }
    wchar_t full[MAX_PATH];
    if (GetFullPathNameW(dll, MAX_PATH, full, NULL)) wcscpy(dll, full);
    if (GetFileAttributesW(dll) == INVALID_FILE_ATTRIBUTES) {
        wprintf(L"[inject] hook dll not found: %ls\n", dll);
        return 2;
    }

    DWORD pid = 0;
    if (wantPid) {                                                 // explicit instance
        pid = wantPid;
        wprintf(L"[inject] target pid %lu (explicit)\n", pid);
    } else if (attachRunning) {                                    // first running instance
        pid = find_pid(L"Discipl2.exe");
        if (!pid) { wprintf(L"[inject] Discipl2.exe not running\n"); return 3; }
        wprintf(L"[inject] target pid %lu (first found)\n", pid);
    } else {                                                       // no args: launch + attach
        wchar_t game[MAX_PATH];
        GetModuleFileNameW(NULL, game, MAX_PATH);
        wchar_t* slash = wcsrchr(game, L'\\');
        if (slash) wcscpy(slash + 1, L"Discipl2.exe"); else wcscpy(game, L"Discipl2.exe");
        if (GetFileAttributesW(game) == INVALID_FILE_ATTRIBUTES) {
            wprintf(L"[launch] game not found next to loader: %ls\n", game);
            return 8;
        }
        HANDLE hGame = NULL;
        pid = launch_game(game, &hGame);
        if (!pid) { wprintf(L"[launch] CreateProcess failed: %lu\n", GetLastError()); return 9; }
        wprintf(L"[launch] started %ls pid %lu; waiting for it to come up...\n", game, pid);
        WaitForInputIdle(hGame, 30000);
        if (!wait_for_module(pid, L"mss32.dll", 30000))
            wprintf(L"[launch] warning: mss32.dll not seen within 30s, injecting anyway\n");
        if (hGame) CloseHandle(hGame);
        wprintf(L"[inject] target pid %lu (launched)\n", pid);
    }

    return inject(pid, dll);
}
