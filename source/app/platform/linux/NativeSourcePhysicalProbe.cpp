#include "NativeSourcePhysical.h"
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace {
using Status = NativeSourcePhysicalStatus;
std::size_t s_Checks = 0;
void Check(bool value, const char* message) {
    ++s_Checks;
    if (!value) { std::fprintf(stderr, "source-physical FAIL: %s\n", message); std::exit(1); }
}
NativeSourcePhysicalState Ped() {
    NativeSourcePhysicalState s;
    s.Mass = 80;
    s.IsPed = s.DisableTurnForce = s.ApplyGravity = s.UsesCollision = s.Collidable = true;
    return s;
}
void Translation() {
    auto s = Ped();
    Check(NativeSourceApplyMoveForce(s, {0, 0, 8.5f}) == Status::Ok && s.MoveSpeed[2] == 8.5f / 80,
        "source jump force divided by physical mass, not velocity assignment");
    const float before = s.MoveSpeed[2];
    Check(NativeSourceApplyGravity(s, 1) == Status::Ok && s.MoveSpeed[2] == before - 0.008f, "source normalized timestep gravity");
    Check(NativeSourceApplyMoveSpeed(s, 1) == Status::Ok && s.Position[2] == s.MoveSpeed[2], "source translation");
    s.DisableZ = true;
    const auto z = s.MoveSpeed[2];
    Check(NativeSourceApplyMoveForce(s, {80, 160, 800}) == Status::Ok && s.MoveSpeed == NativeSourcePhysicalVector{1, 2, z}, "disable Z applies only to force");
    Check(NativeSourceApplyGravity(s, 1) == Status::Ok && s.MoveSpeed[2] == z - 0.008f, "source gravity does not consult DisableZ");
    s.DisableMoveForce = true;
    const auto frozen = s;
    Check(NativeSourceApplyMoveForce(s, {80, 80, 80}) == Status::Ok && s == frozen, "disabled move force is no-op");
    Check(NativeSourceApplyGravity(s, 1) == Status::Ok && s == frozen, "disabled movement blocks gravity");
    Check(NativeSourceApplyMoveSpeed(s, 1) == Status::Ok && s.Position == frozen.Position && s.MoveSpeed == NativeSourcePhysicalVector{}, "disabled translation clears speed");
    s = Ped(); s.InfiniteMass = true;
    const auto infinite = s;
    Check(NativeSourceApplyMoveForce(s, {1, 2, 3}) == Status::Ok && s == infinite, "infinite mass force no-op");
    Check(NativeSourceApplyGravity(s, 1) == Status::Unsupported && s == infinite, "infinite mass gravity torque is not fabricated");
    s = Ped(); s.UsesCollision = false;
    const auto noCollision = s;
    Check(NativeSourceApplyGravity(s, 1) == Status::Ok && s == noCollision, "non-colliding body has no gravity update");
    Check(NativeSourceApplyMoveSpeed(s, -1) == Status::InvalidInput && s == noCollision, "bad timestep atomic");
    Check(NativeSourceApplyMoveForce(s, {0, std::numeric_limits<float>::infinity(), 0}) == Status::InvalidInput && s == noCollision, "nonfinite force atomic");
    s.MoveSpeed[0] = std::numeric_limits<float>::max();
    const auto huge = s;
    Check(NativeSourceApplyMoveSpeed(s, 2) == Status::Overflow && s == huge, "overflow retains full body");
}
void Pair() {
    NativeSourcePhysicalContact contact{{1, 2, 3}, {1, 0, 0}, 7, 9};
    auto a = Ped(), b = Ped();
    a.Mass = b.Mass = 1; a.MoveSpeed[0] = -5; a.PushOtherPeds = true;
    NativeSourcePhysicalContactResult result;
    Check(NativeSourceApplyPedPair(a, b, contact, result) == Status::Ok && result.Applied &&
        a.MoveSpeed[0] == -4 && b.MoveSpeed[0] == -4 && result.DamageA == 1 && result.DamageB == 4,
        "source asymmetric four-times-this-mass policy, not symmetric rigid-body solver");
    Check(result.ReportCount == 2 && !result.Reports[0].ReverseEntities && result.Reports[1].ReverseEntities &&
        result.Reports[0].SurfaceA == 7 && result.Reports[1].SurfaceA == 9 && result.Reports[1].SurfaceB == 7 &&
        result.Reports[1].Normal == contact.Normal && result.Reports[1].Point == contact.Point,
        "collision report order reverses identities/surfaces but preserves normal");
    a = Ped(); b = Ped(); a.Mass = b.Mass = 1; a.MoveSpeed[0] = -5; a.PushOtherPeds = true;
    a.Elasticity = b.Elasticity = 1;
    Check(NativeSourceApplyPedPair(a, b, contact, result) == Status::Ok && a.MoveSpeed[0] == -3 && b.MoveSpeed[0] == -8,
        "source separate reflected target speeds");
    a = Ped(); b = Ped(); a.MoveSpeed[0] = -1;
    Check(NativeSourceApplyPedPair(a, b, contact, result) == Status::Ok && a.MoveSpeed[0] == 0 && b.MoveSpeed[0] == 0 &&
        result.ReportCount == 1 && result.DamageB == 0, "non-pushing ped does not accelerate other body");
    a = Ped(); b = Ped(); a.MoveSpeed[0] = -2; b.MoveSpeed[0] = -1;
    Check(NativeSourceApplyPedPair(a, b, contact, result) == Status::Ok && a.MoveSpeed[0] == -1 && b.MoveSpeed[0] == -1, "non-pusher follows negative entity speed");
    a = Ped(); b = Ped(); a.MoveSpeed[0] = 1;
    const auto separating = a;
    Check(NativeSourceApplyPedPair(a, b, contact, result) == Status::Ok && a == separating && result == NativeSourcePhysicalContactResult{}, "separating contact clears result, applies no force");
    a.MoveSpeed[0] = -1; a.DisableCollisionForce = true;
    const auto disabled = a;
    Check(NativeSourceApplyPedPair(a, b, contact, result) == Status::Ok && a == disabled && !result.Applied, "disabled first body sets common speed to itself");
    a.DisableCollisionForce = false; b.DisableCollisionForce = true;
    Check(NativeSourceApplyPedPair(a, b, contact, result) == Status::Ok && a.MoveSpeed[0] == 0 && result.ReportCount == 1, "disabled second body receives no report/force");
    const auto beforeA = a, beforeB = b;
    const auto beforeResult = result;
    Check(NativeSourceApplyPedPair(a, a, contact, result) == Status::InvalidInput && a == beforeA && result == beforeResult, "aliased pair rejected atomically");
    b.SafePosition = true;
    const auto safe = b;
    Check(NativeSourceApplyPedPair(a, b, contact, result) == Status::Unsupported && a == beforeA && b == safe && result == beforeResult, "safe-position rollback cannot be silently omitted");
    b = beforeB; b.Static = true;
    Check(NativeSourceApplyPedPair(a, b, contact, result) == Status::Unsupported && result == beforeResult, "static collision path is not dynamic ped pair");
    b = beforeB; a.MoveSpeed[0] = -std::numeric_limits<float>::max(); a.PushOtherPeds = true; b.DisableCollisionForce = false;
    const auto large = a;
    Check(NativeSourceApplyPedPair(a, b, contact, result) == Status::Overflow && a == large && result == beforeResult, "pair overflow retains both bodies and reports");
    a = Ped(); b = Ped(); a.Mass = std::numeric_limits<float>::max(); a.MoveSpeed[0] = -0.1f; a.PushOtherPeds = true;
    const auto hugeMass = a, other = b;
    Check(NativeSourceApplyPedPair(a, b, contact, result) == Status::Overflow && a == hugeMass && b == other && result == beforeResult,
        "overflowing mass denominator cannot collapse into a finite false response");
    a = Ped(); b = Ped(); a.Mass = 1;
    a.MoveSpeed = {-16777216.0f, 16777216.0f, -0.5f};
    contact.Normal = {1, 1, 1}; // deliberately not normalized: preserve supplied source expression
    Check(NativeSourceApplyPedPair(a, b, contact, result) == Status::Ok && !result.Applied,
        "DotProduct adds z+y+x; cancellation differs from SquaredMagnitude's x+y+z");
}
void WorldResponse() {
    auto ped = Ped();
    ped.MoveSpeed = {-2, 1, 0}; ped.Elasticity = 1; ped.DisableCollisionForce = true;
    NativeSourcePhysicalContact contact{{0, 0, 0}, {1, 0, 0}, 5, 7};
    NativeSourcePhysicalContactResult result;
    Check(NativeSourceApplyPedCollision(ped, contact, result) == Status::Ok && ped.MoveSpeed == NativeSourcePhysicalVector{0, 1, 0} &&
        result.DamageA == 160 && result.ReportCount == 1 && result.Reports[0].Impact == 2,
        "single-ped source wall response ignores elasticity and DisableCollisionForce");
    Check(NativeSourceApplyPedCollision(ped, contact, result) == Status::Ok && !result.Applied && result.ReportCount == 0,
        "tangential motion has no second impulse");
    ped.MoveSpeed[0] = -2; ped.InfiniteMass = true;
    const auto retained = ped;
    Check(NativeSourceApplyPedCollision(ped, contact, result) == Status::Ok && ped == retained && result.Applied && result.ReportCount == 1,
        "source reports collision even when infinite mass blocks force");
}
}
int main() {
    Translation(); Pair(); WorldResponse();
    std::printf("source-physical-ok checks=%zu source-force-and-dynamic-ped-pair no-narrowphase-or-world-route\n", s_Checks);
}
