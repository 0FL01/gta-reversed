#pragma once
#include "NativeSourcePedCollision.h"
#include "NativeSourceGround.h"

enum class NativeSourcePedModelStatus { Ok, InvalidInput, Unsupported, Overflow };
struct NativeSourcePedModelContacts {
    // Caller-owned seeds preserve the fields individual source branches do not write.
    std::array<NativeSourceContactPoint, 32> Spheres{};
    std::array<NativeSourceContactPoint, 2> Lines{};
    std::array<float, 2> LineFractions{1, 1};
    std::size_t SphereCount{};
    std::array<bool, 2> LineHits{};
    std::size_t CandidateSpheres{}, CandidateBoxes{}, CandidateTriangles{};
    bool operator==(const NativeSourcePedModelContacts&) const = default;
};

// ProcessColModels' standard Ped1 sphere/line side against an owned COL model.
// Both transforms are already source-effective: no quaternion reconjugation.
// No world eligibility/order, sector coverage, standing decision or response.
// Source fixed candidate/contact limits and authored face-group order apply.
// Unsupported shapes/arithmetic failures retain the complete caller output.
NativeSourcePedModelStatus NativeSourceProcessPedModel(const NativeSourcePedCollisionShape&,
    const NativeSourceGroundTransform& pedTransform, const NativeCollisionModel& other,
    const NativeSourceGroundTransform& otherTransform, NativeSourcePedModelContacts& out);
