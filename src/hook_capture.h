#pragma once
// Recorder side of "game capture": injects the hook DLL into the game and reads its frames
// from a shared GPU texture. Works with exclusive-fullscreen D3D9, D3D10/11 and OpenGL games.

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <string>
#include "hook_shared.h"

class HookCapture {
public:
    ~HookCapture() { Stop(); }

    // Creates the shared memory, injects the hook and waits for the first frame (max ~4 s).
    bool Start(DWORD pid, int fps, const std::wstring& exeDir, ID3D11Device* dev, std::string& err);
    void Stop();

    bool Update();   // true when the shared texture changed (new size/format): rebuild the scaler
    bool Alive() const;
    ID3D11Texture2D* Texture() const { return m_tex.Get(); }
    UINT Width() const { return m_w; }
    UINT Height() const { return m_h; }
    uint64_t Frames() const { return m_shm ? (uint64_t)m_shm->frameCounter : 0; }
    int Api() const { return m_shm ? (int)m_shm->api : 0; }

    bool Lock(DWORD ms);  // keyed-mutex guard while the scaler reads the texture
    void Unlock();

private:
    HANDLE m_map = nullptr;
    GkHookShared* m_shm = nullptr;
    HANDLE m_proc = nullptr;
    ID3D11Device* m_dev = nullptr;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_tex;
    Microsoft::WRL::ComPtr<IDXGIKeyedMutex> m_km;
    UINT m_w = 0, m_h = 0;
    uint32_t m_gen = 0;
    bool m_locked = false;
};
