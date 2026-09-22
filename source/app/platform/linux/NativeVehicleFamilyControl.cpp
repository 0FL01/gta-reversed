#include "NativeVehicleFamilyControl.h"

#include <algorithm>
#include <cmath>

namespace {
constexpr float kDegreesToRadians = 0.01745329251994329577f;

bool Finite(const NativeVehicleFamilyControlState& state, const NativeVehicleFamilyControlInput& input) {
    return std::isfinite(state.RawSteer) && std::isfinite(state.SteerRadians) &&
        std::isfinite(state.Gas) && std::isfinite(state.Brake) &&
        std::isfinite(state.TrainSpeed) && std::isfinite(state.RailDistance) &&
        std::isfinite(state.TrailerExtension) && std::isfinite(state.TrailerMinimumRatio) &&
        std::isfinite(input.TimeStep) && std::isfinite(input.SteeringLockDegrees) &&
        std::isfinite(input.ForwardSpeed) && std::isfinite(input.PreviousTrainSpeed) &&
        std::isfinite(input.PreviousRailDistance) && std::isfinite(input.TrainLength) &&
        std::isfinite(input.MoveSpeedSquared);
}

bool RoadFamily(NativeVehicleType family) {
    return family == NativeVehicleType::Automobile || family == NativeVehicleType::MonsterTruck ||
        family == NativeVehicleType::Quad;
}

void ApplyRoad(NativeVehicleFamilyControlState& next, const NativeVehicleFamilyControlInput& input) {
    next.RawSteer += (-float(input.Steering) / 128.0f - next.RawSteer) / 5.0f * input.TimeStep;
    next.RawSteer = std::clamp(next.RawSteer, -1.0f, 1.0f);
    const float signedSquare = std::copysign(next.RawSteer * next.RawSteer, next.RawSteer);
    next.SteerRadians = input.SteeringLockDegrees * kDegreesToRadians * signedSquare;
    next.Handbrake = input.AutomaticHandbrake || input.Handbrake;
    if (input.AutomaticHandbrake) {
        next.Gas = 0.0f;
        next.Brake = 1.0f;
        return;
    }
    const float gas = float(std::int32_t(input.Accelerate) - std::int32_t(input.Brake)) / 255.0f;
    if (std::abs(input.ForwardSpeed) < 0.01f) {
        next.Gas = gas;
        next.Brake = 0.0f;
    } else if (input.ForwardSpeed >= 0.0f && gas < 0.0f) {
        next.Gas = 0.0f;
        next.Brake = std::abs(gas);
    } else if (input.ForwardSpeed < 0.0f && gas >= 0.0f &&
        (next.Gas <= 0.5f || input.ForwardSpeed <= -0.15f)) {
        next.Gas = 0.0f;
        next.Brake = gas;
    } else {
        next.Gas = gas;
        next.Brake = 0.0f;
    }
}
}

NativeVehicleFamilyControlStatus NativeVehicleApplyFamilyControl(
    NativeVehicleFamilyControlState& state,
    const NativeVehicleFamilyControlInput& input) noexcept {
    if (!Finite(state, input) || input.TimeStep < 0.0f || input.Steering < -128 || input.Steering > 128 ||
        input.SteeringLockDegrees < 0.0f || input.MoveSpeedSquared < 0.0f) {
        return NativeVehicleFamilyControlStatus::InvalidInput;
    }
    auto next = state;
    switch (state.Dependency) {
    case NativeVehicleFamilyDependency::Road:
        if (!RoadFamily(state.Family)) return NativeVehicleFamilyControlStatus::Unsupported;
        ApplyRoad(next, input);
        next.AutomobileDelegated = true;
        next.CompleteFamilyControl = state.Family == NativeVehicleType::Automobile;
        break;
    case NativeVehicleFamilyDependency::Water: {
        if (state.Family != NativeVehicleType::Boat) return NativeVehicleFamilyControlStatus::Unsupported;
        next.Brake = std::clamp(std::lerp(next.Brake, float(input.Brake) / 255.0f, 0.1f), 0.0f, 1.0f);
        if (next.Brake >= 0.05f) next.Gas = next.Brake * -0.3f;
        else { next.Brake = 0.0f; next.Gas = float(input.Accelerate) / 255.0f; }
        next.RawSteer += (-float(input.Steering) / 128.0f - next.RawSteer) * 0.2f * input.TimeStep;
        next.RawSteer = std::clamp(next.RawSteer, -1.0f, 1.0f);
        next.SteerRadians = input.SteeringLockDegrees * kDegreesToRadians *
            std::copysign(next.RawSteer * next.RawSteer, next.RawSteer);
        next.CompleteFamilyControl = true;
        break;
    }
    case NativeVehicleFamilyDependency::Rail:
        if (state.Family != NativeVehicleType::Train) return NativeVehicleFamilyControlStatus::Unsupported;
        if (input.HasPreviousCarriage) {
            next.TrainSpeed = input.PreviousTrainSpeed;
            next.RailDistance = input.PreviousRailDistance + input.TrainLength;
        } else {
            next.TrainSpeed *= std::pow(0.9900000095367432f, input.TimeStep);
            next.RailDistance += next.TrainSpeed * input.TimeStep;
        }
        next.CompleteFamilyControl = false;
        break;
    case NativeVehicleFamilyDependency::Flight:
        if (state.Family != NativeVehicleType::Helicopter && state.Family != NativeVehicleType::Plane)
            return NativeVehicleFamilyControlStatus::Unsupported;
        // Reversed common CAutomobile abandoned-status dependency used by both
        // source flight subtypes. Their family-specific control remains address-backed.
        if (input.RestingOnPhysical) next.Brake = 0.5f;
        else if (input.MoveSpeedSquared < 0.01f) next.Brake = 0.2f;
        else next.Brake = 0.0f;
        next.Gas = 0.0f;
        next.SteerRadians = 0.0f;
        next.Handbrake = false;
        next.CompleteFamilyControl = false;
        break;
    case NativeVehicleFamilyDependency::Towing: {
        if (state.Family != NativeVehicleType::Trailer) return NativeVehicleFamilyControlStatus::Unsupported;
        if (next.TrailerExtension < 0.0f || next.TrailerExtension > 1.0f)
            return NativeVehicleFamilyControlStatus::InvalidInput;
        if (input.TowingVehiclePresent && next.TrailerExtension > 0.0f)
            next.TrailerExtension = std::max(next.TrailerExtension - input.TimeStep * 0.002f, 0.0f);
        else if (!input.TowingVehiclePresent && next.TrailerExtension < 1.0f) {
            const float rate = next.TrailerExtension <= 0.1f && input.TrailerWaitRate ? 0.0005f : 0.002f;
            next.TrailerExtension = std::min(next.TrailerExtension + input.TimeStep * rate, 1.0f);
        }
        next.TrailerMinimumRatio = next.TrailerExtension;
        next.CompleteFamilyControl = false;
        break;
    }
    case NativeVehicleFamilyDependency::Special:
        if (state.Family != NativeVehicleType::Quad) return NativeVehicleFamilyControlStatus::Unsupported;
        ApplyRoad(next, input);
        next.AutomobileDelegated = true;
        next.CompleteFamilyControl = false;
        break;
    }
    if (!Finite(next, input)) return NativeVehicleFamilyControlStatus::Overflow;
    state = next;
    return NativeVehicleFamilyControlStatus::Ok;
}
