// Isolated source-semantics fixture for save-token collection. It supplies
// explicit owned activity snapshots; it is not a second gameplay authority.
#include "app/platform/linux/NativeScriptEntities.h"
#include "app/platform/linux/StreamPager.h"

#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
int g_Failures;

void Check(bool ok, const char* text) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", text);
    g_Failures += !ok;
}

void Require(bool ok, const std::string& error) {
    if (!ok) throw std::runtime_error(error);
}

constexpr std::size_t Primary(NativePlayerPrimarySlot slot) {
    return static_cast<std::size_t>(slot);
}

constexpr std::size_t Secondary(NativePlayerSecondarySlot slot) {
    return static_cast<std::size_t>(slot);
}

NativePlayerActivitySnapshot ReadyActivity() {
    NativePlayerActivitySnapshot activity;
    activity.Authority = NativePlayerActivityAuthority::SourceBacked;
    activity.PedState = NativePlayerPedState::Idle;
    activity.Alive = true;
    activity.GamePlaying = true;
    activity.WeaponSlots[0].Type = NativePlayerWeaponType::Unarmed;
    activity.WeaponSlots[0].State = NativePlayerWeaponState::Ready;
    return activity;
}

struct Fixture {
    NativeScriptEntities Entities{1, 1};
    NativeScriptPosition Near{1.0f, -2.0f, 3.0f};
    std::uint32_t Frame = 0;
    std::uint64_t OwnerRevision = 0;
    std::uint64_t Request = 0;
    std::string Error;

    explicit Fixture(const char* dir) { Require(Entities.LoadBeforeWorker(dir, Error), Error); }

    NativeScriptPickupRef Create() {
        NativeScriptPickupRequest request{{8001, ++Request, 100}, 1277, 3, Near};
        const auto result = Entities.CreatePickup(request, Near, Frame);
        Require(result.Result.Status == NativeScriptServiceStatus::Ready, result.Result.Message);
        Require(result.Reference.Value != -1, "fixture pickup allocation");
        return result.Reference;
    }

    bool Publish(const NativePlayerActivitySnapshot& activity, bool alive = true, bool inVehicle = false) {
        Frame += 96; // Slot zero belongs to both source 32-way and 6-way slices.
        Require(Entities.UpdatePlayerActivity(Frame, ++OwnerRevision, activity, Error), Error);
        NativeScriptPropertyInput input; input.FrameCounter = Frame;
        Entities.Tick(Near, Near, alive, inVehicle, input);
        return Entities.AdvanceTime(Frame, Error);
    }

    NativeScriptPickupCollectedResult Collected(NativeScriptPickupRef pickup) {
        return Entities.HasPickupBeenCollected({{8002, ++Request, 200}, pickup});
    }

    NativeScriptServiceResult Remove(NativeScriptPickupRef pickup) {
        return Entities.RemoveScriptPickup({{8003, ++Request, 300}, pickup});
    }
};
} // namespace

int NativePickupCollectionProbe(const char* dir) {
    Fixture fixture(dir);
    const auto pickup = fixture.Create();
    const auto source = ReadyActivity();
    Check(!fixture.Collected({-1}).Collected,
        "source-zeroed collection ring does not report the invalid -1 pickup sentinel");
    NativeScriptPropertyInput legacy; legacy.FrameCounter = 0;
    fixture.Entities.Tick(fixture.Near, fixture.Near, true, false, legacy);
    Check(!fixture.Entities.AdvanceTime(0, fixture.Error) &&
        fixture.Entities.PickupRequirement().Kind == NativeScriptPickupRequirementKind::PlayerTaskEligibility &&
        fixture.Entities.ResolvePickup(pickup), "legacy caller remains fail-closed on its typed task-authority latch");

    struct MissionCase {
        NativePlayerActivitySnapshot Activity;
        NativeMissionStartReason Reason;
        const char* Name;
    };
    std::vector<MissionCase> denied;
    auto activity = source; activity.GamePlaying = false;
    denied.push_back({activity, NativeMissionStartReason::GameNotPlaying, "game-state denial"});
    activity = source; activity.CoopGame = true;
    denied.push_back({activity, NativeMissionStartReason::CoopGame, "coop denial"});
    activity = source; activity.PedState = NativePlayerPedState::Arrested;
    denied.push_back({activity, NativeMissionStartReason::PedNotInControlOrDriving, "ped-control denial"});
    activity = source; activity.PrimaryTasks[Primary(NativePlayerPrimarySlot::PhysicalResponse)] = {NativePlayerTaskType::Jump};
    denied.push_back({activity, NativeMissionStartReason::PhysicalResponseTask, "physical-response denial"});
    activity = source; activity.PrimaryTasks[Primary(NativePlayerPrimarySlot::EventResponseNonTemp)] = {NativePlayerTaskType::Jump};
    denied.push_back({activity, NativeMissionStartReason::EventResponseNonTempTask, "non-temp-event-response denial"});
    activity = source; activity.PrimaryTasks[Primary(NativePlayerPrimarySlot::Primary)] = {NativePlayerTaskType::PlayerOnFoot};
    denied.push_back({activity, NativeMissionStartReason::PrimaryTaskNotCarDrive, "primary-task denial"});
    activity = source; activity.SecondaryTasks[Secondary(NativePlayerSecondarySlot::Attack)] = {NativePlayerTaskType::Jump};
    denied.push_back({activity, NativeMissionStartReason::AttackTask, "attack-task denial"});
    activity = source; activity.PedState = NativePlayerPedState::Driving; activity.Alive = false;
    denied.push_back({activity, NativeMissionStartReason::PlayerNotAlive, "alive denial"});
    activity = source; activity.TypedEvents = {NativePlayerEventType::ScriptCommand};
    denied.push_back({activity, NativeMissionStartReason::ScriptCommandEvent, "script-event denial"});

    for (const auto& test : denied) {
        const auto decision = NativePlayerCanStartMission(test.Activity);
        Check(decision.Outcome == NativeMissionStartOutcome::Denied && decision.Reason == test.Reason, test.Name);
        Check(fixture.Publish(test.Activity) && fixture.Entities.ResolvePickup(pickup) &&
            fixture.Entities.PickupRequirement().Kind == NativeScriptPickupRequirementKind::None &&
            !fixture.Entities.ConsumePadShake(), "denied eligibility is a non-collecting successful frame");
    }

    activity = source; activity.Landing = true; activity.PedState = NativePlayerPedState::Driving;
    Check(NativePlayerCanStartMission(activity).Outcome == NativeMissionStartOutcome::Allowed,
        "source control negation allows driving when IsPedInControl is false");
    activity = source; activity.PrimaryTasks[Primary(NativePlayerPrimarySlot::Primary)] = {NativePlayerTaskType::CarDrive};
    Check(NativePlayerCanStartMission(activity).Outcome == NativeMissionStartOutcome::Allowed,
        "source primary-task negation allows only car drive");
    Check(NativePlayerWantsUnarmedPickup(source), "save model default unarmed slot is immediately wanted");
    activity = source; activity.WeaponSlots[0].Type = NativePlayerWeaponType::Other;
    Check(NativePlayerWantsUnarmedPickup(activity), "known non-unarmed fixture follows source conditional default true");
    activity.PrimaryTasks[Primary(NativePlayerPrimarySlot::Default)] = {NativePlayerTaskType::JetPack};
    Check(!NativePlayerWantsUnarmedPickup(activity), "source unarmed desire jetpack negation");
    activity = source; activity.WeaponSlots[0].Type = NativePlayerWeaponType::Other;
    activity.ActiveWeaponSlot = 0; activity.PedState = NativePlayerPedState::Attack;
    Check(!NativePlayerWantsUnarmedPickup(activity), "source active-slot attack negation");

    for (const auto slot : {NativePlayerPrimarySlot::Default, NativePlayerPrimarySlot::Primary,
             NativePlayerPrimarySlot::EventResponseTemp, NativePlayerPrimarySlot::EventResponseNonTemp}) {
        activity = source; activity.PrimaryTasks[Primary(slot)] = {NativePlayerTaskType::Jump, NativePlayerTaskType::UseMobilePhone};
        Check(NativePlayerPickupBusy(activity), "pickup busy searches each source primary root chain");
    }
    activity = source; activity.PrimaryTasks[Primary(NativePlayerPrimarySlot::Default)] = {NativePlayerTaskType::EnterCarAsDriver};
    Check(fixture.Publish(activity) && fixture.Entities.ResolvePickup(pickup) && !fixture.Entities.ConsumePadShake(),
        "source busy gate skips mission/desire and collection");

    activity = source; activity.Authority = NativePlayerActivityAuthority::Unsupported;
    const auto beforeUnknown = fixture.Entities.Revision();
    Check(!fixture.Publish(activity) && fixture.Entities.PickupRequirement().Kind == NativeScriptPickupRequirementKind::PlayerActivityAuthority &&
        fixture.Entities.ResolvePickup(pickup) && fixture.Entities.Revision() == beforeUnknown && !fixture.Entities.ConsumePadShake(),
        "unknown snapshot authority is explicit Unsupported with no collection state");
    activity = source; activity.PrimaryTasks[Primary(NativePlayerPrimarySlot::Primary)] = {NativePlayerTaskType::Unsupported};
    Check(!fixture.Publish(activity) && fixture.Entities.PickupRequirement().Kind == NativeScriptPickupRequirementKind::PlayerTaskEligibility &&
        fixture.Entities.PickupRequirement().MissionStart.Outcome == NativeMissionStartOutcome::Unsupported && fixture.Entities.ResolvePickup(pickup),
        "unknown mission root is an explicit typed requirement");
    activity = source; activity.GamePlaying = false; activity.WeaponSlots[0].Type = NativePlayerWeaponType::Unsupported;
    Check(fixture.Publish(activity) && fixture.Entities.PickupRequirement().Kind == NativeScriptPickupRequirementKind::None && fixture.Entities.ResolvePickup(pickup),
        "mission denial is ordered before unknown unarmed desire");
    activity = source; activity.WeaponSlots[0].Type = NativePlayerWeaponType::Unsupported;
    Check(!fixture.Publish(activity) && fixture.Entities.PickupRequirement().Kind == NativeScriptPickupRequirementKind::PlayerPickupDesire &&
        fixture.Entities.ResolvePickup(pickup), "unknown unarmed desire is an explicit typed requirement");
    activity = source; activity.WeaponSlots[0].Type = NativePlayerWeaponType::Other;
    activity.PrimaryTasks[Primary(NativePlayerPrimarySlot::Default)] = {NativePlayerTaskType::JetPack};
    Check(fixture.Publish(activity) && fixture.Entities.PickupRequirement().Kind == NativeScriptPickupRequirementKind::None &&
        fixture.Entities.ResolvePickup(pickup) && !fixture.Entities.ConsumePadShake(),
        "known unarmed-desire denial is non-faulting and non-collecting");

    const auto stableRevision = fixture.Entities.Revision();
    const auto stableVertices = fixture.Entities.Pickups()[0].Actor.meshes.front().pos;
    auto invalid = source; invalid.ActiveWeaponSlot = NativePlayerActivitySnapshot::WeaponSlotCount;
    Check(!fixture.Entities.UpdatePlayerActivity(fixture.Frame + 96, fixture.OwnerRevision + 1, invalid, fixture.Error) &&
        fixture.Entities.Revision() == stableRevision && fixture.Entities.Pickups()[0].Actor.meshes.front().pos == stableVertices,
        "invalid activity validation preserves all entity state");
    Check(!fixture.Entities.UpdatePlayerActivity(fixture.Frame - 96, fixture.OwnerRevision, source, fixture.Error) &&
        fixture.Entities.Revision() == stableRevision, "stale activity frame preserves owner copy and entity state");
    Require(fixture.Entities.UpdatePlayerActivity(fixture.Frame, fixture.OwnerRevision, activity, fixture.Error), fixture.Error);
    auto changedSameRevision = activity; changedSameRevision.GamePlaying = false;
    Check(!fixture.Entities.UpdatePlayerActivity(fixture.Frame + 96, fixture.OwnerRevision, changedSameRevision, fixture.Error) &&
        fixture.Entities.Revision() == stableRevision, "changed snapshot requires a new trusted owner revision");

    fixture.Frame += 96;
    auto copiedActivity = source;
    Require(fixture.Entities.UpdatePlayerActivity(fixture.Frame, ++fixture.OwnerRevision, copiedActivity, fixture.Error), fixture.Error);
    copiedActivity.Authority = NativePlayerActivityAuthority::Unsupported;
    NativeScriptPropertyInput acceptedInput; acceptedInput.FrameCounter = fixture.Frame;
    fixture.Entities.Tick(fixture.Near, fixture.Near, true, false, acceptedInput);
    const auto accepted = fixture.Entities.AdvanceTime(fixture.Frame, fixture.Error);
    Check(accepted, "eligible owned snapshot copy collects after the caller mutates its input");
    Check(!fixture.Entities.ResolvePickup(pickup) && !fixture.Entities.Pickups()[0].Active &&
        fixture.Entities.Pickups()[0].Type == 0 && !fixture.Entities.Pickups()[0].ObjectPresent,
        "accepted save pickup atomically becomes source type NONE with no object");
    Check(fixture.Entities.HelpRevision() == 0 && fixture.Entities.Interaction().Status == NativeScriptPropertyInteractionStatus::None,
        "model1277 collection has no help clear, sale, money, health, inventory or audio surrogate");
    Check(fixture.Entities.AdvanceTime(fixture.Frame, fixture.Error),
        "duplicate published frame is idempotent after collection");
    const auto shake = fixture.Entities.ConsumePadShake();
    Check(shake && shake->Pickup.Value == pickup.Value && shake->FrameCounter == fixture.Frame &&
        shake->TimeMs == 120 && shake->Frequency == 100 && shake->Arg2 == 0 && !fixture.Entities.ConsumePadShake(),
        "one collection publishes one source-parameter shake event consumed once");

    const auto queryId = NativeScriptRequestId{8100, ++fixture.Request, 400};
    const auto queried = fixture.Entities.HasPickupBeenCollected({queryId, pickup});
    const auto afterQuery = fixture.Entities.Revision();
    Check(queried.Result.Status == NativeScriptServiceStatus::Ready && queried.Collected &&
        fixture.Entities.HasPickupBeenCollected({queryId, pickup}).Collected && fixture.Entities.Revision() == afterQuery,
        "collected query consumes inactive full reference and replays its result exactly once");
    Check(!fixture.Collected(pickup).Collected, "new collected query observes the consumed ring entry");
    const auto inactiveRevision = fixture.Entities.Revision();
    Check(fixture.Remove(pickup).Status == NativeScriptServiceStatus::Ready && fixture.Entities.Revision() == inactiveRevision,
        "current-generation removed slot is a source-valid removal no-op");

    auto explicitPickup = fixture.Create();
    const auto staleRemoval = NativeScriptRequestId{8101, ++fixture.Request, 401};
    Check(fixture.Entities.RemoveScriptPickup({staleRemoval, pickup}).Status == NativeScriptServiceStatus::Ready &&
        fixture.Entities.ResolvePickup(explicitPickup), "stale-generation script removal is a successful no-op");
    const auto removeId = NativeScriptRequestId{8101, ++fixture.Request, 402};
    const auto beforeRemove = fixture.Entities.Revision();
    Check(fixture.Entities.RemoveScriptPickup({removeId, explicitPickup}).Status == NativeScriptServiceStatus::Ready &&
        !fixture.Entities.ResolvePickup(explicitPickup) && fixture.Entities.Revision() == beforeRemove + 1 &&
        fixture.Entities.RemoveScriptPickup({removeId, explicitPickup}).Status == NativeScriptServiceStatus::Ready &&
        fixture.Entities.Revision() == beforeRemove + 1, "explicit removal is generation-aware and request-id idempotent");
    Check(fixture.Entities.RemoveScriptPickup({removeId, pickup}).Status == NativeScriptServiceStatus::Error &&
        !fixture.Collected(explicitPickup).Collected, "remove replay mismatch preserves state and explicit removal never appends");

    std::vector<NativeScriptPickupRef> wrapped;
    for (unsigned i = 0; i < 21; ++i) {
        const auto ref = fixture.Create();
        Require(fixture.Publish(source), fixture.Error);
        Require(!fixture.Entities.ResolvePickup(ref), "ring fixture collected");
        const auto event = fixture.Entities.ConsumePadShake();
        Require(event && event->Pickup.Value == ref.Value && !fixture.Entities.ConsumePadShake(), "ring fixture one shake");
        wrapped.push_back(ref);
    }
    Check(!fixture.Collected(wrapped.front()).Collected && fixture.Collected(wrapped[1]).Collected,
        "source capacity20 ring wraps cursor and compares the full generation reference");

    const auto live = fixture.Create();
    Check(fixture.Remove(wrapped.back()).Status == NativeScriptServiceStatus::Ready && fixture.Entities.ResolvePickup(live),
        "old collected generation cannot remove a reused live slot");
    const auto beforeStaleTime = fixture.Entities.Revision();
    const auto liveVertices = fixture.Entities.ResolvePickup(live)->Actor.meshes.front().pos;
    NativeScriptPropertyInput stale; stale.FrameCounter = fixture.Frame - 96;
    fixture.Entities.Tick(fixture.Near, fixture.Near, true, false, stale);
    Check(!fixture.Entities.AdvanceTime(fixture.Frame - 96, fixture.Error) && fixture.Entities.Revision() == beforeStaleTime &&
        fixture.Entities.ResolvePickup(live)->Actor.meshes.front().pos == liveVertices && !fixture.Entities.ConsumePadShake(),
        "stale source time/frame validation preserves pickup, ring and feedback state");

    std::printf("NativePickupCollectionProbe failures=%d ring=20 collection=owned feedback=pending-parent-os\n", g_Failures);
    return g_Failures;
}

int main(int argc, char** argv) try {
    const char* dir = argc > 1 ? argv[1] : "/game";
    E2ELoadInfo info{}; char error[512]{};
    Require(StreamPager_Init(dir, info, error, sizeof(error), {.includeStreamed=true, .radius=300, .maxInstances=1200}), error);
    const auto failures = NativePickupCollectionProbe(dir);
    StreamPager_Shutdown();
    return failures ? 1 : 0;
} catch (const std::exception& e) {
    std::fprintf(stderr, "NativePickupCollectionProbe FAIL %s\n", e.what());
    return 2;
}
