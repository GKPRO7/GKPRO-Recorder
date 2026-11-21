#include "audio_capture.h"
#include "mkv_writer.h"

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <avrt.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <wrl/client.h>
#include <algorithm>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {
constexpr int kRate = 48000;
constexpr int kChannels = 2;
constexpr int kAacFrame = 1024;
}

void AudioCapture::Start(MkvWriter* mkv, int64_t t0_100ns) {
    if (m_run.load()) return;
    m_mkv = mkv;
    m_t0 = t0_100ns;
    m_run = true;
    m_thread = std::thread(&AudioCapture::Run, this);
}

void AudioCapture::Stop() {
    m_run = false;
    if (m_thread.joinable()) m_thread.join();
}

void AudioCapture::Run() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION, MFSTARTUP_LITE);
    DWORD taskIdx = 0;
    HANDLE mm = AvSetMmThreadCharacteristicsW(L"Capture", &taskIdx);

    do {
        // ---- WASAPI device ----
        ComPtr<IMMDeviceEnumerator> en;
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en)))) break;
        ComPtr<IMMDevice> dev;
        if (FAILED(en->GetDefaultAudioEndpoint(m_loopback ? eRender : eCapture, eConsole, &dev))) break;
        ComPtr<IAudioClient> ac;
        if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&ac))) break;

        WAVEFORMATEX wf = {};
        wf.wFormatTag = WAVE_FORMAT_PCM;
        wf.nChannels = kChannels;
        wf.nSamplesPerSec = kRate;
        wf.wBitsPerSample = 16;
        wf.nBlockAlign = kChannels * 2;
        wf.nAvgBytesPerSec = kRate * wf.nBlockAlign;
        DWORD flags = AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
        if (m_loopback) flags |= AUDCLNT_STREAMFLAGS_LOOPBACK;
        if (FAILED(ac->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 2000000, 0, &wf, nullptr))) break;
        ComPtr<IAudioCaptureClient> cap;
        if (FAILED(ac->GetService(IID_PPV_ARGS(&cap)))) break;

        // ---- AAC encoder (Media Foundation, software, light) ----
        ComPtr<IMFTransform> enc;
        {
            MFT_REGISTER_TYPE_INFO oi = {MFMediaType_Audio, MFAudioFormat_AAC};
            IMFActivate** acts = nullptr;
            UINT32 count = 0;
            HRESULT hr = MFTEnumEx(MFT_CATEGORY_AUDIO_ENCODER, MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER,
                                   nullptr, &oi, &acts, &count);
            if (FAILED(hr) || count == 0) { if (acts) CoTaskMemFree(acts); break; }
            hr = acts[0]->ActivateObject(IID_PPV_ARGS(&enc));
            for (UINT32 i = 0; i < count; ++i) acts[i]->Release();
            CoTaskMemFree(acts);
            if (FAILED(hr)) break;
        }
        {
            ComPtr<IMFMediaType> ot;
            MFCreateMediaType(&ot);
            ot->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
            ot->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
            ot->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kRate);
            ot->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, kChannels);
            ot->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
            ot->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 16000);  // 128 kbps
            ot->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);                // raw AAC frames
            ot->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0x29);
            if (FAILED(enc->SetOutputType(0, ot.Get(), 0))) break;

            ComPtr<IMFMediaType> it;
            MFCreateMediaType(&it);
            it->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
            it->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
            it->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kRate);
            it->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, kChannels);
            it->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
            it->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, kChannels * 2);
            it->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, kRate * kChannels * 2);
            if (FAILED(enc->SetInputType(0, it.Get(), 0))) break;
        }
        enc->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        enc->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

        int64_t samplesIn = 0;  // PCM frames given to the encoder (includes inserted silence)
        int64_t aacFrames = 0;  // AAC frames produced

        auto drainOutput = [&]() {
            for (;;) {
                MFT_OUTPUT_STREAM_INFO si = {};
                enc->GetOutputStreamInfo(0, &si);
                ComPtr<IMFSample> s;
                MFCreateSample(&s);
                ComPtr<IMFMediaBuffer> mb;
                MFCreateMemoryBuffer(si.cbSize ? si.cbSize : 8192, &mb);
                s->AddBuffer(mb.Get());
                MFT_OUTPUT_DATA_BUFFER ob = {};
                ob.pSample = s.Get();
                DWORD st = 0;
                HRESULT h = enc->ProcessOutput(0, 1, &ob, &st);
                if (ob.pEvents) ob.pEvents->Release();
                if (FAILED(h)) break;  // includes MF_E_TRANSFORM_NEED_MORE_INPUT
                ComPtr<IMFMediaBuffer> cb;
                if (FAILED(s->ConvertToContiguousBuffer(&cb))) break;
                BYTE* p = nullptr;
                DWORD len = 0;
                if (SUCCEEDED(cb->Lock(&p, nullptr, &len))) {
                    int64_t pts100 = aacFrames * kAacFrame * 10000000LL / kRate;
                    m_mkv->WriteAudio(m_track, p, len, pts100 / 10000);
                    ++aacFrames;
                    cb->Unlock();
                }
            }
        };

        auto feedPcm = [&](const int16_t* pcm, int frames) {
            if (frames <= 0) return;
            DWORD bytes = (DWORD)frames * kChannels * 2;
            ComPtr<IMFMediaBuffer> mb;
            if (FAILED(MFCreateMemoryBuffer(bytes, &mb))) return;
            BYTE* d = nullptr;
            if (FAILED(mb->Lock(&d, nullptr, nullptr))) return;
            memcpy(d, pcm, bytes);
            mb->Unlock();
            mb->SetCurrentLength(bytes);
            ComPtr<IMFSample> s;
            MFCreateSample(&s);
            s->AddBuffer(mb.Get());
            HRESULT h = enc->ProcessInput(0, s.Get(), 0);
            if (h == MF_E_NOTACCEPTING) {
                drainOutput();
                h = enc->ProcessInput(0, s.Get(), 0);
            }
            if (SUCCEEDED(h)) samplesIn += frames;
            drainOutput();
        };

        auto feedSilence = [&](int64_t frames) {
            std::vector<int16_t> z(4800 * kChannels, 0);
            while (frames > 0 && m_run.load()) {
                int n = (int)std::min<int64_t>(frames, 4800);
                feedPcm(z.data(), n);
                frames -= n;
            }
        };

        ac->Start();

        while (m_run.load()) {
            Sleep(10);
            UINT32 pkt = 0;
            while (SUCCEEDED(cap->GetNextPacketSize(&pkt)) && pkt > 0) {
                BYTE* d = nullptr;
                UINT32 nf = 0;
                DWORD fl = 0;
                UINT64 devPos = 0, qpcPos = 0;
                if (FAILED(cap->GetBuffer(&d, &nf, &fl, &devPos, &qpcPos))) break;
                const UINT32 nfOrig = nf;

                std::vector<int16_t> zeros;
                const int16_t* src = (const int16_t*)d;
                if (fl & AUDCLNT_BUFFERFLAGS_SILENT) {
                    zeros.assign((size_t)nf * kChannels, 0);
                    src = zeros.data();
                }

                int64_t ts = (int64_t)qpcPos - m_t0;  // 100 ns since recording start
                int64_t expected = samplesIn * 10000000LL / kRate;
                if (ts < 0) {
                    int64_t skip = std::min<int64_t>(nf, (-ts) * kRate / 10000000LL);
                    src += skip * kChannels;
                    nf -= (UINT32)skip;
                } else if (ts - expected > 400000) {  // gap > 40 ms: keep audio in sync with video
                    feedSilence((ts - expected) * kRate / 10000000LL);
                }
                if (nf > 0) feedPcm(src, (int)nf);
                cap->ReleaseBuffer(nfOrig);
            }
        }

        ac->Stop();
        enc->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        enc->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
        drainOutput();
    } while (false);

    if (mm) AvRevertMmThreadCharacteristics(mm);
    MFShutdown();
    CoUninitialize();
}
