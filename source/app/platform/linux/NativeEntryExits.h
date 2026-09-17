// Owned text-IPL ENEX population only. Dynamic DFF EFFECT_ENEX population and
// interior transition tasks/streaming are outside this bounded service extent.
#pragma once
#include "app/platform/linux/NativeScriptSession.h"
#include "app/platform/linux/WorldShot.h"
#include <functional>
#include <string_view>

struct NativeEntryExitRect { float Left = 0, Bottom = 0, Right = 0, Top = 0; };
struct NativeEntryExit {
    NativeEntryExitRect Entrance, Bounds;
    NativeScriptPosition Center, Exit;
    float EntranceAngle = 0, ExitAngle = 0, AuthoredRangeZ = 0; // entrance radians, exit degrees; range Z source-unused
    std::uint16_t Flags = 0, AuthoredFlags = 0;
    std::uint8_t Area = 0, SkyColor = 0, Peds = 0, TimeOn = 0, TimeOff = 24;
    std::array<char, 8> Name{};
    int Link = -1;
    std::string IPL;
    std::size_t Line = 0, Row = 0;
};
enum class NativeEntryExitVehicle { OnFoot, Automobile, Bike, Other };
struct NativeEntryExitView {
    NativeScriptPosition Camera, Forward, Player; // Player = entity origin (PedRoot on foot), not collision feet
    std::uint8_t Hour = 0, Area = 0;
    bool Cutscene = false, ControlsDisabled = false, Coop = false, Replay = false, Disabled = false;
    int TransitionState = 0;
    bool CanStartMission = false, BigVehicle = false;
    NativeEntryExitVehicle Vehicle = NativeEntryExitVehicle::OnFoot;
    // Parent supplies the real camera frustum test. Empty callback is an error,
    // never an implicit all-visible camera. Radius is source 1.0.
    std::function<bool(NativeScriptPosition, float)> SphereVisible;
};
struct NativeEntryExitTransitionRequired {
    std::size_t Entry = 0, Destination = 0;
    NativeScriptPosition Entrance, Exit;
    float ExitAngle = 0; // authored degrees
    std::uint8_t Area = 0;
    std::uint16_t Flags = 0;
};
enum class NativeEntryExitActivationStatus { None, TransitionRequired };
struct NativeEntryExitActivation {
    NativeEntryExitActivationStatus Status = NativeEntryExitActivationStatus::None;
    NativeEntryExitTransitionRequired Transition;
};

class NativeEntryExits {
public:
    static constexpr int MarkerModel = 1559;
    static constexpr std::string_view Extent = "DEFAULT.DAT then GTA.DAT text IPL ENEX; excludes dynamic DFF 2DFX ENEX";
    bool LoadBeforeWorker(const char* gameDir, std::string& error, std::uint32_t randomSeed = 0);
    void SealStartup() { m_Sealed = true; }
    bool Loaded() const { return m_Loaded; }
    std::span<const NativeEntryExit> Entries() const { return m_Entries; }
    std::size_t IPLCount() const { return m_IPLCount; }
    std::uint64_t Revision() const { return m_Revision; }
    const WorldShotScene& PreparedModel() const { return m_Model; }
    // Source C3dMarker::Render: dedicated full-bright cone pass, backface cull,
    // depth test ON / depth writes OFF. Actor image indices address PreparedModel.
    const WorldShotScene& Actors() const { return m_Actors; }
    std::span<const std::size_t> VisibleEntries() const { return m_Visible; }
    // Pure owned queries. Coarse leaves, duplicates, source list order retained.
    std::vector<std::size_t> Candidates(NativeEntryExitRect query) const;
    int FindNearest(float x, float y, float radius, int ignoreArea = -1) const;
    // Frame stamp is unpaused native game ms. No IO, RW, catalog or COL reload.
    // Actor publication and marker phase are atomic on errors.
    bool Tick(const NativeEntryExitView&, std::uint32_t gameMs, std::string& error);
    NativeEntryExitActivation Activation(const NativeEntryExitView&) const;
    // Structured parser shared with synthetic fixtures; exactly 18 fields.
    static bool ParseRow(std::string_view row, NativeEntryExit& entry, bool randomBurglaryClosed, std::string& error);
    // Startup fixtures use the same constructor/link/quadtree build; no assets.
    bool InitializeRegistry(std::vector<NativeEntryExit> entries, std::string& error);
private:
    friend class RealtimeScriptHost; // all script mutations pass host's global ID journal
    NativeScriptServiceResult SetFlag(const NativeScriptEntryExitFlagRequest&);
    NativeScriptServiceResult SetEnabledByName(const NativeScriptEntryExitSwitchRequest&);
    std::vector<std::size_t> PointCandidates(float x, float y) const;
    void BuildRegistry();
    std::vector<NativeEntryExit> m_Entries;
    std::array<std::vector<std::size_t>, 256> m_Leaves;
    WorldShotScene m_Model, m_Actors;
    std::vector<std::size_t> m_Visible;
    std::array<float, 400> m_MarkerZ{}, m_MarkerSize{};
    std::array<bool, 400> m_WasVisible{};
    std::size_t m_IPLCount = 0;
    std::uint64_t m_Revision = 0;
    std::uint32_t m_LastTime = 0;
    float m_DiamondAngle = 0;
    bool m_Loaded = false, m_Registry = false, m_Sealed = false, m_HasTime = false;
};
