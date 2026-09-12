#pragma once
#include "NativeCollisionAssets.h"

struct NativeSourceContactSurface {
    std::uint8_t Material{}, Piece{}, Lighting{};
    bool operator==(const NativeSourceContactSurface&) const = default;
};
struct NativeSourceContactSphere {
    NativeCollisionVector Center{};
    float Radius{};
    NativeSourceContactSurface Surface;
};
struct NativeSourceContactTriangle {
    // Source-effective decompressed vertices, in the sphere's coordinate space.
    // The caller owns COL binding/quantisation and transform provenance.
    std::array<NativeCollisionVector, 3> Vertices{};
    std::uint8_t Material{}, Lighting{};
};
struct NativeSourceContactBox {
    NativeCollisionVector Min{}, Max{};
    NativeSourceContactSurface Surface;
};
struct NativeSourceContactPoint {
    NativeCollisionVector Point{}, Normal{};
    float Depth{};
    NativeSourceContactSurface SurfaceA, SurfaceB;
    bool operator==(const NativeSourceContactPoint&) const = default;
};
enum class NativeSourceContactStatus { Hit, Miss, InvalidInput, Unsupported, Overflow };

// CCollision's current upstream-model sphere branches, not a claim of retail
// bitwise parity: upstream SphereSphere explicitly rearranges its early sqrt
// test; SphereTriangle uses its implemented ClosestPtPointTriangle helper.
// Strict nearest-distance-squared improvement; touching is not penetration.
// Miss/non-Hit preserves BOTH output and maximum distance. Degenerate triangles
// are Unsupported rather than disappearing as a successful no-contact query.
NativeSourceContactStatus NativeSourceSphereSphere(const NativeSourceContactSphere& a,
    const NativeSourceContactSphere& b, float& maxTouchDistanceSquared, NativeSourceContactPoint& out);
NativeSourceContactStatus NativeSourceSphereTriangle(const NativeSourceContactSphere& sphere,
    const NativeSourceContactTriangle&, float& maxTouchDistanceSquared, NativeSourceContactPoint& out);
// Retail helper 0x410850 confirms upstream code, not its conflicting comment:
// x/y require a strict unique minimum; all ties select z, even x=y<z.
// Inside face sets maximum
// to zero EVEN when it was already zero; Piece fields retain prior output,
// matching ProcessSphereBox (unlike the sphere/triangle branches).
NativeSourceContactStatus NativeSourceSphereBox(const NativeSourceContactSphere& sphere,
    const NativeSourceContactBox&, float& maxTouchDistanceSquared, NativeSourceContactPoint& out);
// Source vanilla ProcessLineTriangle, general line (not the generator's forced
// local-vertical path). Reuses the existing source compressed-plane builder.
// Triangle vertices must be source-effective /128 coordinates; unrepresentable
// vertices/planes remain Unsupported. Depth and LightingA are not written by
// this source branch; retain them. Fraction is strictly improved in [0,1].
NativeSourceContactStatus NativeSourceLineTriangle(NativeCollisionVector start, NativeCollisionVector end,
    const NativeSourceContactTriangle&, float& maxFraction, NativeSourceContactPoint& out);
// Default source profile rejects line origins inside spheres. Box edges and
// face endpoints use strict source comparisons, not a slab/epsilon substitute.
// Both branches preserve Depth and PieceA/B; set LightingA=0.
NativeSourceContactStatus NativeSourceLineSphere(NativeCollisionVector start, NativeCollisionVector end,
    const NativeSourceContactSphere&, float& maxFraction, NativeSourceContactPoint& out);
NativeSourceContactStatus NativeSourceLineBox(NativeCollisionVector start, NativeCollisionVector end,
    const NativeSourceContactBox&, float& maxFraction, NativeSourceContactPoint& out);
// Source TestSphereTriangle broadphase (different boundary semantics from
// ProcessSphereTriangle). Uses the same packed plane builder as line tests.
NativeSourceContactStatus NativeSourceTestSphereTriangle(const NativeSourceContactSphere&,
    const NativeSourceContactTriangle&);
