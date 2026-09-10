// Bounded SCM -> persistent native gameplay bridge. Startup RW parsers are
// exclusive; all VM/gameplay calls and world publication are main-thread owned.
#pragma once

#include "app/platform/linux/NativeScriptSession.h"
#include "app/platform/linux/NativeScriptEntities.h"
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
    std::int32_t Index = 0, Reference = -1;
    RealtimeGameplayState Player{};
    RealtimeGameplayCamera Camera{};
};

class RealtimeScriptHost final : public NativeScriptServices {
public:
    using WorldLoader = std::function<NativeScriptServiceResult(const NativeScriptSceneRequest&, RealtimeScriptWorldPublication&)>;
    using CancelLoad = std::function<void(const NativeScriptRequestId&)>;
    explicit RealtimeScriptHost(RealtimeGameplay& gameplay);
    ~RealtimeScriptHost() override;
    RealtimeScriptHost(const RealtimeScriptHost&) = delete;
    RealtimeScriptHost& operator=(const RealtimeScriptHost&) = delete;

    // Parent: StreamPager_Init(includeStreamed=true), then this, then RunPass.
    // Loads main.scm and base player pose caches, never an outfit. Call exactly
    // once BEFORE launching the RW streaming worker. Pager lifecycle stays parent-owned.
    bool InitializeBeforeWorker(const char* gameDir, std::string& error,
        std::shared_ptr<const NativeCollisionContext> collision = {});
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
    std::uint64_t WorldRevision() const { return m_WorldRevision; }
    const std::vector<RealtimeScriptHostEvent>& Events() const { return m_Events; }
    const RealtimeGameplay* ResolvePed(NativeScriptPedRef ref) const;
    const RealtimeScriptGroup* ResolveGroup(NativeScriptGroupRef ref) const;
    NativeScriptEntities& Entities() { return m_Entities; }
    const NativeScriptEntities& Entities() const { return m_Entities; }

    NativeScriptServiceResult RequestCollision(const NativeScriptCollisionRequest&) override;
    NativeScriptServiceResult LoadScene(const NativeScriptSceneRequest&) override;
    NativeScriptServiceResult CreatePlayer(const NativeScriptPlayerRequest&) override;
    NativeScriptReferenceResult<NativeScriptGroupRef> GetPlayerGroup(const NativeScriptPlayerLookupRequest&) override;
    NativeScriptReferenceResult<NativeScriptPedRef> GetPlayerChar(const NativeScriptPlayerLookupRequest&) override;
    NativeScriptServiceResult SetCameraBehindPlayer(const NativeScriptCameraRequest&) override;
    NativeScriptServiceResult SetCharHeading(const NativeScriptHeadingRequest&) override;
    NativeScriptReferenceResult<NativeScriptPickupRef> CreateLockedProperty(const NativeScriptLockedPropertyRequest&) override;
    NativeScriptReferenceResult<NativeScriptBlipRef> CreateContactBlip(const NativeScriptContactBlipRequest&) override;
    NativeScriptServiceResult SetBlipDisplay(const NativeScriptBlipDisplayRequest&) override;

private:
    NativeScriptServiceResult PublishWorld(const NativeScriptSceneRequest& request, bool requireGround = false);
    std::optional<NativeScriptServiceResult> Replay(const RealtimeScriptHostEvent& event);
    void Commit(RealtimeScriptHostEvent event);
    NativeScriptPedRef PedRef() const;
    NativeScriptGroupRef GroupRef() const;

    RealtimeGameplay& m_Gameplay;
    NativeScriptSession m_Session;
    NativeScriptEntities m_Entities;
    std::shared_ptr<const NativeCollisionContext> m_CollisionContext;
    std::shared_ptr<const RealtimeGameplayWorld> m_World;
    RealtimeScriptWorldPublication m_Publication;
    WorldLoader m_Loader;
    CancelLoad m_Cancel;
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
    bool m_Initialized = false, m_Sealed = false;
};
