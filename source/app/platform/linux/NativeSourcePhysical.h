#pragma once
#include <array>
#include <cstdint>

using NativeSourcePhysicalVector = std::array<float, 3>;
struct NativeSourcePhysicalState {
    NativeSourcePhysicalVector Position{}, MoveSpeed{}, FrictionMoveSpeed{};
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
struct NativeSourcePedCollisionStepPlan {
    std::uint8_t Count{};
    // CPed's method leaves both caller-supplied pre-check flags untouched.
    bool PreCheckAtFullSpeed{}, PreCheckAtHalfSpeed{};
    bool operator==(const NativeSourcePedCollisionStepPlan&) const = default;
};

// Source arithmetic only. Caller supplies source-normalized timestep, not
// seconds. No gravity+movement orchestration, collision detection, asset IO,
// spatial membership or position correction is manufactured here. Non-Ok
// retains every state/output; a successful no-contact clears the result.
NativeSourcePhysicalStatus NativeSourceApplyMoveForce(NativeSourcePhysicalState&, NativeSourcePhysicalVector force);
NativeSourcePhysicalStatus NativeSourceApplyGravity(NativeSourcePhysicalState&, float timeStep);
NativeSourcePhysicalStatus NativeSourceApplyMoveSpeed(NativeSourcePhysicalState&, float timeStep);
// Retail CPed::SpecialEntityCalcCollisionSteps 0x5FFBD0. Player minimums
// are TWO / FOUR (standing on an entity), not maximum caps. NPC fast-path
// elasticity doubles; ProcessCollision must later restore its saved value.
// Preserve source low-byte narrowing, including a zero count after wrapping.
// The collision driver must not replace it with an invented saturation cap.
NativeSourcePhysicalStatus NativeSourceCalculatePedCollisionSteps(NativeSourcePhysicalState&,
    float timeStep, bool hasPlayerData, bool standingOnEntity, NativeSourcePedCollisionStepPlan&);

// Physical.cpp:2849–2948, both bodies disable turn force, live dynamic peds.
// No safe-position rollback/attachment/static-object special path. Contact
// provenance and normal orientation must come from the source narrowphase;
// the normal is used as supplied (never silently normalized).
NativeSourcePhysicalStatus NativeSourceApplyPedPair(NativeSourcePhysicalState& a,
    NativeSourcePhysicalState& b, const NativeSourcePhysicalContact&, NativeSourcePhysicalContactResult&);
// Physical.cpp:976–988 single-body disable-turn-force collision response.
// Also the identical early ped branch of ApplyCollisionAlt at1464–1487;
// that branch does not write the caller's accumulated move/turn vectors.
// No elasticity term or DisableCollisionForce gate in this source branch.
NativeSourcePhysicalStatus NativeSourceApplyPedCollision(NativeSourcePhysicalState&,
    const NativeSourcePhysicalContact&, NativeSourcePhysicalContactResult&);
// Physical.cpp:1685–1705 and1762–1797. Accumulate friction separately from
// velocity. Single-body ped friction writes only X/Y; pair friction preserves
// the source's repeated first-body clamp (the second force is NOT clamped).
NativeSourcePhysicalStatus NativeSourceApplyPedFriction(NativeSourcePhysicalState&,
    float friction, float timeStep, const NativeSourcePhysicalContact&, bool& applied);
NativeSourcePhysicalStatus NativeSourceApplyPedPairFriction(NativeSourcePhysicalState& a,
    NativeSourcePhysicalState& b, float friction, float timeStep,
    const NativeSourcePhysicalContact&, bool& applied);
// Translation add/reset component only (Physical.cpp:2547/2549), not the
// DisableZ ground-friction/rotation prepass or a whole physics update.
NativeSourcePhysicalStatus NativeSourceConsumeFrictionMoveSpeed(NativeSourcePhysicalState&);
