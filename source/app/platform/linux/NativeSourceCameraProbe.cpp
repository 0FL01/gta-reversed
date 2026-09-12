#include "NativeSourceCamera.h"

#include <bit>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <type_traits>

namespace {
std::size_t g_Checks;
void Check(bool condition, const char* message) {
    ++g_Checks;
    if (!condition) {
        std::cerr << "sa-core-camera-failed check=" << g_Checks << " " << message << '\n';
        std::exit(1);
    }
}
bool Near(float a, float b) { return std::abs(a - b) < 0.00001f; }
NativeSourceCameraPlayer Player(NativeSourceCameraPlayerState state, bool vehicle = false,
    bool bike = false) {
    return {state, 11, vehicle ? 22u : 0u, vehicle, bike};
}
}

int main() {
    static_assert(std::is_trivially_copyable_v<NativeSourceCameraTarget>);
    static_assert(std::is_trivially_copyable_v<NativeSourceCameraTransition>);
    static_assert(std::is_trivially_copyable_v<NativeSourceCameraEvent>);

    NativeSourceCamera camera;
    Check(!camera.LastCommitted(), "fresh owner has no publication");
    Check(camera.Events().empty(), "fresh owner has no events");
    Check(camera.ResolveView() == NativeSourceCameraViewStatus::Unsupported,
        "unreversed source view solver stays explicit");
    Check(camera.Advance(1) == NativeSourceCameraStatus::NotLoaded, "unloaded advance typed");
    Check(camera.SetDirectlyBehind(1, {1, 0, 0}) == NativeSourceCameraStatus::NotLoaded,
        "unloaded direct-behind typed");
    Check(camera.Restore(1, Player(NativeSourceCameraPlayerState::OnFoot), {1, 0, 0}) ==
        NativeSourceCameraStatus::NotLoaded, "unloaded restore typed");
    Check(camera.Initialize(0, 11, 100) == NativeSourceCameraStatus::InvalidInput,
        "zero epoch rejected");
    Check(camera.Initialize(7, 0, 100) == NativeSourceCameraStatus::InvalidInput,
        "zero ped identity rejected");
    Check(camera.Initialize(7, 11, 100) == NativeSourceCameraStatus::Ok, "spawn camera initialized");
    auto spawn = camera.LastCommitted();
    Check(spawn && spawn->Epoch == 7 && spawn->Generation == 1 && spawn->TimeMs == 100,
        "spawn publication identity");
    Check(spawn->Mode == NativeSourceCameraMode::FollowPed, "source initial follow-ped mode");
    Check(spawn->Target == NativeSourceCameraTarget{NativeSourceCameraTargetKind::Ped, 11},
        "source initial target ped");
    Check(!spawn->Transition.Active && spawn->LookingAtPlayer && !spawn->LookingAtVector,
        "source initial ownership flags");
    Check(spawn->Events.size() == 1 && spawn->Events[0].Sequence == 1 &&
        spawn->Events[0].Kind == NativeSourceCameraEventKind::Spawn, "spawn journal");
    const auto spawnCopy = *spawn;

    Check(camera.SetDirectlyBehind(99, {1, 0, 0}) == NativeSourceCameraStatus::BackwardTime,
        "backward direct-behind rejected");
    Check(camera.LastCommitted() == spawn, "backward direct-behind retains publication");
    Check(camera.SetDirectlyBehind(101, {0, 0, 1}) == NativeSourceCameraStatus::InvalidInput,
        "vertical ped forward rejected");
    Check(camera.SetDirectlyBehind(101, {std::numeric_limits<float>::infinity(), 0, 0}) ==
        NativeSourceCameraStatus::InvalidInput, "nonfinite ped forward rejected");
    Check(camera.LastCommitted() == spawn, "bad direct-behind retains publication");
    Check(camera.SetDirectlyBehind(101, {0, 1, 0}) == NativeSourceCameraStatus::Ok,
        "direct-behind accepted");
    auto behind = camera.LastCommitted();
    Check(behind->DirectlyBehind && !behind->DirectlyInFront, "direct-behind source flag");
    Check(Near(behind->PedOrientationForBehindOrInFront, 1.57079632679f),
        "source atan orientation");
    Check(behind->Events.size() == 2 && behind->Events.back().Sequence == 2 &&
        behind->Events.back().Kind == NativeSourceCameraEventKind::DirectBehind,
        "direct-behind journal order");
    Check(*spawn == spawnCopy, "old spawn publication immutable");

    auto invalid = Player(NativeSourceCameraPlayerState::InVehicle);
    Check(camera.Restore(102, invalid, {1, 0, 0}) == NativeSourceCameraStatus::InvalidInput,
        "in-vehicle requires vehicle owner");
    invalid = Player(NativeSourceCameraPlayerState::OnFoot);
    invalid.PedIdentity = 12;
    Check(camera.Restore(102, invalid, {1, 0, 0}) == NativeSourceCameraStatus::InvalidInput,
        "foreign ped identity rejected");
    invalid = Player(static_cast<NativeSourceCameraPlayerState>(255));
    Check(camera.Restore(102, invalid, {1, 0, 0}) == NativeSourceCameraStatus::InvalidInput,
        "unknown player state rejected");
    Check(camera.LastCommitted() == behind, "invalid restore retains publication");
    Check(camera.StartTransition(102, NativeSourceCameraMode::CamOnAString,
        {NativeSourceCameraTargetKind::Vehicle, 22}, {0, 0, 1},
        NativeSourceCameraSwitch::Interpolation, false) == NativeSourceCameraStatus::InvalidInput,
        "vertical transition front rejected");
    Check(camera.StartTransition(102, NativeSourceCameraMode::FollowPed,
        {NativeSourceCameraTargetKind::Vehicle, 22}, {1, 0, 0},
        NativeSourceCameraSwitch::Interpolation, false) == NativeSourceCameraStatus::InvalidInput,
        "mode-target mismatch rejected");
    Check(camera.LastCommitted() == behind, "bad explicit transition retains publication");
    Check(camera.StartTransition(102, NativeSourceCameraMode::FollowPed,
        {NativeSourceCameraTargetKind::Ped, 99}, {1, 0, 0},
        NativeSourceCameraSwitch::Interpolation, false) == NativeSourceCameraStatus::InvalidInput,
        "foreign follow-ped target rejected");

    Check(camera.StartTransition(102, NativeSourceCameraMode::CamOnAString,
        {NativeSourceCameraTargetKind::Vehicle, 22}, {1, 0, 0},
        NativeSourceCameraSwitch::Interpolation, false) ==
        NativeSourceCameraStatus::Ok, "enter-car transition accepted");
    auto entering = camera.LastCommitted();
    Check(entering->Mode == NativeSourceCameraMode::CamOnAString, "enter selects car string mode");
    Check(entering->Target == NativeSourceCameraTarget{NativeSourceCameraTargetKind::Vehicle, 22},
        "enter selects vehicle target");
    Check(entering->Transition.Active && entering->Transition.JustStarted &&
        entering->Transition.UseTransitionBeta, "enter transition active");
    Check(entering->Transition.StartMs == 102 && entering->Transition.DurationMs == 1350 &&
        entering->Transition.TargetDurationMs == 600, "ordinary source transition durations");
    Check(Near(entering->Transition.StopMoving, 0.25f) &&
        Near(entering->Transition.StopCatchUp, 0.75f), "ordinary source transition fractions");
    Check(Near(entering->Transition.TransitionBeta, 415.0f * 3.14159265358979323846f / 180.0f),
        "explicit active-front transition beta");
    Check(entering->Events.size() == 3 && entering->Events.back().Sequence == 3 &&
        entering->Events.back().From == NativeSourceCameraMode::FollowPed &&
        entering->Events.back().To == NativeSourceCameraMode::CamOnAString,
        "enter event order and modes");
    const auto enteringCopy = *entering;
    Check(camera.Restore(103, Player(NativeSourceCameraPlayerState::ExitCar, true), {1, 0, 0}) ==
        NativeSourceCameraStatus::TransitionOutstanding, "overlapping transition explicit");
    Check(*entering == enteringCopy && camera.LastCommitted() == entering,
        "overlap rejection retains immutable transition");
    Check(camera.Advance(103) == NativeSourceCameraStatus::Ok, "transition advances");
    Check(camera.LastCommitted()->Transition.Active && !camera.LastCommitted()->Transition.JustStarted,
        "just-started clears after first advance");
    Check(camera.LastCommitted()->Events.size() == 3, "advance emits no speculative event");
    Check(camera.Advance(1451) == NativeSourceCameraStatus::Ok &&
        camera.LastCommitted()->Transition.Active, "transition active before exact end");
    Check(camera.Advance(1452) == NativeSourceCameraStatus::Ok, "transition completes at exact end");
    auto inCar = camera.LastCommitted();
    Check(!inCar->Transition.Active && inCar->Events.size() == 4 &&
        inCar->Events.back().Kind == NativeSourceCameraEventKind::TransitionComplete &&
        inCar->Events.back().Sequence == 4, "completion journal");
    Check(*entering == enteringCopy, "enter publication remains immutable after completion");

    Check(camera.Restore(1453, Player(NativeSourceCameraPlayerState::InVehicle, true), {1, 0, 0}) ==
        NativeSourceCameraStatus::Ok, "steady in-car owner accepted");
    Check(camera.LastCommitted()->Events.size() == 4 && camera.LastCommitted()->TimeMs == 1453,
        "steady owner advances without event");
    Check(camera.StartTransition(1454, NativeSourceCameraMode::FollowPed,
        {NativeSourceCameraTargetKind::Ped, 11}, {1, 0, 0},
        NativeSourceCameraSwitch::Interpolation, false, 17) ==
        NativeSourceCameraStatus::Ok, "exit transition accepted");
    auto exiting = camera.LastCommitted();
    Check(exiting->Mode == NativeSourceCameraMode::FollowPed &&
        exiting->Target == NativeSourceCameraTarget{NativeSourceCameraTargetKind::Ped, 11},
        "exit selects ped follow owner");
    Check(exiting->Transition.DurationMs == 1350 && exiting->Events.back().Sequence == 5 &&
        exiting->InputSequence == 17 && exiting->Events.back().InputSequence == 17,
        "ordinary exit duration and journal");
    Check(camera.Advance(2804) == NativeSourceCameraStatus::Ok &&
        !camera.LastCommitted()->Transition.Active && camera.Events().back().Sequence == 6,
        "ordinary exit completes");

    Check(camera.StartTransition(2805, NativeSourceCameraMode::CamOnAString,
        {NativeSourceCameraTargetKind::Vehicle, 22}, {1, 0, 0},
        NativeSourceCameraSwitch::JumpCut, false) == NativeSourceCameraStatus::Ok,
        "jump-cut enter accepted");
    Check(!camera.LastCommitted()->Transition.Active && camera.LastCommitted()->Mode ==
        NativeSourceCameraMode::CamOnAString && camera.Events().back().Switch ==
        NativeSourceCameraSwitch::JumpCut, "jump-cut changes owner without interpolation");
    Check(camera.StartTransition(2806, NativeSourceCameraMode::FollowPed,
        {NativeSourceCameraTargetKind::Ped, 11}, {1, 0, 0},
        NativeSourceCameraSwitch::Interpolation, true) ==
        NativeSourceCameraStatus::Ok, "bike exit transition accepted");
    Check(camera.LastCommitted()->Transition.DurationMs == 800 &&
        Near(camera.LastCommitted()->Transition.StopMoving, 0.02f) &&
        Near(camera.LastCommitted()->Transition.StopCatchUp, 0.98f),
        "source bike-exit transition profile");
    Check(camera.Advance(3606) == NativeSourceCameraStatus::Ok &&
        !camera.LastCommitted()->Transition.Active, "bike exit completes at800ms");
    Check(camera.Restore(3607, Player(NativeSourceCameraPlayerState::OnFoot), {1, 0, 0}) ==
        NativeSourceCameraStatus::Ok, "steady on-foot target accepted after vehicle release");
    auto sampled = camera.LastCommitted();
    Check(camera.Restore(3608, Player(NativeSourceCameraPlayerState::OnFoot), {1, 0, 0},
        NativeSourceCameraSwitch::Interpolation, 16) == NativeSourceCameraStatus::InvalidInput,
        "backward input sequence rejected");
    Check(camera.LastCommitted() == sampled, "bad input sequence retains publication");
    Check(camera.Restore(3608, Player(NativeSourceCameraPlayerState::OnFoot), {1, 0, 0},
        NativeSourceCameraSwitch::Interpolation, 18) == NativeSourceCameraStatus::Ok &&
        camera.LastCommitted()->InputSequence == 18, "new sampled-input sequence published");

    auto oldEpoch = camera.LastCommitted(); const auto oldEpochCopy = *oldEpoch;
    Check(camera.Initialize(0, 33, 4000) == NativeSourceCameraStatus::InvalidInput,
        "failed reload retains owner");
    Check(camera.LastCommitted() == oldEpoch, "failed reload retains publication pointer");
    Check(camera.Initialize(8, 33, 10) == NativeSourceCameraStatus::Ok, "new epoch initialized");
    auto reloaded = camera.LastCommitted();
    Check(reloaded->Epoch == 8 && reloaded->Generation == 1 && reloaded->TimeMs == 10,
        "new epoch resets generation and time");
    Check(reloaded->Target == NativeSourceCameraTarget{NativeSourceCameraTargetKind::Ped, 33} &&
        reloaded->Events.size() == 1 && reloaded->Events[0].Sequence == 1,
        "new epoch resets target and journal");
    Check(*oldEpoch == oldEpochCopy, "held old-epoch publication remains immutable");
    Check(camera.Events().data() != reloaded->Events.data(), "consumer journal is value-owned");
    Check(camera.ResolveView() == NativeSourceCameraViewStatus::Unsupported,
        "transition success does not fake source camera eye");

    std::cout << "source-camera-transition-v1\n";
    for (const auto& event : oldEpochCopy.Events) {
        std::cout << "seq=" << event.Sequence << " kind=" << unsigned(event.Kind)
                  << " from=" << unsigned(event.From) << " to=" << unsigned(event.To)
                  << " target_kind=" << unsigned(event.Target.Kind) << " target=" << event.Target.Identity
                  << " switch=" << unsigned(event.Switch) << " time=" << event.TimeMs
                  << " duration=" << event.DurationMs << " target_duration=" << event.TargetDurationMs
                  << " stop=" << std::hex << std::setw(8) << std::setfill('0')
                  << std::bit_cast<std::uint32_t>(event.StopMoving)
                  << " catch=" << std::setw(8) << std::bit_cast<std::uint32_t>(event.StopCatchUp)
                  << " beta=" << std::setw(8) << std::bit_cast<std::uint32_t>(event.TransitionBeta)
                  << std::dec << " input=" << event.InputSequence << '\n';
    }
    std::cout << "sa-core-camera-ok checks=" << g_Checks
              << " modes=4,18 ordinary_ms=1350 bike_exit_ms=800 pointer_feedback=0\n";
}
