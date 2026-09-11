// Bounded SCM -> persistent native gameplay bridge. Startup RW parsers are
// exclusive; all VM/gameplay calls and world publication are main-thread owned.
#pragma once

#include "app/platform/linux/NativeScriptSession.h"
#include "app/platform/linux/NativeScriptEntities.h"
#include "app/platform/linux/NativeEntryExits.h"
#include "app/platform/linux/NativeGarages.h"
#include "app/platform/linux/NativeVehiclePool.h"
#include "app/platform/linux/RealtimeGameplay.h"
#include "app/platform/linux/StreamPager.h"
#include <functional>
#include <optional>

struct RealtimeScriptWorldPublication {
    // A live loader transfers exclusive scene ownership at a frame boundary.
    // Collision is an owned source-COL query world paired with this scene.
    std::shared_ptr<const WorldShotScene> Scene;
    NativeScriptPosition Center;
    E2EPagerFrame Frame;
    std::shared_ptr<const RealtimeGameplayWorld> Collision;
    std::shared_ptr<const NativeCollisionSnapshot> SourceCollision;
    std::shared_ptr<const NativePlacementOverrides> Overrides;
};

struct RealtimeScriptGroup {
    bool Active = false, MissionGroup = false;
    std::uint16_t Generation = 0;
    NativeScriptPedRef Leader;
    // Source constructor: default RANDOM allocator, no followers initially.
    std::size_t Followers = 0;
};

struct RealtimeScriptHostEvent {
    NativeScriptRequestId Id;
    std::uint16_t Opcode = 0;
    std::array<float, 4> Arguments{};
    std::int32_t Index = 0, Reference = -1, StateArgument = 0;
    std::array<char, 8> Name{};
    RealtimeGameplayState Player{};
    RealtimeGameplayCamera Camera{};
};
struct RealtimeScriptPlayerInfo {
    // PlayerInfo.cpp::Clear, source lifetime owned by this new-game host.
    std::int32_t Money = 0, DisplayMoney = 0;
};

class RealtimeScriptHost final : public NativeScriptServices {
public:
    using WorldLoader = std::function<NativeScriptServiceResult(const NativeScriptSceneRequest&, RealtimeScriptWorldPublication&)>;
    using CancelLoad = std::function<void(const NativeScriptRequestId&)>;
    using RadarSpriteReady = std::function<bool(std::int32_t)>;
    explicit RealtimeScriptHost(RealtimeGameplay& gameplay);
    ~RealtimeScriptHost() override;
    RealtimeScriptHost(const RealtimeScriptHost&) = delete;
    RealtimeScriptHost& operator=(const RealtimeScriptHost&) = delete;

    // Parent: StreamPager_Init(includeStreamed=true), then this, then RunPass.
    // Loads main.scm and base player pose caches, never an outfit. Call exactly
    // once BEFORE launching the RW streaming worker. Pager lifecycle stays parent-owned.
    bool InitializeBeforeWorker(const char* gameDir, std::string& error,
        std::shared_ptr<const NativeCollisionContext> collision = {});
    // After main53 has created the actual player, before world adoption/upload,
    // SealStartup and the worker. Publishes expected first common-update COL;
    // verifies unchanged render poses and never commits garage flags or a Tick.
    // Idempotent before sealing if the expected placement state is unchanged.
    // Failure leaves the publication, overrides and revision intact.
    bool PrepareInitialGarageWorldBeforeWorker(std::string& error);
    // Install a worker handoff/poll callback under exclusive host ownership. Callback
    // must only hand off owned results; never parse on the GL/main thread.
    // SealStartup MUST precede launching the worker, even when no loader exists.
    void SetLiveWorldLoader(WorldLoader loader, CancelLoad cancel);
    void SealStartup();
    NativeScriptResult RunPass(std::size_t quota);
    bool AdvanceTime(std::uint32_t nowMs, std::string& error);
    const NativeScriptState& State() const { return m_Session.State(); }
    const NativeScriptSession& Session() const { return m_Session; }
    // Clock/fade/stats/relationships are owned VM state above. Presentation,
    // stats notifications and full mission-0 world services are NOT implemented.
    // SetupPlayerPed minimum here is MODEL_PLAYER + registered mission actor,
    // playing native on-foot controller + group leader membership. Group AI task
    // allocation/processing, combat/speech and full upstream ped intelligence
    // are not supplied by this bridge. Unknown opcodes stay terminal VM faults.
    static constexpr const char* CollisionSource = "source COL triangles/spheres/oriented boxes; owned text+binary IPL residency";
    const RealtimeGameplayWorld* World() const { return m_World.get(); }
    const RealtimeScriptWorldPublication& Publication() const { return m_Publication; }
    // Pass to CpuWorld::Build / Worker and return it with every live publication.
    std::shared_ptr<const NativePlacementOverrides> InitialPlacementOverrides() const { return m_InitialPlacementOverrides; }
    std::uint64_t WorldRevision() const { return m_WorldRevision; }
    const std::vector<RealtimeScriptHostEvent>& Events() const { return m_Events; }
    const RealtimeGameplay* ResolvePed(NativeScriptPedRef ref) const;
    const RealtimeScriptGroup* ResolveGroup(NativeScriptGroupRef ref) const;
    NativeScriptEntities& Entities() { return m_Entities; }
    const NativeScriptEntities& Entities() const { return m_Entities; }
    NativeEntryExits& EntryExits() { return m_EntryExits; }
    const NativeEntryExits& EntryExits() const { return m_EntryExits; }
    NativeGarages& Garages() { return m_Garages; }
    const NativeGarages& Garages() const { return m_Garages; }
    NativeVehiclePool& Vehicles() { return m_Vehicles; }
    const NativeVehiclePool& Vehicles() const { return m_Vehicles; }
    std::shared_ptr<const NativeVehiclePoolSnapshot> PublishVehicles(std::uint64_t frame, std::string& error);
    const RealtimeScriptPlayerInfo& PlayerInfo() const { return m_PlayerInfo; }
    // Register the main-thread presentation consumer. Production wiring must
    // query live GPU ownership (RealtimeHud::IsRadarSpriteUploaded), not merely
    // the presence of prepared CPU RGBA. The callback is queried per new request.
    void SetRadarSpriteReady(RadarSpriteReady ready);
    NativeScriptServiceResult PrepareContactBlipRequest(NativeScriptContactBlipRequest& request) const;
    // Parent supplies real collect-key edge/target/busy/global suppression and
    // unpaused frame counter. Balance/mission inputs are overwritten from owned
    // player/SCM state. Tick stages only; Entities().AdvanceTime publishes atomically.
    // Also drives ordinary source32/6 scheduling. On AdvanceTime failure, a
    // non-None Entities().PickupRequirement() is explicit Unsupported source
    // task/event work, with the real pickup reference; never acknowledge collect.
    bool TickProperties(NativeScriptPosition camera, bool alive, NativeScriptPropertyInput input, std::string& error);
    // Production authority comes from the controller, not caller alive/busy flags.
    bool TickPlayerEntities(NativeScriptPosition camera, NativeScriptPropertyInput input, std::string& error);

    NativeScriptServiceResult RequestCollision(const NativeScriptCollisionRequest&) override;
    NativeScriptServiceResult LoadScene(const NativeScriptSceneRequest&) override;
    NativeScriptServiceResult CreatePlayer(const NativeScriptPlayerRequest&) override;
    NativeScriptReferenceResult<NativeScriptGroupRef> GetPlayerGroup(const NativeScriptPlayerLookupRequest&) override;
    NativeScriptReferenceResult<NativeScriptPedRef> GetPlayerChar(const NativeScriptPlayerLookupRequest&) override;
    NativeScriptServiceResult SetCameraBehindPlayer(const NativeScriptCameraRequest&) override;
    NativeScriptServiceResult SetCharHeading(const NativeScriptHeadingRequest&) override;
    NativeScriptReferenceResult<NativeScriptPickupRef> CreateLockedProperty(const NativeScriptLockedPropertyRequest&) override;
    NativeScriptReferenceResult<NativeScriptPickupRef> CreateForSaleProperty(const NativeScriptForSalePropertyRequest&) override;
    NativeScriptReferenceResult<NativeScriptPickupRef> CreatePickup(const NativeScriptPickupRequest&) override;
    NativeScriptReferenceResult<NativeScriptBlipRef> CreateContactBlip(const NativeScriptContactBlipRequest&) override;
    NativeScriptServiceResult SetBlipDisplay(const NativeScriptBlipDisplayRequest&) override;
    NativeScriptServiceResult SetEntryExitFlag(const NativeScriptEntryExitFlagRequest&) override;
    NativeScriptServiceResult DeactivateGarage(const NativeScriptGarageRequest&) override;
    NativeScriptPickupCollectedResult HasPickupBeenCollected(const NativeScriptPickupReferenceRequest&) override;
    NativeScriptServiceResult RemoveScriptPickup(const NativeScriptPickupReferenceRequest&) override;

private:
    NativeScriptServiceResult PublishWorld(const NativeScriptSceneRequest& request, bool requireGround = false);
    std::optional<NativeScriptServiceResult> Replay(const RealtimeScriptHostEvent& event);
    void Commit(RealtimeScriptHostEvent event);
    NativeScriptPedRef PedRef() const;
    NativeScriptGroupRef GroupRef() const;

    RealtimeGameplay& m_Gameplay;
    NativeScriptSession m_Session;
    NativeScriptEntities m_Entities;
    RealtimeScriptPlayerInfo m_PlayerInfo;
    NativeEntryExits m_EntryExits;
    NativeGarages m_Garages;
    NativeVehiclePool m_Vehicles;
    std::shared_ptr<const NativeCollisionContext> m_CollisionContext;
    std::shared_ptr<const NativePlacementOverrides> m_InitialPlacementOverrides;
    std::shared_ptr<const RealtimeGameplayWorld> m_World;
    RealtimeScriptWorldPublication m_Publication;
    WorldLoader m_Loader;
    CancelLoad m_Cancel;
    RadarSpriteReady m_RadarSpriteReady;
    std::optional<NativeScriptRequestId> m_PendingLoad;
    NativeScriptPosition m_PendingPosition;
    bool m_PendingGround = false;
    std::optional<NativeScriptPosition> m_CollisionRegion, m_LoadedScene;
    std::vector<RealtimeScriptHostEvent> m_Events;
    // One real persistent player binding, deliberately bounded to player0.
    // CPool: slot << 8 | seven-bit generation, bit7 denotes empty.
    std::uint8_t m_PedGeneration = 0;
    bool m_PedActive = false;
    RealtimeScriptGroup m_Group;
    std::uint64_t m_WorldRevision = 0;
    bool m_Initialized = false, m_Sealed = false, m_InitialGarageWorldPrepared = false;
};
