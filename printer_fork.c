// printer_fork.exe - opens a .3mf in Snapmaker Orca or Bambu Studio.
// Snapmaker Orca if the file name ends in "_u1.3mf" or Shift is held; Bambu Studio otherwise.
// Build: windres printer_fork.rc -O coff -o printer_fork.res
//        gcc -Os -s -municode -mwindows printer_fork.c printer_fork.res -o printer_fork.exe
#include <windows.h>
#include <shellapi.h>
#include <wchar.h>

#ifndef ORCA
#define ORCA  L"C:\\Program Files\\Snapmaker_Orca\\snapmaker-orca.exe"
#endif
#ifndef BAMBU
#define BAMBU L"C:\\Program Files\\Bambu Studio\\bambu-studio.exe"
#endif
#define SUFFIX L"_u1.3mf"

// Returns a pointer to the raw command line after argv[0], so all arguments pass through unchanged.
static const wchar_t *args_after_exe(const wchar_t *p) {
    if (*p == L'"') {
        for (++p; *p && *p != L'"'; ++p) {}
        if (*p) ++p;
    } else {
        while (*p && *p != L' ' && *p != L'\t') ++p;
    }
    while (*p == L' ' || *p == L'\t') ++p;
    return p;
}

static int ends_with_ci(const wchar_t *s, const wchar_t *suffix) {
    size_t n = wcslen(s), m = wcslen(suffix);
    return n >= m && _wcsicmp(s + n - m, suffix) == 0;
}

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE hp, LPWSTR cl, int show) {
    (void)hi; (void)hp; (void)cl; (void)show;

    // Read Shift first, before anything else, while the user is still holding it.
    int shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;

    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    int orca = shift || (argv && argc > 1 && ends_with_ci(argv[1], SUFFIX));
    const wchar_t *exe = orca ? ORCA : BAMBU;

    const wchar_t *rest = args_after_exe(GetCommandLineW());
    size_t len = wcslen(exe) + wcslen(rest) + 4;
    wchar_t *cmd = (wchar_t *)HeapAlloc(GetProcessHeap(), 0, len * sizeof(wchar_t));
    if (!cmd) return 1;
    wcscpy(cmd, L"\"");
    wcscat(cmd, exe);
    wcscat(cmd, L"\"");
    if (*rest) { wcscat(cmd, L" "); wcscat(cmd, rest); }

    // Start the slicer in the folder of the opened file, the way a direct association would.
    wchar_t dir[MAX_PATH] = L"";
    if (argv && argc > 1 && wcslen(argv[1]) < MAX_PATH) {
        wcscpy(dir, argv[1]);
        wchar_t *slash = wcsrchr(dir, L'\\');
        if (slash) *slash = 0; else dir[0] = 0;
        DWORD a = dir[0] ? GetFileAttributesW(dir) : INVALID_FILE_ATTRIBUTES;
        if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) dir[0] = 0;
    }

    STARTUPINFOW si = { sizeof si };
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(exe, cmd, NULL, NULL, FALSE, 0, NULL, dir[0] ? dir : NULL, &si, &pi)) {
        wchar_t msg[600];
        wsprintfW(msg, L"Could not start:\n%s\n\nError %lu", exe, GetLastError());
        MessageBoxW(NULL, msg, L"printer_fork", MB_ICONERROR);
        return 1;
    }
    AllowSetForegroundWindow(pi.dwProcessId);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 0;
}
