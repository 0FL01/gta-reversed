#include "NativeSourcePedResponse.h"

#include <cmath>

namespace {
using Status = NativeSourcePedResponseStatus;
bool Finite(const NativeCollisionVector& v) {
    return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]);
}
bool ValidBody(const NativeSourcePhysicalState& body) {
    return std::isfinite(body.Mass) && body.Mass > 0 && std::isfinite(body.Elasticity) && body.Elasticity >= 0 &&
        Finite(body.Position) && Finite(body.MoveSpeed) && Finite(body.FrictionMoveSpeed);
}
bool Valid(const NativeSourcePhysicalState& body, const NativeSourcePedContactState& state, float timeStep) {
    return body.IsPed && body.DisableTurnForce && ValidBody(body) && std::isfinite(state.HeightLimit) && Finite(state.ContactNormal) &&
        Finite(state.ContactOffset) && std::isfinite(timeStep) && timeStep >= 0;
}
bool ValidPoint(const NativeSourceContactPoint& point) {
    return Finite(point.Point) && Finite(point.Normal) && std::isfinite(point.Depth);
}
NativeSourcePhysicalContact Contact(const NativeSourceContactPoint& point) {
    return {point.Point, point.Normal, point.SurfaceA.Material, point.SurfaceB.Material};
}
Status PhysicalStatus(NativeSourcePhysicalStatus status) {
    switch (status) {
    case NativeSourcePhysicalStatus::Ok: return Status::Ok;
    case NativeSourcePhysicalStatus::InvalidInput: return Status::InvalidInput;
    case NativeSourcePhysicalStatus::Unsupported: return Status::Unsupported;
    case NativeSourcePhysicalStatus::Overflow: return Status::Overflow;
    }
    return Status::Unsupported;
}
bool ValidContacts(const NativeSourcePedCollisionShape& shape, const NativeSourcePedModelContacts& contacts) {
    if (contacts.SphereCount > 31 || shape.LineCount > 2) return false;
    for (std::size_t i = 0; i < contacts.SphereCount; ++i) {
        if (!ValidPoint(contacts.Spheres[i]) || contacts.Spheres[i].SurfaceA.Piece > 2) return false;
    }
    for (std::size_t i = 0; i < shape.LineCount; ++i) {
        if (!std::isfinite(contacts.LineFractions[i]) || contacts.LineFractions[i] < 0 || contacts.LineFractions[i] > 1 ||
            (contacts.LineHits[i] && !ValidPoint(contacts.Lines[i]))) return false;
    }
    return true;
}
Status SurfaceInputs(const NativeSourceSurfaces& surfaces, const NativeSourcePedModelContacts& contacts,
    std::array<float, 31>& friction, std::array<NativeSourceSurfaceProperties, 31>& contactProperties,
    NativeSourceSurfaceProperties& supportProperties) {
    for (std::size_t i = 0; i < contacts.SphereCount; ++i) {
        if (surfaces.AdhesiveLimit(contacts.Spheres[i].SurfaceA.Material,
                contacts.Spheres[i].SurfaceB.Material, friction[i]) != NativeSourceSurfaceStatus::Ok) return Status::SurfaceUnavailable;
        if (surfaces.Describe(contacts.Spheres[i].SurfaceB.Material, contactProperties[i]) != NativeSourceSurfaceStatus::Ok)
            return Status::SurfaceUnavailable;
    }
    if (contacts.LineHits[0] && surfaces.Describe(contacts.Lines[0].SurfaceB.Material, supportProperties) != NativeSourceSurfaceStatus::Ok)
        return Status::SurfaceUnavailable;
    return Status::Ok;
}
void CopyReports(const NativeSourcePhysicalContactResult& physical, NativeSourcePedEntityResponse& result) {
    for (std::uint8_t i = 0; i < physical.ReportCount; ++i) result.Reports[result.ReportCount++] = physical.Reports[i];
}
}

NativeSourcePedResponseStatus NativeSourceBeginPedCollisionCheck(NativeSourcePhysicalState& body,
    NativeSourcePedContactState& state, bool forceHit, bool processingShift, bool skipLine) {
    if (!Valid(body, state, 0)) return Status::InvalidInput;
    if (!body.Attached && !forceHit && !processingShift && !skipLine) {
        if (state.IsStanding) {
            state.IsStanding = false;
            state.WasStanding = true;
        }
    }
    return Status::Ok;
}

NativeSourcePedResponseStatus NativeSourceResolvePedBuilding(NativeSourcePhysicalState& body,
    NativeSourcePedContactState& state, const NativeSourcePedCollisionShape& shape,
    const NativeSourcePedModelContacts& contacts, const NativeSourceGroundTransform& building,
    const NativeSourceSurfaces& surfaces, float timeStep, NativeSourcePedEntityResponse& out) {
    if (!Valid(body, state, timeStep) || !Finite(building.Position) || !ValidContacts(shape, contacts)) return Status::InvalidInput;
    if (body.Attached || state.HeadStuckInCollision || state.HeightLimit != 99999.9921875f) return Status::Unsupported;
    if (!shape.QueryEnabled || !shape.SetCollisionProcessed || shape.LineCount == 0 || shape.LineCount > 1 || contacts.LineHits[1])
        return Status::Unsupported;
    if (contacts.LineHits[0] && contacts.LineFractions[0] < 0.94999998807907104f && body.MoveSpeed[2] < -0.25f)
        return Status::Unsupported; // source fall-damage/task/effect branch is not owned here
    for (const auto& basis : building.Basis) if (!Finite(basis)) return Status::InvalidInput;
    // This slice deliberately excludes the special overhead/head-stuck contact.
    for (std::size_t i = 0; i < contacts.SphereCount; ++i)
        if (contacts.Spheres[i].Normal[2] < -0.8669999837875366f) return Status::Unsupported;
    std::array<float, 31> adhesive{};
    std::array<NativeSourceSurfaceProperties, 31> contactProperties{};
    NativeSourceSurfaceProperties supportProperties;
    if (const auto status = SurfaceInputs(surfaces, contacts, adhesive, contactProperties, supportProperties); status != Status::Ok) return status;

    auto nextBody = body;
    auto nextState = state;
    NativeSourcePedEntityResponse result;
    result.ContactCount = contacts.SphereCount;
    if (contacts.SphereCount) nextBody.HasHitWall = true;
    result.SupportFraction = contacts.LineFractions[0];
    if (contacts.LineHits[0] && contacts.LineFractions[0] < 0.94999998807907104f) {
        const auto& support = contacts.Lines[0];
        const float supportZ = support.Point[2] + 1.0f;
        if (!std::isfinite(supportZ)) return Status::Overflow;
        // The ordinary non-boat WasStanding branch accepts only a strict upward
        // correction. Gravity supplies that correction on a level surface.
        if (!nextState.WasStanding || supportZ >= nextBody.Position[2]) {
            result.SupportAccepted = true;
            nextBody.Position[2] = supportZ;
            nextBody.MoveSpeed[2] = 0;
            nextState.IsStanding = true;
            nextState.SurfaceTouched = support.SurfaceB.Material;
            nextState.ContactNormal = support.Normal;
            for (std::size_t i = 0; i < 3; ++i) nextState.ContactOffset[i] = support.Point[i] - building.Position[i];
            if (supportProperties.SteepSlope) nextState.HitSteepSlope = true;
        }
    }

    const bool alreadyContacted = nextState.HasContacted;
    for (std::size_t i = 0; i < contacts.SphereCount; ++i) {
        if (contactProperties[i].SteepSlope) nextState.HitSteepSlope = true;
        NativeSourcePhysicalContactResult physical;
        const auto physicalStatus = NativeSourceApplyPedCollision(nextBody, Contact(contacts.Spheres[i]), physical);
        if (physicalStatus != NativeSourcePhysicalStatus::Ok) return PhysicalStatus(physicalStatus);
        if (!physical.Applied) continue;
        ++result.AppliedCount;
        result.BlockingCollision = true;
        CopyReports(physical, result);
        if (!alreadyContacted) {
            const float friction = adhesive[i] / float(contacts.SphereCount) * 150.0f * physical.DamageA;
            if (!std::isfinite(friction)) return Status::Overflow;
            bool applied = false;
            const auto frictionStatus = NativeSourceApplyPedFriction(nextBody, friction, timeStep, Contact(contacts.Spheres[i]), applied);
            if (frictionStatus != NativeSourcePhysicalStatus::Ok) return PhysicalStatus(frictionStatus);
            if (applied) { ++result.FrictionCount; nextState.HasContacted = true; }
        }
    }
    body = nextBody; state = nextState; out = result;
    return Status::Ok;
}

NativeSourcePedResponseStatus NativeSourceResolvePedPair(NativeSourcePhysicalState& ped,
    NativeSourcePedContactState& pedContact, NativeSourcePhysicalState& other,
    const NativeSourcePedCollisionShape& shape, const NativeSourcePedModelContacts& contacts,
    const NativeSourceSurfaces& surfaces, float timeStep, NativeSourcePedEntityResponse& out) {
    if (!Valid(ped, pedContact, timeStep) || !ValidBody(other) || !ValidContacts(shape, contacts)) return Status::InvalidInput;
    if (ped.Attached || ped.Static || ped.SafePosition || !other.IsPed || !other.DisableTurnForce || other.Attached ||
        other.Static || other.SafePosition) return Status::Unsupported;
    if (shape.LineCount != 0 || contacts.LineHits[0] || contacts.LineHits[1]) return Status::Unsupported;
    std::array<float, 31> adhesive{};
    std::array<NativeSourceSurfaceProperties, 31> contactProperties{};
    NativeSourceSurfaceProperties unused;
    if (const auto status = SurfaceInputs(surfaces, contacts, adhesive, contactProperties, unused); status != Status::Ok) return status;
    auto first = ped, second = other;
    auto nextContact = pedContact;
    NativeSourcePedEntityResponse result;
    result.ContactCount = contacts.SphereCount;
    const bool alreadyContacted = nextContact.HasContacted;
    for (std::size_t i = 0; i < contacts.SphereCount; ++i) {
        if (contactProperties[i].SteepSlope) nextContact.HitSteepSlope = true;
        NativeSourcePhysicalContactResult physical;
        const auto physicalStatus = NativeSourceApplyPedPair(first, second, Contact(contacts.Spheres[i]), physical);
        if (physicalStatus != NativeSourcePhysicalStatus::Ok) return PhysicalStatus(physicalStatus);
        if (!physical.Applied) continue;
        ++result.AppliedCount;
        result.BlockingCollision = true;
        CopyReports(physical, result);
        if (!alreadyContacted) {
            const float friction = adhesive[i] / float(contacts.SphereCount) * 150.0f * physical.DamageA;
            if (!std::isfinite(friction)) return Status::Overflow;
            bool applied = false;
            const auto frictionStatus = NativeSourceApplyPedPairFriction(first, second, friction, timeStep,
                Contact(contacts.Spheres[i]), applied);
            if (frictionStatus != NativeSourcePhysicalStatus::Ok) return PhysicalStatus(frictionStatus);
            if (applied) { ++result.FrictionCount; nextContact.HasContacted = true; }
        }
    }
    ped = first; other = second; pedContact = nextContact; out = result;
    return Status::Ok;
}
