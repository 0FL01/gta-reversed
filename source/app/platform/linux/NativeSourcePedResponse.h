#pragma once

#include "NativeSourcePedModelContact.h"
#include "NativeSourcePhysical.h"
#include "NativeSourceSurfaces.h"

enum class NativeSourcePedResponseStatus { Ok, InvalidInput, Unsupported, Overflow, SurfaceUnavailable };

struct NativeSourcePedContactState {
    bool IsStanding{}, WasStanding{}, HeadStuckInCollision{}, HitSteepSlope{}, HasContacted{};
    float HeightLimit = 99999.9921875f;
    std::uint8_t SurfaceTouched{};
    NativeCollisionVector ContactNormal{}, ContactOffset{};
    bool operator==(const NativeSourcePedContactState&) const = default;
};

struct NativeSourcePedEntityResponse {
    bool SupportAccepted{}, BlockingCollision{};
    float SupportFraction = 1.0f;
    std::size_t ContactCount{}, AppliedCount{}, FrictionCount{}, ReportCount{};
    std::array<NativeSourcePhysicalReport, 62> Reports{};
    bool operator==(const NativeSourcePedEntityResponse&) const = default;
};

// CPhysical::CheckCollision ped prefix. Call once for a world scan, not once
// per entity. No standing-entity pointer is represented by this static slice.
NativeSourcePedResponseStatus NativeSourceBeginPedCollisionCheck(
    NativeSourcePhysicalState&, NativeSourcePedContactState&,
    bool forceHitReturnFalse = false, bool processingShift = false, bool skipLineCollision = false);

// Ordinary static-building path of CPed::ProcessEntityCollision plus the
// disable-turn-force branch used by ProcessCollisionSectorList. The supplied
// contacts must come from NativeSourceProcessPedModel. Low-piece standard Ped1
// contacts only; blocked-position, above-head, moving support, fall damage,
// lighting and effects remain explicit Unsupported branches.
// Non-Ok retains body/state/out; successful blocking contacts change velocity
// and let the outer driver decide source matrix rollback.
NativeSourcePedResponseStatus NativeSourceResolvePedBuilding(
    NativeSourcePhysicalState&, NativeSourcePedContactState&,
    const NativeSourcePedCollisionShape&, const NativeSourcePedModelContacts&,
    const NativeSourceGroundTransform& buildingTransform, const NativeSourceSurfaces&,
    float timeStep, NativeSourcePedEntityResponse& out);

// Dynamic standard-ped contact branch. Support lines must be suppressed by the
// prepared shape. Both bodies are value owners; entity/world membership and
// the outer body's matrix rollback stay with the collision driver.
NativeSourcePedResponseStatus NativeSourceResolvePedPair(
    NativeSourcePhysicalState& ped, NativeSourcePedContactState& pedContact,
    NativeSourcePhysicalState& other, const NativeSourcePedCollisionShape&,
    const NativeSourcePedModelContacts&, const NativeSourceSurfaces&,
    float timeStep, NativeSourcePedEntityResponse& out);
