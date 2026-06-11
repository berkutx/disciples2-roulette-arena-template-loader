// ============================================================================
//  berkutx_loader — LoadLibrary injector for hook.dll (x86, matches the game).
//  No args: find the running Discipl2.exe and inject hook.dll from next to this
//  exe. Optional args (any order): a PID to target a specific instance, and/or a
//  path to the DLL. See loader/build.ps1.
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

int wmain(int argc, wchar_t** argv)
{
    // Args (any order): a numeric token = explicit target PID; a non-numeric token
    // = path to the hook DLL. Defaults: DLL = hook.dll next to this exe; PID = first
    // Discipl2.exe found. ALWAYS prefer an explicit PID to target a specific instance.
    wchar_t dll[MAX_PATH]; dll[0] = 0;
    DWORD wantPid = 0;
    for (int i = 1; i < argc; ++i) {
        wchar_t* a = argv[i];
        wchar_t* end = NULL;
        unsigned long v = wcstoul(a, &end, 10);
        if (end && *end == 0 && a[0]) wantPid = (DWORD)v;   // pure number => PID
        else { wcsncpy(dll, a, MAX_PATH); dll[MAX_PATH - 1] = 0; }
    }
    if (!dll[0]) {
        GetModuleFileNameW(NULL, dll, MAX_PATH);
        wchar_t* slash = wcsrchr(dll, L'\\');
        if (slash) wcscpy(slash + 1, L"hook.dll"); else wcscpy(dll, L"hook.dll");
    }
    // absolute path (the remote process must resolve it)
    wchar_t full[MAX_PATH];
    if (GetFullPathNameW(dll, MAX_PATH, full, NULL)) wcscpy(dll, full);

    if (GetFileAttributesW(dll) == INVALID_FILE_ATTRIBUTES) {
        wprintf(L"[inject] hook dll not found: %ls\n", dll);
        return 2;
    }

    DWORD pid = wantPid ? wantPid : find_pid(L"Discipl2.exe");
    if (!pid) { wprintf(L"[inject] Discipl2.exe not running\n"); return 3; }
    wprintf(L"[inject] target pid %lu%ls\n", pid, wantPid ? L" (explicit)" : L" (first found)");

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
