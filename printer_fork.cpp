// printer_fork - opens a file in one of several programs, picked by ordered rules.
//
//   printer_fork.exe <file>      apply the rules and launch the matching program (no window)
//   printer_fork.exe             open the settings window (same with --settings)
//
// Rules and programs live in printer_fork.ini next to the exe
// (or %APPDATA%\printer_fork\printer_fork.ini if the exe folder is not writable).
#define _WIN32_WINNT 0x0A00
#ifndef UNICODE
#define UNICODE
#endif
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <initializer_list>
#include <regex>
#include <string>
#include <utility>
#include <vector>
#include "resource.h"

using std::wstring;
using std::vector;

// ---------------------------------------------------------------- model

enum RuleType { RT_ENDS, RT_STARTS, RT_CONTAINS, RT_REGEX, RT_FOLDER, RT_ANY, RT_COUNT };

static const wchar_t *TYPE_KEYS[RT_COUNT] = {L"endswith", L"startswith", L"contains", L"regex", L"folder", L"any"};
static const wchar_t *TYPE_LABELS[RT_COUNT] = {L"Ends with", L"Starts with", L"Contains", L"Matches regex",
                                               L"Is in folder (incl. subfolders)", L"Any file"};

// Modifier keys: a rule either ignores them (KEYS_ANY) or requires exactly this set to be held
// (0 = no modifier key held).
enum { KEY_SHIFT = 1, KEY_CTRL = 2, KEY_ALT = 4, KEYS_ANY = -1 };
static const int KEY_CHOICES[] = {KEYS_ANY, 0, KEY_SHIFT, KEY_CTRL, KEY_ALT, KEY_CTRL | KEY_SHIFT,
                                  KEY_ALT | KEY_SHIFT, KEY_CTRL | KEY_ALT, KEY_CTRL | KEY_ALT | KEY_SHIFT};

static wstring modsLabel(int m) {
    if (m == KEYS_ANY) return L"Doesn't matter";
    if (m == 0) return L"None held";
    wstring s;
    if (m & KEY_CTRL) s += L"Ctrl+";
    if (m & KEY_ALT) s += L"Alt+";
    if (m & KEY_SHIFT) s += L"Shift+";
    s.pop_back();
    return s;
}

static wstring modsKey(int m) {  // ini spelling: any, none, ctrl+alt+shift
    if (m == KEYS_ANY) return L"any";
    if (m == 0) return L"none";
    wstring s;
    if (m & KEY_CTRL) s += L"ctrl+";
    if (m & KEY_ALT) s += L"alt+";
    if (m & KEY_SHIFT) s += L"shift+";
    s.pop_back();
    return s;
}

static int heldMods() {
    int m = 0;
    if (GetAsyncKeyState(VK_SHIFT) & 0x8000) m |= KEY_SHIFT;
    if (GetAsyncKeyState(VK_CONTROL) & 0x8000) m |= KEY_CTRL;
    if (GetAsyncKeyState(VK_MENU) & 0x8000) m |= KEY_ALT;
    return m;
}

struct Program {
    wstring name, path, args;
};

struct Rule {
    bool enabled = true;
    RuleType type = RT_ENDS;
    wstring pattern;
    bool fullPath = false;
    int mods = KEYS_ANY;
    wstring program;
};

struct Config {
    vector<Program> programs;
    vector<Rule> rules;
    wstring defaultProgram;
};

static HINSTANCE g_inst;
static wstring g_cfgPath;  // file the config was loaded from (empty: built-in defaults)

// ---------------------------------------------------------------- string / path helpers

static wstring lower(wstring s) {
    if (!s.empty()) CharLowerBuffW(&s[0], (DWORD)s.size());
    return s;
}
static bool ieq(const wstring &a, const wstring &b) { return lower(a) == lower(b); }
static wstring trim(const wstring &s, const wchar_t *chars = L" \t") {
    size_t a = s.find_first_not_of(chars);
    if (a == wstring::npos) return wstring();
    size_t b = s.find_last_not_of(chars);
    return s.substr(a, b - a + 1);
}
static wstring fullPath(const wstring &p) {
    DWORD n = GetFullPathNameW(p.c_str(), 0, nullptr, nullptr);
    if (!n) return p;
    wstring s(n, L'\0');
    n = GetFullPathNameW(p.c_str(), n, &s[0], nullptr);
    s.resize(n);
    return s;
}
static wstring baseName(const wstring &p) {
    size_t i = p.find_last_of(L"\\/");
    return i == wstring::npos ? p : p.substr(i + 1);
}
static wstring dirName(const wstring &p) {
    size_t i = p.find_last_of(L"\\/");
    return i == wstring::npos ? wstring() : p.substr(0, i);
}
static bool fileExists(const wstring &p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
static bool dirExists(const wstring &p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

// ---------------------------------------------------------------- config file

static Config defaultConfig() {
    Config c;
    c.programs.push_back({L"Bambu Studio", L"C:\\Program Files\\Bambu Studio\\bambu-studio.exe", L"%*"});
    c.programs.push_back({L"Snapmaker Orca", L"C:\\Program Files\\Snapmaker_Orca\\snapmaker-orca.exe", L"%*"});
    Rule shift;
    shift.type = RT_ANY;
    shift.mods = KEY_SHIFT;
    shift.program = L"Snapmaker Orca";
    Rule u1;
    u1.type = RT_ENDS;
    u1.pattern = L"_u1.3mf";
    u1.program = L"Snapmaker Orca";
    c.rules = {shift, u1};
    c.defaultProgram = L"Bambu Studio";
    return c;
}

static wstring exeDirIni() {
    wchar_t buf[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(nullptr, buf, ARRAYSIZE(buf));
    return dirName(wstring(buf, n)) + L"\\printer_fork.ini";
}

static wstring appDataIni() {
    PWSTR p = nullptr;
    wstring r;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &p)))
        r = wstring(p) + L"\\printer_fork\\printer_fork.ini";
    CoTaskMemFree(p);
    return r;
}

static bool readUtf8(const wstring &path, wstring &out) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz;
    GetFileSizeEx(f, &sz);
    std::string b((size_t)sz.QuadPart, '\0');
    DWORD got = 0;
    BOOL ok = b.empty() || ReadFile(f, &b[0], (DWORD)b.size(), &got, nullptr);
    CloseHandle(f);
    if (!ok) return false;
    b.resize(got);
    size_t off = (b.size() >= 3 && (unsigned char)b[0] == 0xEF && (unsigned char)b[1] == 0xBB &&
                  (unsigned char)b[2] == 0xBF) ? 3 : 0;
    int n = MultiByteToWideChar(CP_UTF8, 0, b.data() + off, (int)(b.size() - off), nullptr, 0);
    out.assign(n, L'\0');
    if (n) MultiByteToWideChar(CP_UTF8, 0, b.data() + off, (int)(b.size() - off), &out[0], n);
    return true;
}

static bool writeUtf8(const wstring &path, const wstring &text) {
    int n = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0, nullptr, nullptr);
    std::string b = "\xEF\xBB\xBF";
    size_t off = b.size();
    b.resize(off + n);
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), &b[off], n, nullptr, nullptr);
    wstring tmp = path + L".tmp";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0;
    BOOL ok = WriteFile(f, b.data(), (DWORD)b.size(), &w, nullptr) && w == b.size();
    CloseHandle(f);
    if (!ok || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

static int keyIndex(const wchar_t *const *keys, int n, const wstring &v, int def) {
    for (int i = 0; i < n; i++)
        if (ieq(trim(v), keys[i])) return i;
    return def;
}

static int parseMods(const wstring &v) {
    wstring s = lower(trim(v));
    if (s.empty() || s == L"any") return KEYS_ANY;
    int m = 0;
    size_t pos = 0;
    while (pos <= s.size()) {
        size_t e = s.find(L'+', pos);
        if (e == wstring::npos) e = s.size();
        wstring k = trim(s.substr(pos, e - pos));
        if (k == L"shift") m |= KEY_SHIFT;
        else if (k == L"ctrl" || k == L"control") m |= KEY_CTRL;
        else if (k == L"alt") m |= KEY_ALT;
        pos = e + 1;
    }
    return m;  // "none" (or anything unrecognised) = no modifier held
}

static Config parseConfig(const wstring &text) {
    Config c;
    enum { NONE, PROG, RULE, SETTINGS } sec = NONE;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t e = text.find(L'\n', pos);
        if (e == wstring::npos) e = text.size();
        wstring line = text.substr(pos, e - pos);
        pos = e + 1;
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        size_t s = line.find_first_not_of(L" \t");
        if (s == wstring::npos || line[s] == L';' || line[s] == L'#') continue;
        if (line[s] == L'[') {
            size_t close = line.find(L']', s);
            wstring name = lower(trim(line.substr(s + 1, close == wstring::npos ? wstring::npos : close - s - 1)));
            if (name == L"program") {
                c.programs.push_back({L"", L"", L"%*"});
                sec = PROG;
            } else if (name == L"rule") {
                c.rules.push_back(Rule());
                sec = RULE;
            } else {
                sec = name == L"settings" ? SETTINGS : NONE;
            }
            continue;
        }
        size_t eq = line.find(L'=');
        if (eq == wstring::npos) continue;
        wstring key = lower(trim(line.substr(s, eq - s)));
        wstring val = line.substr(eq + 1);  // kept verbatim: spaces can matter in patterns
        if (sec == PROG) {
            Program &p = c.programs.back();
            if (key == L"name") p.name = trim(val);
            else if (key == L"path") p.path = trim(val);
            else if (key == L"args") p.args = val;
        } else if (sec == RULE) {
            Rule &r = c.rules.back();
            if (key == L"enabled") r.enabled = trim(val) != L"0";
            else if (key == L"type") r.type = (RuleType)keyIndex(TYPE_KEYS, RT_COUNT, val, RT_ENDS);
            else if (key == L"pattern") r.pattern = val;
            else if (key == L"fullpath") r.fullPath = trim(val) == L"1";
            else if (key == L"keys") r.mods = parseMods(val);
            else if (key == L"shift") {  // v1.1 format
                wstring v = lower(trim(val));
                r.mods = v == L"held" ? KEY_SHIFT : v == L"notheld" ? 0 : KEYS_ANY;
            }
            else if (key == L"program") r.program = trim(val);
        } else if (sec == SETTINGS && key == L"default") {
            c.defaultProgram = trim(val);
        }
    }
    return c;
}

static wstring serialize(const Config &c) {
    wstring s =
        L"; printer_fork configuration. Rules are checked top to bottom; the first match wins.\r\n"
        L"; Easiest to edit in the settings window: run printer_fork.exe without a file.\r\n"
        L"; Rule types: endswith, startswith, contains, regex, folder, any.\r\n"
        L"; keys: any (ignore modifier keys), none (no modifier held), or an exact combination like ctrl+shift.\r\n"
        L"; Program args: %* = arguments as received, \"%f\" = the opened file.\r\n\r\n";
    s += L"[settings]\r\ndefault=" + c.defaultProgram + L"\r\n";
    for (auto &p : c.programs)
        s += L"\r\n[program]\r\nname=" + p.name + L"\r\npath=" + p.path + L"\r\nargs=" + p.args + L"\r\n";
    for (auto &r : c.rules) {
        s += L"\r\n[rule]\r\nenabled=";
        s += r.enabled ? L"1" : L"0";
        s += L"\r\ntype=";
        s += TYPE_KEYS[r.type];
        s += L"\r\npattern=" + r.pattern;
        s += L"\r\nfullpath=";
        s += r.fullPath ? L"1" : L"0";
        s += L"\r\nkeys=" + modsKey(r.mods);
        s += L"\r\nprogram=" + r.program + L"\r\n";
    }
    return s;
}

static Config loadConfig() {
    wstring text, a = exeDirIni();
    if (readUtf8(a, text)) {
        g_cfgPath = a;
        return parseConfig(text);
    }
    wstring b = appDataIni();
    if (!b.empty() && readUtf8(b, text)) {
        g_cfgPath = b;
        return parseConfig(text);
    }
    g_cfgPath.clear();
    return defaultConfig();
}

static bool saveConfig(const Config &c) {
    wstring text = serialize(c);
    if (!g_cfgPath.empty() && writeUtf8(g_cfgPath, text)) return true;
    wstring a = exeDirIni();
    if (writeUtf8(a, text)) {
        g_cfgPath = a;
        return true;
    }
    wstring b = appDataIni();
    if (b.empty()) return false;
    CreateDirectoryW(dirName(b).c_str(), nullptr);
    if (writeUtf8(b, text)) {
        g_cfgPath = b;
        return true;
    }
    return false;
}

// ---------------------------------------------------------------- matching

// Full path with 8.3 short names (ALEX-D~1) expanded, so both spellings of a folder compare equal.
static wstring longPath(const wstring &p) {
    wstring f = fullPath(p);
    DWORD n = GetLongPathNameW(f.c_str(), nullptr, 0);
    if (!n) return f;
    wstring s(n, L'\0');
    n = GetLongPathNameW(f.c_str(), &s[0], n);
    if (!n) return f;
    s.resize(n);
    return s;
}

static bool inFolder(const wstring &file, const wstring &folder) {
    if (trim(folder).empty()) return false;
    wstring f = lower(longPath(file)), d = lower(longPath(trim(folder, L" \t\"")));
    while (!d.empty() && (d.back() == L'\\' || d.back() == L'/')) d.pop_back();
    return f.size() > d.size() && f.compare(0, d.size(), d) == 0 && f[d.size()] == L'\\';
}

static bool ruleMatches(const Rule &r, const wstring &file, int mods) {
    if (!r.enabled) return false;
    if (r.mods != KEYS_ANY && r.mods != mods) return false;
    if (r.type == RT_ANY) return true;
    if (r.type == RT_FOLDER) return inFolder(file, r.pattern);
    wstring subject = r.fullPath ? fullPath(file) : baseName(file);
    if (r.type == RT_REGEX) {
        try {
            return std::regex_search(subject, std::wregex(r.pattern, std::regex::ECMAScript | std::regex::icase));
        } catch (...) {
            return false;
        }
    }
    wstring s = lower(subject), p = lower(r.pattern);
    if (p.size() > s.size()) return false;
    switch (r.type) {
        case RT_ENDS: return s.compare(s.size() - p.size(), p.size(), p) == 0;
        case RT_STARTS: return s.compare(0, p.size(), p) == 0;
        default: return s.find(p) != wstring::npos;
    }
}

static int firstMatch(const Config &c, const wstring &file, int mods) {
    for (size_t i = 0; i < c.rules.size(); i++)
        if (ruleMatches(c.rules[i], file, mods)) return (int)i;
    return -1;
}

static const Program *findProgram(const Config &c, const wstring &name) {
    for (auto &p : c.programs)
        if (ieq(p.name, name)) return &p;
    return nullptr;
}

static wstring describeRule(const Rule &r) {
    switch (r.type) {
        case RT_ANY: return L"any file";
        case RT_FOLDER: return L"in folder " + r.pattern;
        default: {
            static const wchar_t *verbs[] = {L"ends with", L"starts with", L"contains", L"matches"};
            wstring s = r.fullPath ? L"path " : L"name ";
            s += verbs[r.type];
            s += r.type == RT_REGEX ? L" /" + r.pattern + L"/" : L" \"" + r.pattern + L"\"";
            return s;
        }
    }
}

// ---------------------------------------------------------------- launching

// Raw command line after argv[0], so arguments pass through exactly as Explorer sent them.
static const wchar_t *argsAfterExe(const wchar_t *p) {
    if (*p == L'"') {
        for (++p; *p && *p != L'"'; ++p) {}
        if (*p) ++p;
    } else {
        while (*p && *p != L' ' && *p != L'\t') ++p;
    }
    while (*p == L' ' || *p == L'\t') ++p;
    return p;
}

static wstring expandArgs(const wstring &t, const wstring &file, const wstring &rest) {
    wstring out;
    for (size_t i = 0; i < t.size(); i++) {
        if (t[i] == L'%' && i + 1 < t.size()) {
            wchar_t c = t[i + 1];
            if (c == L'f' || c == L'F') { out += file; i++; continue; }
            if (c == L'*') { out += rest; i++; continue; }
            if (c == L'%') { out += L'%'; i++; continue; }
        }
        out += t[i];
    }
    return out;
}

static bool launch(const Program &p, const wstring &file, const wstring &rest) {
    wstring cmd = L"\"" + p.path + L"\"";
    wstring a = trim(expandArgs(p.args, file, rest));
    if (!a.empty()) cmd += L" " + a;

    // Start the program in the opened file's folder, as a direct association would.
    wstring dir = dirName(file);
    if (dir.size() == 2 && dir[1] == L':') dir += L'\\';
    if (!dirExists(dir)) dir.clear();

    STARTUPINFOW si = {sizeof si};
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(p.path.c_str(), &cmd[0], nullptr, nullptr, FALSE, 0, nullptr,
                        dir.empty() ? nullptr : dir.c_str(), &si, &pi)) {
        DWORD err = GetLastError();
        wstring m = L"Could not start " + p.name + L":\n" + p.path + L"\n\nError " + std::to_wstring(err) +
                    L"\n\nRun printer_fork.exe without a file to change the settings.";
        MessageBoxW(nullptr, m.c_str(), L"printer_fork", MB_ICONERROR);
        return false;
    }
    AllowSetForegroundWindow(pi.dwProcessId);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

// ---------------------------------------------------------------- UI helpers

static wstring getText(HWND h, int id) {
    HWND c = GetDlgItem(h, id);
    int n = GetWindowTextLengthW(c);
    wstring s(n, L'\0');
    if (n) GetWindowTextW(c, &s[0], n + 1);
    return s;
}

static int msg(HWND h, const wstring &text, UINT flags) { return MessageBoxW(h, text.c_str(), L"printer_fork", flags); }

static bool pickPath(HWND owner, bool folder, wstring &path, const wchar_t *filterName, const wchar_t *filterSpec) {
    IFileOpenDialog *d = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_IFileOpenDialog,
                                (void **)&d)))
        return false;
    DWORD opt = 0;
    d->GetOptions(&opt);
    d->SetOptions(opt | FOS_FORCEFILESYSTEM | (folder ? FOS_PICKFOLDERS : FOS_FILEMUSTEXIST));
    if (!folder) {
        COMDLG_FILTERSPEC spec[2] = {{filterName, filterSpec}, {L"All files", L"*.*"}};
        d->SetFileTypes(2, spec);
    }
    wstring start = folder ? trim(path, L" \t\"") : dirName(trim(path, L" \t\""));
    IShellItem *si = nullptr;
    if (!start.empty() && dirExists(start) &&
        SUCCEEDED(SHCreateItemFromParsingName(start.c_str(), nullptr, IID_IShellItem, (void **)&si))) {
        d->SetFolder(si);
        si->Release();
    }
    bool ok = false;
    if (SUCCEEDED(d->Show(owner))) {
        IShellItem *r = nullptr;
        if (SUCCEEDED(d->GetResult(&r))) {
            PWSTR p = nullptr;
            if (SUCCEEDED(r->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
                path = p;
                ok = true;
                CoTaskMemFree(p);
            }
            r->Release();
        }
    }
    d->Release();
    return ok;
}

static const int PROG_COLS[] = {28, 56, 16};       // column widths in % of the list width
static const int RULE_COLS[] = {10, 52, 13, 25};
#define WM_APP_FITCOLUMNS (WM_APP + 1)

static void fitColumns(HWND h, int id, const int *pct, int n) {
    HWND lv = GetDlgItem(h, id);
    RECT rc;
    GetClientRect(lv, &rc);
    int width = rc.right - GetSystemMetricsForDpi(SM_CXVSCROLL, GetDpiForWindow(h)) - 1;
    for (int i = 0; i < n; i++) ListView_SetColumnWidth(lv, i, width * pct[i] / 100);
}

static void fitAllColumns(HWND h) {
    fitColumns(h, IDC_PROGS, PROG_COLS, ARRAYSIZE(PROG_COLS));
    fitColumns(h, IDC_RULES, RULE_COLS, ARRAYSIZE(RULE_COLS));
}

static void initList(HWND h, int id, std::initializer_list<const wchar_t *> cols, bool checks) {
    HWND lv = GetDlgItem(h, id);
    ListView_SetExtendedListViewStyle(lv, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | (checks ? LVS_EX_CHECKBOXES : 0));
    int i = 0;
    for (auto c : cols) {
        LVCOLUMNW col = {};
        col.mask = LVCF_TEXT | LVCF_WIDTH;
        col.pszText = (LPWSTR)c;
        col.cx = 50;
        ListView_InsertColumn(lv, i++, &col);
    }
}

static void addRow(HWND lv, int i, std::initializer_list<wstring> cells) {
    int c = 0;
    for (auto &s : cells) {
        if (c == 0) {
            LVITEMW it = {};
            it.mask = LVIF_TEXT;
            it.iItem = i;
            it.pszText = (LPWSTR)s.c_str();
            ListView_InsertItem(lv, &it);
        } else {
            ListView_SetItemText(lv, i, c, (LPWSTR)s.c_str());
        }
        c++;
    }
}

static int selectedRow(HWND h, int id) { return ListView_GetNextItem(GetDlgItem(h, id), -1, LVNI_SELECTED); }

static void selectRow(HWND lv, int i) {
    if (i < 0 || i >= ListView_GetItemCount(lv)) return;
    ListView_SetItemState(lv, i, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    ListView_EnsureVisible(lv, i, FALSE);
}

static void comboFill(HWND h, int id, const vector<wstring> &items, int sel) {
    HWND cb = GetDlgItem(h, id);
    SendMessageW(cb, CB_RESETCONTENT, 0, 0);
    for (auto &s : items) SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)s.c_str());
    SendMessageW(cb, CB_SETCURSEL, sel, 0);
}

static int comboSel(HWND h, int id) { return (int)SendDlgItemMessageW(h, id, CB_GETCURSEL, 0, 0); }

static vector<wstring> programNames(const Config &c) {
    vector<wstring> v;
    for (auto &p : c.programs) v.push_back(p.name);
    return v;
}

static int programIndex(const Config &c, const wstring &name) {
    for (size_t i = 0; i < c.programs.size(); i++)
        if (ieq(c.programs[i].name, name)) return (int)i;
    return -1;
}

// ---------------------------------------------------------------- program dialog

struct ProgEdit {
    Program p;
    const Config *cfg;
    int index;  // -1 when adding
};

static INT_PTR CALLBACK ProgProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    ProgEdit *e = (ProgEdit *)GetWindowLongPtrW(h, DWLP_USER);
    switch (m) {
        case WM_INITDIALOG:
            e = (ProgEdit *)l;
            SetWindowLongPtrW(h, DWLP_USER, l);
            SetWindowTextW(h, e->index < 0 ? L"Add program" : L"Edit program");
            SetDlgItemTextW(h, IDC_PNAME, e->p.name.c_str());
            SetDlgItemTextW(h, IDC_PPATH, e->p.path.c_str());
            SetDlgItemTextW(h, IDC_PARGS, e->p.args.c_str());
            return TRUE;
        case WM_COMMAND:
            switch (LOWORD(w)) {
                case IDC_PBROWSE: {
                    wstring path = getText(h, IDC_PPATH);
                    if (pickPath(h, false, path, L"Programs (*.exe)", L"*.exe")) {
                        SetDlgItemTextW(h, IDC_PPATH, path.c_str());
                        if (trim(getText(h, IDC_PNAME)).empty()) {
                            wstring n = baseName(path);
                            size_t dot = n.rfind(L'.');
                            if (dot != wstring::npos) n.resize(dot);
                            SetDlgItemTextW(h, IDC_PNAME, n.c_str());
                        }
                    }
                    return TRUE;
                }
                case IDOK: {
                    Program p{trim(getText(h, IDC_PNAME)), trim(getText(h, IDC_PPATH), L" \t\""), getText(h, IDC_PARGS)};
                    if (p.name.empty()) {
                        msg(h, L"Please enter a name for the program.", MB_ICONWARNING);
                        SetFocus(GetDlgItem(h, IDC_PNAME));
                        return TRUE;
                    }
                    for (size_t i = 0; i < e->cfg->programs.size(); i++) {
                        if ((int)i != e->index && ieq(e->cfg->programs[i].name, p.name)) {
                            msg(h, L"There is already a program called \"" + p.name + L"\".", MB_ICONWARNING);
                            SetFocus(GetDlgItem(h, IDC_PNAME));
                            return TRUE;
                        }
                    }
                    if (p.path.empty()) {
                        msg(h, L"Please choose the program file (.exe).", MB_ICONWARNING);
                        SetFocus(GetDlgItem(h, IDC_PPATH));
                        return TRUE;
                    }
                    if (!fileExists(p.path) &&
                        msg(h, L"This file doesn't exist:\n" + p.path + L"\n\nSave anyway?", MB_YESNO | MB_ICONWARNING) != IDYES)
                        return TRUE;
                    e->p = p;
                    EndDialog(h, IDOK);
                    return TRUE;
                }
                case IDCANCEL:
                    EndDialog(h, IDCANCEL);
                    return TRUE;
            }
            break;
    }
    return FALSE;
}

// ---------------------------------------------------------------- rule dialog

struct RuleEdit {
    Rule r;
    const Config *cfg;
    bool adding;
};

static void ruleDlgUpdate(HWND h) {
    int t = comboSel(h, IDC_RTYPE);
    bool any = t == RT_ANY, folder = t == RT_FOLDER;
    EnableWindow(GetDlgItem(h, IDC_RPATTERN), !any);
    EnableWindow(GetDlgItem(h, IDC_RBROWSE), folder);
    EnableWindow(GetDlgItem(h, IDC_RFULL), !any && !folder);
    SetDlgItemTextW(h, IDC_RPATLABEL, folder ? L"Folder:" : t == RT_REGEX ? L"Regex:" : L"Text:");
}

static INT_PTR CALLBACK RuleProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    RuleEdit *e = (RuleEdit *)GetWindowLongPtrW(h, DWLP_USER);
    switch (m) {
        case WM_INITDIALOG: {
            e = (RuleEdit *)l;
            SetWindowLongPtrW(h, DWLP_USER, l);
            SetWindowTextW(h, e->adding ? L"Add rule" : L"Edit rule");
            CheckDlgButton(h, IDC_RENABLED, e->r.enabled ? BST_CHECKED : BST_UNCHECKED);
            comboFill(h, IDC_RTYPE, vector<wstring>(TYPE_LABELS, TYPE_LABELS + RT_COUNT), e->r.type);
            SetDlgItemTextW(h, IDC_RPATTERN, e->r.pattern.c_str());
            CheckDlgButton(h, IDC_RFULL, e->r.fullPath ? BST_CHECKED : BST_UNCHECKED);
            {
                vector<wstring> labels;
                int sel = 0;
                for (int i = 0; i < (int)ARRAYSIZE(KEY_CHOICES); i++) {
                    labels.push_back(modsLabel(KEY_CHOICES[i]));
                    if (KEY_CHOICES[i] == e->r.mods) sel = i;
                }
                comboFill(h, IDC_RSHIFT, labels, sel);
            }
            comboFill(h, IDC_RPROG, programNames(*e->cfg), programIndex(*e->cfg, e->r.program));
            ruleDlgUpdate(h);
            return TRUE;
        }
        case WM_COMMAND:
            switch (LOWORD(w)) {
                case IDC_RTYPE:
                    if (HIWORD(w) == CBN_SELCHANGE) ruleDlgUpdate(h);
                    return TRUE;
                case IDC_RBROWSE: {
                    wstring path = getText(h, IDC_RPATTERN);
                    if (pickPath(h, true, path, nullptr, nullptr)) SetDlgItemTextW(h, IDC_RPATTERN, path.c_str());
                    return TRUE;
                }
                case IDOK: {
                    Rule r;
                    r.enabled = IsDlgButtonChecked(h, IDC_RENABLED) == BST_CHECKED;
                    r.type = (RuleType)comboSel(h, IDC_RTYPE);
                    r.pattern = getText(h, IDC_RPATTERN);
                    r.fullPath = IsDlgButtonChecked(h, IDC_RFULL) == BST_CHECKED;
                    int mi = comboSel(h, IDC_RSHIFT);
                    r.mods = KEY_CHOICES[mi >= 0 && mi < (int)ARRAYSIZE(KEY_CHOICES) ? mi : 0];
                    int pi = comboSel(h, IDC_RPROG);
                    if (r.type == RT_ANY) r.pattern.clear(), r.fullPath = false;
                    if (r.type == RT_FOLDER) r.pattern = trim(r.pattern, L" \t\""), r.fullPath = false;
                    if (r.type != RT_ANY && r.pattern.empty()) {
                        msg(h, r.type == RT_FOLDER ? L"Please choose a folder." : L"Please enter the text to match.", MB_ICONWARNING);
                        SetFocus(GetDlgItem(h, IDC_RPATTERN));
                        return TRUE;
                    }
                    if (r.type == RT_REGEX) {
                        try {
                            std::wregex re(r.pattern, std::regex::ECMAScript | std::regex::icase);
                        } catch (const std::regex_error &ex) {
                            wstring what(ex.what(), ex.what() + strlen(ex.what()));
                            msg(h, L"This regular expression is not valid:\n" + what, MB_ICONWARNING);
                            SetFocus(GetDlgItem(h, IDC_RPATTERN));
                            return TRUE;
                        }
                    }
                    if (r.type == RT_FOLDER && !dirExists(r.pattern) &&
                        msg(h, L"This folder doesn't exist:\n" + r.pattern + L"\n\nSave anyway?", MB_YESNO | MB_ICONWARNING) != IDYES)
                        return TRUE;
                    if (pi < 0) {
                        msg(h, L"Please choose which program opens matching files.", MB_ICONWARNING);
                        SetFocus(GetDlgItem(h, IDC_RPROG));
                        return TRUE;
                    }
                    r.program = e->cfg->programs[pi].name;
                    e->r = r;
                    EndDialog(h, IDOK);
                    return TRUE;
                }
                case IDCANCEL:
                    EndDialog(h, IDCANCEL);
                    return TRUE;
            }
            break;
    }
    return FALSE;
}

// ---------------------------------------------------------------- main settings dialog

struct MainState {
    Config cfg;
    wstring saved;  // serialized config as loaded, to detect unsaved changes
    bool filling = false;
};
static MainState *g_ms;

static void refreshPrograms(HWND h, int sel) {
    const Config &c = g_ms->cfg;
    HWND lv = GetDlgItem(h, IDC_PROGS);
    ListView_DeleteAllItems(lv);
    for (size_t i = 0; i < c.programs.size(); i++)
        addRow(lv, (int)i, {c.programs[i].name, c.programs[i].path, c.programs[i].args});
    selectRow(lv, sel);
    comboFill(h, IDC_DEFAULT, programNames(c), programIndex(c, c.defaultProgram));
}

static void refreshRules(HWND h, int sel) {
    const Config &c = g_ms->cfg;
    HWND lv = GetDlgItem(h, IDC_RULES);
    g_ms->filling = true;
    ListView_DeleteAllItems(lv);
    for (size_t i = 0; i < c.rules.size(); i++) {
        const Rule &r = c.rules[i];
        wstring prog = r.program + (findProgram(c, r.program) ? L"" : L" (missing)");
        wstring keys = r.mods == KEYS_ANY ? wstring(L"\u2013") : r.mods == 0 ? wstring(L"none") : modsLabel(r.mods);
        addRow(lv, (int)i, {std::to_wstring(i + 1), describeRule(r), keys, prog});
        ListView_SetCheckState(lv, (int)i, r.enabled);
    }
    selectRow(lv, sel);
    g_ms->filling = false;
}

static void updateTest(HWND h) {
    const Config &c = g_ms->cfg;
    wstring path = trim(getText(h, IDC_TESTPATH), L" \t\"");
    wstring s;
    if (path.empty()) {
        s = L"Enter or browse to a file to see which program would open it.";
    } else {
        int mods = (IsDlgButtonChecked(h, IDC_TESTSHIFT) == BST_CHECKED ? KEY_SHIFT : 0) |
                   (IsDlgButtonChecked(h, IDC_TESTCTRL) == BST_CHECKED ? KEY_CTRL : 0) |
                   (IsDlgButtonChecked(h, IDC_TESTALT) == BST_CHECKED ? KEY_ALT : 0);
        int i = firstMatch(c, path, mods);
        wstring prog = i >= 0 ? c.rules[i].program : c.defaultProgram;
        s = L"\u2192 Opens in " + (prog.empty() ? wstring(L"(nothing)") : prog) + L"   (" +
            (i >= 0 ? L"rule " + std::to_wstring(i + 1) + L": " + describeRule(c.rules[i])
                    : wstring(L"no rule matched, using the default")) + L")";
    }
    SetDlgItemTextW(h, IDC_TESTRESULT, s.c_str());
}

static void refreshAll(HWND h, int progSel, int ruleSel) {
    refreshPrograms(h, progSel);
    refreshRules(h, ruleSel);
    updateTest(h);
}

static INT_PTR CALLBACK MainProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    Config &c = g_ms->cfg;
    switch (m) {
        case WM_INITDIALOG: {
            UINT dpi = GetDpiForWindow(h);
            HICON big = (HICON)LoadImageW(g_inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetricsForDpi(SM_CXICON, dpi),
                                          GetSystemMetricsForDpi(SM_CYICON, dpi), 0);
            HICON sm = (HICON)LoadImageW(g_inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetricsForDpi(SM_CXSMICON, dpi),
                                         GetSystemMetricsForDpi(SM_CYSMICON, dpi), 0);
            SendMessageW(h, WM_SETICON, ICON_BIG, (LPARAM)big);
            SendMessageW(h, WM_SETICON, ICON_SMALL, (LPARAM)sm);
            initList(h, IDC_PROGS, {L"Name", L"Program", L"Arguments"}, false);
            initList(h, IDC_RULES, {L"#", L"Condition", L"Keys", L"Open with"}, true);
            fitAllColumns(h);
            refreshAll(h, -1, -1);
            wstring where = g_cfgPath.empty() ? exeDirIni() + L"  (not saved yet)" : g_cfgPath;
            SetDlgItemTextW(h, IDC_CFGPATH, where.c_str());
            return TRUE;
        }
        case WM_COMMAND: {
            int id = LOWORD(w);
            switch (id) {
                case IDC_PADD:
                case IDC_PEDIT: {
                    int i = selectedRow(h, IDC_PROGS);
                    if (id == IDC_PEDIT && i < 0) return TRUE;
                    ProgEdit e{id == IDC_PEDIT ? c.programs[i] : Program{L"", L"", L"%*"}, &c, id == IDC_PEDIT ? i : -1};
                    if (DialogBoxParamW(g_inst, MAKEINTRESOURCEW(IDD_PROGRAM), h, ProgProc, (LPARAM)&e) != IDOK) return TRUE;
                    if (id == IDC_PEDIT) {
                        wstring old = c.programs[i].name;
                        if (old != e.p.name) {
                            for (auto &r : c.rules)
                                if (ieq(r.program, old)) r.program = e.p.name;
                            if (ieq(c.defaultProgram, old)) c.defaultProgram = e.p.name;
                        }
                        c.programs[i] = e.p;
                    } else {
                        c.programs.push_back(e.p);
                        i = (int)c.programs.size() - 1;
                        if (c.defaultProgram.empty()) c.defaultProgram = e.p.name;
                    }
                    refreshAll(h, i, selectedRow(h, IDC_RULES));
                    return TRUE;
                }
                case IDC_PDEL: {
                    int i = selectedRow(h, IDC_PROGS);
                    if (i < 0) return TRUE;
                    const wstring name = c.programs[i].name;
                    for (size_t k = 0; k < c.rules.size(); k++) {
                        if (ieq(c.rules[k].program, name)) {
                            msg(h, L"\"" + name + L"\" is used by rule " + std::to_wstring(k + 1) +
                                       L".\nPoint that rule at another program or remove it first.", MB_ICONWARNING);
                            return TRUE;
                        }
                    }
                    c.programs.erase(c.programs.begin() + i);
                    if (ieq(c.defaultProgram, name)) c.defaultProgram = c.programs.empty() ? L"" : c.programs[0].name;
                    refreshAll(h, i < (int)c.programs.size() ? i : i - 1, selectedRow(h, IDC_RULES));
                    return TRUE;
                }
                case IDC_RADD:
                case IDC_REDIT: {
                    if (c.programs.empty()) {
                        msg(h, L"Add a program first.", MB_ICONINFORMATION);
                        return TRUE;
                    }
                    int i = selectedRow(h, IDC_RULES);
                    if (id == IDC_REDIT && i < 0) return TRUE;
                    RuleEdit e{id == IDC_REDIT ? c.rules[i] : Rule(), &c, id == IDC_RADD};
                    if (id == IDC_RADD) {
                        // Suggest a program other than the default, since that's what a rule is usually for.
                        e.r.program = c.programs[0].name;
                        for (auto &p : c.programs)
                            if (!ieq(p.name, c.defaultProgram)) { e.r.program = p.name; break; }
                    }
                    if (DialogBoxParamW(g_inst, MAKEINTRESOURCEW(IDD_RULE), h, RuleProc, (LPARAM)&e) != IDOK) return TRUE;
                    if (id == IDC_RADD) {
                        c.rules.push_back(e.r);
                        i = (int)c.rules.size() - 1;
                    } else {
                        c.rules[i] = e.r;
                    }
                    refreshRules(h, i);
                    updateTest(h);
                    return TRUE;
                }
                case IDC_RDEL: {
                    int i = selectedRow(h, IDC_RULES);
                    if (i < 0) return TRUE;
                    c.rules.erase(c.rules.begin() + i);
                    refreshRules(h, i < (int)c.rules.size() ? i : i - 1);
                    updateTest(h);
                    return TRUE;
                }
                case IDC_RUP:
                case IDC_RDOWN: {
                    int i = selectedRow(h, IDC_RULES);
                    int j = i + (id == IDC_RUP ? -1 : 1);
                    if (i < 0 || j < 0 || j >= (int)c.rules.size()) return TRUE;
                    std::swap(c.rules[i], c.rules[j]);
                    refreshRules(h, j);
                    updateTest(h);
                    return TRUE;
                }
                case IDC_DEFAULT:
                    if (HIWORD(w) == CBN_SELCHANGE) {
                        int i = comboSel(h, IDC_DEFAULT);
                        if (i >= 0) c.defaultProgram = c.programs[i].name;
                        updateTest(h);
                    }
                    return TRUE;
                case IDC_TESTPATH:
                    if (HIWORD(w) == EN_CHANGE) updateTest(h);
                    return TRUE;
                case IDC_TESTSHIFT:
                case IDC_TESTCTRL:
                case IDC_TESTALT:
                    updateTest(h);
                    return TRUE;
                case IDC_TESTBROWSE: {
                    wstring path = getText(h, IDC_TESTPATH);
                    if (pickPath(h, false, path, L"3MF files (*.3mf)", L"*.3mf")) SetDlgItemTextW(h, IDC_TESTPATH, path.c_str());
                    return TRUE;
                }
                case IDOK:
                    if (!c.programs.empty() && !findProgram(c, c.defaultProgram)) {
                        msg(h, L"Choose the program to use when no rule matches.", MB_ICONWARNING);
                        SetFocus(GetDlgItem(h, IDC_DEFAULT));
                        return TRUE;
                    }
                    if (!saveConfig(c)) {
                        msg(h, L"Could not save the settings to\n" + exeDirIni() + L"\nor " + appDataIni(), MB_ICONERROR);
                        return TRUE;
                    }
                    EndDialog(h, IDOK);
                    return TRUE;
                case IDCANCEL:
                    if (serialize(c) != g_ms->saved &&
                        msg(h, L"Discard your changes?", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES)
                        return TRUE;
                    EndDialog(h, IDCANCEL);
                    return TRUE;
            }
            break;
        }
        case WM_DPICHANGED:
            // The dialog manager rescales the controls; refit the list columns once that's done.
            PostMessageW(h, WM_APP_FITCOLUMNS, 0, 0);
            return FALSE;
        case WM_APP_FITCOLUMNS:
            fitAllColumns(h);
            return TRUE;
        case WM_NOTIFY: {
            NMHDR *nh = (NMHDR *)l;
            if (nh->code == NM_DBLCLK) {
                if (nh->idFrom == IDC_PROGS) PostMessageW(h, WM_COMMAND, IDC_PEDIT, 0);
                if (nh->idFrom == IDC_RULES) PostMessageW(h, WM_COMMAND, IDC_REDIT, 0);
            } else if (nh->code == LVN_ITEMCHANGED && nh->idFrom == IDC_RULES && !g_ms->filling) {
                NMLISTVIEW *lv = (NMLISTVIEW *)l;
                if ((lv->uChanged & LVIF_STATE) && ((lv->uNewState ^ lv->uOldState) & LVIS_STATEIMAGEMASK) &&
                    lv->iItem >= 0 && lv->iItem < (int)c.rules.size()) {
                    c.rules[lv->iItem].enabled = ((lv->uNewState & LVIS_STATEIMAGEMASK) >> 12) == 2;
                    updateTest(h);
                }
            } else if (nh->code == LVN_KEYDOWN && ((NMLVKEYDOWN *)l)->wVKey == VK_DELETE) {
                if (nh->idFrom == IDC_PROGS) PostMessageW(h, WM_COMMAND, IDC_PDEL, 0);
                if (nh->idFrom == IDC_RULES) PostMessageW(h, WM_COMMAND, IDC_RDEL, 0);
            }
            return FALSE;
        }
    }
    return FALSE;
}

static int runSettings(const Config &cfg) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    INITCOMMONCONTROLSEX icc = {sizeof icc, ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    MainState ms;
    ms.cfg = cfg;
    ms.saved = serialize(cfg);
    g_ms = &ms;
    INT_PTR r = DialogBoxParamW(g_inst, MAKEINTRESOURCEW(IDD_MAIN), nullptr, MainProc, 0);
    CoUninitialize();
    return r == IDOK ? 0 : 1;
}

// ---------------------------------------------------------------- entry

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, LPWSTR, int) {
    // Read the modifier keys first, while the user is still holding them.
    int mods = heldMods();
    g_inst = hi;

    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool settings = !argv || argc < 2 || ieq(argv[1], L"--settings");
    Config cfg = loadConfig();

    if (!settings) {
        wstring file = argv[1];
        int i = firstMatch(cfg, file, mods);
        const Program *p = findProgram(cfg, i >= 0 ? cfg.rules[i].program : cfg.defaultProgram);
        if (p) return launch(*p, file, argsAfterExe(GetCommandLineW())) ? 0 : 1;
        if (msg(nullptr, L"No program is set up to open\n" + file + L"\n\nOpen the printer_fork settings?",
                MB_YESNO | MB_ICONWARNING) != IDYES)
            return 1;
    }
    return runSettings(cfg);
}
