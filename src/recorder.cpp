// GKPRO Recorder - capture (DXGI Desktop Duplication) + hardware H.264 (Media Foundation)
// Frames stay on the GPU the whole way: capture texture -> encoder. No CPU copies.

#include "recorder.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <codecapi.h>
#include <wrl/client.h>
#include <vector>
#include <chrono>

using Microsoft::WRL::ComPtr;

namespace {
constexpr int kPoolSize = 8;  // textures in flight; the encoder reads them asynchronously

#define CHECK(hr, msg) do { if (FAILED(hr)) { err = msg; goto fail; } } while (0)
}

bool Recorder::Start(const std::wstring& path, const RecorderConfig& cfg) {
    if (m_running.load()) return false;
    m_error.clear();
    m_ready = false;
    m_failed = false;
    m_running = true;
    m_thread = std::thread(&Recorder::Loop, this, path, cfg);

    // Wait until the capture thread either starts writing or fails.
    while (!m_ready.load() && !m_failed.load()) Sleep(5);
    if (m_failed.load()) {
        if (m_thread.joinable()) m_thread.join();
        m_running = false;
        return false;
    }
    return true;
}

void Recorder::Stop() {
    m_running = false;
    if (m_thread.joinable()) m_thread.join();
}

void Recorder::Loop(std::wstring path, RecorderConfig cfg) {
    const wchar_t* err = nullptr;
    HRESULT hr = S_OK;

    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<IDXGIOutputDuplication> dup;
    ComPtr<IMFDXGIDeviceManager> devMgr;
    ComPtr<IMFSinkWriter> writer;
    ComPtr<ID3D11Texture2D> pool[kPoolSize];
    ComPtr<ID3D11Texture2D> latest;  // last texture holding a complete frame
    UINT resetToken = 0;
    UINT w = 0, h = 0;
    bool writing = false;
    int poolIdx = 0;
    LONGLONG frameIndex = 0;

    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION, MFSTARTUP_LITE);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);  // never fight the game

    // ---- D3D11 device ----
    {
        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr, 0,
                               D3D11_SDK_VERSION, &dev, nullptr, &ctx);
        CHECK(hr, L"Could not create the GPU device.");
        ComPtr<ID3D10Multithread> mt;
        if (SUCCEEDED(dev.As(&mt))) mt->SetMultithreadProtected(TRUE);
    }

    // ---- Desktop Duplication on primary monitor ----
    {
        ComPtr<IDXGIDevice> dxgiDev;
        ComPtr<IDXGIAdapter> adapter;
        ComPtr<IDXGIOutput> output;
        ComPtr<IDXGIOutput1> output1;
        hr = dev.As(&dxgiDev);              CHECK(hr, L"DXGI device error.");
        hr = dxgiDev->GetAdapter(&adapter); CHECK(hr, L"DXGI adapter error.");
        hr = adapter->EnumOutputs(0, &output); CHECK(hr, L"No monitor found.");
        hr = output.As(&output1);           CHECK(hr, L"Monitor does not support capture.");
        hr = output1->DuplicateOutput(dev.Get(), &dup);
        CHECK(hr, L"Screen capture failed to start (another capture app may be running).");
        DXGI_OUTDUPL_DESC dd;
        dup->GetDesc(&dd);
        w = dd.ModeDesc.Width & ~1u;  // H.264 needs even dimensions
        h = dd.ModeDesc.Height & ~1u;
    }

    // ---- Texture pool (GPU only) ----
    {
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        for (auto& t : pool) {
            hr = dev->CreateTexture2D(&td, nullptr, &t);
            CHECK(hr, L"Out of GPU memory.");
        }
    }

    // ---- Media Foundation hardware encoder ----
    {
        hr = MFCreateDXGIDeviceManager(&resetToken, &devMgr); CHECK(hr, L"MF device manager error.");
        hr = devMgr->ResetDevice(dev.Get(), resetToken);      CHECK(hr, L"MF device reset error.");

        const int fps = cfg.fps > 0 ? cfg.fps : 30;
        int kbps = cfg.bitrateKbps;
        if (kbps <= 0) kbps = (int)((double)w * h * fps * 0.07 / 1000.0);  // ~4.3 Mbps for 1080p30
        if (kbps < 1500) kbps = 1500;

        ComPtr<IMFAttributes> attrs;
        MFCreateAttributes(&attrs, 5);
        attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
        attrs->SetUnknown(MF_SINK_WRITER_D3D_MANAGER, devMgr.Get());
        attrs->SetUINT32(MF_LOW_LATENCY, TRUE);
        attrs->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
        // Fragmented MP4: file stays playable even if the PC crashes mid-recording.
        attrs->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_FMPEG4);

        hr = MFCreateSinkWriterFromURL(path.c_str(), nullptr, attrs.Get(), &writer);
        CHECK(hr, L"Could not create the output file.");

        ComPtr<IMFMediaType> out;
        MFCreateMediaType(&out);
        out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        out->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        out->SetUINT32(MF_MT_AVG_BITRATE, (UINT32)kbps * 1000);
        out->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        out->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Main);
        MFSetAttributeSize(out.Get(), MF_MT_FRAME_SIZE, w, h);
        MFSetAttributeRatio(out.Get(), MF_MT_FRAME_RATE, fps, 1);
        MFSetAttributeRatio(out.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        DWORD stream = 0;
        hr = writer->AddStream(out.Get(), &stream); CHECK(hr, L"No H.264 encoder available.");

        ComPtr<IMFMediaType> in;
        MFCreateMediaType(&in);
        in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        in->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_ARGB32);  // matches BGRA textures
        in->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        MFSetAttributeSize(in.Get(), MF_MT_FRAME_SIZE, w, h);
        MFSetAttributeRatio(in.Get(), MF_MT_FRAME_RATE, fps, 1);
        MFSetAttributeRatio(in.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        hr = writer->SetInputMediaType(stream, in.Get(), nullptr);
        CHECK(hr, L"Encoder rejected the video format.");

        hr = writer->BeginWriting(); CHECK(hr, L"Could not start writing.");
        writing = true;
    }

    m_ready = true;

    // ---- Capture loop ----
    {
        const int fps = cfg.fps > 0 ? cfg.fps : 30;
        const LONGLONG frameDur100ns = 10000000LL / fps;
        using clock = std::chrono::steady_clock;
        const auto t0 = clock::now();

        while (m_running.load()) {
            // Wait until this frame is due (keeps CPU near zero between frames).
            auto due = t0 + std::chrono::microseconds(frameIndex * 1000000LL / fps);
            auto now = clock::now();
            DWORD waitMs = due > now
                ? (DWORD)std::chrono::duration_cast<std::chrono::milliseconds>(due - now).count()
                : 0;

            DXGI_OUTDUPL_FRAME_INFO info;
            ComPtr<IDXGIResource> res;
            hr = dup->AcquireNextFrame(waitMs, &info, &res);
            if (hr == DXGI_ERROR_ACCESS_LOST) break;  // display mode changed; MVP just stops cleanly
            if (SUCCEEDED(hr)) {
                ComPtr<ID3D11Texture2D> src;
                res.As(&src);
                ID3D11Texture2D* dst = pool[poolIdx].Get();
                D3D11_BOX box = {0, 0, 0, w, h, 1};
                ctx->CopySubresourceRegion(dst, 0, 0, 0, 0, src.Get(), 0, &box);
                latest = pool[poolIdx];
                poolIdx = (poolIdx + 1) % kPoolSize;
                dup->ReleaseFrame();
            }

            if (clock::now() < due || !latest) continue;  // not time yet (new frame arrived early)

            // Submit `latest` (repeats the last frame if the screen didn't change).
            // Copy into the next pool slot so the encoder's texture is never overwritten.
            ID3D11Texture2D* slot = pool[poolIdx].Get();
            if (slot != latest.Get()) ctx->CopyResource(slot, latest.Get());
            ComPtr<ID3D11Texture2D> submit = pool[poolIdx];
            poolIdx = (poolIdx + 1) % kPoolSize;

            ComPtr<IMFMediaBuffer> buf;
            if (SUCCEEDED(MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), submit.Get(), 0, FALSE, &buf))) {
                buf->SetCurrentLength(w * h * 4);
                ComPtr<IMFSample> sample;
                MFCreateSample(&sample);
                sample->AddBuffer(buf.Get());
                sample->SetSampleTime(frameIndex * frameDur100ns);
                sample->SetSampleDuration(frameDur100ns);
                writer->WriteSample(0, sample.Get());
            }
            ++frameIndex;
        }
    }

    if (writing) writer->Finalize();
    writer.Reset();
    goto cleanup;

fail:
    m_error = err ? err : L"Unknown error.";
    m_failed = true;

cleanup:
    dup.Reset();
    for (auto& t : pool) t.Reset();
    latest.Reset();
    devMgr.Reset();
    ctx.Reset();
    dev.Reset();
    MFShutdown();
    CoUninitialize();
    m_running = false;
}
