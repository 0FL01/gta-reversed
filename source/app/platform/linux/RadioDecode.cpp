// RadioDecode implementation: real radio bytes via OS_File* + libvorbisfile.
#include "app/platform/linux/RadioDecode.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include <vorbis/vorbisfile.h>

using int8 = int8_t;
using int16 = int16_t;
using int32 = int32_t;
using int64 = int64_t;
using uint8 = uint8_t;
using uint16 = uint16_t;
using uint32 = uint32_t;
using uint64 = uint64_t;

#ifndef __stdcall
#define __stdcall
#endif

#include "oswrapper/oswrapper.h"

namespace {

constexpr int kPackNameBytes = 16;
constexpr int kTrackLookupBytes = 12;
constexpr uint32_t kTrackInfoBytes = 0x1F84; // sizeof(tTrackInfo)
constexpr int kReadChunkBytes = 1 << 20;

// CAEStreamTransformer::table (AEStreamTransformer.cpp 0x4f1750).
constexpr uint8_t kXorTable[16] = {
    0xEA, 0x3A, 0xC4, 0xA1, 0x9A, 0xA8, 0x14, 0xF3,
    0x48, 0xB0, 0xD7, 0x23, 0x9D, 0xE8, 0xFF, 0xF1,
};

uint32_t ReadU32LE(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

// Reads exactly `size` bytes from absolute file position `pos` in chunks.
// OS_File* uses int32 sizes, so large ranges are split.
bool ReadRange(void* file, uint32_t pos, uint8_t* dst, size_t size) {
    uint64_t remaining = size;
    uint64_t cursor = pos;
    while (remaining > 0) {
        OS_FileSetPosition(file, static_cast<int32_t>(cursor));
        const size_t want =
            remaining > kReadChunkBytes ? kReadChunkBytes : remaining;
        if (OS_FileRead(file, dst, static_cast<int32_t>(want)) != 0) {
            return false;
        }
        dst += want;
        cursor += want;
        remaining -= want;
    }
    return true;
}

bool ReadWholeFile(const char* path, std::vector<uint8_t>& out) {
    void* file = nullptr;
    if (OS_FileOpen(FILE_DATA_AREA_DEFAULT, &file, path,
                    FILE_ACCESS_READ) != 0 ||
        !file) {
        return false;
    }
    const int32_t size = OS_FileSize(file);
    bool ok = size >= 0;
    out.clear();
    if (ok && size > 0) {
        out.resize(static_cast<size_t>(size));
        ok = ReadRange(file, 0, out.data(), out.size());
    }
    OS_FileClose(file);
    return ok;
}

std::string Upper(std::string s) {
    for (char& c : s) {
        if (c >= 'a' && c <= 'z') {
            c = static_cast<char>(c - 'a' + 'A');
        }
    }
    return s;
}

uint64_t Fnv1a64(uint64_t hash, const uint8_t* data, size_t size) {
    for (size_t i = 0; i < size; ++i) {
        hash ^= data[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::string Hex16(const uint8_t* p) {
    char buf[33] = {};
    for (int i = 0; i < 16; ++i) {
        (void)std::snprintf(buf + i * 2, 3, "%02x", p[i]);
    }
    return std::string(buf);
}

// In-memory decrypted Ogg payload exposed through ov_callbacks, mirroring
// CAEVorbisDecoder's ov_open_callbacks over CAEDataStream (Read/Seek/Close/
// Tell in AEVorbisDecoder.cpp). The game decrypts inside FillBuffer during
// ov_read; we decrypt once up front with the identical position-keyed XOR,
// so the byte stream ov_* sees is the same.
struct MemStream {
    const uint8_t* data = nullptr;
    size_t size = 0;
    size_t pos = 0;
};

size_t VorbisRead(void* ptr, size_t size, size_t nmemb, void* opaque) {
    auto* s = static_cast<MemStream*>(opaque);
    if (!s || size == 0 || nmemb == 0) {
        return 0;
    }
    size_t want = size * nmemb;
    size_t avail = s->size > s->pos ? s->size - s->pos : 0;
    size_t got = want < avail ? want : avail;
    if (got > 0) {
        (void)std::memcpy(ptr, s->data + s->pos, got);
        s->pos += got;
    }
    return got / size;
}

int VorbisSeek(void* opaque, ogg_int64_t offset, int whence) {
    auto* s = static_cast<MemStream*>(opaque);
    if (!s) {
        return -1;
    }
    int64_t base = 0;
    if (whence == SEEK_SET) {
        base = 0;
    } else if (whence == SEEK_CUR) {
        base = static_cast<int64_t>(s->pos);
    } else if (whence == SEEK_END) {
        base = static_cast<int64_t>(s->size);
    } else {
        return -1;
    }
    int64_t target = base + static_cast<int64_t>(offset);
    if (target < 0 || static_cast<uint64_t>(target) > s->size) {
        return -1;
    }
    s->pos = static_cast<size_t>(target);
    return 0;
}

int VorbisClose(void* /*opaque*/) {
    return 0;
}

long VorbisTell(void* opaque) {
    auto* s = static_cast<MemStream*>(opaque);
    if (!s) {
        return -1;
    }
    return static_cast<long>(s->pos);
}

} // namespace

bool RadioDecode_Station(const std::string& station, int wantSeconds,
                         RadioDecodeResult& out) {
    out = RadioDecodeResult{};
    if (wantSeconds <= 0) {
        out.failReason = "bad seconds count";
        return false;
    }
    out.station = Upper(station);

    // 1. StrmPaks.dat: resolve the pack index by stream filename.
    std::vector<uint8_t> packs;
    if (!ReadWholeFile("audio/CONFIG/StrmPaks.dat", packs) ||
        packs.size() % kPackNameBytes != 0 || packs.empty()) {
        out.failReason = "cannot read StrmPaks.dat";
        return false;
    }
    int packNo = -1;
    char packName[17] = {};
    const size_t packCount = packs.size() / kPackNameBytes;
    for (size_t i = 0; i < packCount; ++i) {
        const uint8_t* e = packs.data() + i * kPackNameBytes;
        char name[17] = {};
        size_t len = 0;
        while (len < 16 && e[len] != '\0') {
            name[len] = static_cast<char>(e[len]);
            ++len;
        }
        name[len] = '\0';
        if (len > 0 && out.station == name) {
            packNo = static_cast<int>(i);
            (void)std::snprintf(packName, sizeof(packName), "%s", name);
            break;
        }
    }
    if (packNo < 0) {
        out.failReason = std::string("unknown station '") + station + "'";
        return false;
    }
    out.packId = packNo;

    // 2. TrakLkup.dat: collect this pack's tracks in file (global-id) order.
    std::vector<uint8_t> lookup;
    if (!ReadWholeFile("audio/CONFIG/TrakLkup.dat", lookup) ||
        lookup.size() % kTrackLookupBytes != 0 || lookup.empty()) {
        out.failReason = "cannot read TrakLkup.dat";
        return false;
    }
    struct TrackRef {
        uint32_t id;
        uint32_t offset;
        uint32_t size;
    };
    std::vector<TrackRef> tracks;
    const size_t trackTotal = lookup.size() / kTrackLookupBytes;
    for (size_t i = 0; i < trackTotal; ++i) {
        const uint8_t* e = lookup.data() + i * kTrackLookupBytes;
        if (e[0] == static_cast<uint8_t>(packNo)) {
            tracks.push_back(TrackRef{
                static_cast<uint32_t>(i),
                ReadU32LE(e + 4),
                ReadU32LE(e + 8),
            });
        }
    }
    if (tracks.empty()) {
        out.failReason = "station has no tracks";
        return false;
    }
    out.trackCount = static_cast<int>(tracks.size());
    const TrackRef& first = tracks.front();
    out.trackId = first.id;
    out.audioOffset = first.offset + kTrackInfoBytes;
    out.audioSize = first.size;

    // 3. Open the stream file; bounds-check the slice.
    char streamPath[64] = {};
    (void)std::snprintf(streamPath, sizeof(streamPath), "audio/streams/%s",
                        packName);
    void* stream = nullptr;
    if (OS_FileOpen(FILE_DATA_AREA_DEFAULT, &stream, streamPath,
                    FILE_ACCESS_READ) != 0 ||
        !stream) {
        out.failReason = "cannot open stream file";
        return false;
    }
    const int32_t streamSize = OS_FileSize(stream);
    const uint64_t sliceEnd =
        static_cast<uint64_t>(first.offset) + kTrackInfoBytes + first.size;
    if (streamSize < 0 || sliceEnd > static_cast<uint64_t>(streamSize)) {
        out.failReason = "track slice out of bounds";
        OS_FileClose(stream);
        return false;
    }

    // 4. tTrackInfo header: decrypt (absolute-position XOR, same as
    // CAEDataStream::FillBuffer) and parse the cut table: 1000 beats
    // {u32 time, u32 key} + 16 u32 + u16 (AETrackLoader.h tTrackInfo).
    std::vector<uint8_t> header(kTrackInfoBytes);
    if (!ReadRange(stream, first.offset, header.data(), header.size())) {
        out.failReason = "short header read";
        OS_FileClose(stream);
        return false;
    }
    for (uint32_t i = 0; i < kTrackInfoBytes; ++i) {
        header[i] ^= kXorTable[(first.offset + i) & 0xF];
    }
    uint32_t beats = 0;
    for (int b = 0; b < 1000; ++b) {
        const uint32_t time = ReadU32LE(header.data() + b * 8);
        const uint32_t key = ReadU32LE(header.data() + b * 8 + 4);
        if (b == 0) {
            out.firstBeatTime = time;
            out.firstBeatKey = key;
        }
        if (!(time == 0xFFFFFFFFu && key == 0u)) {
            ++beats;
        }
    }
    out.beatCount = beats;

    // 5. Ogg payload: read + decrypt with the same absolute-position XOR.
    std::vector<uint8_t> ogg(first.size);
    if (!ReadRange(stream, out.audioOffset, ogg.data(), ogg.size())) {
        out.failReason = "short audio read";
        OS_FileClose(stream);
        return false;
    }
    OS_FileClose(stream);
    for (uint32_t i = 0; i < first.size; ++i) {
        ogg[i] ^= kXorTable[(out.audioOffset + i) & 0xF];
    }
    if (ogg.size() < 16 || std::memcmp(ogg.data(), "OggS", 4) != 0) {
        out.failReason = "payload is not Ogg after de-obfuscation";
        return false;
    }
    // headRawHex shows ON-DISK bytes: raw = dec ^ table at absolute pos.
    {
        uint8_t raw[16];
        for (int i = 0; i < 16; ++i) {
            raw[i] = static_cast<uint8_t>(
                ogg[static_cast<size_t>(i)] ^
                kXorTable[(out.audioOffset + static_cast<uint32_t>(i)) & 0xF]);
        }
        out.headRawHex = Hex16(raw);
    }
    out.headDecHex = Hex16(ogg.data());

    // 6. Vorbis decode (game parity: CAEVorbisDecoder over ov_callbacks).
    MemStream mem{ogg.data(), ogg.size(), 0};
    ov_callbacks cbs{VorbisRead, VorbisSeek, VorbisClose, VorbisTell};
    OggVorbis_File vf{};
    if (ov_open_callbacks(&mem, &vf, nullptr, 0, cbs) != 0) {
        out.failReason = "ov_open_callbacks failed";
        return false;
    }
    bool vfOk = true;
    vorbis_info* info = ov_info(&vf, -1);
    if (!info || info->channels < 1 || info->channels > 2 ||
        info->rate <= 0) {
        out.failReason = "bad vorbis stream info";
        vfOk = false;
    }
    long rate = 0;
    int srcCh = 0;
    if (vfOk) {
        rate = info->rate;
        srcCh = info->channels;
        out.rateHz = static_cast<uint32_t>(rate);
        out.srcChannels = srcCh;
    }
    // Game always sinks stereo (mono is duplicated in FillBuffer); the
    // output buffer follows that: stereo PCM16 at the vorbis rate.
    const size_t framesWanted =
        vfOk ? static_cast<size_t>(rate) * static_cast<size_t>(wantSeconds)
             : 0;
    std::vector<int16_t> stereo;
    if (vfOk) {
        stereo.reserve(framesWanted * 2);
        std::vector<char> chunk(8192);
        const size_t bytesWanted = framesWanted * 2 /*ch*/ * 2 /*s16*/;
        size_t bytesGot = 0;
        int bitstream = 0;
        std::vector<int16_t> mono;
        if (srcCh == 1) {
            mono.reserve(framesWanted);
        }
        while (bytesGot < bytesWanted) {
            size_t want = chunk.size();
            if (srcCh == 1) {
                want /= 2; // game reads half for mono, then duplicates
            }
            if (want > bytesWanted - bytesGot) {
                want = bytesWanted - bytesGot;
                if (srcCh == 1) {
                    want &= ~static_cast<size_t>(1);
                }
            }
            if (want == 0) {
                break;
            }
            long got = ov_read(&vf, chunk.data(),
                               static_cast<int>(want), 0, 2, 1, &bitstream);
            if (got <= 0) {
                break; // EOF (0) or error (<0): stop honestly
            }
            if (srcCh == 1) {
                const size_t samples =
                    static_cast<size_t>(got) / sizeof(int16_t);
                for (size_t k = 0; k < samples; ++k) {
                    int16_t v = 0;
                    (void)std::memcpy(&v, chunk.data() + k * 2, 2);
                    mono.push_back(v);
                }
                bytesGot += static_cast<size_t>(got) * 2;
            } else {
                const size_t samples =
                    static_cast<size_t>(got) / sizeof(int16_t);
                const int16_t* p =
                    reinterpret_cast<const int16_t*>(chunk.data());
                stereo.insert(stereo.end(), p, p + samples);
                bytesGot += static_cast<size_t>(got);
            }
        }
        if (srcCh == 1) {
            stereo.reserve(mono.size() * 2);
            for (int16_t v : mono) {
                stereo.push_back(v);
                stereo.push_back(v);
            }
        }
        if (stereo.size() < framesWanted * 2) {
            out.failReason = "track shorter than requested seconds";
            vfOk = false;
        }
    }
    ov_clear(&vf);
    if (!vfOk) {
        return false;
    }
    stereo.resize(framesWanted * 2);
    out.seconds = wantSeconds;
    out.pcm = std::move(stereo);

    // 7. Metrics over the decoded bytes (same FNV-1a/64 as SfxDecode).
    out.decodedBytes = out.pcm.size() * sizeof(int16_t);
    uint64_t fnv = 14695981039346656037ULL;
    int64_t sumSquares = 0;
    int peak = 0;
    for (int16_t v : out.pcm) {
        sumSquares += static_cast<int64_t>(v) * v;
        const int a = v < 0 ? -v : v;
        if (a > peak) {
            peak = a;
        }
    }
    fnv = Fnv1a64(fnv, reinterpret_cast<const uint8_t*>(out.pcm.data()),
                  out.decodedBytes);
    out.bufChecksum = fnv;
    out.peak = peak;
    out.rms = !out.pcm.empty()
                  ? std::sqrt(static_cast<double>(sumSquares) /
                              static_cast<double>(out.pcm.size()))
                  : 0.0;
    return true;
}
