#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

enum class NativeSourceCameraMode : std::uint8_t { FollowPed = 4, CamOnAString = 18 };
enum class NativeSourceCameraTargetKind : std::uint8_t { None, Ped, Vehicle };
enum class NativeSourceCameraSwitch : std::uint8_t { Interpolation = 1, JumpCut = 2 };
enum class NativeSourceCameraPlayerState : std::uint8_t { OnFoot, EnterCar, Carjack, OpenDoor, InVehicle, ExitCar, DraggedFromCar };
enum class NativeSourceCameraStatus : std::uint8_t { Ok, NotLoaded, InvalidInput, BackwardTime, TransitionOutstanding, Overflow };
enum class NativeSourceCameraEventKind : std::uint8_t { Spawn, DirectBehind, Transition, TransitionComplete };
enum class NativeSourceCameraViewStatus : std::uint8_t { Unsupported };

struct NativeSourceCameraTarget {
    NativeSourceCameraTargetKind Kind{};
    std::uint64_t Identity{};
    bool operator==(const NativeSourceCameraTarget&) const = default;
};
struct NativeSourceCameraTransition {
    bool Active{}, JustStarted{}, UseTransitionBeta{};
    std::uint32_t StartMs{}, DurationMs{}, TargetDurationMs{};
    float StopMoving = 0.25f, StopCatchUp = 0.75f;
    float TargetStopMoving{}, TargetStopCatchUp = 1;
    float TransitionBeta{};
    bool operator==(const NativeSourceCameraTransition&) const = default;
};
struct NativeSourceCameraEvent {
    std::uint64_t Sequence{};
    NativeSourceCameraEventKind Kind{};
    NativeSourceCameraMode From = NativeSourceCameraMode::FollowPed, To = NativeSourceCameraMode::FollowPed;
    NativeSourceCameraTarget Target;
    NativeSourceCameraSwitch Switch = NativeSourceCameraSwitch::Interpolation;
    std::uint32_t TimeMs{}, DurationMs{}, TargetDurationMs{};
    float StopMoving{}, StopCatchUp{}, TransitionBeta{};
    bool operator==(const NativeSourceCameraEvent&) const = default;
};
struct NativeSourceCameraSnapshot {
    std::uint64_t Epoch{}, Generation{};
    std::uint32_t TimeMs{};
    NativeSourceCameraMode Mode = NativeSourceCameraMode::FollowPed;
    NativeSourceCameraTarget Target;
    NativeSourceCameraTransition Transition;
    std::uint64_t InputSequence{};
    bool LookingAtPlayer = true, LookingAtVector{}, DirectlyBehind{}, DirectlyInFront{};
    float PedOrientationForBehindOrInFront{};
    std::vector<NativeSourceCameraEvent> Events;
    bool operator==(const NativeSourceCameraSnapshot&) const = default;
};
struct NativeSourceCameraPlayer {
    NativeSourceCameraPlayerState State = NativeSourceCameraPlayerState::OnFoot;
    std::uint64_t PedIdentity{}, VehicleIdentity{};
    bool VehiclePresent{}, PlayerWasOnBike{};
};

// Pure source camera mode/target/transition owner. It ports CCamera::Restore,
// SetCameraDirectlyBehind... and the FollowPed<->CamOnAString portion of
// StartTransition. It intentionally does NOT replace the still-unreversed
// CCam FollowPed/FollowCar eye solver or camera collision with a Godot orbit.
// Consumers get immutable transition snapshots; no presentation pose is fed in.
class NativeSourceCamera {
public:
    NativeSourceCameraStatus Initialize(std::uint64_t epoch, std::uint64_t pedIdentity,
        std::uint32_t nowMs);
    NativeSourceCameraStatus SetDirectlyBehind(std::uint32_t nowMs, std::array<float, 3> pedForward);
    NativeSourceCameraStatus Restore(std::uint32_t nowMs, const NativeSourceCameraPlayer&,
        NativeSourceCameraSwitch = NativeSourceCameraSwitch::Interpolation,
        std::uint64_t inputSequence = 0);
    NativeSourceCameraStatus StartTransition(std::uint32_t nowMs, NativeSourceCameraMode,
        NativeSourceCameraTarget, std::array<float, 3> activeFront,
        NativeSourceCameraSwitch, bool playerWasOnBike, std::uint64_t inputSequence = 0);
    NativeSourceCameraStatus Advance(std::uint32_t nowMs);
    std::shared_ptr<const NativeSourceCameraSnapshot> LastCommitted() const noexcept { return m_Published; }
    std::span<const NativeSourceCameraEvent> Events() const noexcept { return m_Events; }
    // Until the source CCam FollowPed/FollowCar bodies and their world query are
    // owned, presentation must not manufacture an eye from this transition owner.
    NativeSourceCameraViewStatus ResolveView() const noexcept { return NativeSourceCameraViewStatus::Unsupported; }

private:
    NativeSourceCameraStatus Publish(NativeSourceCameraSnapshot&&, std::vector<NativeSourceCameraEvent>&&);
    std::shared_ptr<const NativeSourceCameraSnapshot> m_Published;
    std::vector<NativeSourceCameraEvent> m_Events;
    std::uint64_t m_NextSequence = 1;
    std::uint64_t m_PedIdentity{};
};
