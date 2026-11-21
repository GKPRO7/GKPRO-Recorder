// GKPRO Recorder - tiny tray app. F9 = start/stop. Right-click tray icon for menu.
// No window, no UI framework: idle RAM stays very small.

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <string>
#include "recorder.h"

#pragma comment(lib, "shell32.lib")

namespace {
constexpr UINT WM_TRAY = WM_APP + 1;
constexpr int HOTKEY_ID = 1;
constexpr UINT ID_TOGGLE = 1001, ID_OPEN = 1002, ID_EXIT = 1003;

Recorder g_rec;
NOTIFYICONDATAW g_nid = {};
RecorderConfig g_cfg;  // defaults: 30 fps, auto bitrate (low-end friendly)

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

std::wstring NewFilePath() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t name[64];
    swprintf_s(name, L"\\GKPRO_%04d-%02d-%02d_%02d-%02d-%02d.mkv",
               t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    return OutputDir() + name;
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

void Toggle() {
    if (g_rec.IsRecording()) {
        g_rec.Stop();
        SetTip(L"GKPRO Recorder - idle (F9 to record)");
        Balloon(L"GKPRO Recorder", L"Recording saved to Videos\\GKPRO Recorder");
    } else {
        if (g_rec.Start(NewFilePath(), g_cfg)) {
            SetTip(L"GKPRO Recorder - RECORDING (F9 to stop)");
        } else {
            Balloon(L"GKPRO Recorder - error", g_rec.LastError().c_str());
        }
    }
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_HOTKEY:
        if (wp == HOTKEY_ID) Toggle();
        return 0;
    case WM_TRAY:
        if (lp == WM_RBUTTONUP || lp == WM_LBUTTONUP) {
            HMENU menu = CreatePopupMenu();
            AppendMenuW(menu, MF_STRING, ID_TOGGLE,
                        g_rec.IsRecording() ? L"Stop recording (F9)" : L"Start recording (F9)");
            AppendMenuW(menu, MF_STRING, ID_OPEN, L"Open recordings folder");
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING, ID_EXIT, L"Exit");
            POINT p;
            GetCursorPos(&p);
            SetForegroundWindow(hwnd);
            TrackPopupMenu(menu, TPM_RIGHTBUTTON, p.x, p.y, 0, hwnd, nullptr);
            DestroyMenu(menu);
        }
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_TOGGLE: Toggle(); break;
        case ID_OPEN: ShellExecuteW(nullptr, L"open", OutputDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL); break;
        case ID_EXIT: DestroyWindow(hwnd); break;
        }
        return 0;
    case WM_DESTROY:
        g_rec.Stop();
        UnregisterHotKey(hwnd, HOTKEY_ID);
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
}  // namespace

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int) {
    // Single instance only.
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"GKPRORecorder_SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"GKPRORecorderWnd";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"GKPRO Recorder", 0, 0, 0, 0, 0,
                                HWND_MESSAGE, nullptr, hInst, nullptr);

    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAY;
    g_nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(g_nid.szTip, L"GKPRO Recorder - idle (F9 to record)");
    Shell_NotifyIconW(NIM_ADD, &g_nid);

    if (!RegisterHotKey(hwnd, HOTKEY_ID, MOD_NOREPEAT, VK_F9))
        Balloon(L"GKPRO Recorder", L"F9 is in use by another app. Use the tray menu instead.");

    MessageBoxW(nullptr,
                L"GKPRO Recorder is running in the system tray (near the clock, click the ^ arrow if hidden).\n\n"
                L"Press F9 to start and stop recording.\n"
                L"Videos are saved in Videos\\GKPRO Recorder.",
                L"GKPRO Recorder", MB_OK | MB_ICONINFORMATION);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    CloseHandle(mutex);
    return 0;
}
