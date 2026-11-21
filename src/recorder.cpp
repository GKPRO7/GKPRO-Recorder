// GKPRO Recorder
// Pipeline (all on the GPU):
//   Desktop Duplication (BGRA) -> D3D11 video processor (scale to 720p + convert to NV12)
//   -> hardware H.264 encoder MFT (Intel QuickSync / NVENC / AMF) -> MKV muxer -> disk

#include "recorder.h"
#include "mkv_writer.h"
#include "wgc_capture.h"
#include "audio_capture.h"

#include <windows.h>
#include <timeapi.h>
#include <avrt.h>
#include <oleauto.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <strmif.h>
#include <codecapi.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>
#include <memory>

using Microsoft::WRL::ComPtr;

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

#define REQ(hr, msg) do { if (FAILED(hr)) return msg; } while (0)

namespace {
constexpr int kPool = 12;  // NV12 720p frames are only ~1.4 MB each

void LogLine(const std::wstring& path, const char* text) {
    HANDLE f = CreateFileW((path + L".log").c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD w = 0;
    WriteFile(f, text, (DWORD)strlen(text), &w, nullptr);
    WriteFile(f, "\r\n", 2, &w, nullptr);
    CloseHandle(f);
}

void SetCodec(ICodecAPI* c, const GUID& key, ULONG v) {
    if (!c) return;
    VARIANT var;
    VariantInit(&var);
    var.vt = VT_UI4;
    var.ulVal = v;
    c->SetValue(&key, &var);
}
}  // namespace

bool Recorder::Start(const std::wstring& path, const RecorderConfig& cfg) {
    if (m_running.load()) return false;
    m_error.clear();
    m_ready = false;
    m_failed = false;
    m_running = true;
    m_thread = std::thread(&Recorder::Loop, this, path, cfg);

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
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION, MFSTARTUP_LITE);
    // MMCSS "Capture" gives steady scheduling (below-normal priority made frame timing uneven on 2-core CPUs).
    DWORD mmTask = 0;
    HANDLE mm = AvSetMmThreadCharacteristicsW(L"Capture", &mmTask);

    const wchar_t* err = Session(path, cfg);
    if (err && !m_ready.load()) {
        m_error = err;
        m_failed = true;
    }

    if (mm) AvRevertMmThreadCharacteristics(mm);
    MFShutdown();
    CoUninitialize();
    m_running = false;
}

const wchar_t* Recorder::Session(const std::wstring& path, const RecorderConfig& cfg) {
    HRESULT hr;
    const int fps = cfg.fps > 0 ? cfg.fps : 30;
    const UINT outW = (UINT)cfg.outWidth & ~1u;
    const UINT outH = (UINT)cfg.outHeight & ~1u;
    const int kbps = cfg.bitrateKbps > 0 ? cfg.bitrateKbps : 5000;

    // ---------- D3D11 device ----------
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                           D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
                           nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, &ctx);
    REQ(hr, L"Could not create the GPU device.");
    {
        ComPtr<ID3D10Multithread> mt;
        if (SUCCEEDED(dev.As(&mt))) mt->SetMultithreadProtected(TRUE);
    }

    // ---------- Capture source: Windows Graphics Capture (window or monitor), else Desktop Duplication ----------
    WgcCapture wgc;  // declared after dev/ctx so it is destroyed first
    bool useWgc = false;
    ComPtr<IDXGIOutputDuplication> dup;
    UINT srcW = 0, srcH = 0;
    const char* captureKind = "";
    if (WgcCapture::Supported()) {
        HWND hwnd = (HWND)cfg.targetWindow;
        HMONITOR mon = (HMONITOR)cfg.targetMonitor;
        if (!mon) mon = MonitorFromWindow(hwnd ? hwnd : GetDesktopWindow(), MONITOR_DEFAULTTOPRIMARY);
        if (hwnd && IsWindow(hwnd)) {
            useWgc = wgc.Create(dev.Get(), ctx.Get(), hwnd, nullptr, srcW, srcH);
            captureKind = "window (Windows Graphics Capture)";
        }
        if (!useWgc) {
            useWgc = wgc.Create(dev.Get(), ctx.Get(), nullptr, mon, srcW, srcH);
            captureKind = "monitor (Windows Graphics Capture)";
        }
    }
    if (!useWgc) {
        ComPtr<IDXGIDevice> dxgiDev;
        ComPtr<IDXGIAdapter> adapter;
        ComPtr<IDXGIOutput> output;
        ComPtr<IDXGIOutput1> output1;
        hr = dev.As(&dxgiDev);                 REQ(hr, L"DXGI device error.");
        hr = dxgiDev->GetAdapter(&adapter);    REQ(hr, L"DXGI adapter error.");
        hr = adapter->EnumOutputs(0, &output); REQ(hr, L"No monitor found.");
        hr = output.As(&output1);              REQ(hr, L"Monitor does not support capture.");
        hr = output1->DuplicateOutput(dev.Get(), &dup);
        REQ(hr, L"Screen capture failed to start (another capture app may be running).");
        DXGI_OUTDUPL_DESC dd;
        dup->GetDesc(&dd);
        srcW = dd.ModeDesc.Width;
        srcH = dd.ModeDesc.Height;
        captureKind = "monitor (Desktop Duplication fallback)";
    }

    // ---------- Capture target (full-res BGRA) ----------
    ComPtr<ID3D11Texture2D> latest;
    {
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = srcW; td.Height = srcH; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        hr = dev->CreateTexture2D(&td, nullptr, &latest);
        REQ(hr, L"Out of GPU memory.");
    }

    // ---------- GPU video processor: scale + BGRA -> NV12 ----------
    ComPtr<ID3D11VideoDevice> vdev;
    ComPtr<ID3D11VideoContext> vctx;
    ComPtr<ID3D11VideoProcessorEnumerator> venum;
    ComPtr<ID3D11VideoProcessor> vp;
    ComPtr<ID3D11VideoProcessorInputView> inView;
    ComPtr<ID3D11Texture2D> pool[kPool];
    ComPtr<ID3D11VideoProcessorOutputView> outViews[kPool];
    {
        hr = dev.As(&vdev);  REQ(hr, L"This GPU has no video processor.");
        hr = ctx.As(&vctx);  REQ(hr, L"This GPU has no video context.");

        D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd = {};
        cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        cd.InputWidth = srcW;  cd.InputHeight = srcH;
        cd.OutputWidth = outW; cd.OutputHeight = outH;
        cd.Usage = D3D11_VIDEO_USAGE_OPTIMAL_SPEED;
        hr = vdev->CreateVideoProcessorEnumerator(&cd, &venum); REQ(hr, L"Video processor error.");
        hr = vdev->CreateVideoProcessor(venum.Get(), 0, &vp);   REQ(hr, L"Video processor error.");

        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC iv = {};
        iv.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        hr = vdev->CreateVideoProcessorInputView(latest.Get(), venum.Get(), &iv, &inView);
        REQ(hr, L"Video processor input error.");

        // Keep aspect ratio (letterbox) when the screen is not 16:9.
        double sc = std::min((double)outW / srcW, (double)outH / srcH);
        LONG dw = (LONG)(srcW * sc) & ~1, dh = (LONG)(srcH * sc) & ~1;
        LONG dx = ((LONG)outW - dw) / 2 & ~1, dy = ((LONG)outH - dh) / 2 & ~1;
        RECT srcRect = {0, 0, (LONG)srcW, (LONG)srcH};
        RECT dstRect = {dx, dy, dx + dw, dy + dh};
        RECT target = {0, 0, (LONG)outW, (LONG)outH};
        vctx->VideoProcessorSetStreamFrameFormat(vp.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
        vctx->VideoProcessorSetStreamSourceRect(vp.Get(), 0, TRUE, &srcRect);
        vctx->VideoProcessorSetStreamDestRect(vp.Get(), 0, TRUE, &dstRect);
        vctx->VideoProcessorSetOutputTargetRect(vp.Get(), TRUE, &target);

        D3D11_VIDEO_COLOR black = {};
        black.YCbCr = {0.0625f, 0.5f, 0.5f, 1.0f};
        vctx->VideoProcessorSetOutputBackgroundColor(vp.Get(), TRUE, &black);

        D3D11_VIDEO_PROCESSOR_COLOR_SPACE inCs = {};   // RGB, full range
        inCs.RGB_Range = 0;
        vctx->VideoProcessorSetStreamColorSpace(vp.Get(), 0, &inCs);
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE outCs = {};  // YCbCr BT.709, studio range
        outCs.YCbCr_Matrix = 1;
        outCs.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
        vctx->VideoProcessorSetOutputColorSpace(vp.Get(), &outCs);

        D3D11_TEXTURE2D_DESC td = {};
        td.Width = outW; td.Height = outH; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_NV12;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET;
        for (int i = 0; i < kPool; ++i) {
            hr = dev->CreateTexture2D(&td, nullptr, &pool[i]);
            REQ(hr, L"Could not create NV12 frames (GPU not supported).");
            D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ov = {};
            ov.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
            hr = vdev->CreateVideoProcessorOutputView(pool[i].Get(), venum.Get(), &ov, &outViews[i]);
            REQ(hr, L"Video processor output error.");
        }
    }

    // ---------- Hardware H.264 encoder (MFT) ----------
    ComPtr<IMFDXGIDeviceManager> devMgr;  // declared before mft => destroyed after it
    ComPtr<IMFTransform> mft;
    ComPtr<IMFMediaEventGenerator> gen;
    {
        UINT token = 0;
        hr = MFCreateDXGIDeviceManager(&token, &devMgr); REQ(hr, L"MF device manager error.");
        hr = devMgr->ResetDevice(dev.Get(), token);      REQ(hr, L"MF device reset error.");

        MFT_REGISTER_TYPE_INFO outInfo = {MFMediaType_Video, MFVideoFormat_H264};
        IMFActivate** acts = nullptr;
        UINT32 count = 0;
        hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
                       nullptr, &outInfo, &acts, &count);
        if (FAILED(hr) || count == 0) {
            if (acts) CoTaskMemFree(acts);
            return L"No hardware H.264 encoder found on this PC.";
        }
        hr = acts[0]->ActivateObject(IID_PPV_ARGS(&mft));
        for (UINT32 i = 0; i < count; ++i) acts[i]->Release();
        CoTaskMemFree(acts);
        REQ(hr, L"Could not start the hardware encoder.");

        ComPtr<IMFAttributes> ma;
        UINT32 isAsync = 0;
        if (SUCCEEDED(mft->GetAttributes(&ma)) && ma) {
            ma->GetUINT32(MF_TRANSFORM_ASYNC, &isAsync);
            if (isAsync) ma->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
            ma->SetUINT32(MF_LOW_LATENCY, TRUE);
        }
        if (!isAsync) return L"This encoder type is not supported yet.";

        mft->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, (ULONG_PTR)devMgr.Get());

        ComPtr<ICodecAPI> codec;
        if (SUCCEEDED(mft.As(&codec))) {
            SetCodec(codec.Get(), CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_PeakConstrainedVBR);
            SetCodec(codec.Get(), CODECAPI_AVEncCommonMeanBitRate, (ULONG)kbps * 1000);
            SetCodec(codec.Get(), CODECAPI_AVEncCommonMaxBitRate, (ULONG)kbps * 1500);
            SetCodec(codec.Get(), CODECAPI_AVEncMPVGOPSize, (ULONG)fps * 2);
            SetCodec(codec.Get(), CODECAPI_AVEncMPVDefaultBPictureCount, 0);
            SetCodec(codec.Get(), CODECAPI_AVEncCommonQualityVsSpeed, 50);
            SetCodec(codec.Get(), CODECAPI_AVEncCommonLowLatency, 1);
        }

        ComPtr<IMFMediaType> ot;
        MFCreateMediaType(&ot);
        ot->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        ot->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        ot->SetUINT32(MF_MT_AVG_BITRATE, (UINT32)kbps * 1000);
        ot->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        ot->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High);
        MFSetAttributeSize(ot.Get(), MF_MT_FRAME_SIZE, outW, outH);
        MFSetAttributeRatio(ot.Get(), MF_MT_FRAME_RATE, fps, 1);
        MFSetAttributeRatio(ot.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        hr = mft->SetOutputType(0, ot.Get(), 0);
        REQ(hr, L"Encoder rejected the output format.");

        ComPtr<IMFMediaType> it;
        MFCreateMediaType(&it);
        it->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        it->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        it->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        MFSetAttributeSize(it.Get(), MF_MT_FRAME_SIZE, outW, outH);
        MFSetAttributeRatio(it.Get(), MF_MT_FRAME_RATE, fps, 1);
        MFSetAttributeRatio(it.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        hr = mft->SetInputType(0, it.Get(), 0);
        REQ(hr, L"Encoder rejected the input format.");

        hr = mft.As(&gen);
        REQ(hr, L"Encoder has no event interface.");

        mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    }

    // ---------- MKV output ----------
    MkvWriter mkv;
    if (!mkv.Open(path, (int)outW, (int)outH, fps)) return L"Could not create the output file.";

    // ---------- Audio tracks (no UI, one small thread per source) ----------
    std::vector<std::unique_ptr<AudioCapture>> audio;
    {
        const uint8_t asc[2] = {0x11, 0x90};  // AAC-LC, 48 kHz, stereo
        if (cfg.gameAudio) {
            mkv.AddAudioTrack(2, 48000, 2, asc, 2, "Game audio");
            audio.emplace_back(new AudioCapture(true, 2));
        }
        if (cfg.mic) {
            mkv.AddAudioTrack(3, 48000, 2, asc, 2, "Microphone");
            audio.emplace_back(new AudioCapture(false, 3));
        }
    }

    // ---------- Encoder event handling ----------
    int needInput = 0;
    bool drained = false;
    int nSubmitted = 0, nOutputs = 0, nNeedEvents = 0, nInputFail = 0, nBltFail = 0;
    char lb[256];

    auto grabSeqHeader = [&]() {
        if (mkv.HasParamSets()) return;
        ComPtr<IMFMediaType> cur;
        if (FAILED(mft->GetOutputCurrentType(0, &cur))) return;
        UINT32 sz = 0;
        if (FAILED(cur->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &sz)) || sz == 0) return;
        std::vector<uint8_t> blob(sz);
        if (SUCCEEDED(cur->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, blob.data(), sz, nullptr)))
            mkv.SetSequenceHeader(blob.data(), sz);
    };

    auto pullOutput = [&]() {
        MFT_OUTPUT_STREAM_INFO si = {};
        mft->GetOutputStreamInfo(0, &si);
        const bool provides = (si.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) != 0;
        ComPtr<IMFSample> own;
        if (!provides) {
            MFCreateSample(&own);
            ComPtr<IMFMediaBuffer> mb;
            MFCreateMemoryBuffer(si.cbSize ? si.cbSize : (1u << 20), &mb);
            own->AddBuffer(mb.Get());
        }
        MFT_OUTPUT_DATA_BUFFER ob = {};
        ob.pSample = own.Get();
        DWORD status = 0;
        HRESULT h = mft->ProcessOutput(0, 1, &ob, &status);
        if (ob.pEvents) ob.pEvents->Release();
        ComPtr<IMFSample> got;
        if (provides) got.Attach(ob.pSample); else got = own;
        if (h == MF_E_TRANSFORM_STREAM_CHANGE) {
            ComPtr<IMFMediaType> nt;
            if (SUCCEEDED(mft->GetOutputAvailableType(0, 0, &nt))) mft->SetOutputType(0, nt.Get(), 0);
            return;
        }
        if (FAILED(h) || !got) return;
        ++nOutputs;
        if (nOutputs == 1) {
            sprintf_s(lb, "first encoder output received (%s)", mkv.HasParamSets() ? "have SPS/PPS" : "no SPS/PPS yet");
            LogLine(path, lb);
        }
        grabSeqHeader();
        UINT32 clean = 0;
        got->GetUINT32(MFSampleExtension_CleanPoint, &clean);

        ComPtr<IMFMediaBuffer> cb;
        if (FAILED(got->ConvertToContiguousBuffer(&cb))) return;
        BYTE* p = nullptr;
        DWORD len = 0;
        if (SUCCEEDED(cb->Lock(&p, nullptr, &len))) {
            LONGLONG t = 0;
            got->GetSampleTime(&t);
            mkv.WriteAccessUnit(p, len, t / 10000, clean != 0);  // 100 ns -> ms
            cb->Unlock();
        }
    };

    auto pump = [&]() {
        for (;;) {
            ComPtr<IMFMediaEvent> ev;
            if (FAILED(gen->GetEvent(MF_EVENT_FLAG_NO_WAIT, &ev))) break;
            MediaEventType t = MEUnknown;
            ev->GetType(&t);
            if (t == METransformNeedInput) { ++needInput; ++nNeedEvents; }
            else if (t == MEError) {
                HRESULT st = S_OK;
                ev->GetStatus(&st);
                sprintf_s(lb, "encoder error event 0x%08X", (unsigned)st);
                LogLine(path, lb);
            }
            else if (t == METransformHaveOutput) pullOutput();
            else if (t == METransformDrainComplete) drained = true;
        }
    };

    // ---------- Precise frame timer ----------
    bool plainTimer = false;
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer) {
        timer = CreateWaitableTimerW(nullptr, TRUE, nullptr);
        timeBeginPeriod(1);
        plainTimer = true;
    }

    grabSeqHeader();
    sprintf_s(lb, "start: %s %ux%u -> %ux%u @ %d fps, %d kbps, game audio=%d, mic=%d",
              captureKind, srcW, srcH, outW, outH, fps, kbps, (int)cfg.gameAudio, (int)cfg.mic);
    LogLine(path, lb);

    m_ready = true;

    // ---------- Capture loop ----------
    LARGE_INTEGER fq, t0;
    QueryPerformanceFrequency(&fq);
    QueryPerformanceCounter(&t0);
    {
        // QPC -> 100 ns without overflow (same clock WASAPI uses for packet timestamps)
        int64_t q = t0.QuadPart, f = fq.QuadPart;
        int64_t t0_100ns = (q / f) * 10000000LL + (q % f) * 10000000LL / f;
        for (auto& a : audio) a->Start(&mkv, t0_100ns);
    }
    if (useWgc) wgc.Begin(latest.Get(), srcW, srcH);
    const LONGLONG dur100ns = 10000000LL / fps;
    LONGLONG idx = 0;
    bool haveFrame = false;
    int slot = 0;

    while (m_running.load()) {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        LONGLONG due = t0.QuadPart + idx * fq.QuadPart / fps;
        if (due > now.QuadPart) {
            LARGE_INTEGER rel;
            rel.QuadPart = -((due - now.QuadPart) * 10000000LL / fq.QuadPart);
            if (rel.QuadPart == 0) rel.QuadPart = -1;
            SetWaitableTimer(timer, &rel, 0, nullptr, nullptr, FALSE);
            WaitForSingleObject(timer, 200);
        } else if (now.QuadPart - due > 2 * fq.QuadPart / fps) {
            // Fell behind: skip ahead instead of sending a burst of late frames.
            idx = (now.QuadPart - t0.QuadPart) * fps / fq.QuadPart;
        }

        // Newest image of the window/screen.
        if (useWgc) {
            // Frames are copied into `latest` by the capture callback the moment they arrive.
            if (wgc.Frames() > 0) haveFrame = true;
            if (!wgc.Alive()) break;  // the recorded window was closed: stop and save
        } else {
            DXGI_OUTDUPL_FRAME_INFO fi;
            ComPtr<IDXGIResource> res;
            hr = dup->AcquireNextFrame(0, &fi, &res);
            if (hr == DXGI_ERROR_ACCESS_LOST) break;  // display mode changed
            if (SUCCEEDED(hr)) {
                ComPtr<ID3D11Texture2D> src;
                if (SUCCEEDED(res.As(&src))) {
                    ctx->CopyResource(latest.Get(), src.Get());
                    haveFrame = true;
                }
                src.Reset();
                res.Reset();
                dup->ReleaseFrame();
            }
        }

        pump();

        // Submit one frame (repeats the last image if the screen didn't change).
        if (haveFrame && needInput > 0) {
            D3D11_VIDEO_PROCESSOR_STREAM st = {};
            st.Enable = TRUE;
            st.pInputSurface = inView.Get();
            HRESULT bh = vctx->VideoProcessorBlt(vp.Get(), outViews[slot].Get(), 0, 1, &st);
            if (FAILED(bh) && ++nBltFail <= 3) {
                sprintf_s(lb, "VideoProcessorBlt failed 0x%08X", (unsigned)bh);
                LogLine(path, lb);
            }
            if (SUCCEEDED(bh)) {
                ComPtr<IMFMediaBuffer> buf;
                if (SUCCEEDED(MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), pool[slot].Get(), 0, FALSE, &buf))) {
                    buf->SetCurrentLength(outW * outH * 3 / 2);
                    ComPtr<IMFSample> sample;
                    MFCreateSample(&sample);
                    sample->AddBuffer(buf.Get());
                    sample->SetSampleTime(idx * dur100ns);
                    sample->SetSampleDuration(dur100ns);
                    HRESULT ph = mft->ProcessInput(0, sample.Get(), 0);
                    if (SUCCEEDED(ph)) {
                        --needInput;
                        ++nSubmitted;
                        slot = (slot + 1) % kPool;
                    } else if (++nInputFail <= 3) {
                        sprintf_s(lb, "ProcessInput failed 0x%08X", (unsigned)ph);
                        LogLine(path, lb);
                    }
                }
            }
        }
        ++idx;
    }

    // ---------- Finish: stop capture and audio, flush encoder, close file ----------
    wgc.Stop();
    for (auto& a : audio) a->Stop();
    mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
    mft->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
    ULONGLONG deadline = GetTickCount64() + 3000;
    while (!drained && GetTickCount64() < deadline) {
        pump();
        Sleep(5);
    }
    pump();
    mkv.Close();
    sprintf_s(lb, "capture frames received: %llu", (unsigned long long)wgc.Frames());
    LogLine(path, lb);
    sprintf_s(lb, "stop: need-input events=%d, frames submitted=%d, encoder outputs=%d, header=%s",
              nNeedEvents, nSubmitted, nOutputs, mkv.HasParamSets() ? "yes" : "no");
    LogLine(path, lb);

    mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
    mft->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, 0);
    if (timer) CloseHandle(timer);
    if (plainTimer) timeEndPeriod(1);
    return nullptr;
}
