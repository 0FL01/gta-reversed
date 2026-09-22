// Bounded SCM -> persistent native gameplay bridge. Startup RW parsers are
// exclusive; all VM/gameplay calls and world publication are main-thread owned.
#pragma once

#include "app/platform/linux/NativeScriptSession.h"
#include "app/platform/linux/NativeScriptServiceTransaction.h"
#include "app/platform/linux/NativeScriptEntities.h"
#include "app/platform/linux/NativeEntryExits.h"
#include "app/platform/linux/NativeGarages.h"
#include "app/platform/linux/NativeRestarts.h"
#include "app/platform/linux/NativeStuntJumps.h"
#include "app/platform/linux/NativeSetPieces.h"
#include "app/platform/linux/NativeZonePopulation.h"
#include "app/platform/linux/NativePathPolicy.h"
#include "app/platform/linux/NativeExternalScriptTriggers.h"
#include "app/platform/linux/NativeScriptModelAnims.h"
#include "app/platform/linux/NativeScriptIplRequests.h"
#include "app/platform/linux/NativeWorldObjectOverrides.h"
#include "app/platform/linux/NativeScriptWeather.h"
#include "app/platform/linux/NativeScriptClothes.h"
#include "app/platform/linux/NativeMissionText.h"
#include "app/platform/linux/NativeCutscene.h"
#include "app/platform/linux/NativeCarRecordings.h"
#include "app/platform/linux/NativeBeatTrack.h"
#include "app/platform/linux/NativeMissionAudio.h"
#include "app/platform/linux/NativeScriptPeds.h"
#include "app/platform/linux/NativeScriptTrains.h"
#include "app/platform/linux/NativeScriptObjects.h"
#include "app/platform/linux/NativeVehiclePool.h"
#include "app/platform/linux/NativeCarGeneratorResidency.h"
#include "app/platform/linux/NativeSourceRng.h"
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
    std::uint64_t Generation = 0;
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
    std::array<float, 16> Arguments{};
    std::int32_t Index = 0, Reference = -1, StateArgument = 0;
    std::array<char, 8> Name{};
    std::array<std::int32_t, 8> GeneratorArguments{};
    NativeScriptServiceStatus Status = NativeScriptServiceStatus::Ready;
    RealtimeGameplayState Player{};
    RealtimeGameplayCamera Camera{};
    std::array<char, 24> ModelName{};
};
struct RealtimeScriptPlayerInfo {
    // PlayerInfo.cpp::Clear, source lifetime owned by this new-game host.
    std::int32_t Money = 0, DisplayMoney = 0;
};

class RealtimeScriptHost final : public NativeScriptServices {
public:
    using WorldLoader = std::function<NativeScriptAsyncPrepareResult(
        const NativeScriptSceneRequest&, const NativeScriptServiceTicket&, RealtimeScriptWorldPublication&)>;
    using CancelLoad = std::function<void(const NativeScriptServiceTicket&)>;
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
    // Call immediately after native RW initialization, before source consumers.
    // InitializeBeforeWorker also calls this if the parent has not yet done so.
    // Captures OS_TimeMS exactly once; repeated calls never reseed.
    bool SeedSourceRngAfterRwInit(std::string& error);
    NativeSourceRngRef SourceRng() { return m_SourceRng.Reference(); }
    NativeSourceRngInspection InspectSourceRng() const { return m_SourceRng.Inspect(); }
    // Local CRT algorithm authority, not original global draw-order parity:
    // source ENEX random startup draws are still unported.
    static constexpr bool OriginalRandomDrawOrderParity = false;
    // After main53 has created the actual player, before world adoption/upload,
    // SealStartup and the worker. Publishes expected first common-update COL;
    // verifies unchanged render poses and never commits garage flags or a Tick.
    // Idempotent before sealing if the expected placement state is unchanged.
    // Failure leaves the publication, overrides and revision intact.
    bool PrepareInitialGarageWorldBeforeWorker(std::string& error);
    // Install a worker handoff/poll callback under exclusive host ownership. Callback
    // must only hand off owned results; never parse on the GL/main thread. Its
    // Prepared is internal: only this host can return script Ready after validating
    // and committing the publication. Every worker request is keyed by the supplied
    // owner/attempt ticket, including cancellation and stale-result rejection.
    // SealStartup MUST precede launching the worker, even when no loader exists.
    void SetLiveWorldLoader(WorldLoader loader, CancelLoad cancel);
    // Cancellation is two-phase: notify the worker exactly once, then wait for
    // its ticket acknowledgement before the same SCM request may start a new
    // attempt. Late results from the cancelled ticket are always stale.
    bool CancelPendingWorld(const NativeScriptRequestId& id, std::string& error);
    bool AcknowledgeWorldCancellation(const NativeScriptServiceTicket& ticket, std::string& error);
    const NativeScriptServiceTransactionState& WorldTransaction() const { return m_WorldTransaction.State(); }
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
    bool AdoptLiveWorld(std::uint64_t generation, RealtimeScriptWorldPublication publication, std::string& error);
    const std::vector<RealtimeScriptHostEvent>& Events() const { return m_Events; }
    const RealtimeGameplay* ResolvePed(NativeScriptPedRef ref) const;
    const RealtimeScriptGroup* ResolveGroup(NativeScriptGroupRef ref) const;
    NativeScriptEntities& Entities() { return m_Entities; }
    const NativeScriptEntities& Entities() const { return m_Entities; }
    NativeEntryExits& EntryExits() { return m_EntryExits; }
    const NativeEntryExits& EntryExits() const { return m_EntryExits; }
    NativeGarages& Garages() { return m_Garages; }
    const NativeGarages& Garages() const { return m_Garages; }
    const NativeRestarts& Restarts() const { return m_Restarts; }
    const NativeStuntJumps& StuntJumps() const { return m_StuntJumps; }
    const NativeSetPieces& SetPieces() const { return m_SetPieces; }
    const NativeZonePopulation& ZonePopulation() const { return m_ZonePopulation; }
    const NativePathPolicy& PathPolicy() const{return m_PathPolicy;}
    const NativeExternalScriptTriggers& ExternalTriggers()const{return m_ExternalTriggers;}
    const NativeScriptModelAnims& ModelAnims() const { return m_ModelAnims; }
    const NativeScriptIplRequests& IplRequests() const { return m_IplRequests; }
    const NativeWorldObjectOverrides& WorldObjectOverrides() const { return m_WorldObjectOverrides; }
    const NativeScriptWeather& Weather() const { return m_Weather; }
    const NativeScriptClothes& Clothes() const { return m_Clothes; }
    const NativeMissionText& MissionText() const { return m_MissionText; }
    const NativeCutscene& Cutscene() const { return m_Cutscene; }
    const NativeCarRecordings& CarRecordings() const { return m_CarRecordings; }
    const NativeMissionAudio& MissionAudio() const { return m_MissionAudio; }
    const NativeBeatTrack& BeatTrack() const { return m_BeatTrack; }
    const NativeScriptPeds& ScriptPeds() const { return m_ScriptPeds; }
    const NativeScriptTrains& ScriptTrains() const { return m_ScriptTrains; }
    std::size_t ScriptCameraCommandCount() const { return m_ScriptCameraCommands.size(); }
    bool Widescreen() const { return m_ScriptWidescreen; }
    bool ZoneNamesVisible() const { return m_ZoneNamesVisible; }
    const std::array<std::uint8_t, 3>& FadeColour() const { return m_FadeColour; }
    bool PlayerControlEnabled() const { return m_PlayerControlEnabled; }
    std::optional<NativeScriptPosition> ScriptPlayerPosition() const { return m_ScriptPlayerPosition; }
    bool UpdateStatsVisible() const { return m_UpdateStatsVisible; }
    const NativeScriptObjects& Objects() const { return m_Objects; }
    // Explicit query inputs carry source area/ENEX authority; city unlock is
    // always read from this host's SCM stats. Returns reset work, never teleport.
    NativeRestartSelection QueryRestart(NativeRestartQuery query) const;
    NativeVehiclePool& Vehicles() { return m_Vehicles; }
    const NativeVehiclePool& Vehicles() const { return m_Vehicles; }
    NativeCarGenerators& CarGenerators() { return m_CarGenerators; }
    const NativeCarGenerators& CarGenerators() const { return m_CarGenerators; }
    const NativeCarGeneratorResidency& CarGeneratorResidency() const { return m_CarGeneratorResidency; }
    // Parent provision transaction: exact selected immutable source-COL packet,
    // BEFORE active world commit/render. Ready requires an immediate non-failing
    // world handoff. Pending/Error keep the previous registry; retry the same
    // generation/snapshot after the actual runtime owner fulfills removals.
    NativeCarGeneratorResidencyResult ReconcileCarGeneratorsBeforeWorldCommit(
        std::uint64_t generation, std::shared_ptr<const NativeCollisionSnapshot> sourceCollision,
        const NativeCarGeneratorResidencyCleanup* cleanup = nullptr);
    // Complete obligations for the exact privately held Prepared world. The
    // caller cannot substitute a callback snapshot: this host rebuilds and owns
    // source COL before reconciliation. Ready here only unblocks the next same-ID
    // service poll; it does not publish the world or return script Ready.
    NativeCarGeneratorResidencyResult ReconcilePendingWorldCleanup(
        const NativeCarGeneratorResidencyCleanup& cleanup);
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
    NativeScriptServiceResult LoadSceneInDirection(const NativeScriptDirectionalSceneRequest&) override;
    NativeScriptServiceResult ClearArea(const NativeScriptClearAreaRequest&) override;
    NativeScriptServiceResult CreatePlayer(const NativeScriptPlayerRequest&) override;
    NativeScriptReferenceResult<NativeScriptGroupRef> GetPlayerGroup(const NativeScriptPlayerLookupRequest&) override;
    NativeScriptReferenceResult<NativeScriptPedRef> GetPlayerChar(const NativeScriptPlayerLookupRequest&) override;
    NativeScriptServiceResult SetCameraBehindPlayer(const NativeScriptCameraRequest&) override;
    NativeScriptServiceResult SetCharHeading(const NativeScriptHeadingRequest&) override;
    NativeScriptReferenceResult<NativeScriptPickupRef> CreateLockedProperty(const NativeScriptLockedPropertyRequest&) override;
    NativeScriptReferenceResult<NativeScriptPickupRef> CreateForSaleProperty(const NativeScriptForSalePropertyRequest&) override;
    NativeScriptReferenceResult<NativeScriptPickupRef> CreatePickup(const NativeScriptPickupRequest&) override;
    NativeScriptReferenceResult<NativeScriptPickupRef> CreatePickupWithAmmo(const NativeScriptPickupAmmoRequest&) override;
    NativeScriptReferenceResult<NativeScriptBlipRef> CreateContactBlip(const NativeScriptContactBlipRequest&) override;
    NativeScriptReferenceResult<NativeScriptBlipRef> CreateCoordinateBlip(const NativeScriptCoordinateBlipRequest&) override;
    NativeScriptServiceResult SetBlipDisplay(const NativeScriptBlipDisplayRequest&) override;
    NativeScriptServiceResult RemoveBlip(const NativeScriptBlipReferenceRequest&) override;
    NativeScriptBooleanResult DoesBlipExist(const NativeScriptBlipReferenceRequest&) override;
    NativeScriptReferenceResult<NativeScriptUserMarkerRef> CreateUserMarker(const NativeScriptUserMarkerRequest&) override;
    NativeScriptServiceResult RemoveUserMarker(const NativeScriptUserMarkerReferenceRequest&) override;
    NativeScriptServiceResult SetEntryExitFlag(const NativeScriptEntryExitFlagRequest&) override;
    NativeScriptServiceResult SwitchEntryExit(const NativeScriptEntryExitSwitchRequest&) override;
    NativeScriptServiceResult DeactivateGarage(const NativeScriptGarageRequest&) override;
    NativeScriptServiceResult ChangeGarageType(const NativeScriptGarageTypeRequest&) override;
    NativeScriptServiceResult AddRestart(const NativeScriptRestartRequest&) override;
    NativeScriptServiceResult AddStuntJump(const NativeScriptStuntJumpRequest&) override;
    NativeScriptServiceResult AddSetPiece(const NativeScriptSetPieceRequest&) override;
    NativeScriptServiceResult InitZonePopulationSettings(const NativeScriptRequestId&) override;
    NativeScriptServiceResult SetZonePopulationType(const NativeScriptZonePopulationRequest&) override;
    NativeScriptServiceResult SetZonePopulationRaces(const NativeScriptZonePopulationRequest&) override;
    NativeScriptServiceResult SetZoneDealerStrength(const NativeScriptZonePopulationRequest&) override;
    NativeScriptServiceResult SetZoneGangStrength(const NativeScriptZoneGangRequest&) override;
    NativeScriptServiceResult SetZoneNoCops(const NativeScriptZonePopulationRequest&) override;
    NativeScriptServiceResult AddPathPolicy(const NativeScriptPathPolicyRequest&) override;
    NativeScriptServiceResult AddExternalScriptTrigger(const NativeScriptExternalTriggerRequest&) override;
    NativeScriptServiceResult AddCodeScriptBrain(const NativeScriptCodeBrainRequest&) override;
    NativeScriptServiceResult AttachAnimsToModel(const NativeScriptModelAnimRequest&) override;
    NativeScriptServiceResult SetIplRequested(const NativeScriptIplRequest&) override;
    NativeScriptServiceResult SetClosestObjectVisibility(const NativeScriptWorldObjectVisibilityRequest&) override;
    NativeScriptServiceResult SetZoneNamesVisible(const NativeScriptRequestId&, bool) override;
    NativeScriptBooleanResult IsPlayerPlaying(const NativeScriptPlayerLookupRequest&) override;
    NativeScriptIntegerResult GetCharAreaVisible(const NativeScriptPedQueryRequest&) override;
    NativeScriptIntegerResult GetAreaVisible(const NativeScriptRequestId&) override;
    NativeScriptIntegerResult GetCurrentDayOfWeek(const NativeScriptRequestId&) override;
    NativeScriptIntegerResult GetCurrentLanguage(const NativeScriptRequestId&) override;
    NativeScriptBooleanResult HasLanguageChanged(const NativeScriptRequestId&) override;
    NativeScriptIntegerResult GetCityPlayerIsIn(const NativeScriptPlayerLookupRequest&) override;
    NativeScriptIntegerResult GetNumberTagsTagged(const NativeScriptRequestId&) override;
    NativeScriptBooleanResult AreCarCheatsActivated(const NativeScriptRequestId&) override;
    NativeScriptBooleanResult HasDeathArrestBeenExecuted(const NativeScriptRequestId&) override;
    NativeScriptBooleanResult IsCharDead(const NativeScriptPedQueryRequest&) override;
    NativeScriptServiceResult SetPedSpeechDisabled(const NativeScriptPedStateRequest&) override;
    NativeScriptBooleanResult IsGarageOpen(const NativeScriptGarageRequest&) override;
    NativeScriptBooleanResult HasCharGotWeapon(const NativeScriptPedWeaponRequest&) override;
    NativeScriptServiceResult RequestModel(const NativeScriptModelRequest&) override;
    NativeScriptBooleanResult HasModelLoaded(const NativeScriptModelRequest&) override;
    NativeScriptServiceResult MarkModelNoLongerNeeded(const NativeScriptModelRequest&) override;
    NativeScriptServiceResult LoadSpecialCharacter(const NativeScriptSpecialModelRequest&) override;
    NativeScriptBooleanResult HasSpecialCharacterLoaded(const NativeScriptSpecialModelRequest&) override;
    NativeScriptServiceResult UnloadSpecialCharacter(const NativeScriptSpecialModelRequest&) override;
    NativeScriptServiceResult RequestCarRecording(const NativeScriptCarRecordingRequest&) override;
    NativeScriptBooleanResult HasCarRecordingLoaded(const NativeScriptCarRecordingRequest&) override;
    NativeScriptServiceResult PreloadBeatTrack(const NativeScriptBeatTrackRequest&) override;
    NativeScriptServiceResult PlayBeatTrack(const NativeScriptRequestId&) override;
    NativeScriptServiceResult StopBeatTrack(const NativeScriptRequestId&) override;
    NativeScriptServiceResult BeginSkippableCutscene(const NativeScriptSkipCutsceneRequest&) override;
    NativeScriptServiceResult EndSkippableCutscene(const NativeScriptRequestId&) override;
    NativeScriptServiceResult ApplyCameraCommand(const NativeScriptCameraCommandRequest&) override;
    NativeScriptServiceResult ClearPrints(const NativeScriptRequestId&) override;
    NativeScriptServiceResult ClearMissionAudio(const NativeScriptRequestId&, std::int32_t slot) override;
    NativeScriptServiceResult LoadMissionAudio(const NativeScriptMissionAudioRequest&) override;
    NativeScriptBooleanResult HasMissionAudioLoaded(const NativeScriptRequestId&, std::int32_t slot) override;
    NativeScriptServiceResult PlayMissionAudio(const NativeScriptRequestId&, std::int32_t slot) override;
    NativeScriptBooleanResult HasMissionAudioFinished(const NativeScriptRequestId&, std::int32_t slot) override;
    NativeScriptIntegerResult GetBeatTrackStatus(const NativeScriptRequestId&) override;
    NativeScriptBooleanResult AreSubtitlesEnabled(const NativeScriptRequestId&) override;
    NativeScriptServiceResult SetDensityMultiplier(const NativeScriptDensityRequest&) override;
    NativeScriptServiceResult SetRandomTrains(const NativeScriptBooleanRequest&) override;
    NativeScriptReferenceResult<NativeScriptVehicleRef> CreateVehicle(const NativeScriptVehicleCreateRequest&) override;
    NativeScriptServiceResult SetVehicleHeading(const NativeScriptVehicleHeadingRequest&) override;
    NativeScriptServiceResult SetVehicleLights(const NativeScriptVehicleStateRequest&) override;
    NativeScriptServiceResult SetVehicleCollision(const NativeScriptVehicleStateRequest&) override;
    NativeScriptServiceResult AddScore(const NativeScriptScoreRequest&) override;
    NativeScriptServiceResult WarpPedIntoVehiclePassenger(const NativeScriptPedVehicleRequest&) override;
    NativeScriptServiceResult TaskLeaveVehicleImmediately(const NativeScriptPedVehicleRequest&) override;
    NativeScriptReferenceResult<NativeScriptPedRef> CreatePedInsideVehicle(const NativeScriptCreatePedInVehicleRequest&) override;
    NativeScriptReferenceResult<NativeScriptPedRef> CreatePed(const NativeScriptPedCreateRequest&) override;
    NativeScriptServiceResult SetFixedCameraPosition(const NativeScriptFixedCameraRequest&) override;
    NativeScriptServiceResult PointCameraAtPoint(const NativeScriptPointCameraRequest&) override;
    NativeScriptServiceResult SetWidescreen(const NativeScriptBooleanRequest&) override;
    NativeScriptVehicleResult GetPedVehicleNoSave(const NativeScriptPedQueryRequest&) override;
    NativeScriptVehicleStatsResult GetWheelieStats(const NativeScriptVehicleStateRequest&) override;
    NativeScriptBooleanResult IsVehicleInAirProper(const NativeScriptVehicleStateRequest&) override;
    NativeScriptBooleanResult IsVehicleDead(const NativeScriptVehicleStateRequest&) override;
    NativeScriptBooleanResult IsVehiclePlaybackActive(const NativeScriptVehicleStateRequest&) override;
    NativeScriptServiceResult StartVehiclePlayback(const NativeScriptVehicleStateRequest&) override;
    NativeScriptReferenceResult<NativeScriptVehicleRef> CreateMissionTrain(const NativeScriptTrainCreateRequest&) override;
    NativeScriptServiceResult SetTrainSpeed(const NativeScriptTrainSpeedRequest&, bool cruise) override;
    NativeScriptServiceResult DeleteMissionTrains(const NativeScriptRequestId&) override;
    NativeScriptCarModelResult GetRandomResidentCarModel(const NativeScriptRequestId&, bool normalOnly) override;
    NativeScriptServiceResult MarkPedNoLongerNeeded(const NativeScriptPedQueryRequest&) override;
    NativeScriptServiceResult MarkVehicleNoLongerNeeded(const NativeScriptVehicleStateRequest&) override;
    NativeScriptServiceResult DeletePed(const NativeScriptPedQueryRequest&) override;
    NativeScriptServiceResult DeleteVehicle(const NativeScriptVehicleStateRequest&) override;
    NativeScriptReferenceResult<NativeScriptPedRef> CreateRandomDriver(const NativeScriptVehicleStateRequest&) override;
    NativeScriptServiceResult AssignCarDriveTask(const NativeScriptCarDriveTaskRequest&) override;
    NativeScriptServiceResult AssignGoStraightTask(const NativeScriptGoStraightTaskRequest&) override;
    struct PendingScriptModel {
        NativeScriptRequestId Id;
        std::int32_t Model = 0;
        std::string Name, Texture;
        bool Vehicle = false;
    };
    const std::optional<PendingScriptModel>& PendingModel() const { return m_PendingModel; }
    bool FulfillPendingModel(const NativeScriptRequestId&, std::shared_ptr<const WorldShotScene>,
        std::shared_ptr<const NativeCollisionModel>, std::string& error);
    NativeScriptBooleanResult QueryPlayerState(const NativeScriptPlayerStateQueryRequest&) override;
    NativeScriptServiceResult ForceWeatherNow(const NativeScriptWeatherRequest&) override;
    NativeScriptServiceResult ReleaseWeather(const NativeScriptRequestId&) override;
    NativeScriptServiceResult GivePlayerClothes(const NativeScriptClothesRequest&) override;
    NativeScriptServiceResult BuildPlayerModel(const NativeScriptPlayerLookupRequest&) override;
    NativeScriptServiceResult StoreClothesState(const NativeScriptRequestId&) override;
    NativeScriptServiceResult SetFadeColour(const NativeScriptFadeColourRequest&) override;
    NativeScriptServiceResult SetAreaVisible(const NativeScriptAreaRequest&) override;
    NativeScriptServiceResult SetPlayerControl(const NativeScriptPlayerControlRequest&) override;
    NativeScriptServiceResult SetPedHealth(const NativeScriptPedHealthRequest&) override;
    NativeScriptServiceResult RemoveAllPedWeapons(const NativeScriptPedQueryRequest&) override;
    NativeScriptBooleanResult IsPedSwimming(const NativeScriptPedQueryRequest&) override;
    NativeScriptServiceResult SetPlayerNeverTired(const NativeScriptPlayerControlRequest&) override;
    NativeScriptServiceResult ShutAllCharsUp(const NativeScriptBooleanRequest&) override;
    NativeScriptPositionResult GetPedCoordinates(const NativeScriptPedQueryRequest&) override;
    NativeScriptServiceResult RemoveTextureDictionary(const NativeScriptRequestId&) override;
    NativeScriptServiceResult LoadTextureDictionary(const NativeScriptTextureDictionaryRequest&) override;
    NativeScriptServiceResult LoadSprite(const NativeScriptSpriteRequest&) override;
    NativeScriptServiceResult ReportAudioEventAtPosition(const NativeScriptAudioEventRequest&) override;
    struct PendingScriptTexture {
        NativeScriptRequestId Id;
        std::string Name;
    };
    const std::optional<PendingScriptTexture>& PendingTexture() const { return m_PendingTexture; }
    bool FulfillPendingTexture(const NativeScriptRequestId&,
        std::shared_ptr<const NativeScriptTextureDictionaryPacket>, std::string& error);
    NativeScriptServiceResult LoadMissionText(const NativeScriptMissionTextRequest&) override;
    NativeScriptServiceResult ClearText(const NativeScriptMissionTextRequest&) override;
    NativeScriptServiceResult UseTextCommands(const NativeScriptTextCommandsRequest&) override;
    NativeScriptServiceResult SetTextDrawBeforeFade(const NativeScriptRequestId&, bool) override;
    NativeScriptServiceResult SetTextFont(const NativeScriptRequestId&, std::int32_t) override;
    NativeScriptServiceResult SetTextStyle(const NativeScriptTextStyleRequest&) override;
    NativeScriptServiceResult DisplayText(const NativeScriptTextDisplayRequest&) override;
    NativeScriptServiceResult PrintNow(const NativeScriptPrintRequest&) override;
    NativeScriptServiceResult LoadCutscene(const NativeScriptCutsceneRequest&) override;
    NativeScriptServiceResult StartCutscene(const NativeScriptRequestId&) override;
    NativeScriptServiceResult ClearCutscene(const NativeScriptRequestId&) override;
    NativeScriptBooleanResult HasCutsceneLoaded(const NativeScriptRequestId&) override;
    NativeScriptBooleanResult HasCutsceneFinished(const NativeScriptRequestId&) override;
    NativeScriptBooleanResult WasCutsceneSkipped(const NativeScriptRequestId&) override;
    NativeScriptStringResult GetCharEntryExitName(const NativeScriptPedQueryRequest&) override;
    NativeScriptServiceResult SetUpdateStatsVisible(const NativeScriptRequestId&, bool) override;
    NativeScriptServiceResult ClearHelp(const NativeScriptRequestId&) override;
    NativeScriptServiceResult StreamScript(const NativeScriptStreamedRequest&) override;
    NativeScriptServiceResult MarkStreamedScriptNoLongerNeeded(const NativeScriptStreamedRequest&) override;
    bool FulfillPendingStreamedScript(const char* gameDir, std::string& error);
    NativeScriptBooleanResult LocateChar(const NativeScriptLocateCharRequest&) override;
    NativeScriptBooleanResult DoesObjectExist(const NativeScriptObjectCleanupRequest&) override;
    NativeScriptBooleanResult LocateCharObject2D(const NativeScriptLocateCharObjectRequest&) override;
    NativeScriptReferenceResult<NativeScriptObjectRef> CreateObjectNoOffset(const NativeScriptObjectRequest&) override;
    NativeScriptReferenceResult<NativeScriptObjectRef> CreateObject(const NativeScriptObjectRequest&) override;
    NativeScriptServiceResult SetObjectHeading(const NativeScriptObjectHeadingRequest&) override;
    NativeScriptServiceResult MarkObjectNoLongerNeeded(const NativeScriptObjectCleanupRequest&) override;
    NativeScriptServiceResult SetObjectCollisionDamageEffect(const NativeScriptObjectDamageRequest&) override;
    NativeScriptServiceResult FreezeObjectPosition(const NativeScriptObjectFreezeRequest&) override;
    NativeScriptServiceResult SetObjectDynamic(const NativeScriptObjectDynamicRequest&) override;
    NativeScriptServiceResult SetObjectVelocity(const NativeScriptObjectVelocityRequest&) override;
    NativeScriptServiceResult SetObjectProofs(const NativeScriptObjectProofRequest&) override;
    NativeScriptServiceResult RotateObject(const NativeScriptObjectRotateRequest&) override;
    NativeScriptServiceResult SetObjectRotation(const NativeScriptObjectRotateRequest&) override;
    NativeScriptServiceResult SetObjectAreaVisible(const NativeScriptObjectAreaRequest&) override;
    NativeScriptObjectCoordinatesResult GetObjectCoordinates(const NativeScriptObjectCoordinatesRequest&) override;
    NativeScriptObjectCoordinatesResult GetObjectOffsetInWorld(const NativeScriptObjectCoordinatesRequest&) override;
    NativeScriptObjectHeadingResult GetObjectHeading(const NativeScriptObjectCoordinatesRequest&) override;
    NativeScriptServiceResult ConnectObjectLods(const NativeScriptObjectLodRequest&) override;
    NativeScriptReferenceResult<NativeScriptCarGeneratorRef> CreateCarGenerator(const NativeScriptCarGeneratorRequest&) override;
    NativeScriptReferenceResult<NativeScriptCarGeneratorRef> CreateCarGeneratorWithPlate(const NativeScriptCarGeneratorPlateRequest&) override;
    NativeScriptServiceResult SwitchCarGenerator(const NativeScriptCarGeneratorSwitchRequest&) override;
    NativeScriptServiceResult SetCarGeneratorOwned(const NativeScriptCarGeneratorOwnedRequest&) override;
    NativeScriptPickupCollectedResult HasPickupBeenCollected(const NativeScriptPickupReferenceRequest&) override;
    NativeScriptServiceResult RemoveScriptPickup(const NativeScriptPickupReferenceRequest&) override;

private:
    NativeScriptReferenceResult<NativeScriptObjectRef> CreateObjectInternal(const NativeScriptObjectRequest&, bool noOffset);
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
    NativeRestarts m_Restarts;
    NativeStuntJumps m_StuntJumps;
    NativeSetPieces m_SetPieces;
    NativeZonePopulation m_ZonePopulation;
    NativePathPolicy m_PathPolicy;
    NativeExternalScriptTriggers m_ExternalTriggers;
    NativeScriptModelAnims m_ModelAnims;
    NativeScriptIplRequests m_IplRequests;
    NativeWorldObjectOverrides m_WorldObjectOverrides;
    NativeScriptWeather m_Weather;
    NativeScriptClothes m_Clothes;
    NativeMissionText m_MissionText;
    NativeCutscene m_Cutscene;
    NativeCarRecordings m_CarRecordings;
    NativeBeatTrack m_BeatTrack;
    NativeMissionAudio m_MissionAudio;
    NativeScriptPeds m_ScriptPeds;
    NativeScriptTrains m_ScriptTrains;
    float m_CarDensityMultiplier = 1.0f, m_PedDensityMultiplier = 1.0f;
    bool m_RandomTrains = true;
    bool m_ZoneNamesVisible = true;
    std::int32_t m_PlayerArea = 0;
    std::int32_t m_DayOfWeek = 4; // source clock Initialise: Thursday
    bool m_CarCheatsActivated = false;
    bool m_DeathArrestExecuted = false;
    std::array<bool, 47> m_PlayerWeapons{};
    struct UserMarker { NativeScriptPosition Position; std::int32_t Colour = 0; bool Used = false; };
    std::array<UserMarker, 5> m_UserMarkers{};
    std::optional<PendingScriptModel> m_PendingModel;
    std::map<std::int32_t, std::shared_ptr<const WorldShotScene>> m_ScriptModels;
    std::map<std::int32_t, std::shared_ptr<const NativeCollisionModel>> m_ScriptModelCollisions;
    std::map<std::int32_t, std::int32_t> m_ScriptVehicleLights;
    std::map<std::int32_t, bool> m_ScriptVehicleCollision;
    std::optional<NativeScriptVehicleRef> m_PlayerScriptVehicle;
    std::int32_t m_PlayerScriptSeat = -1;
    std::optional<NativeScriptPosition> m_ScriptPlayerPosition;
    std::map<std::int32_t, NativeScriptCarDriveTaskRequest> m_ScriptCarDriveTasks;
    std::map<std::int32_t, NativeScriptGoStraightTaskRequest> m_ScriptGoStraightTasks;
    std::map<std::int32_t, bool> m_ScriptPedSpeechDisabled;
    NativeScriptPosition m_ScriptCameraPosition{}, m_ScriptCameraOffset{}, m_ScriptCameraTarget{};
    std::int32_t m_ScriptCameraSwitchType = 0;
    bool m_ScriptCameraFixed = false;
    bool m_ScriptWidescreen = false;
    std::array<bool, 82> m_StreamedNoLongerNeeded{};
    bool m_PlayerControlEnabled = true;
    float m_PlayerHealth = 100.0f;
    bool m_PlayerNeverTired = false;
    bool m_AllCharsShutUp = false;
    std::uint64_t m_ScriptTextureRevision = 0;
    std::optional<PendingScriptTexture> m_PendingTexture;
    std::shared_ptr<const NativeScriptTextureDictionaryPacket> m_ScriptTextureDictionary;
    std::array<std::int32_t, 64> m_ScriptSpriteImages{};
    bool m_UpdateStatsVisible = true;
    std::optional<std::int32_t> m_CutsceneSkipTarget;
    std::optional<NativeScriptPrintRequest> m_LastPrint;
    std::map<std::uint16_t, NativeScriptCameraCommandRequest> m_ScriptCameraCommands;
    std::array<std::uint8_t, 3> m_FadeColour{};
    NativeScriptObjects m_Objects;
    NativeVehiclePool m_Vehicles;
    NativeSourceRng m_SourceRng;
    NativeCarGenerators m_CarGenerators;
    NativeCarGeneratorResidency m_CarGeneratorResidency;
    std::shared_ptr<const NativeCollisionContext> m_CollisionContext;
    std::shared_ptr<const NativePlacementOverrides> m_InitialPlacementOverrides;
    std::shared_ptr<const RealtimeGameplayWorld> m_World;
    RealtimeScriptWorldPublication m_Publication;
    WorldLoader m_Loader;
    CancelLoad m_Cancel;
    NativeScriptServiceTransaction m_WorldTransaction;
    RadarSpriteReady m_RadarSpriteReady;
    std::optional<NativeScriptRequestId> m_PendingLoad;
    std::optional<RealtimeScriptWorldPublication> m_PendingWorldPublication;
    std::optional<NativeScriptPosition> m_CollisionRegion, m_LoadedScene;
    std::vector<RealtimeScriptHostEvent> m_Events;
    // One real persistent player binding, deliberately bounded to player0.
    // CPool: slot << 8 | seven-bit generation, bit7 denotes empty.
    std::uint8_t m_PedGeneration = 0;
    bool m_PedActive = false;
    RealtimeScriptGroup m_Group;
    std::uint64_t m_WorldRevision = 0;
    bool m_Initialized = false, m_Sealed = false, m_InitialGarageWorldPrepared = false, m_InWorldService = false;
};
