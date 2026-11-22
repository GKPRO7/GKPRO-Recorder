// GKPRO Recorder - game capture hook. Injected into the game process (gkhook64.dll / gkhook32.dll).
// Hooks the "present" call of Direct3D 9, Direct3D 10/11 (DXGI) and OpenGL, copies the finished
// frame into a shared GPU texture, and the recorder reads that texture. Works in exclusive fullscreen.
// It only copies while the recorder asks for frames, and never faster than the recording fps.

#include <windows.h>
#include <d3d9.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <vector>
#include <cstring>
#include "../third_party/minhook/include/MinHook.h"
#include "../hook_shared.h"

using Microsoft::WRL::ComPtr;

static HMODULE g_self = nullptr;
static GkHookShared* g_shm = nullptr;
static HANDLE g_map = nullptr;
static LARGE_INTEGER g_freq, g_last;
static DWORD g_lastOpen = 0;

// ---------------------------------------------------------------- shared memory + pacing
static GkHookShared* Shm() {
    if (g_shm && g_shm->magic == GKHOOK_MAGIC) return g_shm;
    DWORD now = GetTickCount();
    if (g_lastOpen && now - g_lastOpen < 500) return nullptr;
    g_lastOpen = now ? now : 1;
    wchar_t name[64];
    GkHookShmName(GetCurrentProcessId(), name);
    HANDLE m = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name);
    if (!m) return nullptr;
    auto p = (GkHookShared*)MapViewOfFile(m, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(GkHookShared));
    if (!p) { CloseHandle(m); return nullptr; }
    if (g_shm) UnmapViewOfFile((void*)g_shm);
    if (g_map) CloseHandle(g_map);
    g_map = m;
    g_shm = p;
    return p->magic == GKHOOK_MAGIC ? p : nullptr;
}

static bool WantFrame() {
    GkHookShared* s = Shm();
    if (!s || !s->wantFrames) return false;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    LONGLONG fps = s->targetFps ? s->targetFps : 30;
    // never copy faster than ~1.25x the recording fps
    if (g_last.QuadPart && now.QuadPart - g_last.QuadPart < g_freq.QuadPart * 4 / (5 * fps)) return false;
    g_last = now;
    return true;
}

static void Publish(uint32_t api, UINT w, UINT h, DXGI_FORMAT fmt, HANDLE handle, bool keyed) {
    GkHookShared* s = g_shm;
    if (!s) return;
    s->api = api;
    s->width = w;
    s->height = h;
    s->dxgiFormat = (uint32_t)fmt;
    s->sharedHandle = (uint32_t)(uintptr_t)handle;
    s->hasKeyedMutex = keyed ? 1 : 0;
    MemoryBarrier();
    s->generation = s->generation + 1;
}

static HWND MakeDummyWindow() {
    WNDCLASSW wc = {};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = g_self;
    wc.lpszClassName = L"GKPROHookDummy";
    RegisterClassW(&wc);
    return CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPED, 0, 0, 8, 8, nullptr, nullptr, g_self, nullptr);
}

static void CreateAndEnable(void* target, void* detour, void** original) {
    MH_CreateHook(target, detour, original);  // fails harmlessly if this target is already hooked
}

// ---------------------------------------------------------------- Direct3D 10/11 (DXGI)
typedef HRESULT(STDMETHODCALLTYPE* DxgiPresentFn)(IDXGISwapChain*, UINT, UINT);
typedef HRESULT(STDMETHODCALLTYPE* DxgiPresent1Fn)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
typedef HRESULT(STDMETHODCALLTYPE* DxgiResizeFn)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
static DxgiPresentFn oDxgiPresent = nullptr;
static DxgiPresent1Fn oDxgiPresent1 = nullptr;
static DxgiResizeFn oDxgiResize = nullptr;

static struct {
    ComPtr<ID3D11Texture2D> shared;
    ComPtr<IDXGIKeyedMutex> km;
    ID3D11Device* dev = nullptr;
    UINT w = 0, h = 0;
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
} g_dx;

// Shared copy uses the plain UNORM variant so the recorder never has to deal with typeless/sRGB views.
static DXGI_FORMAT Typed(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8X8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default: return f;
    }
}

static void DxgiRelease() {
    g_dx.km.Reset();
    g_dx.shared.Reset();
    g_dx.dev = nullptr;
}

static void DxgiCapture(IDXGISwapChain* sc) {
    if (!WantFrame()) return;
    ComPtr<ID3D11Device> dev;
    if (FAILED(sc->GetDevice(IID_PPV_ARGS(&dev)))) return;  // not a D3D11 device (D3D10/12 are skipped)
    ComPtr<ID3D11Texture2D> bb;
    if (FAILED(sc->GetBuffer(0, IID_PPV_ARGS(&bb)))) return;
    D3D11_TEXTURE2D_DESC d;
    bb->GetDesc(&d);
    DXGI_FORMAT fmt = Typed(d.Format);

    if (!g_dx.shared || g_dx.dev != dev.Get() || g_dx.w != d.Width || g_dx.h != d.Height || g_dx.fmt != fmt) {
        DxgiRelease();
        D3D11_TEXTURE2D_DESC sd = {};
        sd.Width = d.Width; sd.Height = d.Height; sd.MipLevels = 1; sd.ArraySize = 1;
        sd.Format = fmt;
        sd.SampleDesc.Count = 1;
        sd.Usage = D3D11_USAGE_DEFAULT;
        sd.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        sd.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
        if (FAILED(dev->CreateTexture2D(&sd, nullptr, &g_dx.shared))) return;
        ComPtr<IDXGIResource> res;
        HANDLE h = nullptr;
        if (FAILED(g_dx.shared.As(&res)) || FAILED(res->GetSharedHandle(&h)) || !h) { DxgiRelease(); return; }
        g_dx.shared.As(&g_dx.km);
        g_dx.dev = dev.Get();
        g_dx.w = d.Width; g_dx.h = d.Height; g_dx.fmt = fmt;
        Publish(1, d.Width, d.Height, fmt, h, g_dx.km != nullptr);
    }

    ComPtr<ID3D11DeviceContext> ctx;
    dev->GetImmediateContext(&ctx);
    if (g_dx.km && g_dx.km->AcquireSync(0, 0) != S_OK) return;  // recorder is reading: skip this frame
    if (d.SampleDesc.Count > 1) ctx->ResolveSubresource(g_dx.shared.Get(), 0, bb.Get(), 0, fmt);
    else ctx->CopyResource(g_dx.shared.Get(), bb.Get());
    if (g_dx.km) g_dx.km->ReleaseSync(0);
    if (g_shm) g_shm->frameCounter = g_shm->frameCounter + 1;
}

static thread_local bool t_inDxgi = false;

static HRESULT STDMETHODCALLTYPE HkDxgiPresent(IDXGISwapChain* sc, UINT sync, UINT flags) {
    if (!t_inDxgi && !(flags & DXGI_PRESENT_TEST)) {
        t_inDxgi = true;
        DxgiCapture(sc);
        t_inDxgi = false;
    }
    return oDxgiPresent(sc, sync, flags);
}

static HRESULT STDMETHODCALLTYPE HkDxgiPresent1(IDXGISwapChain1* sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* pp) {
    if (!t_inDxgi && !(flags & DXGI_PRESENT_TEST)) {
        t_inDxgi = true;
        DxgiCapture(sc);
        t_inDxgi = false;
    }
    return oDxgiPresent1(sc, sync, flags, pp);
}

static HRESULT STDMETHODCALLTYPE HkDxgiResize(IDXGISwapChain* sc, UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT fl) {
    DxgiRelease();
    return oDxgiResize(sc, n, w, h, f, fl);
}

static bool HookDxgi() {
    typedef HRESULT(WINAPI * CreateFn)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT,
                                       const DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**, ID3D11Device**, D3D_FEATURE_LEVEL*,
                                       ID3D11DeviceContext**);
    auto fn = (CreateFn)GetProcAddress(GetModuleHandleW(L"d3d11.dll"), "D3D11CreateDeviceAndSwapChain");
    if (!fn) return false;
    HWND hw = MakeDummyWindow();
    bool ok = false;
    {
        DXGI_SWAP_CHAIN_DESC sd = {};
        sd.BufferCount = 1;
        sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.BufferDesc.Width = 2;
        sd.BufferDesc.Height = 2;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.OutputWindow = hw;
        sd.SampleDesc.Count = 1;
        sd.Windowed = TRUE;
        sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        ComPtr<IDXGISwapChain> sc;
        ComPtr<ID3D11Device> dev;
        ComPtr<ID3D11DeviceContext> ctx;
        D3D_FEATURE_LEVEL fl;
        if (SUCCEEDED(fn(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd, &sc, &dev, &fl, &ctx))) {
            void** vt = *(void***)sc.Get();
            CreateAndEnable(vt[8], (void*)HkDxgiPresent, (void**)&oDxgiPresent);
            CreateAndEnable(vt[13], (void*)HkDxgiResize, (void**)&oDxgiResize);
            ComPtr<IDXGISwapChain1> sc1;
            if (SUCCEEDED(sc.As(&sc1))) {
                void** vt1 = *(void***)sc1.Get();
                CreateAndEnable(vt1[22], (void*)HkDxgiPresent1, (void**)&oDxgiPresent1);
            }
            MH_EnableHook(MH_ALL_HOOKS);
            ok = true;
        }
    }
    DestroyWindow(hw);
    return ok;
}

// ---------------------------------------------------------------- Direct3D 9
typedef HRESULT(STDMETHODCALLTYPE* D9PresentFn)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
typedef HRESULT(STDMETHODCALLTYPE* D9PresentExFn)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*, DWORD);
typedef HRESULT(STDMETHODCALLTYPE* D9SwapPresentFn)(IDirect3DSwapChain9*, const RECT*, const RECT*, HWND, const RGNDATA*, DWORD);
typedef HRESULT(STDMETHODCALLTYPE* D9ResetFn)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
typedef HRESULT(STDMETHODCALLTYPE* D9ResetExFn)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*, D3DDISPLAYMODEEX*);
static D9PresentFn oD9Present = nullptr;
static D9PresentExFn oD9PresentEx = nullptr;
static D9SwapPresentFn oD9SwapPresent = nullptr;
static D9ResetFn oD9Reset = nullptr;
static D9ResetExFn oD9ResetEx = nullptr;

static struct {
    ComPtr<IDirect3DTexture9> tex;
    ComPtr<IDirect3DSurface9> surf;
    IDirect3DDevice9* dev = nullptr;
    UINT w = 0, h = 0;
} g_d9;

static void D9Release() {
    g_d9.surf.Reset();
    g_d9.tex.Reset();
    g_d9.dev = nullptr;
}

static void D9Capture(IDirect3DDevice9* dev, IDirect3DSurface9* bb) {
    if (!dev || !bb) return;
    D3DSURFACE_DESC d;
    if (FAILED(bb->GetDesc(&d))) return;
    if (!g_d9.tex || g_d9.dev != dev || g_d9.w != d.Width || g_d9.h != d.Height) {
        D9Release();
        HANDLE h = nullptr;
        if (FAILED(dev->CreateTexture(d.Width, d.Height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &g_d9.tex, &h)) || !h) {
            D9Release();
            return;
        }
        g_d9.tex->GetSurfaceLevel(0, &g_d9.surf);
        g_d9.dev = dev;
        g_d9.w = d.Width;
        g_d9.h = d.Height;
        Publish(2, d.Width, d.Height, DXGI_FORMAT_B8G8R8A8_UNORM, h, false);
    }
    if (g_d9.surf && SUCCEEDED(dev->StretchRect(bb, nullptr, g_d9.surf.Get(), nullptr, D3DTEXF_NONE)) && g_shm)
        g_shm->frameCounter = g_shm->frameCounter + 1;
}

static thread_local bool t_inD9 = false;

static void D9FromDevice(IDirect3DDevice9* dev) {
    if (t_inD9 || !WantFrame()) return;
    t_inD9 = true;
    ComPtr<IDirect3DSurface9> bb;
    if (SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) D9Capture(dev, bb.Get());
    t_inD9 = false;
}

static HRESULT STDMETHODCALLTYPE HkD9Present(IDirect3DDevice9* dev, const RECT* a, const RECT* b, HWND c, const RGNDATA* d) {
    D9FromDevice(dev);
    return oD9Present(dev, a, b, c, d);
}
static HRESULT STDMETHODCALLTYPE HkD9PresentEx(IDirect3DDevice9* dev, const RECT* a, const RECT* b, HWND c, const RGNDATA* d, DWORD e) {
    D9FromDevice(dev);
    return oD9PresentEx(dev, a, b, c, d, e);
}
static HRESULT STDMETHODCALLTYPE HkD9SwapPresent(IDirect3DSwapChain9* sc, const RECT* a, const RECT* b, HWND c, const RGNDATA* d, DWORD e) {
    if (!t_inD9 && WantFrame()) {
        t_inD9 = true;
        ComPtr<IDirect3DDevice9> dev;
        ComPtr<IDirect3DSurface9> bb;
        if (SUCCEEDED(sc->GetDevice(&dev)) && SUCCEEDED(sc->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb))) D9Capture(dev.Get(), bb.Get());
        t_inD9 = false;
    }
    return oD9SwapPresent(sc, a, b, c, d, e);
}
static HRESULT STDMETHODCALLTYPE HkD9Reset(IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS* pp) {
    D9Release();
    return oD9Reset(dev, pp);
}
static HRESULT STDMETHODCALLTYPE HkD9ResetEx(IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS* pp, D3DDISPLAYMODEEX* m) {
    D9Release();
    return oD9ResetEx(dev, pp, m);
}

static bool HookD3D9() {
    HMODULE m = GetModuleHandleW(L"d3d9.dll");
    typedef IDirect3D9*(WINAPI * CreateFn)(UINT);
    typedef HRESULT(WINAPI * CreateExFn)(UINT, IDirect3D9Ex**);
    auto create = (CreateFn)GetProcAddress(m, "Direct3DCreate9");
    auto createEx = (CreateExFn)GetProcAddress(m, "Direct3DCreate9Ex");
    HWND hw = MakeDummyWindow();
    bool ok = false;

    D3DPRESENT_PARAMETERS pp = {};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = D3DFMT_UNKNOWN;
    pp.hDeviceWindow = hw;
    const DWORD cf = D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_NOWINDOWCHANGES;

    if (create) {
        ComPtr<IDirect3D9> d3d;
        d3d.Attach(create(D3D_SDK_VERSION));
        ComPtr<IDirect3DDevice9> dev;
        if (d3d && SUCCEEDED(d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hw, cf, &pp, &dev))) {
            void** vt = *(void***)dev.Get();
            CreateAndEnable(vt[17], (void*)HkD9Present, (void**)&oD9Present);
            CreateAndEnable(vt[16], (void*)HkD9Reset, (void**)&oD9Reset);
            ComPtr<IDirect3DSwapChain9> sc;
            if (SUCCEEDED(dev->GetSwapChain(0, &sc))) {
                void** vts = *(void***)sc.Get();
                CreateAndEnable(vts[3], (void*)HkD9SwapPresent, (void**)&oD9SwapPresent);
            }
            ok = true;
        }
    }
    if (createEx) {
        ComPtr<IDirect3D9Ex> d3d;
        ComPtr<IDirect3DDevice9Ex> dev;
        if (SUCCEEDED(createEx(D3D_SDK_VERSION, &d3d)) &&
            SUCCEEDED(d3d->CreateDeviceEx(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hw, cf, &pp, nullptr, &dev))) {
            void** vt = *(void***)dev.Get();
            CreateAndEnable(vt[17], (void*)HkD9Present, (void**)&oD9Present);
            CreateAndEnable(vt[16], (void*)HkD9Reset, (void**)&oD9Reset);
            CreateAndEnable(vt[121], (void*)HkD9PresentEx, (void**)&oD9PresentEx);
            CreateAndEnable(vt[132], (void*)HkD9ResetEx, (void**)&oD9ResetEx);
            ComPtr<IDirect3DSwapChain9> sc;
            if (SUCCEEDED(dev->GetSwapChain(0, &sc))) {
                void** vts = *(void***)sc.Get();
                CreateAndEnable(vts[3], (void*)HkD9SwapPresent, (void**)&oD9SwapPresent);
            }
            ok = true;
        }
    }
    MH_EnableHook(MH_ALL_HOOKS);
    DestroyWindow(hw);
    return ok;
}

// ---------------------------------------------------------------- OpenGL (also old GL games)
typedef BOOL(WINAPI* SwapFn)(HDC);
static SwapFn oWglSwap = nullptr;
static SwapFn oGdiSwap = nullptr;

typedef void(WINAPI* PFN_glReadPixels)(int, int, int, int, unsigned, unsigned, void*);
typedef void(WINAPI* PFN_glPixelStorei)(unsigned, int);
typedef void(WINAPI* PFN_glGetIntegerv)(unsigned, int*);
typedef void(WINAPI* PFN_glReadBuffer)(unsigned);
typedef unsigned(WINAPI* PFN_glGetError)();
typedef void(WINAPI* PFN_glBindBuffer)(unsigned, unsigned);
typedef void(WINAPI* PFN_glBindFramebuffer)(unsigned, unsigned);
typedef HGLRC(WINAPI* PFN_wglGetCurrentContext)();
typedef PROC(WINAPI* PFN_wglGetProcAddress)(LPCSTR);

static struct {
    bool loaded = false;
    PFN_glReadPixels readPixels = nullptr;
    PFN_glPixelStorei pixelStorei = nullptr;
    PFN_glGetIntegerv getIntegerv = nullptr;
    PFN_glReadBuffer readBuffer = nullptr;
    PFN_glGetError getError = nullptr;
    PFN_glBindBuffer bindBuffer = nullptr;
    PFN_glBindFramebuffer bindFramebuffer = nullptr;
    PFN_wglGetCurrentContext getCurrentContext = nullptr;
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<ID3D11Texture2D> tex;
    ComPtr<IDXGIKeyedMutex> km;
    UINT w = 0, h = 0;
    std::vector<uint8_t> read, flip;
} g_gl;

static bool GlLoad() {
    if (g_gl.loaded) return g_gl.readPixels != nullptr;
    g_gl.loaded = true;
    HMODULE gl = GetModuleHandleW(L"opengl32.dll");
    if (!gl) return false;
    g_gl.readPixels = (PFN_glReadPixels)GetProcAddress(gl, "glReadPixels");
    g_gl.pixelStorei = (PFN_glPixelStorei)GetProcAddress(gl, "glPixelStorei");
    g_gl.getIntegerv = (PFN_glGetIntegerv)GetProcAddress(gl, "glGetIntegerv");
    g_gl.readBuffer = (PFN_glReadBuffer)GetProcAddress(gl, "glReadBuffer");
    g_gl.getError = (PFN_glGetError)GetProcAddress(gl, "glGetError");
    g_gl.getCurrentContext = (PFN_wglGetCurrentContext)GetProcAddress(gl, "wglGetCurrentContext");
    auto getProc = (PFN_wglGetProcAddress)GetProcAddress(gl, "wglGetProcAddress");
    if (getProc) {
        g_gl.bindBuffer = (PFN_glBindBuffer)getProc("glBindBuffer");
        g_gl.bindFramebuffer = (PFN_glBindFramebuffer)getProc("glBindFramebuffer");
    }
    if (!g_gl.readPixels || !g_gl.pixelStorei || !g_gl.getIntegerv || !g_gl.readBuffer || !g_gl.getCurrentContext) {
        g_gl.readPixels = nullptr;
        return false;
    }
    return true;
}

static bool GlEnsureTexture(UINT w, UINT h) {
    if (!g_gl.dev) {
        HMODULE d3d = LoadLibraryW(L"d3d11.dll");
        auto create = d3d ? (PFN_D3D11_CREATE_DEVICE)GetProcAddress(d3d, "D3D11CreateDevice") : nullptr;
        if (!create) return false;
        D3D_FEATURE_LEVEL fl;
        if (FAILED(create(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                          D3D11_SDK_VERSION, &g_gl.dev, &fl, &g_gl.ctx)))
            return false;
    }
    if (g_gl.tex && g_gl.w == w && g_gl.h == h) return true;
    g_gl.km.Reset();
    g_gl.tex.Reset();
    D3D11_TEXTURE2D_DESC sd = {};
    sd.Width = w; sd.Height = h; sd.MipLevels = 1; sd.ArraySize = 1;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.Usage = D3D11_USAGE_DEFAULT;
    sd.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    sd.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
    if (FAILED(g_gl.dev->CreateTexture2D(&sd, nullptr, &g_gl.tex))) return false;
    ComPtr<IDXGIResource> res;
    HANDLE h2 = nullptr;
    if (FAILED(g_gl.tex.As(&res)) || FAILED(res->GetSharedHandle(&h2)) || !h2) { g_gl.tex.Reset(); return false; }
    g_gl.tex.As(&g_gl.km);
    g_gl.w = w;
    g_gl.h = h;
    Publish(3, w, h, DXGI_FORMAT_B8G8R8A8_UNORM, h2, g_gl.km != nullptr);
    return true;
}

static void GlCapture(HDC hdc) {
    if (!WantFrame() || !GlLoad()) return;
    if (!g_gl.getCurrentContext()) return;

    int w = 0, h = 0;
    HWND hw = WindowFromDC(hdc);
    RECT rc;
    if (hw && GetClientRect(hw, &rc)) { w = rc.right - rc.left; h = rc.bottom - rc.top; }
    if (w <= 0 || h <= 0) {
        int vp[4] = {};
        g_gl.getIntegerv(0x0BA2 /*GL_VIEWPORT*/, vp);
        w = vp[2];
        h = vp[3];
    }
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192) return;
    if (!GlEnsureTexture((UINT)w, (UINT)h)) return;

    const size_t pitch = (size_t)w * 4;
    g_gl.read.resize(pitch * h);
    g_gl.flip.resize(pitch * h);

    // Save the game's GL state, read the back buffer, restore.
    int packAlign = 4, packRow = 0, readBuf = 0x0405, pbo = 0, readFbo = 0;
    g_gl.getIntegerv(0x0D05 /*PACK_ALIGNMENT*/, &packAlign);
    g_gl.getIntegerv(0x0D02 /*PACK_ROW_LENGTH*/, &packRow);
    g_gl.getIntegerv(0x0C02 /*READ_BUFFER*/, &readBuf);
    if (g_gl.bindBuffer) g_gl.getIntegerv(0x88ED /*PIXEL_PACK_BUFFER_BINDING*/, &pbo);
    if (g_gl.bindFramebuffer) g_gl.getIntegerv(0x8CAA /*READ_FRAMEBUFFER_BINDING*/, &readFbo);
    if (pbo && g_gl.bindBuffer) g_gl.bindBuffer(0x88EB, 0);
    if (readFbo && g_gl.bindFramebuffer) g_gl.bindFramebuffer(0x8CA8 /*READ_FRAMEBUFFER*/, 0);
    g_gl.pixelStorei(0x0D05, 1);
    g_gl.pixelStorei(0x0D02, 0);
    g_gl.readBuffer(0x0405 /*GL_BACK*/);
    g_gl.readPixels(0, 0, w, h, 0x80E1 /*GL_BGRA*/, 0x1401 /*GL_UNSIGNED_BYTE*/, g_gl.read.data());
    g_gl.pixelStorei(0x0D05, packAlign);
    g_gl.pixelStorei(0x0D02, packRow);
    g_gl.readBuffer((unsigned)readBuf);
    if (readFbo && g_gl.bindFramebuffer) g_gl.bindFramebuffer(0x8CA8, (unsigned)readFbo);
    if (pbo && g_gl.bindBuffer) g_gl.bindBuffer(0x88EB, (unsigned)pbo);
    if (g_gl.getError) for (int i = 0; i < 4 && g_gl.getError(); ++i) {}  // do not leak our errors into the game

    // OpenGL is bottom-up: flip the rows and force opaque alpha.
    for (int y = 0; y < h; ++y) {
        const uint32_t* s = (const uint32_t*)(g_gl.read.data() + (size_t)(h - 1 - y) * pitch);
        uint32_t* d = (uint32_t*)(g_gl.flip.data() + (size_t)y * pitch);
        for (int x = 0; x < w; ++x) d[x] = s[x] | 0xFF000000u;
    }
    if (g_gl.km && g_gl.km->AcquireSync(0, 0) != S_OK) return;
    g_gl.ctx->UpdateSubresource(g_gl.tex.Get(), 0, nullptr, g_gl.flip.data(), (UINT)pitch, 0);
    if (g_gl.km) g_gl.km->ReleaseSync(0);
    if (g_shm) g_shm->frameCounter = g_shm->frameCounter + 1;
}

static thread_local bool t_inGl = false;

static BOOL WINAPI HkWglSwap(HDC hdc) {
    if (!t_inGl) {
        t_inGl = true;
        GlCapture(hdc);
        t_inGl = false;
    }
    return oWglSwap(hdc);
}
static BOOL WINAPI HkGdiSwap(HDC hdc) {
    if (!t_inGl) {
        t_inGl = true;
        GlCapture(hdc);
        t_inGl = false;
    }
    return oGdiSwap(hdc);
}

static bool HookGl() {
    HMODULE gl = GetModuleHandleW(L"opengl32.dll");
    HMODULE gdi = GetModuleHandleW(L"gdi32.dll");
    void* wgl = gl ? (void*)GetProcAddress(gl, "wglSwapBuffers") : nullptr;
    void* swap = gdi ? (void*)GetProcAddress(gdi, "SwapBuffers") : nullptr;
    if (!wgl) return false;
    CreateAndEnable(wgl, (void*)HkWglSwap, (void**)&oWglSwap);
    if (swap) CreateAndEnable(swap, (void*)HkGdiSwap, (void**)&oGdiSwap);
    MH_EnableHook(MH_ALL_HOOKS);
    return true;
}

// ---------------------------------------------------------------- startup
static DWORD WINAPI InitThread(LPVOID) {
    QueryPerformanceFrequency(&g_freq);
    if (MH_Initialize() != MH_OK) return 0;
    bool dxgi = false, d9 = false, gl = false;
    // The game may load its graphics DLLs later, so keep looking for a while (~5 minutes).
    for (int i = 0; i < 1200 && !(dxgi && d9 && gl); ++i) {
        if (!dxgi && GetModuleHandleW(L"d3d11.dll") && GetModuleHandleW(L"dxgi.dll")) dxgi = HookDxgi();
        if (!d9 && GetModuleHandleW(L"d3d9.dll")) d9 = HookD3D9();
        if (!gl && GetModuleHandleW(L"opengl32.dll")) gl = HookGl();
        Sleep(250);
    }
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = inst;
        DisableThreadLibraryCalls(inst);
        HANDLE t = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
        if (t) CloseHandle(t);
    }
    return TRUE;
}
