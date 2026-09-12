#include "NativeSourcePedCollision.h"
#include <cmath>

NativeSourcePedCollisionStatus NativeSourcePreparePedCollision(const NativeSourcePedCollisionInput& input,
    NativeSourcePedCollisionShape& out) {
    using Status = NativeSourcePedCollisionStatus;
    if (!std::isfinite(input.TimeStep) || input.TimeStep < 0) return Status::InvalidInput;
    if (input.Other == NativeSourcePedCollisionEntity::Unknown || !input.ModelIsStandardPed1 ||
        input.TestBlockedPositions || input.PlayerCameraWeaponAdjustment) return Status::Unsupported;
    NativeSourcePedCollisionShape shape;
    shape.Min = {-0.35f, -0.35f, -1}; shape.Max = {0.35f, 0.35f, 0.95f};
    shape.BoundRadius = 1;
    for (std::size_t i = 0; i < shape.Spheres.size(); ++i) {
        shape.Spheres[i] = {{0, 0, i == 0 ? -0.2f : i == 1 ? 0.2f : 0.6f}, 0.35f, {62, std::uint8_t(i), 0}};
    }
    shape.LineStart = {};
    shape.LineEnd = {0, 0, -1};
    shape.HeadLineStart = {};
    shape.HeadLineEnd = {0, 0, 1};
    shape.LegSphereTop = 0.94f; // source default when line query is disabled
    // Retail test byte [+0x42],1 selects physical bit16 (mask value 1,
    // not bit index 1), the same ForceHitReturnFalse flag used below.
    shape.QueryEnabled = input.UsesCollision || input.ForceHitReturnFalse;
    if (!shape.QueryEnabled) { out = shape; return Status::Ok; }
    // Retail 0x5FE3A7: mask0x19000 = SkipLineCol|ProcessingShift|ForceHitReturnFalse;
    // other-ped contacts do not run the support line.
    const bool lines = !input.SkipLineCollision && !input.ProcessingShift && !input.ForceHitReturnFalse &&
        !input.Attached && input.Other != NativeSourcePedCollisionEntity::Ped;
    if (lines) {
        shape.SetCollisionProcessed = true;
        shape.LineCount = 1;
        // TimeStep is float, source multiplier is double carrying float -0.15.
        const float extension = float(double(input.TimeStep) * double(-0.15f));
        if (!std::isfinite(extension)) return Status::Overflow;
        if (input.WasStanding) shape.LineEnd[2] += extension;
        shape.LegSphereTop = shape.Spheres[2].Center[2] + shape.Spheres[2].Radius;
        if (input.CheckAboveHead) {
            // Retail 0x5FE431–0x5FE466: the second line starts below the
            // upper sphere's top and ends at twice (first sphere bottom + 1).
            const float bottomPlusOne = float(double(shape.Spheres[0].Center[2]) - double(shape.Spheres[0].Radius) + 1.0);
            shape.HeadLineStart[2] = shape.LegSphereTop - bottomPlusOne;
            shape.HeadLineEnd[2] = bottomPlusOne;
            shape.HeadLineEnd[2] += bottomPlusOne;
            shape.LineCount = 2;
            shape.BoundRadius = shape.HeadLineEnd[2];
        } else shape.BoundRadius = std::abs(shape.LineEnd[2]);
        shape.Max[2] = shape.BoundRadius;
        shape.Min[2] = shape.LineEnd[2];
    }
    shape.ReturnAllContacts = input.IsStuck &&
        (input.Other == NativeSourcePedCollisionEntity::Building || input.OtherDisableCollisionForce);
    if (!std::isfinite(shape.LineEnd[2])) return Status::Overflow;
    out = shape;
    return Status::Ok;
}
