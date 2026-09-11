// Owned CRestart registry and selector (Restart.cpp 0x460630..0x460A50).
// Registration is not death/arrest execution. Selection reports required work.
#pragma once
#include "app/platform/linux/NativeScriptSession.h"

struct NativeRestartPoint {
    NativeScriptPosition Position;
    float HeadingDegrees = 0;
    std::int32_t WhenToUse = 0;
    bool operator==(const NativeRestartPoint&) const = default;
};
struct NativeRestartMapZone {
    NativeScriptPosition Min, Max; // source int16-narrowed before min/max
    std::uint8_t Level = 0;
};
struct NativeRestartExtra {
    NativeRestartPoint Point;
    float Radius = 0; // <=0 disabled, strict XY distance, independent of unlock
};
struct NativeRestartPolicy {
    bool FadeAfterDeath = true, FadeAfterArrest = true;
    std::optional<NativeRestartPoint> OverrideNext;
    std::optional<NativeScriptPosition> MissionBase;
    NativeRestartExtra Hospital, Police;
};
enum class NativeRestartStatus { RestartRequired, Unsupported, Error };
enum class NativeRestartOrigin { Registry, Override, Extra };
enum class NativeRestartEffect : std::uint32_t {
    ActorHealthArmourWantedTasks = 1, WorldRemoveAddAndClear = 2,
    AreaAndEntryExitReset = 4, SceneZoneAndModelStreaming = 8,
    CameraAndControlReset = 16, TimeAndGameplayReset = 32,
};
struct NativeRestartRequired {
    NativeRestartKind Kind = NativeRestartKind::Hospital;
    NativeRestartOrigin Origin = NativeRestartOrigin::Registry;
    NativeRestartPoint Target;
    std::size_t RegistryIndex = 0;
    NativeScriptPosition ResurrectionPosition; // GameLogic: target + (0,0,1)
    float HeadingRadians = 0;
    std::uint8_t DestinationArea = 0; // source AREA_CODE_NORMAL
    bool Fade = true, ConsumeOverride = false, ConsumeMissionBase = false;
    std::uint32_t Effects = 63; // all NativeRestartEffect bits; NONE completed
    std::uint64_t RegistryRevision = 0;
};
struct NativeRestartSelection {
    NativeRestartStatus Status = NativeRestartStatus::Unsupported;
    std::optional<NativeRestartRequired> Required;
    std::string Message;
};
struct NativeRestartQuery {
    NativeRestartKind Kind = NativeRestartKind::Hospital;
    NativeScriptPosition Position;
    std::optional<std::uint8_t> Area; // absent = missing authority; 0 certifies exterior absent an ENEX conversion
    std::optional<NativeScriptPosition> OutsideWorldPosition; // ENEX conversion
    std::optional<float> CityUnlocked;
};
class NativeRestarts {
public:
    static constexpr std::size_t Capacity = 10, MapCapacity = 39;
    // Startup parses all DAT-listed text IPL zone sections, in source order.
    bool LoadBeforeWorker(const char* gameDir, std::string& error);
    bool InitializeMapZones(std::span<const NativeRestartMapZone> zones, std::string& error);
    void SealStartup() { m_Sealed = true; }
    NativeScriptServiceResult Add(const NativeScriptRestartRequest& request);
    NativeScriptServiceResult Configure(const NativeRestartPolicy& policy);
    const NativeRestartPolicy& Policy() const { return m_Policy; }
    std::span<const NativeRestartPoint> Points(NativeRestartKind kind) const;
    std::span<const NativeRestartMapZone> MapZones() const { return {m_MapZones.data(), m_MapCount}; }
    std::optional<std::uint8_t> LevelAt(NativeScriptPosition point) const;
    std::uint64_t Revision() const { return m_Revision; }
    // Pure owned geometry/query after preparation; no IO or actor
    // mutation. The target carries source one-shot effects for the lifecycle owner.
    NativeRestartSelection Query(const NativeRestartQuery& query) const;
    // Applies ONLY source selection flags after a lifecycle owner accepts target.
    // Revision rejects stale decisions. Does not acknowledge resurrection work.
    bool ConsumeSelection(const NativeRestartRequired& required);
private:
    std::array<std::array<NativeRestartPoint, Capacity>, 2> m_Points{};
    std::array<std::size_t, 2> m_Counts{};
    std::array<NativeRestartMapZone, MapCapacity> m_MapZones{};
    std::size_t m_MapCount = 0;
    NativeRestartPolicy m_Policy;
    std::uint64_t m_Revision = 0;
    bool m_Sealed = false;
};
