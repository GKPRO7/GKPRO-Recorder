#include <unknwn.h>
#include "wgc_capture.h"

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <dxgi.h>
#include <atomic>
#include <algorithm>

namespace wgc = winrt::Windows::Graphics::Capture;
namespace wdx = winrt::Windows::Graphics::DirectX;
namespace wd3d = winrt::Windows::Graphics::DirectX::Direct3D11;

struct WgcCapture::Impl {
    ID3D11DeviceContext* ctx = nullptr;
    wd3d::IDirect3DDevice d3dDevice{nullptr};
    wgc::GraphicsCaptureItem item{nullptr};
    wgc::Direct3D11CaptureFramePool pool{nullptr};
    wgc::GraphicsCaptureSession session{nullptr};
    wgc::Direct3D11CaptureFramePool::FrameArrived_revoker frameRevoker;
    wgc::GraphicsCaptureItem::Closed_revoker closedRevoker;
    std::atomic<ID3D11Texture2D*> target{nullptr};
    std::atomic<UINT> targetW{0}, targetH{0};
    std::atomic<uint64_t> frames{0};
    std::atomic<bool> alive{true};
    winrt::Windows::Graphics::SizeInt32 lastSize{0, 0};
    bool created = false;
};

WgcCapture::WgcCapture() : m(new Impl) {}
WgcCapture::~WgcCapture() { Stop(); }

bool WgcCapture::Supported() {
    try {
        return wgc::GraphicsCaptureSession::IsSupported();
    } catch (...) {
        return false;
    }
}

bool WgcCapture::Create(ID3D11Device* dev, ID3D11DeviceContext* ctx, HWND hwnd, HMONITOR mon, UINT& w, UINT& h) {
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        m->ctx = ctx;

        winrt::com_ptr<IDXGIDevice> dxgi;
        winrt::check_hresult(dev->QueryInterface(IID_PPV_ARGS(dxgi.put())));
        winrt::com_ptr<::IInspectable> insp;
        winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgi.get(), insp.put()));
        m->d3dDevice = insp.as<wd3d::IDirect3DDevice>();

        auto interop = winrt::get_activation_factory<wgc::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        if (hwnd) {
            winrt::check_hresult(interop->CreateForWindow(hwnd, winrt::guid_of<wgc::GraphicsCaptureItem>(),
                                                          winrt::put_abi(m->item)));
        } else {
            winrt::check_hresult(interop->CreateForMonitor(mon, winrt::guid_of<wgc::GraphicsCaptureItem>(),
                                                           winrt::put_abi(m->item)));
        }

        auto size = m->item.Size();
        if (size.Width <= 0 || size.Height <= 0) return false;
        m->lastSize = size;
        w = (UINT)size.Width;
        h = (UINT)size.Height;

        m->pool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(
            m->d3dDevice, wdx::DirectXPixelFormat::B8G8R8A8UIntNormalized, 3, size);
        m->session = m->pool.CreateCaptureSession(m->item);
        try { m->session.IsCursorCaptureEnabled(true); } catch (...) {}

        Impl* impl = m.get();
        m->closedRevoker = m->item.Closed(winrt::auto_revoke, [impl](auto&&, auto&&) { impl->alive = false; });
        m->frameRevoker = m->pool.FrameArrived(winrt::auto_revoke,
            [impl](wgc::Direct3D11CaptureFramePool const& sender, winrt::Windows::Foundation::IInspectable const&) {
                auto frame = sender.TryGetNextFrame();
                if (!frame) return;
                auto content = frame.ContentSize();
                ID3D11Texture2D* target = impl->target.load();
                if (target) {
                    auto access = frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
                    winrt::com_ptr<ID3D11Texture2D> tex;
                    if (SUCCEEDED(access->GetInterface(winrt::guid_of<ID3D11Texture2D>(), tex.put_void()))) {
                        D3D11_TEXTURE2D_DESC d;
                        tex->GetDesc(&d);
                        UINT cw = std::min<UINT>({d.Width, (UINT)content.Width, impl->targetW.load()});
                        UINT ch = std::min<UINT>({d.Height, (UINT)content.Height, impl->targetH.load()});
                        D3D11_BOX box = {0, 0, 0, cw, ch, 1};
                        impl->ctx->CopySubresourceRegion(target, 0, 0, 0, 0, tex.get(), 0, &box);
                        impl->frames++;
                    }
                }
                if (content.Width != impl->lastSize.Width || content.Height != impl->lastSize.Height) {
                    impl->lastSize = content;
                    sender.Recreate(impl->d3dDevice, wdx::DirectXPixelFormat::B8G8R8A8UIntNormalized, 3, content);
                }
            });
        m->created = true;
        return true;
    } catch (...) {
        return false;
    }
}

void WgcCapture::Begin(ID3D11Texture2D* target, UINT w, UINT h) {
    if (!m->created) return;
    m->targetW = w;
    m->targetH = h;
    m->target = target;
    try { m->session.StartCapture(); } catch (...) { m->alive = false; }
}

void WgcCapture::Stop() {
    if (!m) return;
    m->target = nullptr;
    try {
        m->frameRevoker.revoke();
        m->closedRevoker.revoke();
        if (m->session) m->session.Close();
        if (m->pool) m->pool.Close();
    } catch (...) {}
    m->session = nullptr;
    m->pool = nullptr;
    m->item = nullptr;
    m->d3dDevice = nullptr;
    m->created = false;
}

uint64_t WgcCapture::Frames() const { return m->frames.load(); }
bool WgcCapture::Alive() const { return m->alive.load(); }
