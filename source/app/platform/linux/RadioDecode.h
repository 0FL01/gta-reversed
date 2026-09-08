// RadioDecode: real GTA:SA radio-station audio for the Linux native track.
// Round 10 (R6h): proves station bytes reach OpenAL. Layout mirrors the
// game's own streaming path (`source/game_sa/Audio/`, NOT linked):
//   audio/CONFIG/StrmPaks.dat -> StreamPack { char name[16] } (16 bytes
//     each; RE = pack 14, slot 2 is an empty hole)
//   audio/CONFIG/TrakLkup.dat -> tTrackLookup { u8 packId; u8 pad[3];
//     u32 offsetBytes; u32 sizeBytes } (12 bytes each; RE = 161 tracks,
//     first global id 1490. Track slice = 0x1F84-byte tTrackInfo header +
//     sizeBytes of audio: e.g. RE t0 off=0 size=235833, t1 off=243901 =
//     0 + 235833 + 0x1F84)
//   audio/streams/<PACK> -> concatenated slices, every byte XOR-obfuscated
//     with CAEStreamTransformer::table
//     (EA 3A C4 A1 9A A8 14 F3 48 B0 D7 23 9D E8 FF F1) indexed by
//     (absoluteFilePosition + i) & 0xF (AEStreamTransformer.cpp 0x4f17d0,
//     applied in CAEDataStream::FillBuffer 0x4dc1c0 with m_nCurrentPosition
//     = absolute position). Verified: RE bytes [0,16) decrypt to
//     FF*4 00*4 x2 (empty tTrackInfo beats), bytes [8068,8072) decrypt to
//     "OggS" (CAEVorbisDecoder payload, AEVorbisDecoder.cpp).
// Decoding itself follows CAEVorbisDecoder: ov_open_callbacks over the
// decrypted payload, ov_read LE/signed/16-bit loop, mono duplicated to
// stereo (AEVorbisDecoder.cpp FillBuffer), stereo PCM16 at vorbis rate
// (RE t0: 32000 Hz, 2 ch, ~17.7 s). No synthesis: every PCM sample comes
// from audio/streams via OS_File*.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct RadioDecodeResult {
    std::string station;    // pack name as requested (uppercased)
    int packId = -1;        // index into StrmPaks.dat (RE = 14)
    int trackCount = 0;     // tracks of this station in TrakLkup.dat
    uint32_t trackId = 0;   // chosen global track id (first of the station)
    uint32_t audioOffset = 0; // absolute stream offset of Ogg payload
    uint32_t audioSize = 0;   // Ogg payload bytes (TrakLkup m_nSize)
    uint32_t rateHz = 0;      // vorbis sample rate (RE t0 = 32000)
    int srcChannels = 0;      // vorbis channels as decoded
    int seconds = 0;          // requested seconds actually cut
    uint32_t beatCount = 0;   // non-empty beats in the tTrackInfo header
    uint32_t firstBeatTime = 0;
    uint32_t firstBeatKey = 0;
    std::string headRawHex; // 16 hex bytes on disk at audioOffset
    std::string headDecHex; // same 16 bytes after XOR (expect 4f676753…)

    std::vector<int16_t> pcm; // stereo PCM16: seconds * rateHz * 2 samples
    uint64_t decodedBytes = 0; // pcm.size() * 2
    double rms = 0.0;
    int peak = 0;
    uint64_t bufChecksum = 0; // FNV-1a/64 over decoded PCM bytes in order
    std::string failReason;   // set when the call returns false
};

// Decodes the first `wantSeconds` seconds of the station's first track
// (global track order = TrakLkup.dat order) to stereo PCM16. Reads go
// through OS_File* under the current OS_SetFilePathOffset root. Returns
// false (with failReason) when the station is unknown, tables are
// unreadable, bounds fail, the payload is not Vorbis, or the track is
// shorter than wantSeconds. No synthesis, no game_sa linkage.
bool RadioDecode_Station(const std::string& station, int wantSeconds,
                         RadioDecodeResult& out);
