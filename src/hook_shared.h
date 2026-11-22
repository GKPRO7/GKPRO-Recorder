#pragma once
// Shared between GKPRORecorder.exe and the injected game-capture hook (gkhook64.dll / gkhook32.dll).
#include <windows.h>
#include <cstdint>

#define GKHOOK_MAGIC 0x474B5052u  // 'GKPR'

struct GkHookShared {
    volatile uint32_t magic;
    volatile uint32_t wantFrames;    // recorder -> hook: 1 while recording
    volatile uint32_t targetFps;     // recorder -> hook: the hook never copies faster than this
    volatile uint32_t api;           // hook -> recorder: 1 = D3D10/11 (DXGI), 2 = D3D9, 3 = OpenGL
    volatile uint32_t generation;    // bumped whenever the shared texture is (re)created
    volatile uint32_t sharedHandle;  // legacy DXGI shared handle (valid across processes)
    volatile uint32_t width;
    volatile uint32_t height;
    volatile uint32_t dxgiFormat;
    volatile uint32_t hasKeyedMutex;
    volatile uint64_t frameCounter;  // grows by one for every frame the hook delivered
};

inline void GkHookShmName(unsigned long pid, wchar_t* out) {
    wsprintfW(out, L"Local\\GKPRO_HOOK_%lu", pid);  // out must hold >= 48 wchar_t
}
