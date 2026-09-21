#pragma once

#include "app/platform/linux/NativeScriptSession.h"

#include <array>
#include <cstdint>
#include <string>

struct NativeBeatTrackState {
    std::int32_t ScriptTrack = -1;
    std::int32_t SourceTrack = -1;
    std::uint32_t Offset = 0;
    std::uint32_t Size = 0;
    std::uint64_t InfoHash = 0;
    std::uint8_t Status = 0;
    bool PlaybackRequested = false;
};

class NativeBeatTrack {
public:
    static constexpr bool RuntimePlayback = false;

    bool LoadBeforeWorker(const char* gameDir, std::string& error);
    NativeScriptServiceResult Preload(std::int32_t scriptTrack);
    NativeScriptServiceResult Play();
    NativeScriptServiceResult Stop();
    const NativeBeatTrackState& State() const { return m_State; }

private:
    struct Lookup {
        std::uint8_t Pack = 0;
        std::uint32_t Offset = 0;
        std::uint32_t Size = 0;
    };

    std::array<Lookup, 1922> m_Lookups{};
    std::string m_GameDir;
    bool m_Ready = false;
    NativeBeatTrackState m_State;
};
