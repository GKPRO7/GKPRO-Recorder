#pragma once
// Windows Graphics Capture wrapper: captures one window (or a monitor) straight into a GPU texture.
// Works with Vulkan/DXVK and DirectX windows, includes the mouse cursor.

#include <windows.h>
#include <d3d11.h>
#include <memory>
#include <cstdint>

class WgcCapture {
public:
    WgcCapture();
    ~WgcCapture();

    static bool Supported();

    // hwnd != null: capture that window. Otherwise capture monitor `mon`.
    // On success returns the capture size in w/h.
    bool Create(ID3D11Device* dev, ID3D11DeviceContext* ctx, HWND hwnd, HMONITOR mon, UINT& w, UINT& h);

    // Start copying every new frame into `target` (BGRA texture of size w x h).
    void Begin(ID3D11Texture2D* target, UINT w, UINT h);
    void Stop();

    uint64_t Frames() const;   // number of frames received so far
    bool Alive() const;        // false once the captured window was closed

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};
