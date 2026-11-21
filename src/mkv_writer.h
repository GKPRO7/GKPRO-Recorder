#pragma once
// Minimal Matroska (MKV) writer: one H.264 video track + up to a few AAC audio tracks.
// Clusters are written whole at every keyframe (~2 s), so a crash loses at most ~2 s.
// Thread-safe: video and audio threads may call the Write* functions concurrently.

#include <windows.h>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

class MkvWriter {
public:
    ~MkvWriter() { Close(); }

    bool Open(const std::wstring& path, int w, int h, int fps) {
        m_w = w; m_h = h; m_fps = fps;
        m_file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        return m_file != INVALID_HANDLE_VALUE;
    }

    // Must be called before the first video frame is written.
    void AddAudioTrack(int number, int sampleRate, int channels, const uint8_t* asc, size_t ascLen, const char* name) {
        std::lock_guard<std::mutex> lk(m_mx);
        AudioTrack t;
        t.num = number; t.rate = sampleRate; t.ch = channels;
        t.asc.assign(asc, asc + ascLen);
        t.name = name;
        m_audio.push_back(std::move(t));
    }

    // Optional: SPS/PPS from the encoder's output type (Annex-B blob).
    void SetSequenceHeader(const uint8_t* d, size_t n) {
        std::lock_guard<std::mutex> lk(m_mx);
        std::vector<Nal> nals;
        SplitNals(d, n, nals);
        for (auto& x : nals) {
            if (x.n == 0) continue;
            int t = x.p[0] & 0x1F;
            if (t == 7) m_sps.assign(x.p, x.p + x.n);
            if (t == 8) m_pps.assign(x.p, x.p + x.n);
        }
    }
    bool HasParamSets() {
        std::lock_guard<std::mutex> lk(m_mx);
        return !m_sps.empty() && !m_pps.empty();
    }

    // One H.264 access unit (Annex-B or length-prefixed). ptsMs = timestamp in ms.
    void WriteAccessUnit(const uint8_t* data, size_t size, int64_t ptsMs, bool keyHint = false) {
        std::lock_guard<std::mutex> lk(m_mx);
        if (m_file == INVALID_HANDLE_VALUE) return;

        std::vector<Nal> nals;
        SplitNals(data, size, nals);
        if (nals.empty()) {  // not Annex-B: try 4-byte length-prefixed NALs
            size_t i = 0;
            while (i + 4 <= size) {
                size_t n = ((size_t)data[i] << 24) | ((size_t)data[i + 1] << 16) | ((size_t)data[i + 2] << 8) | data[i + 3];
                i += 4;
                if (n == 0 || i + n > size) break;
                nals.push_back({data + i, n});
                i += n;
            }
        }

        Buf payload;
        bool key = false;
        for (auto& n : nals) {
            if (n.n == 0) continue;
            int type = n.p[0] & 0x1F;
            if (type == 7) { m_sps.assign(n.p, n.p + n.n); continue; }
            if (type == 8) { m_pps.assign(n.p, n.p + n.n); continue; }
            if (type == 9) continue;  // access unit delimiter
            if (type == 5) key = true;
            payload.push_back((n.n >> 24) & 0xFF);
            payload.push_back((n.n >> 16) & 0xFF);
            payload.push_back((n.n >> 8) & 0xFF);
            payload.push_back(n.n & 0xFF);
            payload.insert(payload.end(), n.p, n.p + n.n);
        }
        if (payload.empty()) return;
        if (keyHint) key = true;

        bool firstWrite = false;
        if (!m_headerDone) {
            if (!key || m_sps.empty() || m_pps.empty()) return;  // wait for first keyframe + SPS/PPS
            WriteHeader();
            m_headerDone = true;
            m_basePts = ptsMs;
            firstWrite = true;
        }

        int64_t t = ptsMs - m_basePts;
        if (t < 0) t = 0;
        if (key || m_cluster.empty() || t - m_clusterTs > 30000) {
            FlushCluster();
            m_clusterTs = t;
        }
        AppendBlock(1, t, key, payload.data(), payload.size());

        if (firstWrite) {  // audio that arrived before the first keyframe
            for (auto& p : m_pending) {
                int64_t at = p.ptsMs - m_basePts;
                if (at >= 0) AppendBlock(p.track, at, true, p.data.data(), p.data.size());
            }
            m_pending.clear();
        }
    }

    // One raw AAC frame for the given audio track.
    void WriteAudio(int track, const uint8_t* d, size_t n, int64_t ptsMs) {
        std::lock_guard<std::mutex> lk(m_mx);
        if (m_file == INVALID_HANDLE_VALUE) return;
        if (!m_headerDone) {
            if (m_pending.size() < 4000) m_pending.push_back({track, Buf(d, d + n), ptsMs});
            return;
        }
        int64_t t = ptsMs - m_basePts;
        if (t < 0) return;
        if (m_cluster.empty()) m_clusterTs = t;
        AppendBlock(track, t, true, d, n);
    }

    void Close() {
        std::lock_guard<std::mutex> lk(m_mx);
        if (m_file == INVALID_HANDLE_VALUE) return;
        FlushCluster();
        CloseHandle(m_file);
        m_file = INVALID_HANDLE_VALUE;
    }

private:
    using Buf = std::vector<uint8_t>;
    struct Nal { const uint8_t* p; size_t n; };
    struct AudioTrack { int num = 2, rate = 48000, ch = 2; Buf asc; std::string name; };
    struct Pending { int track; Buf data; int64_t ptsMs; };

    std::mutex m_mx;
    HANDLE m_file = INVALID_HANDLE_VALUE;
    int m_w = 0, m_h = 0, m_fps = 30;
    bool m_headerDone = false;
    int64_t m_basePts = 0, m_clusterTs = 0;
    Buf m_cluster;
    Buf m_sps, m_pps;
    std::vector<AudioTrack> m_audio;
    std::vector<Pending> m_pending;

    static void SplitNals(const uint8_t* d, size_t sz, std::vector<Nal>& out) {
        ptrdiff_t start = -1;
        size_t i = 0;
        auto push = [&](ptrdiff_t s, size_t e) {
            while (e > (size_t)s && d[e - 1] == 0) --e;
            if (e > (size_t)s) out.push_back({d + s, e - (size_t)s});
        };
        while (i + 2 < sz) {
            if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) {
                if (start >= 0) push(start, i);
                start = (ptrdiff_t)(i + 3);
                i += 3;
            } else {
                ++i;
            }
        }
        if (start >= 0 && (size_t)start < sz) push(start, sz);
    }

    static void PutId(Buf& b, uint32_t id) {
        if (id > 0xFFFFFF) b.push_back((id >> 24) & 0xFF);
        if (id > 0xFFFF) b.push_back((id >> 16) & 0xFF);
        if (id > 0xFF) b.push_back((id >> 8) & 0xFF);
        b.push_back(id & 0xFF);
    }
    static void PutSize(Buf& b, uint64_t n) {  // always 8-byte size field
        b.push_back(0x01);
        for (int s = 48; s >= 0; s -= 8) b.push_back((n >> s) & 0xFF);
    }
    static void Elem(Buf& b, uint32_t id, const uint8_t* p, size_t n) {
        PutId(b, id);
        PutSize(b, n);
        b.insert(b.end(), p, p + n);
    }
    static void Elem(Buf& b, uint32_t id, const Buf& payload) { Elem(b, id, payload.data(), payload.size()); }
    static void ElemUInt(Buf& b, uint32_t id, uint64_t v) {
        Buf p;
        int n = 1;
        while (n < 8 && (v >> (8 * n))) ++n;
        for (int i = n - 1; i >= 0; --i) p.push_back((v >> (8 * i)) & 0xFF);
        Elem(b, id, p);
    }
    static void ElemStr(Buf& b, uint32_t id, const char* s) { Elem(b, id, (const uint8_t*)s, strlen(s)); }
    static void ElemDouble(Buf& b, uint32_t id, double v) {
        uint64_t bits;
        memcpy(&bits, &v, 8);
        Buf p;
        for (int s = 56; s >= 0; s -= 8) p.push_back((bits >> s) & 0xFF);
        Elem(b, id, p);
    }

    void Write(const Buf& b) {
        DWORD written = 0;
        WriteFile(m_file, b.data(), (DWORD)b.size(), &written, nullptr);
    }

    void AppendBlock(int track, int64_t tMs, bool key, const uint8_t* d, size_t n) {
        int64_t rel = tMs - m_clusterTs;
        if (rel > 32767) rel = 32767;
        if (rel < -32768) rel = -32768;
        uint16_t r = (uint16_t)(int16_t)rel;
        Buf blk;
        blk.reserve(n + 4);
        blk.push_back(0x80 | (track & 0x7F));
        blk.push_back((r >> 8) & 0xFF);
        blk.push_back(r & 0xFF);
        blk.push_back(key ? 0x80 : 0x00);
        blk.insert(blk.end(), d, d + n);
        Elem(m_cluster, 0xA3, blk);
    }

    void WriteHeader() {
        Buf ebml;
        ElemUInt(ebml, 0x4286, 1);
        ElemUInt(ebml, 0x42F7, 1);
        ElemUInt(ebml, 0x42F2, 4);
        ElemUInt(ebml, 0x42F3, 8);
        ElemStr(ebml, 0x4282, "matroska");
        ElemUInt(ebml, 0x4287, 4);
        ElemUInt(ebml, 0x4285, 2);

        Buf out;
        Elem(out, 0x1A45DFA3, ebml);

        PutId(out, 0x18538067);  // Segment, unknown size
        const uint8_t unknown[8] = {0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
        out.insert(out.end(), unknown, unknown + 8);

        Buf info;
        ElemUInt(info, 0x2AD7B1, 1000000);  // 1 ms timestamp scale
        ElemStr(info, 0x4D80, "GKPRO Recorder");
        ElemStr(info, 0x5741, "GKPRO Recorder");
        Elem(out, 0x1549A966, info);

        Buf avcc;
        avcc.push_back(1);
        avcc.push_back(m_sps[1]);
        avcc.push_back(m_sps[2]);
        avcc.push_back(m_sps[3]);
        avcc.push_back(0xFF);
        avcc.push_back(0xE1);
        avcc.push_back((m_sps.size() >> 8) & 0xFF);
        avcc.push_back(m_sps.size() & 0xFF);
        avcc.insert(avcc.end(), m_sps.begin(), m_sps.end());
        avcc.push_back(1);
        avcc.push_back((m_pps.size() >> 8) & 0xFF);
        avcc.push_back(m_pps.size() & 0xFF);
        avcc.insert(avcc.end(), m_pps.begin(), m_pps.end());

        Buf tracks;
        {
            Buf video;
            ElemUInt(video, 0xB0, m_w);
            ElemUInt(video, 0xBA, m_h);
            Buf te;
            ElemUInt(te, 0xD7, 1);
            ElemUInt(te, 0x73C5, 1);
            ElemUInt(te, 0x83, 1);
            ElemUInt(te, 0x9C, 0);
            ElemStr(te, 0x86, "V_MPEG4/ISO/AVC");
            Elem(te, 0x63A2, avcc);
            ElemUInt(te, 0x23E383, 1000000000ull / (m_fps > 0 ? m_fps : 30));
            Elem(te, 0xE0, video);
            Elem(tracks, 0xAE, te);
        }
        for (auto& a : m_audio) {
            Buf au;
            ElemDouble(au, 0xB5, (double)a.rate);
            ElemUInt(au, 0x9F, a.ch);
            Buf te;
            ElemUInt(te, 0xD7, a.num);
            ElemUInt(te, 0x73C5, a.num);
            ElemUInt(te, 0x83, 2);
            ElemUInt(te, 0x9C, 0);
            ElemStr(te, 0x86, "A_AAC");
            ElemStr(te, 0x536E, a.name.c_str());
            Elem(te, 0x63A2, a.asc);
            ElemUInt(te, 0x23E383, 1024ull * 1000000000ull / a.rate);
            Elem(te, 0xE1, au);
            Elem(tracks, 0xAE, te);
        }
        Elem(out, 0x1654AE6B, tracks);
        Write(out);
    }

    void FlushCluster() {
        if (m_cluster.empty()) return;
        Buf c;
        ElemUInt(c, 0xE7, (uint64_t)(m_clusterTs < 0 ? 0 : m_clusterTs));
        c.insert(c.end(), m_cluster.begin(), m_cluster.end());
        Buf out;
        Elem(out, 0x1F43B675, c);
        Write(out);
        m_cluster.clear();
    }
};
