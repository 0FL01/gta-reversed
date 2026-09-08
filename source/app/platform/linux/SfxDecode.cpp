// SfxDecode implementation: real SFX bytes via OS_File*, signed PCM16 mono.
#include "app/platform/linux/SfxDecode.h"

#include <cmath>
#include <cstdio>
#include <cstring>

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

constexpr int kBankLkupEntryBytes = 12;
constexpr int kPakLkupEntryBytes = 52; // 12-byte name + 10 u32 LSNs
constexpr int kMaxSoundsPerBank = 400;
constexpr int kSlotItemBytes = 12;
constexpr int kBankHeaderBytes = 4 + kMaxSoundsPerBank * kSlotItemBytes; // 4804
constexpr int kReadChunkBytes = 1 << 20;

uint32_t ReadU32LE(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

uint16_t ReadU16LE(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

// Reads exactly `size` bytes from absolute file position `pos` in chunks.
// OS_File* uses int32 sizes, so large ranges are split.
bool ReadRange(void* file, uint32_t pos, uint8_t* dst, size_t size) {
    uint64_t remaining = size;
    uint64_t cursor = pos;
    while (remaining > 0) {
        // int32 seek domain is fine: SFX paks are < 2 GiB.
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

} // namespace

bool SfxDecode_PakBank(const std::string& bankName, int wantSamples,
                       SfxDecodeResult& out) {
    out = SfxDecodeResult{};
    if (wantSamples <= 0) {
        out.failReason = "bad sample count";
        return false;
    }
    out.bankName = Upper(bankName);

    // 1. PakFiles.dat: resolve the pak index by base filename.
    std::vector<uint8_t> pakFiles;
    if (!ReadWholeFile("audio/CONFIG/PakFiles.dat", pakFiles) ||
        pakFiles.size() % kPakLkupEntryBytes != 0 || pakFiles.empty()) {
        out.failReason = "cannot read PakFiles.dat";
        return false;
    }
    int pakNo = -1;
    char pakBase[16] = {};
    const size_t pakCount = pakFiles.size() / kPakLkupEntryBytes;
    for (size_t i = 0; i < pakCount; ++i) {
        const uint8_t* e = pakFiles.data() + i * kPakLkupEntryBytes;
        char name[13] = {};
        size_t len = 0;
        while (len < 12 && e[len] != '\0') {
            name[len] = static_cast<char>(e[len]);
            ++len;
        }
        name[len] = '\0';
        if (out.bankName == name) {
            pakNo = static_cast<int>(i);
            std::memcpy(pakBase, name, sizeof(pakBase) - 1);
            break;
        }
    }
    if (pakNo < 0) {
        out.failReason = std::string("unknown bank '") + bankName + "'";
        return false;
    }

    // 2. BankLkup.dat: collect this pak's banks in ascending bank-id order.
    std::vector<uint8_t> lookup;
    if (!ReadWholeFile("audio/CONFIG/BankLkup.dat", lookup) ||
        lookup.size() % kBankLkupEntryBytes != 0 || lookup.empty()) {
        out.failReason = "cannot read BankLkup.dat";
        return false;
    }
    struct BankRef {
        int id;
        uint32_t offset;
        uint32_t size;
    };
    std::vector<BankRef> banks;
    const size_t bankCount = lookup.size() / kBankLkupEntryBytes;
    for (size_t i = 0; i < bankCount; ++i) {
        const uint8_t* e = lookup.data() + i * kBankLkupEntryBytes;
        if (e[0] == static_cast<uint8_t>(pakNo)) {
            banks.push_back(BankRef{
                static_cast<int>(i),
                ReadU32LE(e + 4),
                ReadU32LE(e + 8),
            });
        }
    }
    if (banks.empty()) {
        out.failReason = "pak has no banks";
        return false;
    }

    // 3. Walk banks, decode sounds until wantSamples is reached.
    char pakPath[32] = {};
    (void)std::snprintf(pakPath, sizeof(pakPath), "audio/sfx/%s", pakBase);
    void* pak = nullptr;
    if (OS_FileOpen(FILE_DATA_AREA_DEFAULT, &pak, pakPath,
                    FILE_ACCESS_READ) != 0 ||
        !pak) {
        out.failReason = "cannot open pak file";
        return false;
    }
    const int32_t pakSize = OS_FileSize(pak);

    uint64_t fnv = 14695981039346656037ULL;
    int64_t sumSquares = 0;
    int64_t totalSamples = 0;

    std::vector<uint8_t> header(kBankHeaderBytes);
    for (const BankRef& bank : banks) {
        if (static_cast<int>(out.sounds.size()) >= wantSamples) {
            break;
        }
        // Bounds of the on-disk bank record.
        const uint64_t recEnd =
            static_cast<uint64_t>(bank.offset) + kBankHeaderBytes + bank.size;
        if (pakSize < 0 || recEnd > static_cast<uint64_t>(pakSize)) {
            ++out.skippedBanks;
            continue;
        }
        if (!ReadRange(pak, bank.offset, header.data(), kBankHeaderBytes)) {
            ++out.skippedBanks;
            continue;
        }
        const int numSounds =
            static_cast<int16_t>(ReadU16LE(header.data()));
        if (numSounds <= 0 || numSounds > kMaxSoundsPerBank) {
            ++out.skippedBanks;
            continue;
        }
        // Parse slot items; sound size derives from the next offset
        // (last sound: lookup numBytes - offset), exactly like
        // CAEMP3BankLoader::GetSoundBuffer.
        struct Item {
            uint32_t offset;
            uint16_t rate;
            int16_t headroom;
        };
        std::vector<Item> items(static_cast<size_t>(numSounds));
        bool itemsOk = true;
        uint32_t prevOffset = 0;
        for (int s = 0; s < numSounds; ++s) {
            const uint8_t* e = header.data() + 4 + s * kSlotItemBytes;
            const uint32_t off = ReadU32LE(e);
            const uint16_t rate = ReadU16LE(e + 8);
            const int16_t head =
                static_cast<int16_t>(ReadU16LE(e + 10));
            uint32_t size = 0;
            if (s + 1 < numSounds) {
                const uint32_t next =
                    ReadU32LE(header.data() + 4 + (s + 1) * kSlotItemBytes);
                if (next < off) {
                    itemsOk = false;
                    break;
                }
                size = next - off;
            } else {
                if (bank.size < off) {
                    itemsOk = false;
                    break;
                }
                size = bank.size - off;
            }
            // Honest plausibility gate: offsets ascending, even
            // non-empty size inside the bank, playable DirectSound rate.
            // (Rates as odd as 2021 Hz ship in the tables with real PCM
            // behind them, so the floor stays low.)
            if ((s > 0 && off < prevOffset) || size == 0 ||
                (size % 2) != 0 || off + size > bank.size || rate < 100 ||
                rate > 200000) {
                itemsOk = false;
                break;
            }
            prevOffset = off;
            items[s] = Item{off, rate, head};
        }
        if (!itemsOk) {
            ++out.skippedBanks;
            continue;
        }
        for (int s = 0; s < numSounds &&
                        static_cast<int>(out.sounds.size()) < wantSamples;
             ++s) {
            uint32_t size = 0;
            if (s + 1 < numSounds) {
                size = ReadU32LE(header.data() + 4 + (s + 1) * kSlotItemBytes) -
                       items[s].offset;
            } else {
                size = bank.size - items[s].offset;
            }
            std::vector<uint8_t> raw(size);
            if (!ReadRange(pak,
                           bank.offset + kBankHeaderBytes + items[s].offset,
                           raw.data(), raw.size())) {
                out.failReason = "short pak read";
                OS_FileClose(pak);
                return false;
            }
            SfxSound sound;
            sound.bankId = bank.id;
            sound.soundIndex = s;
            sound.dataSize = size;
            sound.rateHz = items[s].rate;
            sound.headroom = items[s].headroom;
            sound.pcm.resize(size / 2);
            for (size_t i = 0; i < sound.pcm.size(); ++i) {
                sound.pcm[i] = static_cast<int16_t>(
                    ReadU16LE(raw.data() + i * 2));
            }
            for (int16_t v : sound.pcm) {
                sumSquares += static_cast<int64_t>(v) * v;
                const int a = v < 0 ? -v : v;
                if (a > out.peak) {
                    out.peak = a;
                }
            }
            totalSamples += static_cast<int64_t>(sound.pcm.size());
            out.decodedBytes += size;
            out.durationMs += static_cast<double>(sound.pcm.size()) *
                              1000.0 / sound.rateHz;
            fnv = Fnv1a64(fnv, raw.data(), raw.size());
            out.sounds.push_back(std::move(sound));
        }
    }
    OS_FileClose(pak);

    if (static_cast<int>(out.sounds.size()) < wantSamples) {
        char reason[96] = {};
        (void)std::snprintf(reason, sizeof(reason),
                            "only %d valid sounds in pak",
                            static_cast<int>(out.sounds.size()));
        out.failReason = reason;
        return false;
    }
    out.bufChecksum = fnv;
    out.rms = totalSamples > 0
                  ? std::sqrt(static_cast<double>(sumSquares) / totalSamples)
                  : 0.0;
    return true;
}
