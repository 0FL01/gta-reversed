#include "NativeSourceCamera.h"

#include <cmath>
#include <limits>

namespace {
using Status = NativeSourceCameraStatus;
constexpr float Pi = 3.14159265358979323846f;
bool Finite(const std::array<float, 3>& value) {
    return std::isfinite(value[0]) && std::isfinite(value[1]) && std::isfinite(value[2]);
}
float SourceAtan(float x, float y) {
    if (x == 0 && y == 0) return 0;
    const float ax = std::abs(x), ay = std::abs(y);
    if (ax < ay) {
        if (y > 0) return x > 0 ? Pi * 0.5f - std::atan2(x / y, 1.0f) : Pi * 0.5f + std::atan2(-x / y, 1.0f);
        return x > 0 ? Pi * 1.5f + std::atan2(x / -y, 1.0f) : Pi * 1.5f - std::atan2(-x / -y, 1.0f);
    }
    if (y > 0) return x > 0 ? std::atan2(y / x, 1.0f) : Pi - std::atan2(y / -x, 1.0f);
    return x > 0 ? Pi * 2.0f - std::atan2(-y / x, 1.0f) : Pi + std::atan2(-y / -x, 1.0f);
}
NativeSourceCameraTarget DesiredTarget(const NativeSourceCameraPlayer& player) {
    if (player.VehiclePresent) return {NativeSourceCameraTargetKind::Vehicle, player.VehicleIdentity};
    return {NativeSourceCameraTargetKind::Ped, player.PedIdentity};
}
NativeSourceCameraMode DesiredMode(const NativeSourceCameraPlayer& player) {
    switch (player.State) {
    case NativeSourceCameraPlayerState::EnterCar:
    case NativeSourceCameraPlayerState::Carjack:
    case NativeSourceCameraPlayerState::OpenDoor:
    case NativeSourceCameraPlayerState::InVehicle:
        return NativeSourceCameraMode::CamOnAString;
    case NativeSourceCameraPlayerState::OnFoot:
    case NativeSourceCameraPlayerState::ExitCar:
    case NativeSourceCameraPlayerState::DraggedFromCar:
        return NativeSourceCameraMode::FollowPed;
    }
    return NativeSourceCameraMode::FollowPed;
}
bool ValidState(NativeSourceCameraPlayerState state) {
    switch (state) {
    case NativeSourceCameraPlayerState::OnFoot:
    case NativeSourceCameraPlayerState::EnterCar:
    case NativeSourceCameraPlayerState::Carjack:
    case NativeSourceCameraPlayerState::OpenDoor:
    case NativeSourceCameraPlayerState::InVehicle:
    case NativeSourceCameraPlayerState::ExitCar:
    case NativeSourceCameraPlayerState::DraggedFromCar:
        return true;
    }
    return false;
}
}

NativeSourceCameraStatus NativeSourceCamera::Publish(NativeSourceCameraSnapshot&& snapshot,
    std::vector<NativeSourceCameraEvent>&& events) {
    try {
        snapshot.Events = events;
        auto publication = std::make_shared<const NativeSourceCameraSnapshot>(std::move(snapshot));
        m_Events = std::move(events);
        m_Published = std::move(publication);
        return Status::Ok;
    } catch (...) {
        return Status::Overflow;
    }
}

NativeSourceCameraStatus NativeSourceCamera::Initialize(std::uint64_t epoch, std::uint64_t ped,
    std::uint32_t nowMs) {
    if (!epoch || !ped) return Status::InvalidInput;
    NativeSourceCameraSnapshot snapshot;
    snapshot.Epoch = epoch; snapshot.Generation = 1; snapshot.TimeMs = nowMs;
    snapshot.Mode = NativeSourceCameraMode::FollowPed;
    snapshot.Target = {NativeSourceCameraTargetKind::Ped, ped};
    std::vector<NativeSourceCameraEvent> events;
    try { events.push_back({1, NativeSourceCameraEventKind::Spawn, snapshot.Mode, snapshot.Mode,
        snapshot.Target, NativeSourceCameraSwitch::JumpCut, nowMs}); }
    catch (...) { return Status::Overflow; }
    const auto status = Publish(std::move(snapshot), std::move(events));
    if (status == Status::Ok) { m_NextSequence = 2; m_PedIdentity = ped; }
    return status;
}

NativeSourceCameraStatus NativeSourceCamera::SetDirectlyBehind(std::uint32_t nowMs, std::array<float, 3> forward) {
    if (!m_Published) return Status::NotLoaded;
    if (nowMs < m_Published->TimeMs) return Status::BackwardTime;
    if (!Finite(forward) || (forward[0] == 0 && forward[1] == 0)) return Status::InvalidInput;
    auto snapshot = *m_Published; auto events = m_Events;
    if (m_NextSequence == std::numeric_limits<std::uint64_t>::max()) return Status::Overflow;
    snapshot.Generation++; snapshot.TimeMs = nowMs; snapshot.DirectlyBehind = true;
    snapshot.PedOrientationForBehindOrInFront = SourceAtan(forward[0], forward[1]);
    try { events.push_back({m_NextSequence, NativeSourceCameraEventKind::DirectBehind, snapshot.Mode, snapshot.Mode,
        snapshot.Target, NativeSourceCameraSwitch::JumpCut, nowMs}); }
    catch (...) { return Status::Overflow; }
    const auto status = Publish(std::move(snapshot), std::move(events));
    if (status == Status::Ok) ++m_NextSequence;
    return status;
}

NativeSourceCameraStatus NativeSourceCamera::Restore(std::uint32_t nowMs, const NativeSourceCameraPlayer& player,
    std::array<float, 3> activeSourceFront, NativeSourceCameraSwitch switchType,
    std::uint64_t inputSequence) {
    if (!m_Published) return Status::NotLoaded;
    if (nowMs < m_Published->TimeMs) return Status::BackwardTime;
    if (!ValidState(player.State) || player.PedIdentity != m_PedIdentity ||
        (player.VehiclePresent && !player.VehicleIdentity) ||
        (!player.VehiclePresent && player.VehicleIdentity))
        return Status::InvalidInput;
    if (player.State == NativeSourceCameraPlayerState::InVehicle && !player.VehiclePresent) return Status::InvalidInput;
    if ((player.State == NativeSourceCameraPlayerState::ExitCar || player.State == NativeSourceCameraPlayerState::DraggedFromCar) &&
        !player.VehiclePresent) {
        // Source permits the stale vehicle pointer to be gone, but the target is explicitly the ped.
    }
    if (switchType != NativeSourceCameraSwitch::Interpolation && switchType != NativeSourceCameraSwitch::JumpCut)
        return Status::InvalidInput;
    const auto mode = DesiredMode(player);
    auto target = DesiredTarget(player);
    if (player.State == NativeSourceCameraPlayerState::ExitCar || player.State == NativeSourceCameraPlayerState::DraggedFromCar)
        target = {NativeSourceCameraTargetKind::Ped, player.PedIdentity};
    if (inputSequence && inputSequence < m_Published->InputSequence) return Status::InvalidInput;
    if (mode == m_Published->Mode && target == m_Published->Target) {
        return Advance(nowMs, inputSequence);
    }
    if (m_Published->Transition.Active) return Status::TransitionOutstanding;
    return StartTransition(nowMs, mode, target, activeSourceFront, switchType,
        player.PlayerWasOnBike, inputSequence);
}

NativeSourceCameraStatus NativeSourceCamera::StartTransition(std::uint32_t nowMs,
    NativeSourceCameraMode mode, NativeSourceCameraTarget target, std::array<float, 3> front,
    NativeSourceCameraSwitch switchType, bool playerWasOnBike, std::uint64_t inputSequence) {
    if (!m_Published) return Status::NotLoaded;
    if (nowMs < m_Published->TimeMs) return Status::BackwardTime;
    if ((mode != NativeSourceCameraMode::FollowPed && mode != NativeSourceCameraMode::CamOnAString) ||
        !target.Identity || target.Kind == NativeSourceCameraTargetKind::None ||
        (mode == NativeSourceCameraMode::FollowPed && target.Kind != NativeSourceCameraTargetKind::Ped) ||
        (mode == NativeSourceCameraMode::FollowPed && target.Identity != m_PedIdentity) ||
        (mode == NativeSourceCameraMode::CamOnAString && target.Kind != NativeSourceCameraTargetKind::Vehicle) ||
        !Finite(front) || (front[0] == 0 && front[1] == 0))
        return Status::InvalidInput;
    if (switchType != NativeSourceCameraSwitch::Interpolation && switchType != NativeSourceCameraSwitch::JumpCut)
        return Status::InvalidInput;
    if (inputSequence && inputSequence < m_Published->InputSequence) return Status::InvalidInput;
    if (m_Published->Transition.Active) return Status::TransitionOutstanding;
    if (m_NextSequence == std::numeric_limits<std::uint64_t>::max()) return Status::Overflow;
    NativeSourceCameraSnapshot snapshot;
    std::vector<NativeSourceCameraEvent> events;
    try { snapshot = *m_Published; events = m_Events; }
    catch (...) { return Status::Overflow; }
    const auto from = snapshot.Mode;
    snapshot.Generation++; snapshot.TimeMs = nowMs; snapshot.Mode = mode; snapshot.Target = target;
    if (inputSequence) snapshot.InputSequence = inputSequence;
    snapshot.LookingAtPlayer = true; snapshot.LookingAtVector = false;
    NativeSourceCameraTransition transition;
    transition.StartMs = nowMs;
    if (switchType == NativeSourceCameraSwitch::Interpolation) {
        transition.Active = transition.JustStarted = true;
        transition.UseTransitionBeta = true;
        transition.TargetDurationMs = 600;
        const float angle = SourceAtan(front[0], front[1]);
        transition.TransitionBeta = angle + (std::abs(angle) <= Pi * 0.5f ? Pi * (235.0f / 180.0f) : Pi * (55.0f / 180.0f));
        if (playerWasOnBike && mode == NativeSourceCameraMode::FollowPed &&
            from == NativeSourceCameraMode::CamOnAString) {
            transition.DurationMs = 800; transition.StopMoving = 0.02f; transition.StopCatchUp = 0.98f;
        } else {
            transition.DurationMs = 1350; transition.StopMoving = 0.25f; transition.StopCatchUp = 0.75f;
        }
    }
    snapshot.Transition = transition;
    try { events.push_back({m_NextSequence, NativeSourceCameraEventKind::Transition, from, mode, target, switchType,
        nowMs, transition.DurationMs, transition.TargetDurationMs, transition.StopMoving,
        transition.StopCatchUp, transition.TransitionBeta, inputSequence}); }
    catch (...) { return Status::Overflow; }
    const auto status = Publish(std::move(snapshot), std::move(events));
    if (status == Status::Ok) ++m_NextSequence;
    return status;
}

NativeSourceCameraStatus NativeSourceCamera::Advance(std::uint32_t nowMs) {
    return Advance(nowMs, 0);
}

NativeSourceCameraStatus NativeSourceCamera::Advance(std::uint32_t nowMs, std::uint64_t inputSequence) {
    if (!m_Published) return Status::NotLoaded;
    if (nowMs < m_Published->TimeMs) return Status::BackwardTime;
    if (inputSequence && inputSequence < m_Published->InputSequence) return Status::InvalidInput;
    NativeSourceCameraSnapshot snapshot;
    std::vector<NativeSourceCameraEvent> events;
    try { snapshot = *m_Published; events = m_Events; }
    catch (...) { return Status::Overflow; }
    bool completed = false;
    if (snapshot.Transition.Active && nowMs - snapshot.Transition.StartMs >= snapshot.Transition.DurationMs) {
        if (m_NextSequence == std::numeric_limits<std::uint64_t>::max()) return Status::Overflow;
        snapshot.Transition.Active = snapshot.Transition.JustStarted = false;
        try { events.push_back({m_NextSequence, NativeSourceCameraEventKind::TransitionComplete,
            snapshot.Mode, snapshot.Mode, snapshot.Target, NativeSourceCameraSwitch::Interpolation, nowMs}); }
        catch (...) { return Status::Overflow; }
        completed = true;
    }
    snapshot.Generation++; snapshot.TimeMs = nowMs;
    snapshot.Transition.JustStarted = false;
    if (inputSequence) snapshot.InputSequence = inputSequence;
    const auto status = Publish(std::move(snapshot), std::move(events));
    if (status == Status::Ok && completed) ++m_NextSequence;
    return status;
}
