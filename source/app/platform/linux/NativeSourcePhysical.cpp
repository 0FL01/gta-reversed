#include "NativeSourcePhysical.h"
#include <cmath>
#include <limits>

namespace {
using Status = NativeSourcePhysicalStatus;
using Vector = NativeSourcePhysicalVector;
bool Finite(const Vector& value) {
    for (const auto v : value) if (!std::isfinite(v)) return false;
    return true;
}
bool Valid(const NativeSourcePhysicalState& value) {
    return Finite(value.Position) && Finite(value.MoveSpeed) && Finite(value.FrictionMoveSpeed) && std::isfinite(value.Mass) && value.Mass > 0 &&
        std::isfinite(value.Elasticity) && value.Elasticity >= 0;
}
float Dot(const Vector& a, const Vector& b) { return a[2] * b[2] + a[1] * b[1] + a[0] * b[0]; }
void MoveForce(NativeSourcePhysicalState& state, Vector force) {
    if (state.InfiniteMass || state.DisableMoveForce) return;
    if (state.DisableZ) force[2] = 0;
    for (std::size_t i = 0; i < 3; ++i) state.MoveSpeed[i] += force[i] / state.Mass;
}
Vector Scale(const Vector& v, float scale) { return {v[0] * scale, v[1] * scale, v[2] * scale}; }
bool Tangent(const Vector& speed, const Vector& normal, Vector& tangent, float& magnitude) {
    const float dot = Dot(speed, normal);
    if (!std::isfinite(dot)) return false;
    for (std::size_t i = 0; i < 3; ++i) tangent[i] = speed[i] - dot * normal[i];
    const float squared = tangent[0] * tangent[0] + tangent[1] * tangent[1] + tangent[2] * tangent[2];
    if (!Finite(tangent) || !std::isfinite(squared)) return false;
    magnitude = std::sqrt(squared);
    return std::isfinite(magnitude);
}
void FrictionMoveForce(NativeSourcePhysicalState& state, Vector force) {
    if (state.InfiniteMass || state.DisableMoveForce) return;
    if (state.DisableZ) force[2] = 0;
    for (std::size_t i = 0; i < 3; ++i) state.FrictionMoveSpeed[i] += force[i] / state.Mass;
}
}
NativeSourcePhysicalStatus NativeSourceApplyMoveForce(NativeSourcePhysicalState& state, Vector force) {
    if (!Valid(state) || !Finite(force)) return Status::InvalidInput;
    auto candidate = state;
    MoveForce(candidate, force);
    if (!Finite(candidate.MoveSpeed)) return Status::Overflow;
    state = candidate;
    return Status::Ok;
}
NativeSourcePhysicalStatus NativeSourceApplyGravity(NativeSourcePhysicalState& state, float timeStep) {
    if (!Valid(state) || !std::isfinite(timeStep) || timeStep < 0) return Status::InvalidInput;
    if (!state.ApplyGravity || state.DisableMoveForce) return Status::Ok;
    // Infinite-mass gravity applies torque through ApplyForce and centre of
    // mass; do not silently collapse it into this translational owner.
    if (state.InfiniteMass) return Status::Unsupported;
    auto candidate = state;
    if (state.UsesCollision) candidate.MoveSpeed[2] -= timeStep * 0.008f;
    if (!Finite(candidate.MoveSpeed)) return Status::Overflow;
    state = candidate;
    return Status::Ok;
}
NativeSourcePhysicalStatus NativeSourceApplyAirResistance(NativeSourcePhysicalState& state, float resistance, float timeStep) {
    if (!Valid(state) || !std::isfinite(resistance) || resistance < 0 || !std::isfinite(timeStep) || timeStep < 0)
        return Status::InvalidInput;
    const float squared = state.MoveSpeed[0] * state.MoveSpeed[0] + state.MoveSpeed[1] * state.MoveSpeed[1] +
        state.MoveSpeed[2] * state.MoveSpeed[2];
    if (!std::isfinite(squared)) return Status::Overflow;
    float factor;
    if (resistance <= 0.1f) {
        const float magnitude = float(std::sqrt(double(squared)));
        const float speedMagnitude = magnitude * resistance;
        if (!std::isfinite(magnitude) || !std::isfinite(speedMagnitude)) return Status::Overflow;
        factor = float(std::pow(double(1.0f - speedMagnitude), double(timeStep)));
    } else {
        factor = float(std::pow(double(resistance), double(timeStep)));
    }
    if (!std::isfinite(factor)) return Status::Overflow;
    auto candidate = state;
    for (std::size_t i = 0; i < 3; ++i) candidate.MoveSpeed[i] *= factor;
    if (!Finite(candidate.MoveSpeed)) return Status::Overflow;
    state = candidate;
    return Status::Ok;
}
NativeSourcePhysicalStatus NativeSourceApplyMoveSpeed(NativeSourcePhysicalState& state, float timeStep) {
    if (!Valid(state) || !std::isfinite(timeStep) || timeStep < 0) return Status::InvalidInput;
    auto candidate = state;
    if (state.DontApplySpeed || state.DisableMoveForce) candidate.MoveSpeed = {};
    else for (std::size_t i = 0; i < 3; ++i) candidate.Position[i] += state.MoveSpeed[i] * timeStep;
    if (!Finite(candidate.Position)) return Status::Overflow;
    state = candidate;
    return Status::Ok;
}
NativeSourcePhysicalStatus NativeSourceCalculatePedCollisionSteps(NativeSourcePhysicalState& state,
    float timeStep, bool hasPlayerData, bool standingOnEntity, NativeSourcePedCollisionStepPlan& out) {
    if (!Valid(state) || !std::isfinite(timeStep) || timeStep < 0) return Status::InvalidInput;
    if (!state.IsPed) return Status::Unsupported;
    auto plan = out;
    plan.Count = 1;
    if (state.Attached) { out = plan; return Status::Ok; }
    const auto& v = state.MoveSpeed;
    // Retail x87 evaluates products/sums at double precision, then spills the
    // squared magnitude, sqrt, distance and ceil argument to explicit floats.
    const float squared = float(double(v[0]) * v[0] + double(v[1]) * v[1] + double(v[2]) * v[2]);
    if (!std::isfinite(squared)) return Status::Overflow;
    if (!hasPlayerData && double(squared) * timeStep * timeStep < double(0.09f)) { out = plan; return Status::Ok; }
    const float speed = float(std::sqrt(double(squared)));
    const float distance = speed * timeStep;
    if (!std::isfinite(distance)) return Status::Overflow;
    const double scale = hasPlayerData ? (standingOnEntity ? 2.0 : 1.0) : 1.5;
    const float argument = float(double(distance) * scale / double(0.3f));
    if (!std::isfinite(argument)) return Status::Overflow;
    double count = std::ceil(double(argument));
    if (hasPlayerData) {
        const double minimum = standingOnEntity ? 4.0 : 2.0;
        if (count < minimum) count = minimum;
    }
    const float elasticity = hasPlayerData ? state.Elasticity : float(double(state.Elasticity) * 2.0);
    if (!std::isfinite(elasticity)) return Status::Overflow;
    // FISTP int32 followed by AL. Masked out-of-range FISTP produces the
    // integer-indefinite 0x80000000, whose low byte is also zero. Do not use
    // undefined C++ floating-to-integer conversion or clamp the source count.
    plan.Count = count > double(std::numeric_limits<std::int32_t>::max()) ? 0 :
        static_cast<std::uint8_t>(static_cast<std::uint32_t>(count));
    state.Elasticity = elasticity;
    out = plan;
    return Status::Ok;
}
NativeSourcePhysicalStatus NativeSourceApplyPedPair(NativeSourcePhysicalState& a, NativeSourcePhysicalState& b,
    const NativeSourcePhysicalContact& contact, NativeSourcePhysicalContactResult& out) {
    if (&a == &b || !Valid(a) || !Valid(b) || !Finite(contact.Point) || !Finite(contact.Normal)) return Status::InvalidInput;
    const auto eligible = [](const auto& s) {
        return s.IsPed && s.DisableTurnForce && !s.Static && !s.Attached && !s.SafePosition;
    };
    if (!eligible(a) || !eligible(b)) return Status::Unsupported;
    auto first = a, second = b;
    NativeSourcePhysicalContactResult result;
    const float speedA = Dot(a.MoveSpeed, contact.Normal), speedB = Dot(b.MoveSpeed, contact.Normal);
    float shared = 0;
    bool applyB = true;
    if (a.DisableCollisionForce || a.DontApplySpeed) shared = speedA;
    else if (b.DisableCollisionForce || b.DontApplySpeed) { shared = speedB; applyB = false; }
    else if (!a.PushOtherPeds) { shared = speedB >= 0 ? 0 : speedB; applyB = false; }
    else {
        const float momentumA = a.Mass * speedA * 4.0f, momentumB = b.Mass * speedB;
        const float numerator = momentumA + momentumB, denominator = a.Mass * 4.0f + b.Mass;
        if (!std::isfinite(momentumA) || !std::isfinite(momentumB) || !std::isfinite(numerator) || !std::isfinite(denominator)) return Status::Overflow;
        shared = numerator / denominator;
    }
    const float difference = speedA - shared;
    if (!std::isfinite(speedA) || !std::isfinite(speedB) || !std::isfinite(shared) || !std::isfinite(difference)) return Status::Overflow;
    if (difference >= 0) { out = result; return Status::Ok; }
    const float elasticity = (b.Elasticity + a.Elasticity) * 0.5f;
    const float targetA = a.HasHitWall ? shared : shared - elasticity * difference;
    result.DamageA = (targetA - speedA) * a.Mass;
    const auto report = [&](bool reverse, float impact) {
        result.Reports[result.ReportCount++] = {reverse,
            reverse ? contact.SurfaceB : contact.SurfaceA, reverse ? contact.SurfaceA : contact.SurfaceB,
            contact.Point, contact.Normal, impact};
    };
    if (!a.DisableCollisionForce && !a.DontApplySpeed) {
        MoveForce(first, Scale(contact.Normal, result.DamageA));
        report(false, result.DamageA / a.Mass);
    }
    if (applyB) {
        const float targetB = b.HasHitWall ? shared : shared - (speedB - shared) * elasticity;
        result.DamageB = -((targetB - speedB) * b.Mass);
        if (!b.DisableCollisionForce && !b.DontApplySpeed) {
            // Preserve the two source multiplies rather than folding -DamageB.
            MoveForce(second, Scale(Scale(contact.Normal, result.DamageB), -1.0f));
            report(true, result.DamageB / b.Mass);
        }
    }
    if (!std::isfinite(elasticity) || !std::isfinite(result.DamageA) || !std::isfinite(result.DamageB) ||
        !Finite(first.MoveSpeed) || !Finite(second.MoveSpeed)) return Status::Overflow;
    for (std::uint8_t i = 0; i < result.ReportCount; ++i) if (!std::isfinite(result.Reports[i].Impact)) return Status::Overflow;
    result.Applied = true;
    a = first; b = second; out = result;
    return Status::Ok;
}
NativeSourcePhysicalStatus NativeSourceApplyPedCollision(NativeSourcePhysicalState& state,
    const NativeSourcePhysicalContact& contact, NativeSourcePhysicalContactResult& out) {
    if (!Valid(state) || !Finite(contact.Point) || !Finite(contact.Normal)) return Status::InvalidInput;
    if (!state.IsPed || !state.DisableTurnForce || state.Attached) return Status::Unsupported;
    const float speed = Dot(state.MoveSpeed, contact.Normal);
    if (!std::isfinite(speed)) return Status::Overflow;
    NativeSourcePhysicalContactResult result;
    if (speed >= 0) { out = result; return Status::Ok; }
    auto candidate = state;
    result.DamageA = -(speed * state.Mass);
    MoveForce(candidate, Scale(contact.Normal, result.DamageA));
    const float impact = result.DamageA / state.Mass;
    if (!Finite(candidate.MoveSpeed) || !std::isfinite(result.DamageA) || !std::isfinite(impact)) return Status::Overflow;
    result.Applied = true;
    result.ReportCount = 1;
    result.Reports[0] = {false, contact.SurfaceA, contact.SurfaceB, contact.Point, contact.Normal, impact};
    state = candidate; out = result;
    return Status::Ok;
}
NativeSourcePhysicalStatus NativeSourceApplyPedFriction(NativeSourcePhysicalState& state,
    float friction, float timeStep, const NativeSourcePhysicalContact& contact, bool& applied) {
    if (!Valid(state) || !std::isfinite(friction) || friction < 0 || !std::isfinite(timeStep) || timeStep < 0 ||
        !Finite(contact.Point) || !Finite(contact.Normal)) return Status::InvalidInput;
    if (!state.IsPed || !state.DisableTurnForce) return Status::Unsupported;
    if (state.DisableCollisionForce) { applied = false; return Status::Ok; }
    Vector tangent; float magnitude;
    if (!Tangent(state.MoveSpeed, contact.Normal, tangent, magnitude)) return Status::Overflow;
    if (magnitude <= 0) { applied = false; return Status::Ok; }
    Vector direction;
    for (std::size_t i = 0; i < 3; ++i) direction[i] = tangent[i] / magnitude;
    float speed = -magnitude;
    const float force = -(timeStep / state.Mass * friction);
    if (!std::isfinite(force) || !Finite(direction)) return Status::Overflow;
    if (speed < force) speed = force;
    auto next = state;
    next.FrictionMoveSpeed[0] += direction[0] * speed;
    next.FrictionMoveSpeed[1] += direction[1] * speed;
    if (!Finite(next.FrictionMoveSpeed)) return Status::Overflow;
    state = next; applied = true;
    return Status::Ok;
}
NativeSourcePhysicalStatus NativeSourceApplyPedPairFriction(NativeSourcePhysicalState& a,
    NativeSourcePhysicalState& b, float friction, float timeStep,
    const NativeSourcePhysicalContact& contact, bool& applied) {
    if (&a == &b || !Valid(a) || !Valid(b) || !std::isfinite(friction) || friction < 0 ||
        !std::isfinite(timeStep) || timeStep < 0 || !Finite(contact.Point) || !Finite(contact.Normal)) return Status::InvalidInput;
    if (!a.IsPed || !b.IsPed || !a.DisableTurnForce || !b.DisableTurnForce) return Status::Unsupported;
    Vector tangentA, tangentB; float magnitudeA, magnitudeB;
    if (!Tangent(a.MoveSpeed, contact.Normal, tangentA, magnitudeA) ||
        !Tangent(b.MoveSpeed, contact.Normal, tangentB, magnitudeB)) return Status::Overflow;
    const float momentumB = b.Mass * magnitudeB, momentumA = a.Mass * magnitudeA;
    const float sum = momentumB + momentumA, mass = b.Mass + a.Mass;
    if (!std::isfinite(momentumB) || !std::isfinite(momentumA) || !std::isfinite(sum) || !std::isfinite(mass)) return Status::Overflow;
    const float shared = sum / mass;
    if (!std::isfinite(shared)) return Status::Overflow;
    if (magnitudeA <= shared) { applied = false; return Status::Ok; }
    // The accepted branch proves magnitudeA>0. The original computes the
    // unused zero-magnitude division earlier; no FP-exception-flag parity claim.
    Vector direction;
    for (std::size_t i = 0; i < 3; ++i) direction[i] = tangentA[i] / magnitudeA;
    float forceA = a.Mass * (shared - magnitudeA);
    const float forceB = b.Mass * (shared - magnitudeB);
    const float limit = -(timeStep * friction);
    if (!Finite(direction) || !std::isfinite(forceA) || !std::isfinite(forceB) || !std::isfinite(limit)) return Status::Overflow;
    if (forceA < limit) forceA = limit;
    // Source repeats that same A comparison; do NOT "correct" it to clamp B.
    auto first = a, second = b;
    const auto impulseA = Scale(direction, forceA), impulseB = Scale(direction, forceB);
    if (!Finite(impulseA) || !Finite(impulseB)) return Status::Overflow;
    FrictionMoveForce(first, impulseA); FrictionMoveForce(second, impulseB);
    if (!Finite(first.FrictionMoveSpeed) || !Finite(second.FrictionMoveSpeed)) return Status::Overflow;
    a = first; b = second; applied = true;
    return Status::Ok;
}
NativeSourcePhysicalStatus NativeSourceConsumeFrictionMoveSpeed(NativeSourcePhysicalState& state) {
    if (!Valid(state)) return Status::InvalidInput;
    auto next = state;
    for (std::size_t i = 0; i < 3; ++i) next.MoveSpeed[i] += state.FrictionMoveSpeed[i];
    next.FrictionMoveSpeed = {};
    if (!Finite(next.MoveSpeed)) return Status::Overflow;
    state = next;
    return Status::Ok;
}
