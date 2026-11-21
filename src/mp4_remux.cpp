#include "mp4_remux.h"

#include <windows.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace {

// ---------------- EBML reading (memory-mapped MKV) ----------------
bool ReadId(const uint8_t*& p, const uint8_t* end, uint32_t& id) {
    if (p >= end) return false;
    uint8_t b = *p;
    int len = 1;
    uint8_t mask = 0x80;
    while (len <= 4 && !(b & mask)) { mask >>= 1; ++len; }
    if (len > 4 || p + len > end) return false;
    id = 0;
    for (int i = 0; i < len; ++i) id = (id << 8) | p[i];
    p += len;
    return true;
}

bool ReadSize(const uint8_t*& p, const uint8_t* end, uint64_t& sz, bool& unknown) {
    if (p >= end) return false;
    uint8_t b = *p;
    int len = 1;
    uint8_t mask = 0x80;
    while (len <= 8 && !(b & mask)) { mask >>= 1; ++len; }
    if (len > 8 || p + len > end) return false;
    uint8_t first = b & (uint8_t)(mask - 1);
    uint64_t v = first;
    bool ones = (first == (uint8_t)(mask - 1));
    for (int i = 1; i < len; ++i) {
        v = (v << 8) | p[i];
        if (p[i] != 0xFF) ones = false;
    }
    unknown = ones;
    sz = v;
    p += len;
    return true;
}

uint64_t ReadUInt(const uint8_t* p, const uint8_t* e) {
    uint64_t v = 0;
    while (p < e) v = (v << 8) | *p++;
    return v;
}

template <class F>
void ForEach(const uint8_t* p, const uint8_t* end, F f) {
    while (p < end) {
        uint32_t id;
        uint64_t sz;
        bool unk;
        if (!ReadId(p, end, id) || !ReadSize(p, end, sz, unk)) break;
        const uint8_t* e = (unk || sz > (uint64_t)(end - p)) ? end : p + sz;
        f(id, p, e);
        p = e;
    }
}

// ---------------- Data model ----------------
struct Sample { uint64_t off; uint32_t size; int64_t ptsMs; bool key; };

struct Track {
    int number = 0;
    int type = 0;  // 1 = video, 2 = audio
    std::vector<uint8_t> priv;
    uint32_t w = 0, h = 0, rate = 48000, channels = 2;
    std::vector<Sample> samples;
};

// ---------------- MP4 box builder ----------------
struct Bx {
    std::vector<uint8_t> b;
    void u8(uint32_t v) { b.push_back((uint8_t)v); }
    void u16(uint32_t v) { u8(v >> 8); u8(v); }
    void u32(uint32_t v) { u16(v >> 16); u16(v); }
    void u64(uint64_t v) { u32((uint32_t)(v >> 32)); u32((uint32_t)v); }
    void bytes(const void* p, size_t n) { auto c = (const uint8_t*)p; b.insert(b.end(), c, c + n); }
    void zeros(size_t n) { b.insert(b.end(), n, 0); }
    size_t begin(const char* type) { size_t pos = b.size(); u32(0); bytes(type, 4); return pos; }
    void end(size_t pos) {
        uint32_t sz = (uint32_t)(b.size() - pos);
        b[pos] = sz >> 24; b[pos + 1] = sz >> 16; b[pos + 2] = sz >> 8; b[pos + 3] = sz;
    }
    void matrix() {
        u32(0x00010000); u32(0); u32(0);
        u32(0); u32(0x00010000); u32(0);
        u32(0); u32(0); u32(0x40000000);
    }
};

// ---------------- Buffered output ----------------
struct Out {
    HANDLE h = INVALID_HANDLE_VALUE;
    std::vector<uint8_t> buf;
    uint64_t pos = 0;  // absolute position including buffered bytes
    bool ok = true;
    void flush() {
        if (buf.empty()) return;
        DWORD w = 0;
        if (!WriteFile(h, buf.data(), (DWORD)buf.size(), &w, nullptr) || w != buf.size()) ok = false;
        buf.clear();
    }
    void write(const void* p, size_t n) {
        auto c = (const uint8_t*)p;
        buf.insert(buf.end(), c, c + n);
        pos += n;
        if (buf.size() >= (4u << 20)) flush();
    }
};

void WriteDecoderConfig(Bx& x, const Track& t, uint32_t trackId);

void BuildTrak(Bx& x, const Track& t, uint32_t trackId, uint32_t movieDurMs) {
    const bool video = t.type == 1;
    const uint32_t timescale = video ? 1000 : t.rate;

    // media duration + stts
    std::vector<std::pair<uint32_t, uint32_t>> stts;  // (count, delta)
    uint64_t mediaDur = 0;
    if (video) {
        for (size_t i = 0; i < t.samples.size(); ++i) {
            int64_t d = (i + 1 < t.samples.size()) ? t.samples[i + 1].ptsMs - t.samples[i].ptsMs : 33;
            if (d < 1) d = 1;
            mediaDur += d;
            if (!stts.empty() && stts.back().second == (uint32_t)d) stts.back().first++;
            else stts.push_back({1, (uint32_t)d});
        }
    } else {
        stts.push_back({(uint32_t)t.samples.size(), 1024});
        mediaDur = (uint64_t)t.samples.size() * 1024;
    }

    size_t trak = x.begin("trak");
    {
        size_t tkhd = x.begin("tkhd");
        x.u32(0x00000003);
        x.u32(0); x.u32(0); x.u32(trackId); x.u32(0);
        x.u32(movieDurMs);
        x.u32(0); x.u32(0);
        x.u16(0); x.u16(0);
        x.u16(video ? 0 : 0x0100);
        x.u16(0);
        x.matrix();
        x.u32(video ? (t.w << 16) : 0);
        x.u32(video ? (t.h << 16) : 0);
        x.end(tkhd);

        size_t mdia = x.begin("mdia");
        {
            size_t mdhd = x.begin("mdhd");
            x.u32(0);
            x.u32(0); x.u32(0);
            x.u32(timescale);
            x.u32((uint32_t)mediaDur);
            x.u16(0x55C4);
            x.u16(0);
            x.end(mdhd);

            size_t hdlr = x.begin("hdlr");
            x.u32(0); x.u32(0);
            x.bytes(video ? "vide" : "soun", 4);
            x.zeros(12);
            const char* name = video ? "VideoHandler" : "SoundHandler";
            x.bytes(name, strlen(name) + 1);
            x.end(hdlr);

            size_t minf = x.begin("minf");
            {
                if (video) {
                    size_t vmhd = x.begin("vmhd");
                    x.u32(1); x.u16(0); x.zeros(6);
                    x.end(vmhd);
                } else {
                    size_t smhd = x.begin("smhd");
                    x.u32(0); x.u16(0); x.u16(0);
                    x.end(smhd);
                }
                size_t dinf = x.begin("dinf");
                size_t dref = x.begin("dref");
                x.u32(0); x.u32(1);
                size_t url = x.begin("url ");
                x.u32(1);
                x.end(url);
                x.end(dref);
                x.end(dinf);

                size_t stbl = x.begin("stbl");
                {
                    size_t stsd = x.begin("stsd");
                    x.u32(0); x.u32(1);
                    WriteDecoderConfig(x, t, trackId);
                    x.end(stsd);

                    size_t b = x.begin("stts");
                    x.u32(0); x.u32((uint32_t)stts.size());
                    for (auto& e : stts) { x.u32(e.first); x.u32(e.second); }
                    x.end(b);

                    if (video) {
                        b = x.begin("stss");
                        std::vector<uint32_t> keys;
                        for (size_t i = 0; i < t.samples.size(); ++i)
                            if (t.samples[i].key) keys.push_back((uint32_t)i + 1);
                        x.u32(0); x.u32((uint32_t)keys.size());
                        for (auto k : keys) x.u32(k);
                        x.end(b);
                    }

                    b = x.begin("stsc");
                    x.u32(0); x.u32(1);
                    x.u32(1); x.u32(1); x.u32(1);
                    x.end(b);

                    b = x.begin("stsz");
                    x.u32(0); x.u32(0); x.u32((uint32_t)t.samples.size());
                    for (auto& s : t.samples) x.u32(s.size);
                    x.end(b);

                    b = x.begin("co64");
                    x.u32(0); x.u32((uint32_t)t.samples.size());
                    for (auto& s : t.samples) x.u64(s.off);
                    x.end(b);
                }
                x.end(stbl);
            }
            x.end(minf);
        }
        x.end(mdia);
    }
    x.end(trak);
}

void WriteDecoderConfig(Bx& x, const Track& t, uint32_t trackId) {
    if (t.type == 1) {
        size_t e = x.begin("avc1");
        x.zeros(6); x.u16(1);
        x.u16(0); x.u16(0); x.zeros(12);
        x.u16(t.w); x.u16(t.h);
        x.u32(0x00480000); x.u32(0x00480000);
        x.u32(0);
        x.u16(1);
        x.zeros(32);
        x.u16(0x0018);
        x.u16(0xFFFF);
        size_t c = x.begin("avcC");
        x.bytes(t.priv.data(), t.priv.size());
        x.end(c);
        x.end(e);
    } else {
        size_t e = x.begin("mp4a");
        x.zeros(6); x.u16(1);
        x.u32(0); x.u32(0);
        x.u16(t.channels); x.u16(16);
        x.u16(0); x.u16(0);
        x.u32(t.rate << 16);
        size_t es = x.begin("esds");
        x.u32(0);
        uint32_t ascLen = (uint32_t)t.priv.size();
        x.u8(0x03); x.u8(23 + ascLen); x.u16(trackId); x.u8(0);
        x.u8(0x04); x.u8(15 + ascLen); x.u8(0x40); x.u8(0x15);
        x.zeros(3);
        x.u32(128000); x.u32(128000);
        x.u8(0x05); x.u8(ascLen);
        x.bytes(t.priv.data(), t.priv.size());
        x.u8(0x06); x.u8(1); x.u8(0x02);
        x.end(es);
        x.end(e);
    }
}

}  // namespace

bool RemuxMkvToMp4(const std::wstring& mkvPath, const std::wstring& mp4Path, std::wstring& error) {
    // ---- map the MKV ----
    HANDLE f = CreateFileW(mkvPath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) { error = L"Cannot open the MKV file."; return false; }
    LARGE_INTEGER fs;
    GetFileSizeEx(f, &fs);
    if (fs.QuadPart < 64) { CloseHandle(f); error = L"The MKV file is empty."; return false; }
    HANDLE map = CreateFileMappingW(f, nullptr, PAGE_READONLY, 0, 0, nullptr);
    const uint8_t* base = map ? (const uint8_t*)MapViewOfFile(map, FILE_MAP_READ, 0, 0, 0) : nullptr;
    if (!base) {
        if (map) CloseHandle(map);
        CloseHandle(f);
        error = L"Cannot read the MKV file.";
        return false;
    }
    const uint8_t* end = base + fs.QuadPart;

    // ---- output file: ftyp + mdat header ----
    Out out;
    out.h = CreateFileW(mp4Path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    auto cleanup = [&]() {
        UnmapViewOfFile(base);
        CloseHandle(map);
        CloseHandle(f);
        if (out.h != INVALID_HANDLE_VALUE) CloseHandle(out.h);
    };
    if (out.h == INVALID_HANDLE_VALUE) { cleanup(); error = L"Cannot create the MP4 file."; return false; }

    {
        Bx ft;
        size_t p = ft.begin("ftyp");
        ft.bytes("isom", 4); ft.u32(512);
        ft.bytes("isom", 4); ft.bytes("iso2", 4); ft.bytes("avc1", 4); ft.bytes("mp41", 4);
        ft.end(p);
        out.write(ft.b.data(), ft.b.size());
    }
    const uint64_t mdatStart = out.pos;
    {
        Bx md;
        md.u32(1); md.bytes("mdat", 4); md.u64(0);  // 64-bit size patched at the end
        out.write(md.b.data(), md.b.size());
    }

    // ---- parse ----
    std::vector<Track> tracks;
    auto findTrack = [&](int n) -> Track* {
        for (auto& t : tracks) if (t.number == n) return &t;
        return nullptr;
    };

    ForEach(base, end, [&](uint32_t id, const uint8_t* p, const uint8_t* e) {
        if (id != 0x18538067) return;  // Segment
        ForEach(p, e, [&](uint32_t id2, const uint8_t* q, const uint8_t* qe) {
            if (id2 == 0x1654AE6B) {  // Tracks
                ForEach(q, qe, [&](uint32_t id3, const uint8_t* r, const uint8_t* re) {
                    if (id3 != 0xAE) return;
                    Track t;
                    ForEach(r, re, [&](uint32_t id4, const uint8_t* s, const uint8_t* se) {
                        switch (id4) {
                        case 0xD7: t.number = (int)ReadUInt(s, se); break;
                        case 0x83: t.type = (int)ReadUInt(s, se); break;
                        case 0x63A2: t.priv.assign(s, se); break;
                        case 0xE0:
                            ForEach(s, se, [&](uint32_t i5, const uint8_t* a, const uint8_t* ae) {
                                if (i5 == 0xB0) t.w = (uint32_t)ReadUInt(a, ae);
                                if (i5 == 0xBA) t.h = (uint32_t)ReadUInt(a, ae);
                            });
                            break;
                        case 0xE1:
                            ForEach(s, se, [&](uint32_t i5, const uint8_t* a, const uint8_t* ae) {
                                if (i5 == 0xB5) {
                                    if (ae - a == 8) {
                                        uint64_t bits = ReadUInt(a, ae);
                                        double d;
                                        memcpy(&d, &bits, 8);
                                        t.rate = (uint32_t)d;
                                    } else if (ae - a == 4) {
                                        uint32_t bits = (uint32_t)ReadUInt(a, ae);
                                        float fl;
                                        memcpy(&fl, &bits, 4);
                                        t.rate = (uint32_t)fl;
                                    }
                                }
                                if (i5 == 0x9F) t.channels = (uint32_t)ReadUInt(a, ae);
                            });
                            break;
                        }
                    });
                    if (t.number > 0 && (t.type == 1 || t.type == 2)) tracks.push_back(std::move(t));
                });
            } else if (id2 == 0x1F43B675) {  // Cluster
                int64_t clusterTs = 0;
                ForEach(q, qe, [&](uint32_t id3, const uint8_t* r, const uint8_t* re) {
                    if (id3 == 0xE7) { clusterTs = (int64_t)ReadUInt(r, re); return; }
                    if (id3 != 0xA3) return;  // SimpleBlock
                    uint64_t tn;
                    bool unk;
                    if (!ReadSize(r, re, tn, unk)) return;
                    if (re - r < 4) return;
                    int16_t rel = (int16_t)((r[0] << 8) | r[1]);
                    uint8_t flags = r[2];
                    const uint8_t* data = r + 3;
                    Track* t = findTrack((int)tn);
                    if (!t) return;
                    uint32_t n = (uint32_t)(re - data);
                    Sample s;
                    s.off = out.pos;
                    s.size = n;
                    s.ptsMs = clusterTs + rel;
                    s.key = (flags & 0x80) != 0;
                    out.write(data, n);
                    t->samples.push_back(s);
                });
            }
        });
    });
    UnmapViewOfFile(base);
    base = nullptr;

    // ---- drop empty tracks, shift video to start at 0 ----
    tracks.erase(std::remove_if(tracks.begin(), tracks.end(), [](const Track& t) { return t.samples.empty(); }), tracks.end());
    Track* video = nullptr;
    for (auto& t : tracks) if (t.type == 1) video = &t;
    if (!video || out.pos == mdatStart + 16 || !out.ok) {
        out.flush();
        CloseHandle(map);
        CloseHandle(f);
        CloseHandle(out.h);
        DeleteFileW(mp4Path.c_str());
        error = L"No video data found in the MKV.";
        return false;
    }

    uint64_t durMs = 0;
    for (auto& t : tracks) {
        uint64_t d = (t.type == 1)
            ? (uint64_t)(t.samples.back().ptsMs - t.samples.front().ptsMs + 33)
            : (uint64_t)t.samples.size() * 1024 * 1000 / t.rate;
        durMs = std::max(durMs, d);
    }

    // ---- moov ----
    Bx x;
    size_t moov = x.begin("moov");
    {
        size_t mvhd = x.begin("mvhd");
        x.u32(0); x.u32(0); x.u32(0);
        x.u32(1000);
        x.u32((uint32_t)durMs);
        x.u32(0x00010000);
        x.u16(0x0100);
        x.zeros(10);
        x.matrix();
        x.zeros(24);
        x.u32((uint32_t)tracks.size() + 1);
        x.end(mvhd);
    }
    uint32_t id = 1;
    for (auto& t : tracks) BuildTrak(x, t, id++, (uint32_t)durMs);
    x.end(moov);
    out.write(x.b.data(), x.b.size());
    out.flush();

    // patch the 64-bit mdat size
    {
        uint64_t mdatSize = (out.pos - x.b.size()) - mdatStart;
        uint8_t sz[8];
        for (int i = 0; i < 8; ++i) sz[i] = (uint8_t)(mdatSize >> (56 - 8 * i));
        LARGE_INTEGER li;
        li.QuadPart = (LONGLONG)mdatStart + 8;
        SetFilePointerEx(out.h, li, nullptr, FILE_BEGIN);
        DWORD w = 0;
        WriteFile(out.h, sz, 8, &w, nullptr);
    }

    bool ok = out.ok;
    CloseHandle(map);
    CloseHandle(f);
    CloseHandle(out.h);
    if (!ok) { error = L"Disk write error while creating the MP4."; return false; }
    return true;
}
