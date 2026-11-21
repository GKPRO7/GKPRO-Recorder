#pragma once
// Captures one audio source with WASAPI (game audio via loopback, or the microphone),
// encodes it to AAC with the Windows hardware-independent AAC encoder, and feeds the MKV muxer.
// No UI, one small thread per source.

#include <atomic>
#include <cstdint>
#include <thread>

class MkvWriter;

class AudioCapture {
public:
    AudioCapture(bool loopback, int trackNumber) : m_loopback(loopback), m_track(trackNumber) {}
    ~AudioCapture() { Stop(); }

    // t0_100ns: the recording's start time on the QPC clock, in 100 ns units.
    void Start(MkvWriter* mkv, int64_t t0_100ns);
    void Stop();

private:
    void Run();

    bool m_loopback;
    int m_track;
    MkvWriter* m_mkv = nullptr;
    int64_t m_t0 = 0;
    std::atomic<bool> m_run{false};
    std::thread m_thread;
};
