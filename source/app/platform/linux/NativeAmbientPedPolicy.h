#pragma once

#include <array>
#include <cstdint>

enum class NativeAmbientPedStatus { Ok, InvalidInput, Overflow };
enum class NativeAmbientPedAction { Keep, Remove };

struct NativeAmbientPedState {
    std::array<float, 3> Position{};
    std::uint32_t PedType = 4;
    std::uint32_t DeathTimeMs{};
    std::uint32_t NeededUntilMs{};
    float RemovalDistanceMultiplier = 1.0f;
    std::uint8_t ClumpAlpha = 255;
    bool Player{};
    bool Deletable = true;
    bool InVehicle{};
    bool AttachedToVehicle{};
    bool Dead{};
    bool FadeOut{};
    bool CullExtraFar{};
    bool DeadInFrontOfCar{};
    bool HasDeadInFrontVehicle{};
    bool operator==(const NativeAmbientPedState&) const = default;
};

struct NativeAmbientPedInput {
    std::array<float, 3> PlayerPosition{};
    std::uint32_t GameMs{};
    float CreationDistanceMultiplier = 1.0f;
    float CameraGenerationMultiplier = 1.0f;
    std::int32_t CameraMode = 4;
    bool OnScreen{};
    bool Frenzy{};
    bool GangWarFighting{};
    bool LookingLeft{};
    bool LookingRight{};
    bool LookingBehind{};
};

struct NativeAmbientPedDecision {
    NativeAmbientPedAction Action = NativeAmbientPedAction::Keep;
    float RemovalDistance{};
    bool DistanceEvaluated{};
    bool DeadlineRefreshed{};
    bool operator==(const NativeAmbientPedDecision&) const = default;
};

// CPopulation::ManagePed's source value policy. It requests removal; the actual
// owner must perform CWorld::Remove and release the same ped generation. Visibility,
// attachment, CanBeDeleted and camera values must come from their existing owners.
// This neither generates ambient peds nor certifies a complete dynamic census.
// Non-Ok leaves both state and decision unchanged; valid source uint32 timer wrap
// is preserved rather than replaced by an invented monotonic/wrap-aware deadline.
NativeAmbientPedStatus NativeAmbientManagePed(NativeAmbientPedState&,
    const NativeAmbientPedInput&, NativeAmbientPedDecision&);
