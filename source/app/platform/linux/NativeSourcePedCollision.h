#pragma once
#include "NativeSourceContact.h"

enum class NativeSourcePedCollisionEntity { Unknown, Building, Vehicle, Ped, Object, Dummy };
enum class NativeSourcePedCollisionStatus { Ok, InvalidInput, Unsupported, Overflow };
struct NativeSourcePedCollisionInput {
    NativeSourcePedCollisionEntity Other = NativeSourcePedCollisionEntity::Unknown;
    float TimeStep{};
    bool UsesCollision{}, ForceHitReturnFalse{}, WasStanding{};
    bool SkipLineCollision{}, ProcessingShift{}, Attached{}, CheckAboveHead{};
    bool TestBlockedPositions{}, PlayerCameraWeaponAdjustment{};
    bool ModelIsStandardPed1{}; // verified by model owner, never a default fallback
    bool IsStuck{}, OtherDisableCollisionForce{};
};
struct NativeSourcePedCollisionShape {
    NativeCollisionVector Min{}, Max{}, BoundCenter{};
    float BoundRadius{};
    std::array<NativeSourceContactSphere, 3> Spheres{};
    NativeCollisionVector LineStart{}, LineEnd{};
    NativeCollisionVector HeadLineStart{}, HeadLineEnd{};
    std::uint8_t LineCount{};
    float LegSphereTop{};
    // Entity flag bit1 (0x2), NOT HasContacted/bit3 (0x8). The latter selects
    // friction handling and must not be set merely by preparing support lines.
    bool QueryEnabled{}, SetCollisionProcessed{}, ReturnAllContacts{};
};

// TempColModels::Initialise Ped1 plus the normal preparation
// prefix of retail CPed::ProcessEntityCollision 0x5FE210. Pure owned copy:
// source temporarily mutates shared model lines/bounds then restores them.
// No such global mutation, no model-selection fallback, no contact query or
// standing/position decision is performed by this preparation function.
NativeSourcePedCollisionStatus NativeSourcePreparePedCollision(const NativeSourcePedCollisionInput&,
    NativeSourcePedCollisionShape& out);
