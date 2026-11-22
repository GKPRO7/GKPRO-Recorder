#pragma once
#include <atomic>
#include <string>
#include <thread>

struct RecorderConfig {
    int fps = 30;
    int outWidth = 1280;       // 720p default
    int outHeight = 720;
    int bitrateKbps = 5000;
    void* targetWindow = nullptr;   // HWND to record; null = whole monitor
    void* targetMonitor = nullptr;  // HMONITOR used when targetWindow is null
    bool gameAudio = true;          // system/game sound (WASAPI loopback)
    bool mic = false;               // microphone as a separate track
    unsigned long hookPid = 0;      // != 0: use game capture (hook) on this process
    std::wstring hookDir;           // folder holding gkhook64.dll / gkhook32.dll / gkinject32.exe
};

class Recorder {
public:
    ~Recorder() { Stop(); }
    bool Start(const std::wstring& path, const RecorderConfig& cfg);  // path = .mkv file
    void Stop();
    bool IsRecording() const { return m_running.load(); }
    const std::wstring& LastError() const { return m_error; }
    const std::wstring& Warning() const { return m_warning; }  // non-fatal notice (e.g. hook failed, fell back)

private:
    void Loop(std::wstring path, RecorderConfig cfg);
    const wchar_t* Session(const std::wstring& path, const RecorderConfig& cfg);

    std::thread m_thread;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_ready{false};
    std::atomic<bool> m_failed{false};
    std::wstring m_error;
    std::wstring m_warning;
};
