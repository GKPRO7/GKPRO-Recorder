#pragma once
#include <atomic>
#include <string>
#include <thread>

struct RecorderConfig {
    int fps = 30;            // 30 is the low-end default; use 60 on stronger PCs
    int bitrateKbps = 0;     // 0 = auto (based on resolution and fps)
};

class Recorder {
public:
    ~Recorder() { Stop(); }

    // Starts recording the primary monitor to an MP4 file. Returns false on failure.
    bool Start(const std::wstring& path, const RecorderConfig& cfg);
    void Stop();
    bool IsRecording() const { return m_running.load(); }
    const std::wstring& LastError() const { return m_error; }

private:
    void Loop(std::wstring path, RecorderConfig cfg);

    std::thread m_thread;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_ready{false};
    std::atomic<bool> m_failed{false};
    std::wstring m_error;
};
