// Source CreateHitColModelSkinned projection. Matrix observations are explicit;
// parsed frame LTMs alone do not authorize a current HAnim matrix array.
#pragma once

#include "NativePlayerAssets.h"
#include <span>

struct NativePedHitMatrix {
    NativePlayerMatrix Value{};
    std::uint32_t Flags = 0;
};
struct NativePedHitBone {
    std::int32_t Tag = -1;
    bool MatrixKnown = false;
    NativePedHitMatrix Matrix;
};
struct NativePedHitCollisionInput {
    bool RootKnown = false, HierarchyKnown = false;
    NativePedHitMatrix RootLocal;
    std::span<const NativePedHitBone> Bones;
};
struct NativePedHitSphere {
    std::array<float, 3> Center{};
    float Radius = 0;
    std::uint8_t Material = 62, Piece = 0;
    bool operator==(const NativePedHitSphere&) const = default;
};
struct NativePedHitCollision {
    std::array<NativePedHitSphere, 12> Spheres;
    std::array<float, 3> BoundCenter{}, BoxMin{-0.5F, -0.5F, -1.2F}, BoxMax{0.5F, 0.5F, 1.2F};
    float BoundRadius = 1.5F;
    std::uint8_t ColSlot = 0;
    bool operator==(const NativePedHitCollision&) const = default;
};
enum class NativePedHitCollisionStatus {
    Constructed, UnknownRoot, UnknownHierarchy, UnknownBoneMatrix, InvalidInput
};
// Atomic output, no resource/actor publication or procedural bone fallback.
// Uses the initialized RW default identity optimization mask (0x20000).
NativePedHitCollisionStatus NativeConstructPedHitCollision(
    const NativePedHitCollisionInput& input, NativePedHitCollision& out);
