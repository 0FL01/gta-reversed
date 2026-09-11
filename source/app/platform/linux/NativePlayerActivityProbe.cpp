// Headless source-predicate and production-controller probe.
#include "app/platform/linux/NativePlayerActivity.h"
#include "app/platform/linux/IfpAnim.h"
#include "app/platform/linux/RealtimeGameplay.h"
#include "app/platform/linux/StreamPager.h"

#include <rw.h>

#include <array>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {
constexpr std::size_t Index(NativePlayerPrimarySlot slot) {
    return static_cast<std::size_t>(slot);
}

constexpr std::size_t Index(NativePlayerSecondarySlot slot) {
    return static_cast<std::size_t>(slot);
}

void Require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "native-player-activity-probe FAIL %s\n", message);
        std::exit(1);
    }
}

NativePlayerActivitySnapshot SourceInitial() {
    NativePlayerActivitySnapshot snapshot;
    snapshot.Authority = NativePlayerActivityAuthority::SourceBacked;
    snapshot.PrimaryTasks[Index(NativePlayerPrimarySlot::Default)] = {NativePlayerTaskType::PlayerOnFoot};
    snapshot.SecondaryTasks[Index(NativePlayerSecondarySlot::Facial)] = {NativePlayerTaskType::Facial};
    snapshot.PedState = NativePlayerPedState::Idle;
    snapshot.Alive = true;
    snapshot.GamePlaying = true;
    for (auto& weapon : snapshot.WeaponSlots) {
        weapon = {NativePlayerWeaponType::Unarmed, NativePlayerWeaponState::Ready, 0, 0};
    }
    return snapshot;
}

void ExpectMission(const NativePlayerActivitySnapshot& snapshot, NativeMissionStartOutcome outcome,
                   NativeMissionStartReason reason, const char* fixture) {
    const auto actual = NativePlayerCanStartMission(snapshot);
    if (actual.Outcome != outcome || actual.Reason != reason) {
        std::fprintf(stderr, "native-player-activity-probe FAIL mission fixture %s outcome=%u reason=%u\n",
                     fixture, static_cast<unsigned>(actual.Outcome), static_cast<unsigned>(actual.Reason));
        std::exit(1);
    }
}

void PredicateFixtures() {
    const auto initial = SourceInitial();
    ExpectMission(initial, NativeMissionStartOutcome::Allowed, NativeMissionStartReason::None, "source initial");

    // Independent expected results follow CPlayerPed.cpp:213-238 in gate order.
    auto ordered = initial;
    ordered.GamePlaying = false;
    ordered.CoopGame = true;
    ordered.InAir = true;
    ordered.PrimaryTasks[Index(NativePlayerPrimarySlot::PhysicalResponse)] = {NativePlayerTaskType::Unsupported};
    ordered.PrimaryTasks[Index(NativePlayerPrimarySlot::EventResponseNonTemp)] = {NativePlayerTaskType::Unsupported};
    ordered.PrimaryTasks[Index(NativePlayerPrimarySlot::Primary)] = {NativePlayerTaskType::Jump};
    ordered.SecondaryTasks[Index(NativePlayerSecondarySlot::Attack)] = {NativePlayerTaskType::Unsupported};
    ordered.Alive = false;
    ordered.TypedEvents = {NativePlayerEventType::ScriptCommand};
    ExpectMission(ordered, NativeMissionStartOutcome::Denied, NativeMissionStartReason::GameNotPlaying, "gate 1 game");
    ordered.GamePlaying = true;
    ExpectMission(ordered, NativeMissionStartOutcome::Denied, NativeMissionStartReason::CoopGame, "gate 1 coop");
    ordered.CoopGame = false;
    ExpectMission(ordered, NativeMissionStartOutcome::Denied, NativeMissionStartReason::PedNotInControlOrDriving, "gate 2 control");
    ordered.InAir = false;
    ordered.Alive = true;
    ExpectMission(ordered, NativeMissionStartOutcome::Denied, NativeMissionStartReason::PhysicalResponseTask, "gate 3 physical");
    ordered.PrimaryTasks[Index(NativePlayerPrimarySlot::PhysicalResponse)].clear();
    ExpectMission(ordered, NativeMissionStartOutcome::Denied, NativeMissionStartReason::EventResponseNonTempTask, "gate 4 nontemp");
    ordered.PrimaryTasks[Index(NativePlayerPrimarySlot::EventResponseNonTemp)].clear();
    ExpectMission(ordered, NativeMissionStartOutcome::Denied, NativeMissionStartReason::PrimaryTaskNotCarDrive, "gate 5 primary");
    ordered.PrimaryTasks[Index(NativePlayerPrimarySlot::Primary)].clear();
    ExpectMission(ordered, NativeMissionStartOutcome::Denied, NativeMissionStartReason::AttackTask, "gate 6 attack");
    ordered.SecondaryTasks[Index(NativePlayerSecondarySlot::Attack)].clear();
    ordered.PedState = NativePlayerPedState::Driving; // IsStateDriving passes gate 2 independently.
    ordered.Alive = false;
    ExpectMission(ordered, NativeMissionStartOutcome::Denied, NativeMissionStartReason::PlayerNotAlive, "gate 7 alive");
    ordered.Alive = true;
    ExpectMission(ordered, NativeMissionStartOutcome::Denied, NativeMissionStartReason::ScriptCommandEvent, "gate 8 script event");
    ordered.TypedEvents.clear();
    ExpectMission(ordered, NativeMissionStartOutcome::Allowed, NativeMissionStartReason::None, "driving allowed");

    auto unsupported = initial;
    unsupported.Authority = NativePlayerActivityAuthority::Unsupported;
    ExpectMission(unsupported, NativeMissionStartOutcome::Unsupported, NativeMissionStartReason::ActivityUnsupported, "unknown authority");
    unsupported = initial;
    unsupported.PrimaryTasks[Index(NativePlayerPrimarySlot::Primary)] = {NativePlayerTaskType::Unsupported};
    ExpectMission(unsupported, NativeMissionStartOutcome::Unsupported, NativeMissionStartReason::ActivityUnsupported, "unknown primary command");
    unsupported = initial;
    unsupported.TypedEvents = {NativePlayerEventType::Unsupported};
    ExpectMission(unsupported, NativeMissionStartOutcome::Unsupported, NativeMissionStartReason::ActivityUnsupported, "unknown event");
    Require(NativePlayerPickupBusy(NativePlayerActivitySnapshot{}), "unknown authority is conservatively pickup-busy");
    Require(!NativePlayerWantsUnarmedPickup(NativePlayerActivitySnapshot{}), "unknown authority cannot claim unarmed desire");

    constexpr std::array busySlots{
        NativePlayerPrimarySlot::Default,
        NativePlayerPrimarySlot::Primary,
        NativePlayerPrimarySlot::EventResponseTemp,
        NativePlayerPrimarySlot::EventResponseNonTemp,
    };
    for (const auto slot : busySlots) {
        for (const auto blocker : {NativePlayerTaskType::EnterCarAsDriver, NativePlayerTaskType::UseMobilePhone}) {
            auto busy = initial;
            for (auto& chain : busy.PrimaryTasks) chain.clear();
            busy.PrimaryTasks[Index(slot)] = {NativePlayerTaskType::PlayerOnFoot, blocker};
            Require(NativePlayerPickupBusy(busy), "pickup busy scans source primary root/subtask chains");
        }
    }
    auto notBusy = initial;
    notBusy.PrimaryTasks[Index(NativePlayerPrimarySlot::PhysicalResponse)] = {NativePlayerTaskType::EnterCarAsDriver};
    notBusy.SecondaryTasks[Index(NativePlayerSecondarySlot::Attack)] = {NativePlayerTaskType::UseMobilePhone};
    Require(!NativePlayerPickupBusy(notBusy), "pickup busy excludes physical and secondary slots");

    Require(NativePlayerWantsUnarmedPickup(initial), "source initial unarmed slot wants unarmed pickup");
    auto weapon = initial;
    weapon.WeaponSlots[0].Type = NativePlayerWeaponType::Other;
    weapon.ActiveWeaponSlot = 1;
    Require(NativePlayerWantsUnarmedPickup(weapon), "different weapon slot accepts unarmed");
    weapon.PrimaryTasks[Index(NativePlayerPrimarySlot::Primary)] = {NativePlayerTaskType::Jump, NativePlayerTaskType::JetPack};
    Require(!NativePlayerWantsUnarmedPickup(weapon), "active simplest jetpack blocks replacement");
    weapon.PrimaryTasks[Index(NativePlayerPrimarySlot::PhysicalResponse)] = {NativePlayerTaskType::PlayerOnFoot};
    Require(NativePlayerWantsUnarmedPickup(weapon), "jetpack outside first active chain does not block");
    weapon.PrimaryTasks[Index(NativePlayerPrimarySlot::PhysicalResponse)].clear();
    weapon.PrimaryTasks[Index(NativePlayerPrimarySlot::Primary)].clear();
    weapon.ActiveWeaponSlot = 0;
    weapon.PedState = NativePlayerPedState::Attack;
    Require(!NativePlayerWantsUnarmedPickup(weapon), "active-slot attack blocks replacement");
    weapon.PedState = NativePlayerPedState::AimGun;
    Require(!NativePlayerWantsUnarmedPickup(weapon), "active-slot aim blocks replacement");
    weapon.PedState = NativePlayerPedState::Unsupported;
    Require(!NativePlayerWantsUnarmedPickup(weapon), "unknown active-slot ped state cannot fabricate desire");
    weapon = initial;
    weapon.WeaponSlots[0].Type = NativePlayerWeaponType::Unsupported;
    Require(!NativePlayerWantsUnarmedPickup(weapon), "unknown unarmed slot cannot fabricate desire");
}

void Triangle(WorldShotMesh& mesh, RealtimeVec3 a, RealtimeVec3 b, RealtimeVec3 c) {
    for (const auto point : {a, b, c}) {
        mesh.pos.push_back(point.X);
        mesh.pos.push_back(point.Y);
        mesh.pos.push_back(point.Z);
    }
    ++mesh.tris;
}

WorldShotScene WorldFixture() {
    WorldShotScene scene;
    scene.meshes.emplace_back();
    auto& mesh = scene.meshes.back();
    Triangle(mesh, {-100, -100, 7}, {100, -100, 7}, {100, 100, 7});
    Triangle(mesh, {-100, -100, 7}, {100, 100, 7}, {-100, 100, 7});
    return scene;
}

void Quad(WorldShotScene& scene, RealtimeVec3 a, RealtimeVec3 b, RealtimeVec3 c, RealtimeVec3 d) {
    if (scene.meshes.empty()) scene.meshes.emplace_back();
    Triangle(scene.meshes[0], a, b, c);
    Triangle(scene.meshes[0], a, c, d);
}

WorldShotScene CurbFixture() {
    WorldShotScene scene;
    Quad(scene, {-20, -15, 7}, {2, -15, 7}, {2, 15, 7}, {-20, 15, 7});
    Quad(scene, {2, -15, 7.15f}, {20, -15, 7.15f}, {20, 15, 7.15f}, {2, 15, 7.15f});
    Quad(scene, {2, -15, 7}, {2, 15, 7}, {2, 15, 7.15f}, {2, -15, 7.15f});
    return scene;
}

void FramesAt(RealtimeGameplay& game, const RealtimeGameplayWorld& world, int count, double dt,
              RealtimeGameplayInput input = {}) {
    for (int i = 0; i < count; ++i) {
        game.Tick(dt, input, world);
        input.Jump = input.Interact = false;
    }
}

void Frames(RealtimeGameplay& game, const RealtimeGameplayWorld& world, int count,
            RealtimeGameplayInput input = {}) {
    FramesAt(game, world, count, 1.0 / 60.0, input);
}

double SourceClipDuration(const char* gameDir, const char* name) {
    std::vector<IfpAnimSeqFrame> sequence;
    char error[512] = {};
    auto* const dictionary = rw::TexDictionary::getCurrent();
    Require(IfpAnim_Seq(gameDir, "player", name, 2, sequence, error, sizeof(error)), error);
    Require(sequence.front().stats.bones == 32 && sequence.front().stats.mapped == 26 &&
            sequence.front().stats.animTotal > 0, "land clip comes from mapped ped.ifp source data");
    const double duration = sequence.front().stats.animTotal;
    IfpAnim_Shutdown();
    rw::TexDictionary::setCurrent(dictionary);
    return duration;
}

double CompleteLanding(RealtimeGameplay& game, const RealtimeGameplayWorld& world, double dt,
                       double sourceDuration, const char* animation, RealtimeGameplayInput input = {}) {
    Require(game.Activity().Landing && std::string(game.State().Animation) == animation,
            "landing starts on the selected source IFP clip");
    const auto revision = game.Activity().Revision;
    const auto firstPose = game.Actors().meshes[0].pos;
    const double started = game.State().SimulatedSeconds;
    bool poseAdvanced = false;
    int frames = 0;
    while (game.Activity().Landing && frames++ < 240) {
        ExpectMission(game.Activity(), NativeMissionStartOutcome::Denied,
                      NativeMissionStartReason::PedNotInControlOrDriving, "actual source landing lifecycle");
        Require(std::string(game.State().Animation) == animation, "landing task and rendered clip agree");
        game.Tick(dt, input, world);
        input.Jump = input.Interact = false;
        if (game.Activity().Landing) {
            Require(game.Activity().Revision == revision, "landing revision changes only at source task boundaries");
            poseAdvanced = poseAdvanced || game.Actors().meshes[0].pos != firstPose;
        }
    }
    Require(!game.Activity().Landing && frames < 240, "landing clears after IFP finish callback and next task process");
    const double elapsed = game.State().SimulatedSeconds - started;
    Require(elapsed + 1e-6 >= sourceDuration && elapsed <= sourceDuration + 2.0 * dt + 1e-5,
            "landing lifetime is source IFP duration plus at most callback/process quantization");
    Require(poseAdvanced && game.Activity().Revision > revision, "landing advances real pose and publishes completion");
    ExpectMission(game.Activity(), NativeMissionStartOutcome::Allowed, NativeMissionStartReason::None,
                  "post-landing source activity");
    return elapsed;
}

void CheckInitialActivity(const NativePlayerActivitySnapshot& activity) {
    Require(activity.Authority == NativePlayerActivityAuthority::SourceBacked, "spawn has source authority");
    Require(activity.PedState == NativePlayerPedState::Idle && !activity.InAir && !activity.Landing &&
            activity.Alive && activity.GamePlaying && !activity.CoopGame, "0053 source ped/game initial state");
    Require(activity.PrimaryTasks[Index(NativePlayerPrimarySlot::Default)] ==
            NativePlayerTaskChain{NativePlayerTaskType::PlayerOnFoot}, "0053 default player-on-foot root");
    for (std::size_t i = 0; i < activity.PrimaryTasks.size(); ++i) {
        if (i != Index(NativePlayerPrimarySlot::Default)) Require(activity.PrimaryTasks[i].empty(), "0053 other primary slots null");
    }
    Require(activity.SecondaryTasks[Index(NativePlayerSecondarySlot::Facial)] ==
            NativePlayerTaskChain{NativePlayerTaskType::Facial}, "0053 facial secondary root");
    for (std::size_t i = 0; i < activity.SecondaryTasks.size(); ++i) {
        if (i != Index(NativePlayerSecondarySlot::Facial)) Require(activity.SecondaryTasks[i].empty(), "0053 other secondary slots null");
    }
    Require(activity.TypedEvents.empty(), "0053 event group empty");
    for (const auto& weapon : activity.WeaponSlots) {
        Require(weapon.Type == NativePlayerWeaponType::Unarmed && weapon.State == NativePlayerWeaponState::Ready &&
                weapon.AmmoInClip == 0 && weapon.TotalAmmo == 0, "0053 unarmed ready zero-ammo weapon slots");
    }
    ExpectMission(activity, NativeMissionStartOutcome::Allowed, NativeMissionStartReason::None, "actual initial activity");
}

struct ReplayResult {
    RealtimeVec3 Ped;
    RealtimeVec3 Car;
    double WalkDistance;
    double DriveDistance;
    std::uint64_t Jumps;
    std::uint64_t Landings;
    std::uint64_t Entries;
    std::uint64_t Exits;
};

ReplayResult ControllerReplay(RealtimeGameplay& game, const RealtimeGameplayWorld& world, double fallLandDuration) {
    std::string error;
    Require(game.Spawn(world, 0, 0, 20, 0, error), error.c_str());
    CheckInitialActivity(game.Activity());
    const auto* const ownedActivity = &game.Activity();
    const auto spawnRevision = game.Activity().Revision;

    Frames(game, world, 30);
    Require(game.Activity().Revision == spawnRevision, "steady on-foot ticks do not invent activity revisions");
    Frames(game, world, 60, {.Forward = 1});
    Frames(game, world, 1, {.Jump = true});
    const auto& jumping = game.Activity();
    const auto& jumpChain = jumping.PrimaryTasks[Index(NativePlayerPrimarySlot::Primary)];
    Require(&jumping == ownedActivity && jumping.InAir && !jumping.Landing && jumpChain == NativePlayerTaskChain{
        NativePlayerTaskType::Jump, NativePlayerTaskType::InAirAndLand, NativePlayerTaskType::InAir},
        "jump publishes owned source task chain");
    ExpectMission(jumping, NativeMissionStartOutcome::Denied, NativeMissionStartReason::PedNotInControlOrDriving, "actual jump");

    const auto landings = game.State().Landings;
    for (int i = 0; i < 180 && game.State().Landings == landings; ++i) Frames(game, world, 1);
    Require(game.State().Landings == landings + 1, "jump reaches real controller landing");
    const auto& landing = game.Activity();
    Require(!landing.InAir && landing.Landing &&
            landing.PrimaryTasks[Index(NativePlayerPrimarySlot::Primary)].back() == NativePlayerTaskType::Land,
            "jump landing publishes SIMPLE_LAND and landing flag");
    CompleteLanding(game, world, 1.0 / 60.0, fallLandDuration, "FALL_land");
    Require(!game.Activity().InAir && !game.Activity().Landing &&
            game.Activity().PrimaryTasks[Index(NativePlayerPrimarySlot::Primary)].empty(),
            "landing completion restores default task projection");

    Frames(game, world, 45, {.Forward = -1});
    Frames(game, world, 30, {.Side = 1});
    Frames(game, world, 1, {.Interact = true});
    Require(game.State().InVehicle && game.Activity().PedState == NativePlayerPedState::Driving &&
            game.Activity().PrimaryTasks[Index(NativePlayerPrimarySlot::Primary)] ==
                NativePlayerTaskChain{NativePlayerTaskType::CarDrive}, "entry reaches source car-drive activity");
    for (const auto& chain : game.Activity().PrimaryTasks) {
        Require(std::find(chain.begin(), chain.end(), NativePlayerTaskType::EnterCarAsDriver) == chain.end(),
                "instant native entry does not claim an unexecuted enter-car task");
    }
    Require(!NativePlayerPickupBusy(game.Activity()), "instant completed entry is not source pickup-busy");
    ExpectMission(game.Activity(), NativeMissionStartOutcome::Allowed, NativeMissionStartReason::None, "actual driving");
    const auto driveRevision = game.Activity().Revision;
    Frames(game, world, 20, {.Forward = 1, .Side = 0.25f});
    Require(game.State().Speed > 1 && game.Activity().PedState == NativePlayerPedState::Driving &&
            game.Activity().Revision == driveRevision,
            "drive retains car activity while production physics advances");
    Frames(game, world, 120, {.Brake = true});
    Require(game.State().Speed < 0.01f && game.State().Speed > -0.01f, "vehicle stops before exit");
    Frames(game, world, 1, {.Interact = true});
    Require(!game.State().InVehicle && game.Activity().PedState == NativePlayerPedState::Idle &&
            game.Activity().PrimaryTasks[Index(NativePlayerPrimarySlot::Primary)].empty() &&
            game.Activity().PrimaryTasks[Index(NativePlayerPrimarySlot::Default)] ==
                NativePlayerTaskChain{NativePlayerTaskType::PlayerOnFoot}, "exit restores source on-foot activity");

    const auto& state = game.State();
    return {state.Ped, state.Car, state.WalkDistance, state.DriveDistance,
            state.Jumps, state.Landings, state.Entries, state.Exits};
}

void LandingRateAudit(RealtimeGameplay& game, const RealtimeGameplayWorld& world, double hz,
                      double jumpLandDuration, double fallLandDuration) {
    std::string error;
    const double dt = 1.0 / hz;
    Require(game.Spawn(world, 0, 0, 20, 0, error), error.c_str());
    const auto resetRevision = game.Activity().Revision;
    Require(game.State().Ticks == 0, "spawn resets simulation ticks");
    game.Tick(dt, {.Jump = true}, world);
    Require(game.Activity().Revision > resetRevision && game.Activity().InAir,
            "jump publishes a distinct in-air activity revision");
    const auto landings = game.State().Landings;
    while (game.State().Landings == landings) {
        Require(game.Activity().InAir && !game.Activity().Landing, "jump remains in-air until terrain contact");
        game.Tick(dt, {}, world);
    }
    const double fallObserved = CompleteLanding(game, world, dt, fallLandDuration, "FALL_land");

    const auto previousRevision = game.Activity().Revision;
    Require(game.Spawn(world, 0, 0, 20, 0, error), error.c_str());
    Require(game.State().Ticks == 0 && game.Activity().Revision > previousRevision,
            "activity revision remains monotonic across tick-resetting spawn");
    RealtimeGameplayInput sprint{.Forward = 1, .Sprint = true, .Jump = true};
    game.Tick(dt, sprint, world);
    const auto sprintLandings = game.State().Landings;
    sprint.Jump = false;
    while (game.State().Landings == sprintLandings) game.Tick(dt, sprint, world);
    const double jumpObserved = CompleteLanding(game, world, dt, jumpLandDuration / 2.0, "JUMP_land", sprint);
    std::printf("landing hz=%.0f fall=%.6f/%.6f jumpSprint=%.6f/%.6f\n",
                hz, fallObserved, fallLandDuration, jumpObserved, jumpLandDuration / 2.0);
}

void CurbRateAudit(RealtimeGameplay& game, RealtimeGameplayWorld& world, double hz) {
    std::string error;
    Require(world.Rebuild(CurbFixture(), error), error.c_str());
    Require(game.Spawn(world, 0, 0, 20, 0, error), error.c_str());
    const auto revision = game.Activity().Revision;
    const int ticks = static_cast<int>(std::round(hz * 3.0));
    for (int i = 0; i < ticks; ++i) {
        game.Tick(1.0 / hz, {.Forward = 1}, world);
        Require(game.State().Grounded && !game.Activity().InAir && !game.Activity().Landing &&
                game.Activity().Revision == revision, "source curb support does not invent in-air activity");
        ExpectMission(game.Activity(), NativeMissionStartOutcome::Allowed, NativeMissionStartReason::None,
                      "supported curb activity");
    }
    Require(game.State().Ped.X > 5.9f && std::abs(game.State().Ped.Z - 7.15f) < 0.01f &&
            game.State().Landings == 0, "source curb traversal remains continuously supported");
}

void ControllerFixtures(const char* gameDir) {
    char streamError[512] = {};
    E2ELoadInfo load;
    Require(StreamPager_Init(gameDir, load, streamError, sizeof(streamError), {true, 900.0f, 4096}), streamError);

    const double jumpLandDuration = SourceClipDuration(gameDir, "JUMP_land");
    const double fallLandDuration = SourceClipDuration(gameDir, "FALL_land");
    Require(std::abs(jumpLandDuration - 7.0 / 30.0) < 0.0001 &&
            std::abs(fallLandDuration - 14.0 / 30.0) < 0.0001,
            "installed ped.ifp landing durations match source animation clocks");

    std::string error;
    RealtimeGameplayWorld world;
    Require(world.Rebuild(WorldFixture(), error), error.c_str());
    RealtimeGameplay game;
    Require(game.Activity().Authority == NativePlayerActivityAuthority::Unsupported, "unspawned controller activity unsupported");
    Require(game.Initialize(gameDir, error, RealtimeGameplayModel::BasePlayer), error.c_str());

    Require(game.SpawnScriptPlayer(world, {0, 0, 9}, error), error.c_str());
    CheckInitialActivity(game.Activity());
    const auto headingRevision = game.Activity().Revision;
    const auto headingTicks = game.State().Ticks;
    Require(game.SetScriptHeading(0.25f, error), error.c_str());
    Require(game.Activity().Revision == headingRevision && game.State().Ticks == headingTicks,
            "heading changes pose but not player activity revision");
    Require(!game.State().Grounded, "airborne authored 0053 keeps authored height");
    Frames(game, world, 1);
    Require(game.Activity().InAir && !game.Activity().Landing &&
            game.Activity().PrimaryTasks[Index(NativePlayerPrimarySlot::EventResponseTemp)] == NativePlayerTaskChain{
                NativePlayerTaskType::InAirAndLand, NativePlayerTaskType::InAir},
            "real unsupported-by-0053 footing becomes source in-air event response");
    const auto fallingLandings = game.State().Landings;
    for (int i = 0; i < 180 && game.State().Landings == fallingLandings; ++i) Frames(game, world, 1);
    Require(game.State().Landings == fallingLandings + 1 && game.Activity().Landing &&
            game.Activity().PrimaryTasks[Index(NativePlayerPrimarySlot::EventResponseTemp)].back() == NativePlayerTaskType::Land,
            "fall transitions through source landing task");
    CompleteLanding(game, world, 1.0 / 60.0, fallLandDuration, "FALL_land");

    for (const double hz : {30.0, 60.0, 144.0}) LandingRateAudit(game, world, hz, jumpLandDuration, fallLandDuration);
    for (const double hz : {30.0, 60.0, 144.0}) CurbRateAudit(game, world, hz);
    Require(world.Rebuild(WorldFixture(), error), error.c_str());

    const auto first = ControllerReplay(game, world, fallLandDuration);
    const auto second = ControllerReplay(game, world, fallLandDuration);
    Require(first.Ped.X == second.Ped.X && first.Ped.Y == second.Ped.Y && first.Ped.Z == second.Ped.Z &&
            first.Car.X == second.Car.X && first.Car.Y == second.Car.Y && first.Car.Z == second.Car.Z &&
            first.WalkDistance == second.WalkDistance && first.DriveDistance == second.DriveDistance &&
            first.Jumps == second.Jumps && first.Landings == second.Landings &&
            first.Entries == second.Entries && first.Exits == second.Exits,
            "production controller activity replay remains deterministic");

    std::printf("controller replay walk=%.3f drive=%.3f jumps=%llu landings=%llu entries=%llu exits=%llu\n",
                first.WalkDistance, first.DriveDistance, static_cast<unsigned long long>(first.Jumps),
                static_cast<unsigned long long>(first.Landings), static_cast<unsigned long long>(first.Entries),
                static_cast<unsigned long long>(first.Exits));
    StreamPager_Shutdown();
}
} // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    PredicateFixtures();
    ControllerFixtures(argc > 1 ? argv[1] : "/game");
    std::puts("native-player-activity-probe PASS");
    return 0;
}
