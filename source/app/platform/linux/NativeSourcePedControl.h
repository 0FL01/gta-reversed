// Source-closed ped control/association primitives. No world simulation,
// original-address calls, Godot/RW pointers, assets or OS clock.
#pragma once

#include <cstdint>

enum class NativePedControlStatus { Ok, InvalidInput };
enum class NativeSourceMoveState : std::uint8_t { Still = 1, Walk = 4, Run = 6 };

struct NativeSourceMoveSample {
    std::int16_t LeftRight = 0, UpDown = 0;
    float CameraOrientation = 0, TimeStep = 0;
    bool WalkModifier = false, Attached = false, DirectionAllowed = true;
};
struct NativeSourceMoveBlend {
    float Ratio = 0, AimingRotation = 0;
    bool operator==(const NativeSourceMoveBlend&) const = default;
};
// TaskSimplePlayerOnFoot::PlayerControlZelda input/ratio branch only. Direction
// eligibility is a caller-owned source predicate, NOT an invented world query.
NativePedControlStatus NativeSourceUpdateMoveBlend(const NativeSourceMoveSample& sample, NativeSourceMoveBlend& state);

struct NativeSourceWalkRunWeights {
    NativeSourceMoveState Move = NativeSourceMoveState::Still;
    float Walk = 0, Run = 0;
    bool operator==(const NativeSourceWalkRunWeights&) const = default;
};
// Normal walk/run association branch ONLY: no sprint, start/stop/turn/idle
// association, adrenaline or weapon branch. Caller must establish that scope.
// Retail SetRealMoveAnim 0x628102..0x62819a; zero ratio belongs to idle instead.
NativePedControlStatus NativeSourceSelectWalkRun(float ratio, NativeSourceWalkRunWeights& out);

struct NativeSourceAnimAssociation {
    float TotalTime = 0, CurrentTime = 0, TimeStep = 0, Speed = 1; // duration must be supplied
    float BlendAmount = 0, BlendDelta = 0;
    bool Playing = true, Looped = false, Synchronised = false;
    bool FinishAutoRemove = false, BlendAutoRemove = false, Alive = true;
    // Source callback pointer becomes a caller-qualified opaque token. Zero is
    // the default/no-op callback. No actual pointer or callback escapes.
    std::uint64_t FinishToken = 0;
    bool operator==(const NativeSourceAnimAssociation&) const = default;
};
struct NativeSourceAnimEvent {
    std::uint64_t FinishToken = 0;
    bool Removed = false;
    bool operator==(const NativeSourceAnimEvent&) const = default;
};

// AnimBlendAssociation.cpp:237–318. Keep source operations separate: the caller
// owns animation update order. Rejections retain state AND output event.
NativePedControlStatus NativeSourceAnimUpdateStep(NativeSourceAnimAssociation& state, float seconds, float totalTimeReciprocal);
NativePedControlStatus NativeSourceAnimUpdateTime(NativeSourceAnimAssociation& state, NativeSourceAnimEvent& event);
NativePedControlStatus NativeSourceAnimUpdateBlend(NativeSourceAnimAssociation& state, float seconds, NativeSourceAnimEvent& event);
