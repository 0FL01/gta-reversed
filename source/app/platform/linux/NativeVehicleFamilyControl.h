#pragma once

#include "NativeVehiclePool.h"

#include <cstdint>

enum class NativeVehicleFamilyDependency : std::uint8_t {
    Road,
    Water,
    Rail,
    Flight,
    Towing,
    Special,
};

enum class NativeVehicleFamilyControlStatus : std::uint8_t {
    Ok,
    InvalidInput,
    Unsupported,
    Overflow,
};

struct NativeVehicleFamilyControlInput {
    float TimeStep = 0.0f;
    std::int32_t Steering = 0;
    std::uint8_t Accelerate = 0;
    std::uint8_t Brake = 0;
    float SteeringLockDegrees = 0.0f;
    float ForwardSpeed = 0.0f;
    bool AutomaticHandbrake = false;
    bool Handbrake = false;

    bool HasPreviousCarriage = false;
    float PreviousTrainSpeed = 0.0f;
    float PreviousRailDistance = 0.0f;
    float TrainLength = 0.0f;

    bool TowingVehiclePresent = false;
    bool TrailerWaitRate = false;
    bool RestingOnPhysical = false;
    float MoveSpeedSquared = 0.0f;
};

struct NativeVehicleFamilyControlState {
    NativeVehicleType Family = NativeVehicleType::Unsupported;
    NativeVehicleFamilyDependency Dependency = NativeVehicleFamilyDependency::Road;
    float RawSteer = 0.0f;
    float SteerRadians = 0.0f;
    float Gas = 0.0f;
    float Brake = 0.0f;
    bool Handbrake = false;

    float TrainSpeed = 0.0f;
    float RailDistance = 0.0f;
    float TrailerExtension = 1.0f;
    float TrailerMinimumRatio = 1.0f;
    bool AutomobileDelegated = false;
    bool CompleteFamilyControl = false;
    bool operator==(const NativeVehicleFamilyControlState&) const = default;
};

// Source-backed family dependencies only. Heli/plane/bike complete input and
// physics bodies remain address-backed and are deliberately not claimed here.
NativeVehicleFamilyControlStatus NativeVehicleApplyFamilyControl(
    NativeVehicleFamilyControlState& state,
    const NativeVehicleFamilyControlInput& input) noexcept;
