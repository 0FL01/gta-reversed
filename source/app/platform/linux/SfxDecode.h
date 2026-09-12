// SfxDecode: real GTA:SA SFX bank parsing for the Linux native track.
// Round 6 (R6d): proves SFX bytes reach OpenAL. Layout mirrors the game's
// own loaders (`source/game_sa/Audio/Loaders/AEBankLoader.h`, NOT linked):
//   audio/CONFIG/PakFiles.dat -> AEPakLookup { char base[12]; u32 lsn[10] }
//     (52 bytes each)
//   audio/CONFIG/BankLkup.dat -> AEBankLookup { u8 pakFileNo; u8 pad[3];
//     u32 fileOffsetBytes; u32 numBytes } (12 bytes each; despite the
//     "(#Sectors)" comment in the fork, fileOffset is a BYTE offset into
//     the pak: e.g. FEET bank0 {off=0,size=90998}, bank1 off=95802 =
//     90998 + 4804 header bytes, verified against the shipped files)
//   audio/sfx/<PAK> -> concatenated banks, each an on-disk AEAudioStream:
//     { i16 numSounds; i16 pad; CAEBankSlotItem sounds[400];
//       u8 bankData[lookup.numBytes] }, CAEBankSlotItem =
//     { u32 bankOffsetBytes; i32 loopStartOffset (-1 = no loop);
//       u16 sampleFrequencyHz; i16 headroom (/100 dB) } (12 bytes each,
//     4804-byte header). bankOffsetBytes is relative to bankData start;
//     sound size = next.bankOffsetBytes - cur.bankOffsetBytes (last sound:
//     lookup.numBytes - cur.bankOffsetBytes).
// Samples are SIGNED PCM16 little-endian MONO at the per-sound rate
// (verified: GENRL bank 7 sound 0 = 23464 bytes, rate 18000 Hz, RMS 7333,
// peak 26028, smooth attack ramp - real audio, not a table). No synthesis:
// every PCM byte comes from audio/sfx via OS_File*.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

// One decoded sound: PCM bytes copied verbatim from the pak, viewed as
// signed 16-bit mono at `rateHz`.
struct SfxSound {
    int bankId = -1;     // index into BankLkup.dat (e.g. 7 = first GENRL bank)
    int soundIndex = -1; // index within the bank header
    uint32_t dataSize = 0; // bytes (always even)
    uint16_t rateHz = 0;   // per-sound SampleFrequency
    int16_t headroom = 0;  // per-sound Headroom (/100 dB, informational)
    int32_t loopStartOffset = -1;
    std::vector<int16_t> pcm; // dataSize/2 signed samples
    bool operator==(const SfxSound&) const = default;
};

struct SfxSingleSoundResult {
    std::string pakName;
    SfxSound sound;
    uint64_t bufChecksum = 0;
    double durationMs = 0.0;
    double rms = 0.0;
    int peak = 0;
    bool operator==(const SfxSingleSoundResult&) const = default;
};

struct SfxDecodeResult {
    std::string bankName; // pak base name as requested (uppercased)
    std::vector<SfxSound> sounds;
    uint64_t decodedBytes = 0; // sum of dataSize
    double durationMs = 0.0;   // sum(samples * 1000 / rateHz)
    double rms = 0.0;          // over all decoded samples
    int peak = 0;              // max |sample| over all sounds
    uint64_t bufChecksum = 0;  // FNV-1a/64 over decoded PCM bytes in order
    int skippedBanks = 0;      // banks failing validation (deterministic skip)
    std::string failReason;    // set when the call returns false
};

// Decodes up to `wantSamples` sounds from pak `bankName` (e.g. "GENRL"),
// walking banks in BankLkup order (ascending bank id) and sounds in header
// order. Reads go through OS_File* under the current OS_SetFilePathOffset
// root. Returns false (with failReason) when the pak is unknown or fewer
// than wantSamples valid sounds exist. No synthesis, no game_sa linkage.
bool SfxDecode_PakBank(const std::string& bankName, int wantSamples,
                       SfxDecodeResult& out);

// Decodes one exact global BankLkup.dat bank/sound pair. The caller owns the
// OS_File path root. Failure leaves `out` unchanged and reports a typed reason;
// no neighbouring bank/sample can silently substitute for the requested ID.
bool SfxDecode_Sound(int bankId, int soundIndex, SfxSingleSoundResult& out,
                     std::string& error);
