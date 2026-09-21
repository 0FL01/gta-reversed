#pragma once

#include "app/platform/linux/NativeScriptSession.h"

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

struct NativeCarRecordingEntry {
    std::int32_t Id = 0;
    std::uint32_t Offset = 0, Size = 0;
};

struct NativeCarRecordingFrame {
    std::uint32_t Time = 0;
    std::array<float, 3> Velocity{}, Right{}, Forward{}, Position{};
    float Steering = 0.0f, Gas = 0.0f, Brake = 0.0f;
    bool Handbrake = false;
};

struct NativeCarRecordingUpdate {
    NativeScriptVehicleRef Vehicle;
    std::array<std::array<float, 3>, 3> Basis{};
    std::array<float, 3> Position{};
    bool Finished = false;
};

struct NativeCarRecordingInspection {
    std::size_t Active = 0;
    NativeScriptVehicleRef Vehicle;
    std::int32_t Recording = -1;
    float RunningTime = 0.0f;
    std::uint32_t EndTime = 0;
};

class NativeCarRecordings {
public:
    static constexpr std::size_t PlaybackCapacity = 16;
    static constexpr bool RuntimePlayback = true;
    bool LoadBeforeWorker(const char* gameDir, std::string& error);
    NativeScriptServiceResult Request(std::int32_t id);
    NativeScriptServiceResult Start(NativeScriptVehicleRef vehicle, std::int32_t id,
        bool useCarAI = false, bool looped = false);
    bool Advance(std::uint32_t nowMs, std::vector<NativeCarRecordingUpdate>& updates, std::string& error);
    bool IsPlaybackActive(NativeScriptVehicleRef vehicle) const;
    NativeCarRecordingInspection Inspect() const noexcept;
    void Stop(NativeScriptVehicleRef vehicle) noexcept;
    bool IsLoaded(std::int32_t id) const { return m_Loaded.contains(id); }
    std::size_t Count() const { return m_Entries.size(); }
    std::size_t LoadedCount() const { return m_Loaded.size(); }
private:
    std::map<std::int32_t, NativeCarRecordingEntry> m_Entries;
    std::set<std::int32_t> m_Loaded;
    std::map<std::int32_t, std::vector<NativeCarRecordingFrame>> m_Frames;
    struct Playback {
        NativeScriptVehicleRef Vehicle;
        std::int32_t Recording = 0;
        float RunningTime = 0.0f, Speed = 1.0f;
        bool Active = false, Looped = false, UseCarAI = false;
    };
    std::array<Playback, PlaybackCapacity> m_Playbacks{};
    std::uint32_t m_LastTimeMs = 0;
    std::shared_ptr<const std::vector<std::uint8_t>> m_Archive;
};
