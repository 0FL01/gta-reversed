#pragma once

#include "NativeScriptSession.h"

#include <array>
#include <cstdint>
#include <string>

struct NativeMissionAudioSlot {
    std::int32_t AudioId = -1;
    std::int32_t Pack = -1;
    std::uint32_t Offset = 0;
    std::uint32_t Size = 0;
    std::uint32_t DurationMs = 0;
    std::uint32_t StartedMs = 0;
    std::uint64_t MetadataHash = 0;
    bool Loaded = false;
    bool PlaybackRequested = false;
    bool Finished = false;
    bool operator==(const NativeMissionAudioSlot&) const = default;
};

// Exact script-slot stream metadata owner. It proves the configured stream
// extent is readable; decode, mixing, attachment and playback remain later.
class NativeMissionAudio {
public:
    bool LoadBeforeWorker(const char* gameDir, std::string& error);
    NativeScriptServiceResult Request(std::int32_t slot, std::int32_t event);
    NativeScriptServiceResult Clear(std::int32_t slot);
    NativeScriptServiceResult Play(std::int32_t slot, std::uint32_t nowMs);
    void Advance(std::uint32_t nowMs) noexcept;
    bool Finished(std::int32_t slot) const noexcept;
    bool Loaded(std::int32_t slot) const noexcept;
    const NativeMissionAudioSlot* Slot(std::int32_t slot) const noexcept;
    static constexpr bool RuntimePlayback = false;

private:
    struct Lookup { std::uint8_t Pack = 0; std::uint32_t Offset = 0, Size = 0; };
    std::array<Lookup, 1922> m_Lookups{};
    std::array<std::array<char, 16>, 32> m_Packs{};
    std::array<NativeMissionAudioSlot, 4> m_Slots{};
    std::string m_GameDir;
    std::size_t m_PackCount = 0;
    bool m_Ready = false;
};
