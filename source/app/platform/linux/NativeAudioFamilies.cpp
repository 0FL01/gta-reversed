#include "app/platform/linux/NativeAudioFamilies.h"

#include "app/platform/linux/NativeMissionAudio.h"
#include "app/platform/linux/SfxDecode.h"

#include <cmath>
#include <cstring>
#include <limits>

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
std::shared_ptr<const std::vector<std::uint8_t>> PcmBytes(const SfxSound& sound) {
    auto bytes = std::make_shared<std::vector<std::uint8_t>>();
    bytes->resize(sound.pcm.size() * 2);
    for (std::size_t i = 0; i < sound.pcm.size(); ++i) {
        const auto value = static_cast<std::uint16_t>(sound.pcm[i]);
        (*bytes)[i * 2] = std::uint8_t(value);
        (*bytes)[i * 2 + 1] = std::uint8_t(value >> 8);
    }
    return bytes;
}

bool DecodeSfx(NativeAudioFamily family, std::int32_t event, std::int32_t bank,
    std::int32_t sound, NativeAudioFamilyAsset& out, std::string& error) {
    SfxSingleSoundResult decoded;
    if (!SfxDecode_Sound(bank, sound, decoded, error) || decoded.durationMs <= 0.0 ||
        decoded.durationMs > std::numeric_limits<std::uint32_t>::max() || !decoded.sound.rateHz) return false;
    out = {family, NativeAudioEncoding::Pcm16Mono, event, bank, sound,
        decoded.sound.rateHz, std::uint32_t(std::ceil(decoded.durationMs)),
        decoded.sound.loopStartOffset, decoded.bufChecksum, PcmBytes(decoded.sound)};
    return out.Payload && !out.Payload->empty();
}
}

bool NativeAudioFamilies::Load(const char* gameDir, std::string& error) {
    if (m_Loaded || !gameDir || !*gameDir) { error = "audio family load contract rejected"; return false; }
    OS_SetFilePathOffset(gameDir);
    std::array<NativeAudioFamilyAsset, 3> candidate;
    // VehicleAudioSettings model400 DoorType::NEW: AE events80/86 map to
    // GENRL vehicle bank138, open sound40 and close sound33. Pin one SFX.
    if (!DecodeSfx(NativeAudioFamily::Sfx, 80, 138, 40, candidate[0], error)) return false;
    // CAEWeatherAudioEntity uses GENRL_RAIN bank105. Pin the authored long rain loop.
    if (!DecodeSfx(NativeAudioFamily::Environment, 0, 105, 0, candidate[2], error)) return false;

    NativeMissionAudio speech;
    if (!speech.LoadBeforeWorker(gameDir, error)) return false;
    const auto requested = speech.Request(1, 43200);
    const auto* slot = speech.Slot(1);
    if (requested.Status != NativeScriptServiceStatus::Ready || !slot || !slot->Payload ||
        slot->Payload->size() < 4 || std::memcmp(slot->Payload->data(), "OggS", 4)) {
        error = requested.Message.empty() ? "speech family payload rejected" : requested.Message;
        return false;
    }
    candidate[1] = {NativeAudioFamily::Speech, NativeAudioEncoding::OggVorbis,
        43200, slot->Pack, -1, 0, slot->DurationMs, -1, slot->MetadataHash, slot->Payload};
    m_Assets = std::move(candidate);
    m_Loaded = true;
    error.clear();
    return true;
}
