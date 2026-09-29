#include "NativeAmbientPedPolicy.h"

#include <algorithm>
#include <cmath>

namespace {
static bool Finite(const std::array<float, 3>& value) {
    return std::ranges::all_of(value, [](float component) { return std::isfinite(component); });
}
}

NativeAmbientPedStatus NativeAmbientManagePed(NativeAmbientPedState& state,
    const NativeAmbientPedInput& input, NativeAmbientPedDecision& out) {
    NativeAmbientPedDecision decision;
    // Source early returns do not observe the distance/camera/timer payload.
    if (state.Player || !state.Deletable || state.InVehicle || state.AttachedToVehicle) {
        out = decision;
        return NativeAmbientPedStatus::Ok;
    }

    auto next = state;
    if (next.Dead) {
        const std::uint32_t elapsed = input.GameMs - next.DeathTimeMs;
        if (elapsed > 30'000u || (input.Frenzy && elapsed > 15'000u) ||
            (input.GangWarFighting && elapsed > 8'000u)) {
            next.FadeOut = true;
        }
    }
    if (next.FadeOut && next.ClumpAlpha == 0) {
        decision.Action = NativeAmbientPedAction::Remove;
        state = next;
        out = decision;
        return NativeAmbientPedStatus::Ok;
    }

    if (next.PedType >= 32 || !Finite(next.Position) || !Finite(input.PlayerPosition) ||
        !std::isfinite(next.RemovalDistanceMultiplier) || next.RemovalDistanceMultiplier < 0.0f ||
        !std::isfinite(input.CreationDistanceMultiplier) || input.CreationDistanceMultiplier < 1.0f ||
        input.CreationDistanceMultiplier > 1.5f ||
        !std::isfinite(input.CameraGenerationMultiplier) || input.CameraGenerationMultiplier < 0.0f) {
        return NativeAmbientPedStatus::InvalidInput;
    }

    // Source tests gangs before the dead-in-front-of-car shortcut.
    const bool gang = next.PedType >= 7 && next.PedType <= 16;
    float distance{};
    if (gang || !next.DeadInFrontOfCar || !next.HasDeadInFrontVehicle) {
        const float x = next.Position[0] - input.PlayerPosition[0];
        const float y = next.Position[1] - input.PlayerPosition[1];
        const float squared = x * x + y * y;
        distance = std::sqrt(squared) * next.RemovalDistanceMultiplier;
        if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(squared) ||
            !std::isfinite(distance)) {
            return NativeAmbientPedStatus::Overflow;
        }
        if (gang) distance -= 30.0f;
    }
    distance /= input.CreationDistanceMultiplier;
    const float threshold = input.CameraGenerationMultiplier * (next.CullExtraFar ? 65.0f : 54.5f);
    if (!std::isfinite(distance) || !std::isfinite(threshold)) {
        return NativeAmbientPedStatus::Overflow;
    }
    decision.RemovalDistance = distance;
    decision.DistanceEvaluated = true;
    if (threshold <= distance) {
        if (input.OnScreen) next.FadeOut = true;
        else decision.Action = NativeAmbientPedAction::Remove;
    } else if (distance <= 25.0f || input.OnScreen) {
        next.NeededUntilMs = input.GameMs + (next.PedType == 6 ? 10'000u : 4'000u);
        decision.DeadlineRefreshed = true;
    } else if (input.GameMs > next.NeededUntilMs) {
        // eCamMode: sniper7, sniper-runabout39, camera46 retain the ped.
        const bool protectedCamera = input.CameraMode == 7 || input.CameraMode == 39 || input.CameraMode == 46;
        if (!protectedCamera && !input.LookingLeft && !input.LookingRight && !input.LookingBehind) {
            decision.Action = NativeAmbientPedAction::Remove;
        }
    }
    state = next;
    out = decision;
    return NativeAmbientPedStatus::Ok;
}
