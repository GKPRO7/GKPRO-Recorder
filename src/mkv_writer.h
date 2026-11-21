#pragma once
// Minimal Matroska (MKV) writer for a single H.264 video track.
// Clusters are written whole at every keyframe (~2 s), so a crash loses at most ~2 s.

#include <windows.h>
#include <cstdint>
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

    // One H.264 access unit in Annex-B format (start codes). ptsMs = timestamp in ms.
    void WriteAccessUnit(const uint8_t* data, size_t size, int64_t ptsMs) {
        if (m_file == INVALID_HANDLE_VALUE) return;

        std::vector<Nal> nals;
        SplitNals(data, size, nals);

        Buf payload;
        bool key = false;
        Buf sps, pps;
        for (auto& n : nals) {
            if (n.n == 0) continue;
            int type = n.p[0] & 0x1F;
            if (type == 7) { sps.assign(n.p, n.p + n.n); continue; }
            if (type == 8) { pps.assign(n.p, n.p + n.n); continue; }
            if (type == 9) continue;  // access unit delimiter
            if (type == 5) key = true;
            payload.push_back((n.n >> 24) & 0xFF);
            payload.push_back((n.n >> 16) & 0xFF);
            payload.push_back((n.n >> 8) & 0xFF);
            payload.push_back(n.n & 0xFF);
            payload.insert(payload.end(), n.p, n.p + n.n);
        }
        if (payload.empty()) return;

        if (!m_headerDone) {
            if (!key || sps.empty() || pps.empty()) return;  // wait for the first keyframe with SPS/PPS
            WriteHeader(sps, pps);
            m_headerDone = true;
            m_basePts = ptsMs;
        }

        int64_t t = ptsMs - m_basePts;
        if (t < 0) t = 0;
        if (key || m_cluster.empty() || t - m_clusterTs > 30000) {
            FlushCluster();
            m_clusterTs = t;
        }
        int64_t rel = t - m_clusterTs;
        if (rel > 32767) rel = 32767;

        Buf blk;
        blk.push_back(0x81);  // track number 1
        blk.push_back((rel >> 8) & 0xFF);
        blk.push_back(rel & 0xFF);
        blk.push_back(key ? 0x80 : 0x00);
        blk.insert(blk.end(), payload.begin(), payload.end());
        Elem(m_cluster, 0xA3, blk);
    }

    void Close() {
        if (m_file == INVALID_HANDLE_VALUE) return;
        FlushCluster();
        CloseHandle(m_file);
        m_file = INVALID_HANDLE_VALUE;
    }

private:
    using Buf = std::vector<uint8_t>;
    struct Nal { const uint8_t* p; size_t n; };

    HANDLE m_file = INVALID_HANDLE_VALUE;
    int m_w = 0, m_h = 0, m_fps = 30;
    bool m_headerDone = false;
    int64_t m_basePts = 0, m_clusterTs = 0;
    Buf m_cluster;

    static void SplitNals(const uint8_t* d, size_t sz, std::vector<Nal>& out) {
        ptrdiff_t start = -1;
        size_t i = 0;
        auto push = [&](ptrdiff_t s, size_t e) {
            while (e > (size_t)s && d[e - 1] == 0) --e;  // trailing zero bytes belong to the next start code
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
    static void ElemStr(Buf& b, uint32_t id, const char* s) {
        Elem(b, id, (const uint8_t*)s, strlen(s));
    }

    void Write(const Buf& b) {
        DWORD written = 0;
        WriteFile(m_file, b.data(), (DWORD)b.size(), &written, nullptr);
    }

    void WriteHeader(const Buf& sps, const Buf& pps) {
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

        // Segment with unknown size (valid for live/crash-safe writing)
        PutId(out, 0x18538067);
        const uint8_t unknown[8] = {0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
        out.insert(out.end(), unknown, unknown + 8);

        Buf info;
        ElemUInt(info, 0x2AD7B1, 1000000);  // 1 ms timestamp scale
        ElemStr(info, 0x4D80, "GKPRO Recorder");
        ElemStr(info, 0x5741, "GKPRO Recorder");
        Elem(out, 0x1549A966, info);

        Buf avcc;
        avcc.push_back(1);
        avcc.push_back(sps[1]);
        avcc.push_back(sps[2]);
        avcc.push_back(sps[3]);
        avcc.push_back(0xFF);  // 4-byte NAL lengths
        avcc.push_back(0xE1);  // one SPS
        avcc.push_back((sps.size() >> 8) & 0xFF);
        avcc.push_back(sps.size() & 0xFF);
        avcc.insert(avcc.end(), sps.begin(), sps.end());
        avcc.push_back(1);     // one PPS
        avcc.push_back((pps.size() >> 8) & 0xFF);
        avcc.push_back(pps.size() & 0xFF);
        avcc.insert(avcc.end(), pps.begin(), pps.end());

        Buf video;
        ElemUInt(video, 0xB0, m_w);
        ElemUInt(video, 0xBA, m_h);

        Buf te;
        ElemUInt(te, 0xD7, 1);       // track number
        ElemUInt(te, 0x73C5, 1);     // track UID
        ElemUInt(te, 0x83, 1);       // type: video
        ElemUInt(te, 0x9C, 0);       // no lacing
        ElemStr(te, 0x86, "V_MPEG4/ISO/AVC");
        Elem(te, 0x63A2, avcc);
        ElemUInt(te, 0x23E383, 1000000000ull / (m_fps > 0 ? m_fps : 30));
        Elem(te, 0xE0, video);

        Buf tracks;
        Elem(tracks, 0xAE, te);
        Elem(out, 0x1654AE6B, tracks);
        Write(out);
    }

    void FlushCluster() {
        if (m_cluster.empty()) return;
        Buf c;
        ElemUInt(c, 0xE7, (uint64_t)m_clusterTs);
        c.insert(c.end(), m_cluster.begin(), m_cluster.end());
        Buf out;
        Elem(out, 0x1F43B675, c);
        Write(out);
        m_cluster.clear();
    }
};
