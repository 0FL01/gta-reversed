#pragma once

#include "NativeSourceContact.h"
#include "NativeSourceGround.h"

#include <array>
#include <span>

enum class NativeSourceModelStatus { Ok, InvalidInput, Unsupported, Overflow };

struct NativeSourceModelLine {
    NativeCollisionVector Start{}, End{};
};

struct NativeSourceModelContacts {
    std::array<NativeSourceContactPoint, 32> Spheres{};
    std::array<NativeSourceContactPoint, 16> Lines{};
    std::array<float, 16> LineFractions = [] {
        std::array<float, 16> values{};
        values.fill(1.0f);
        return values;
    }();
    std::array<bool, 16> LineHits{};
    std::size_t SphereCount{}, LineCount{};
    std::size_t CandidateSpheresA{}, CandidateSpheresB{};
    std::size_t CandidateBoxesA{}, CandidateBoxesB{};
    std::size_t CandidateTrianglesA{}, CandidateTrianglesB{};
    bool operator==(const NativeSourceModelContacts&) const = default;
};

// Address-free ProcessColModels authority for ordinary sphere/box/triangle
// models and source line arrays. Disk-backed wheel models are explicitly
// outside this seam. Both matrices are already source-effective. The optional
// A bound radius is the post-SetupSuspensionLines value; zero uses the authored
// model radius. Source candidate/contact limits and face-group order apply.
// Any non-Ok result retains the complete caller output.
NativeSourceModelStatus NativeSourceProcessModels(const NativeCollisionModel& a,
    const NativeSourceGroundTransform& transformA, std::span<const NativeSourceModelLine> linesA,
    float effectiveBoundRadiusA, const NativeCollisionModel& b,
    const NativeSourceGroundTransform& transformB, bool returnAllCollisions,
    NativeSourceModelContacts& out);
