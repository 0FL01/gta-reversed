#pragma once
#include <array>
#include <cstdint>

using NativeSourcePhysicalVector = std::array<float, 3>;
struct NativeSourcePhysicalState {
    NativeSourcePhysicalVector Position{}, MoveSpeed{};
    float Mass = 0, Elasticity = 0;
    bool InfiniteMass{}, DisableMoveForce{}, DisableZ{}, DontApplySpeed{};
    bool ApplyGravity{}, UsesCollision{};
    bool IsPed{}, DisableTurnForce{}, DisableCollisionForce{}, Collidable{};
    bool Static{}, Attached{}, SafePosition{}, HasHitWall{}, PushOtherPeds{};
    bool operator==(const NativeSourcePhysicalState&) const = default;
};
enum class NativeSourcePhysicalStatus { Ok, InvalidInput, Unsupported, Overflow };
struct NativeSourcePhysicalContact {
    NativeSourcePhysicalVector Point{}, Normal{};
    std::uint8_t SurfaceA{}, SurfaceB{};
};
struct NativeSourcePhysicalReport {
    // Source AudioEngine.ReportCollision call order; B reverses entities and
    // surfaces but does NOT negate the reported normal.
    bool ReverseEntities{};
    std::uint8_t SurfaceA{}, SurfaceB{};
    NativeSourcePhysicalVector Point{}, Normal{};
    float Impact{};
    bool operator==(const NativeSourcePhysicalReport&) const = default;
};
struct NativeSourcePhysicalContactResult {
    bool Applied{};
    float DamageA{}, DamageB{};
    std::array<NativeSourcePhysicalReport, 2> Reports{};
    std::uint8_t ReportCount{};
    bool operator==(const NativeSourcePhysicalContactResult&) const = default;
};

// Source arithmetic only. Caller supplies source-normalized timestep, not
// seconds. No gravity+movement orchestration, collision detection, asset IO,
// spatial membership or position correction is manufactured here. Non-Ok
// retains every state/output; a successful no-contact clears the result.
NativeSourcePhysicalStatus NativeSourceApplyMoveForce(NativeSourcePhysicalState&, NativeSourcePhysicalVector force);
NativeSourcePhysicalStatus NativeSourceApplyGravity(NativeSourcePhysicalState&, float timeStep);
NativeSourcePhysicalStatus NativeSourceApplyMoveSpeed(NativeSourcePhysicalState&, float timeStep);

// Physical.cpp:2849–2948, both bodies disable turn force, live dynamic peds.
// No safe-position rollback/attachment/static-object special path. Contact
// provenance and normal orientation must come from the source narrowphase;
// the normal is used as supplied (never silently normalized).
NativeSourcePhysicalStatus NativeSourceApplyPedPair(NativeSourcePhysicalState& a,
    NativeSourcePhysicalState& b, const NativeSourcePhysicalContact&, NativeSourcePhysicalContactResult&);
// Physical.cpp:976–988 single-body disable-turn-force collision response.
// No elasticity term or DisableCollisionForce gate in this source branch.
NativeSourcePhysicalStatus NativeSourceApplyPedCollision(NativeSourcePhysicalState&,
    const NativeSourcePhysicalContact&, NativeSourcePhysicalContactResult&);
