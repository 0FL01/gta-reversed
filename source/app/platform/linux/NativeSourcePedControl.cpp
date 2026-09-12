#include "NativeSourcePedControl.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace {
constexpr float Pi = std::numbers::pi_v<float>;
bool Valid(const NativeSourceAnimAssociation& s) {
    return s.Alive && std::isfinite(s.TotalTime) && s.TotalTime > 0 &&
        std::isfinite(s.CurrentTime) && std::isfinite(s.TimeStep) && std::isfinite(s.Speed) &&
        std::isfinite(s.BlendAmount) && std::isfinite(s.BlendDelta);
}
float RadianAngle(float x, float y) {
    // CGeneral::GetRadianAngleBetweenPoints(0,0,x,y), NOT std::atan2(y,x).
    if (y == 0) y = 0.0001f;
    if (x > 0) return y > 0 ? Pi - std::atan2(x / y, 1.0f) : -std::atan2(x / y, 1.0f);
    return y > 0 ? -(Pi + std::atan2(x / y, 1.0f)) : -std::atan2(x / y, 1.0f);
}
float LimitAngle(float angle) {
    float result = std::clamp(angle, -25.0f, 25.0f);
    while (result >= Pi) result -= 2 * Pi;
    while (result < -Pi) result += 2 * Pi;
    return result;
}
} // namespace

NativePedControlStatus NativeSourceUpdateMoveBlend(const NativeSourceMoveSample& s, NativeSourceMoveBlend& state) {
    if (s.LeftRight < -128 || s.LeftRight > 128 || s.UpDown < -128 || s.UpDown > 128 ||
        !std::isfinite(s.CameraOrientation) || !std::isfinite(s.TimeStep) || s.TimeStep < 0 ||
        !std::isfinite(state.Ratio) || state.Ratio < 0 || !std::isfinite(state.AimingRotation))
        return NativePedControlStatus::InvalidInput;
    const float x = float(s.LeftRight), y = float(s.UpDown);
    // Retail Zelda spills sqrt to float before dividing by double 60.0.
    float target = float(double(std::sqrt(y * y + x * x)) / 60.0);
    if (s.Attached) target = 0;
    if (s.WalkModifier && target > 1) target = 1;
    else if (target <= 0) { state.Ratio = 0; return NativePedControlStatus::Ok; }
    auto next = state;
    next.AimingRotation = LimitAngle(RadianAngle(-x, y) - s.CameraOrientation);
    if (!s.DirectionAllowed) next.Ratio = 0;
    else {
        const float step = s.TimeStep * 0.07f;
        if (!std::isfinite(step)) return NativePedControlStatus::InvalidInput;
        if (target - next.Ratio <= step) {
            if (-step <= target - next.Ratio) next.Ratio = target;
            else next.Ratio -= step;
        } else next.Ratio = step + next.Ratio;
    }
    state = next;
    return NativePedControlStatus::Ok;
}

NativePedControlStatus NativeSourceSelectWalkRun(float ratio, NativeSourceWalkRunWeights& out) {
    if (!std::isfinite(ratio) || ratio <= 0) return NativePedControlStatus::InvalidInput;
    if (ratio < 1) out = {NativeSourceMoveState::Walk, 1, 0};
    else if (ratio < 2) out = {NativeSourceMoveState::Run, 2 - ratio, ratio - 1};
    else out = {NativeSourceMoveState::Run, 0, 1};
    return NativePedControlStatus::Ok;
}

NativePedControlStatus NativeSourceAnimUpdateStep(NativeSourceAnimAssociation& state, float seconds, float reciprocal) {
    if (!Valid(state) || !std::isfinite(seconds) || seconds < 0 || !std::isfinite(reciprocal))
        return NativePedControlStatus::InvalidInput;
    if (state.Playing) {
        float step = state.Synchronised ? state.TotalTime * reciprocal : state.Speed;
        step *= seconds;
        if (!std::isfinite(step)) return NativePedControlStatus::InvalidInput;
        state.TimeStep = step;
    }
    return NativePedControlStatus::Ok;
}

NativePedControlStatus NativeSourceAnimUpdateTime(NativeSourceAnimAssociation& state, NativeSourceAnimEvent& event) {
    if (!Valid(state)) return NativePedControlStatus::InvalidInput;
    auto next = state;
    NativeSourceAnimEvent observed;
    if (next.Playing) {
        if (next.CurrentTime >= double(next.TotalTime)) next.Playing = false;
        else {
            next.CurrentTime += next.TimeStep;
            if (!std::isfinite(next.CurrentTime)) return NativePedControlStatus::InvalidInput;
            if (next.CurrentTime >= next.TotalTime) {
                if (next.Looped) next.CurrentTime -= next.TotalTime; // ONE subtraction, not fmod
                else {
                    next.CurrentTime = next.TotalTime;
                    if (next.FinishAutoRemove) { next.BlendAutoRemove = true; next.BlendDelta = -4; }
                    observed.FinishToken = next.FinishToken;
                    next.FinishToken = 0;
                }
            }
        }
    }
    state = next;
    event = observed;
    return NativePedControlStatus::Ok;
}

NativePedControlStatus NativeSourceAnimUpdateBlend(NativeSourceAnimAssociation& state, float seconds, NativeSourceAnimEvent& event) {
    if (!Valid(state) || !std::isfinite(seconds) || seconds < 0) return NativePedControlStatus::InvalidInput;
    auto next = state;
    next.BlendAmount += next.BlendDelta * seconds;
    if (!std::isfinite(next.BlendAmount)) return NativePedControlStatus::InvalidInput;
    NativeSourceAnimEvent observed;
    if (next.BlendAmount <= 0 && next.BlendDelta < 0) {
        next.BlendAmount = 0;
        next.BlendDelta = std::max(0.0f, next.BlendDelta);
        if (next.BlendAutoRemove) {
            observed.FinishToken = next.FinishToken;
            observed.Removed = true;
            next.FinishToken = 0;
            next.Alive = false;
        }
    }
    if (next.BlendAmount > 1) {
        next.BlendAmount = 1;
        next.BlendDelta = std::min(0.0f, next.BlendDelta);
    }
    state = next;
    event = observed;
    return NativePedControlStatus::Ok;
}
