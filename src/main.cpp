// GKPRO Recorder v0.3 - tiny tray app. F9 = start/stop. Right-click the tray icon for options.
// No window, no UI framework: idle RAM stays very small.

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <atomic>
#include <string>
#include <thread>
#include "recorder.h"
#include "mp4_remux.h"

namespace {
constexpr UINT WM_TRAY = WM_APP + 1;
constexpr UINT WM_REMUX_DONE = WM_APP + 2;
constexpr int HOTKEY_ID = 1;
constexpr UINT_PTR TIMER_POLL = 1;
enum : UINT {
    ID_TOGGLE = 1001, ID_OPEN = 1002, ID_EXIT = 1003,
    ID_MODE_WINDOW = 1010, ID_MODE_SCREEN = 1011,
    ID_GAME_AUDIO = 1012, ID_MIC = 1013, ID_AUTO_MP4 = 1014, ID_DEL_MKV = 1015,
    ID_MODE_HOOK = 1016, ID_OPEN_LOG = 1017, ID_PRESET_BASE = 1100, ID_PRESET_CUSTOM = 1199
};

struct Settings {
    int captureMode = 0;   // 0 = game window (Windows Graphics Capture), 1 = full screen, 2 = game hook (fullscreen/OpenGL/D3D9/D3D11)
    int preset = 1;        // index into kPresets, or 99 = custom values from settings.ini
    int gameAudio = 1;
    int mic = 0;
    int autoMp4 = 1;
    int deleteMkv = 0;
    int fps = 30;
    int width = 1280;
    int height = 720;
    int bitrate = 5000;
};

Settings g_set;
Recorder g_rec;
NOTIFYICONDATAW g_nid = {};
HWND g_hwnd = nullptr;
bool g_active = false;
std::wstring g_curFile;
std::atomic<int> g_remuxing{0};

// ---------- settings (tiny ini in %LOCALAPPDATA%) ----------
std::wstring IniPath() {
    wchar_t buf[MAX_PATH] = {};
    PWSTR p = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p))) {
        dir = p;
        CoTaskMemFree(p);
    } else {
        GetTempPathW(MAX_PATH, buf);
        dir = buf;
    }
    dir += L"\\GKPRO Recorder";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\settings.ini";
}

void LoadSettings() {
    std::wstring ini = IniPath();
    auto rd = [&](const wchar_t* k, int d) { return (int)GetPrivateProfileIntW(L"GKPRO", k, d, ini.c_str()); };
    g_set.captureMode = rd(L"CaptureMode", 0);
    g_set.preset = rd(L"Preset", 1);
    if (g_set.captureMode > 2) g_set.captureMode = 0;
    if (g_set.preset > 5 && g_set.preset != 99) g_set.preset = 1;
    g_set.gameAudio = rd(L"GameAudio", 1);
    g_set.mic = rd(L"Microphone", 0);
    g_set.autoMp4 = rd(L"AutoMp4", 1);
    g_set.deleteMkv = rd(L"DeleteMkvAfterMp4", 0);
    g_set.fps = rd(L"Fps", 30);
    g_set.width = rd(L"Width", 1280);
    g_set.height = rd(L"Height", 720);
    g_set.bitrate = rd(L"BitrateKbps", 5000);
    if (g_set.fps < 10 || g_set.fps > 120) g_set.fps = 30;
    if (g_set.width < 320 || g_set.height < 240) { g_set.width = 1280; g_set.height = 720; }
    if (g_set.bitrate < 500) g_set.bitrate = 5000;
}

void SaveSettings() {
    std::wstring ini = IniPath();
    auto wr = [&](const wchar_t* k, int v) { WritePrivateProfileStringW(L"GKPRO", k, std::to_wstring(v).c_str(), ini.c_str()); };
    wr(L"CaptureMode", g_set.captureMode);
    wr(L"Preset", g_set.preset);
    wr(L"GameAudio", g_set.gameAudio);
    wr(L"Microphone", g_set.mic);
    wr(L"AutoMp4", g_set.autoMp4);
    wr(L"DeleteMkvAfterMp4", g_set.deleteMkv);
    wr(L"Fps", g_set.fps);
    wr(L"Width", g_set.width);
    wr(L"Height", g_set.height);
    wr(L"BitrateKbps", g_set.bitrate);
}

// ---------- quality presets ----------
struct Preset { const wchar_t* name; int w, h, fps, kbps; };
const Preset kPresets[] = {
    {L"Low-end: 540p, 30 fps (2.5 Mbps)", 960, 540, 30, 2500},
    {L"720p, 30 fps (5 Mbps)  - default", 1280, 720, 30, 5000},
    {L"720p, 60 fps (8 Mbps)", 1280, 720, 60, 8000},
    {L"1080p, 30 fps (8 Mbps)", 1920, 1080, 30, 8000},
    {L"1080p, 60 fps (12 Mbps)", 1920, 1080, 60, 12000},
    {L"Native: game resolution, 30 fps (sharpest for old games)", 0, 0, 30, 4000},
};

// ---------- helpers ----------
std::wstring ExeDir() {
    wchar_t p[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    std::wstring s = p;
    size_t k = s.find_last_of(L'\\');
    return k == std::wstring::npos ? L"." : s.substr(0, k);
}

// Gives unused memory back to Windows (keeps the idle footprint in Task Manager tiny).
void TrimMemory() { SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1); }

std::wstring OutputDir() {
    PWSTR videos = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Videos, 0, nullptr, &videos))) {
        dir = videos;
        CoTaskMemFree(videos);
    } else {
        dir = L".";
    }
    dir += L"\\GKPRO Recorder";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

std::wstring Sanitize(std::wstring s) {
    for (auto& c : s) if (wcschr(L"\\/:*?\"<>|", c) || c < 32) c = L' ';
    while (!s.empty() && (s.back() == L' ' || s.back() == L'.')) s.pop_back();
    while (!s.empty() && s.front() == L' ') s.erase(s.begin());
    if (s.size() > 40) s.resize(40);
    return s;
}

// The window to record: the one in front when F9 is pressed (your game), if it is a real app window.
HWND PickWindow() {
    HWND fg = GetForegroundWindow();
    if (!fg) return nullptr;
    fg = GetAncestor(fg, GA_ROOT);
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    if (pid == GetCurrentProcessId()) return nullptr;
    if (!IsWindowVisible(fg) || IsIconic(fg)) return nullptr;
    wchar_t cls[64] = {};
    GetClassNameW(fg, cls, 64);
    if (!wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW") ||
        !wcscmp(cls, L"Shell_TrayWnd") || !wcscmp(cls, L"Shell_SecondaryTrayWnd"))
        return nullptr;
    return fg;
}

std::wstring NewFilePath(HWND win) {
    std::wstring name = L"GKPRO";
    if (win) {
        wchar_t title[128] = {};
        GetWindowTextW(win, title, 128);
        std::wstring t = Sanitize(title);
        if (!t.empty()) name = t;
    } else {
        name = L"Desktop";
    }
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t stamp[48];
    swprintf_s(stamp, L"_%04d-%02d-%02d_%02d-%02d-%02d.mkv", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    return OutputDir() + L"\\" + name + stamp;
}

void SetTip(const wchar_t* text) {
    wcsncpy_s(g_nid.szTip, text, _TRUNCATE);
    g_nid.uFlags = NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

void Balloon(const wchar_t* title, const wchar_t* msg) {
    g_nid.uFlags = NIF_INFO;
    wcsncpy_s(g_nid.szInfoTitle, title, _TRUNCATE);
    wcsncpy_s(g_nid.szInfo, msg, _TRUNCATE);
    g_nid.dwInfoFlags = NIIF_INFO | NIIF_NOSOUND;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

// ---------- MKV -> MP4 in the background (low priority so it never disturbs a running game) ----------
void StartRemux(std::wstring mkv, bool deleteAfter) {
    ++g_remuxing;
    std::thread([mkv, deleteAfter]() {
        SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
        std::wstring mp4 = mkv.substr(0, mkv.size() - 4) + L".mp4";
        std::wstring err;
        bool ok = RemuxMkvToMp4(mkv, mp4, err);
        if (ok && deleteAfter) DeleteFileW(mkv.c_str());
        size_t slash = mp4.find_last_of(L'\\');
        std::wstring shown = ok ? mp4.substr(slash == std::wstring::npos ? 0 : slash + 1) : err;
        PostMessageW(g_hwnd, WM_REMUX_DONE, ok ? 1 : 0, (LPARAM) new std::wstring(shown));
        --g_remuxing;
    }).detach();
}

void FinishRecording() {
    g_rec.Stop();
    g_active = false;
    SetTip(L"GKPRO Recorder - idle (F9 to record)");
    if (g_set.autoMp4) {
        Balloon(L"GKPRO Recorder", L"Saved. Converting to MP4...");
        StartRemux(g_curFile, g_set.deleteMkv != 0);
    } else {
        Balloon(L"GKPRO Recorder", L"Recording saved to Videos\\GKPRO Recorder");
    }
    TrimMemory();
}

void Toggle() {
    if (g_active) {
        FinishRecording();
        return;
    }
    // Window and game-hook modes record the window that is in front when F9 is pressed.
    HWND win = (g_set.captureMode != 1) ? PickWindow() : nullptr;
    if (g_set.captureMode == 2 && !win) {
        Balloon(L"GKPRO Recorder", L"Click your game window first, then press F9 (game capture needs a game window).");
        return;
    }

    RecorderConfig cfg;
    if (g_set.preset >= 0 && g_set.preset < (int)(sizeof(kPresets) / sizeof(kPresets[0]))) {
        const Preset& p = kPresets[g_set.preset];
        cfg.outWidth = p.w; cfg.outHeight = p.h; cfg.fps = p.fps; cfg.bitrateKbps = p.kbps;
    } else {  // custom values from settings.ini
        cfg.outWidth = g_set.width; cfg.outHeight = g_set.height; cfg.fps = g_set.fps; cfg.bitrateKbps = g_set.bitrate;
    }
    cfg.targetWindow = win;
    cfg.targetMonitor = nullptr;
    cfg.gameAudio = g_set.gameAudio != 0;
    cfg.mic = g_set.mic != 0;
    if (g_set.captureMode == 2 && win) {
        DWORD pid = 0;
        GetWindowThreadProcessId(win, &pid);
        cfg.hookPid = pid;
        cfg.hookDir = ExeDir();
    }

    g_curFile = NewFilePath(win);
    if (g_rec.Start(g_curFile, cfg)) {
        g_active = true;
        SetTip(L"GKPRO Recorder - RECORDING (F9 to stop)");
        if (!g_rec.Warning().empty())
            Balloon(L"GKPRO Recorder", g_rec.Warning().c_str());
        else if (g_set.captureMode == 0 && !win)
            Balloon(L"GKPRO Recorder", L"No game window in front, so the whole screen is being recorded.");
    } else {
        Balloon(L"GKPRO Recorder - error", g_rec.LastError().c_str());
    }
    TrimMemory();
}

void ShowMenu(HWND hwnd) {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, ID_TOGGLE, g_active ? L"Stop recording (F9)" : L"Start recording (F9)");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (g_set.captureMode == 0 ? MF_CHECKED : 0), ID_MODE_WINDOW, L"Capture: game window (safe, no injection)");
    AppendMenuW(menu, MF_STRING | (g_set.captureMode == 2 ? MF_CHECKED : 0), ID_MODE_HOOK, L"Capture: game hook (fullscreen, OpenGL, DX9/DX11)");
    AppendMenuW(menu, MF_STRING | (g_set.captureMode == 1 ? MF_CHECKED : 0), ID_MODE_SCREEN, L"Capture: full screen");
    HMENU presets = CreatePopupMenu();
    for (int i = 0; i < (int)(sizeof(kPresets) / sizeof(kPresets[0])); ++i)
        AppendMenuW(presets, MF_STRING | (g_set.preset == i ? MF_CHECKED : 0), ID_PRESET_BASE + i, kPresets[i].name);
    AppendMenuW(presets, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(presets, MF_STRING | (g_set.preset == 99 ? MF_CHECKED : 0), ID_PRESET_CUSTOM, L"Custom (from settings.ini)");
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)presets, L"Quality preset");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (g_set.gameAudio ? MF_CHECKED : 0), ID_GAME_AUDIO, L"Record game audio");
    AppendMenuW(menu, MF_STRING | (g_set.mic ? MF_CHECKED : 0), ID_MIC, L"Record microphone");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (g_set.autoMp4 ? MF_CHECKED : 0), ID_AUTO_MP4, L"Convert to MP4 after recording");
    AppendMenuW(menu, MF_STRING | (g_set.deleteMkv ? MF_CHECKED : 0), ID_DEL_MKV, L"Delete the MKV after converting");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_OPEN, L"Open recordings folder");
    AppendMenuW(menu, MF_STRING, ID_OPEN_LOG, L"Open last log (for bug reports)");
    AppendMenuW(menu, MF_STRING, ID_EXIT, L"Exit");
    POINT p;
    GetCursorPos(&p);
    SetForegroundWindow(hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, p.x, p.y, 0, hwnd, nullptr);
    DestroyMenu(menu);  // also destroys the preset submenu
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_HOTKEY:
        if (wp == HOTKEY_ID) Toggle();
        return 0;
    case WM_TIMER:
        // Recording ended by itself (the game window was closed): save and convert.
        if (wp == TIMER_POLL && g_active && !g_rec.IsRecording()) FinishRecording();
        return 0;
    case WM_TRAY:
        if (lp == WM_RBUTTONUP || lp == WM_LBUTTONUP) ShowMenu(hwnd);
        return 0;
    case WM_REMUX_DONE: {
        std::wstring* s = (std::wstring*)lp;
        if (wp) {
            std::wstring m = L"MP4 ready: " + *s;
            Balloon(L"GKPRO Recorder", m.c_str());
        } else {
            std::wstring m = L"MP4 conversion failed (" + *s + L"). The MKV is kept.";
            Balloon(L"GKPRO Recorder", m.c_str());
        }
        delete s;
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_TOGGLE: Toggle(); break;
        case ID_OPEN: ShellExecuteW(nullptr, L"open", OutputDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL); break;
        case ID_OPEN_LOG: {
            if (g_curFile.empty()) { Balloon(L"GKPRO Recorder", L"No recording yet, so there is no log."); break; }
            std::wstring a = L"\"" + g_curFile + L".log\"";
            ShellExecuteW(nullptr, L"open", L"notepad.exe", a.c_str(), nullptr, SW_SHOWNORMAL);
            std::wstring hl = g_curFile + L".hook.log";
            if (GetFileAttributesW(hl.c_str()) != INVALID_FILE_ATTRIBUTES) {
                std::wstring b = L"\"" + hl + L"\"";
                ShellExecuteW(nullptr, L"open", L"notepad.exe", b.c_str(), nullptr, SW_SHOWNORMAL);
            }
            break;
        }
        case ID_EXIT: DestroyWindow(hwnd); break;
        case ID_MODE_WINDOW: g_set.captureMode = 0; SaveSettings(); break;
        case ID_MODE_SCREEN: g_set.captureMode = 1; SaveSettings(); break;
        case ID_MODE_HOOK:
            if (MessageBoxW(hwnd,
                    L"Game capture injects a small DLL into the game to record exclusive-fullscreen, OpenGL and older DirectX games.\n\n"
                    L"Do NOT use it with online games that have anti-cheat (EasyAntiCheat, BattlEye, Vanguard, ...): it could get your account banned.\n\n"
                    L"Use game capture?",
                    L"GKPRO Recorder - game capture", MB_YESNO | MB_ICONWARNING) == IDYES) {
                g_set.captureMode = 2;
                SaveSettings();
            }
            break;
        case ID_PRESET_CUSTOM: g_set.preset = 99; SaveSettings(); break;
        case ID_GAME_AUDIO: g_set.gameAudio = !g_set.gameAudio; SaveSettings(); break;
        case ID_MIC: g_set.mic = !g_set.mic; SaveSettings(); break;
        default:
            if (LOWORD(wp) >= ID_PRESET_BASE && LOWORD(wp) < ID_PRESET_BASE + (int)(sizeof(kPresets) / sizeof(kPresets[0]))) {
                g_set.preset = LOWORD(wp) - ID_PRESET_BASE;
                SaveSettings();
            }
            break;
        case ID_AUTO_MP4: g_set.autoMp4 = !g_set.autoMp4; SaveSettings(); break;
        case ID_DEL_MKV: g_set.deleteMkv = !g_set.deleteMkv; SaveSettings(); break;
        }
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, TIMER_POLL);
        if (g_active) FinishRecording();
        // let a running MP4 conversion finish (up to ~2 min) so the file is not left half-written
        for (int i = 0; i < 1200 && g_remuxing.load() > 0; ++i) Sleep(100);
        UnregisterHotKey(hwnd, HOTKEY_ID);
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
}  // namespace

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int) {
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"GKPRORecorder_SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    SetProcessDPIAware();  // window sizes in real pixels
    LoadSettings();
    SaveSettings();  // creates the ini so it can be edited by hand

    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"GKPRORecorderWnd";
    RegisterClassW(&wc);
    g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"GKPRO Recorder", 0, 0, 0, 0, 0,
                             HWND_MESSAGE, nullptr, hInst, nullptr);

    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAY;
    g_nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(g_nid.szTip, L"GKPRO Recorder - idle (F9 to record)");
    Shell_NotifyIconW(NIM_ADD, &g_nid);

    if (!RegisterHotKey(g_hwnd, HOTKEY_ID, MOD_NOREPEAT, VK_F9))
        Balloon(L"GKPRO Recorder", L"F9 is in use by another app. Use the tray menu instead.");
    else
        Balloon(L"GKPRO Recorder v0.3", L"Running in the tray. Click your game, then press F9 to record it.");

    SetTimer(g_hwnd, TIMER_POLL, 1000, nullptr);
    TrimMemory();

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    CloseHandle(mutex);
    return 0;
}
