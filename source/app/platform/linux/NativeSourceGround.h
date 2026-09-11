// Source-local CCollision::ProcessVerticalLine, independent of render/physics IO.
#pragma once
#include "NativeCollisionAssets.h"
#include <optional>

struct NativeWorldEntityMetadata;

enum class NativeSourceGroundStatus { Unsupported, Miss, Hit };
enum class NativeSourceGroundReason {
    None, InvalidInput, UnsupportedModel, UnrepresentablePlane, UnknownSurfaceTable,
    UnknownCoverage, OutsideCoverage, StaleWorld, UnknownEntityMetadata,
    UnknownTransform, UnknownCollisionModel, UnknownSourceOrder
};
struct NativeSourceGroundTransform {
    NativeCollisionVector Position{};
    // Source-effective matrix columns, already bound. Never reconjugated.
    std::array<NativeCollisionVector, 3> Basis{{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
};
struct NativeSourceGroundRequest {
    NativeCollisionVector Start{}, End{};
    float MaxFraction = 1.0f; // Source valid domain [0,1]; strict improvement only.
    bool SeeThroughCheck{}, ShootThroughCheck{};
    // Only needed if SeeThroughCheck=true: source actually selects IsSeeThrough.
    // ShootThroughCheck is UNUSED by source ProcessVerticalLine (0x417BF0).
    const std::array<bool, 256>* VerifiedSeeThroughMaterials{};
    static NativeSourceGroundRequest Generator(NativeCollisionVector storedPosition);
};
struct NativeSourceGroundPlane {
    // Calculated, not read from optional file-plane bytes: CalculateTrianglePlanes
    // -> ColTrianglePlane.cpp:33-52; int16 truncation, normal4096 / offset128.
    std::array<int16_t, 3> Normal{};
    int16_t Distance{};
    enum class Orientation : uint8_t { PosX, NegX, PosY, NegY, PosZ, NegZ } Direction{};
};
struct NativeSourceGroundResult {
    NativeSourceGroundStatus Status = NativeSourceGroundStatus::Unsupported;
    NativeSourceGroundReason Reason = NativeSourceGroundReason::None;
    float Fraction = 1.0f;
    NativeCollisionVector Point{}, Normal{};
    NativeCollisionPrimitive Primitive = NativeCollisionPrimitive::Triangle;
    uint32_t PrimitiveIndex{};
    NativeCollisionSurface Surface{}; // Raw file provenance; not collision flags.
    uint8_t MaterialB{}, LightingB{};
    NativeSourceGroundPlane Plane{}; // Valid for a triangle hit only.
    size_t TargetIndex = size_t(-1); // Snapshot target provenance, never sorted ID.
    uint64_t WorldGeneration{}, MetadataRevision{};
};

// Caller-owned source authority requirements. Initial Object.dat classification
// alone cannot fill these fields. An adapter from NativeWorldEntityInfo must also
// supply effective world membership, source sector eligibility and revisions.
enum class NativeSourceGroundKnown { Unknown, No, Yes };
enum class NativeSourceGroundClass { Unknown, Building, Other };
struct NativeSourceGroundTarget {
    NativePlacementIdentity Identity;
    std::shared_ptr<const NativeCollisionModel> Model;
    NativeSourceGroundTransform Transform;
    NativeSourceGroundClass EffectiveClass = NativeSourceGroundClass::Unknown;
    NativeSourceGroundKnown InWorld{}, UsesCollision{}, NormalSector{}, BigBuilding{}, Ignored{};
    bool VerifiedClassification{}, SourceEffectiveTransformKnown{}, CollisionModelKnown{};
    // This target belongs to this ONE source normal sector's candidate list.
    // Source scan-code dedup must be performed by the snapshot producer.
    std::optional<uint64_t> SourceListOrdinal;
};
struct NativeSourceGroundSnapshot {
    std::vector<NativeSourceGroundTarget> Targets;
    uint64_t WorldGeneration{}, MetadataRevision{};
    // Explicit native authority domain, not retail CIplStore completeness.
    // Every potential target of this sector, including unresolved targets, must
    // be represented. MissingModels==0 / class-filtered COL is NOT this proof.
    bool CompleteNormalSector{}, MembershipAndOverridesVerified{}, Deduplicated{};
    std::array<float, 2> MinXY{}, MaxXY{}; // half-open extent of that source sector
};

class NativeSourceGround {
public:
    // Import initial metadata only when its source identity/type/area are
    // verified. The world owner must explicitly attest that the initial class
    // is still effective. Other authority fields retain their Unknown defaults.
    static NativeSourceGroundTarget BindInitialMetadata(const NativeCollisionInstance&,
        const NativeWorldEntityMetadata&, bool initialClassStillEffective = false);
    // Pure arithmetic, no IO, allocation, mutable caches, global surface table,
    // BVH, terrain slope filtering or vehicle placement. Source expression order
    // (including Vector.cpp's double sqrt/reciprocal then float assignment);
    // build with FP contraction/fast-math disabled for bitwise oracle parity.
    // Extent: source-valid finite COL1-4 arrays (uint16 counts/face indices),
    // vertices and generated offsets representable in source int16 fixed point,
    // vanilla triangle branch, enabled primitives, sphere-inside option false.
    // Both triangle windings qualify; raw surface flags are not backface,
    // see-through or shoot-through bits. Default generator tests every material.
    static NativeSourceGroundResult ProcessVerticalLine(
        const NativeCollisionModel&, const NativeSourceGroundTransform&, const NativeSourceGroundRequest&);
    static bool CalculatePlane(NativeCollisionVector a, NativeCollisionVector b,
                               NativeCollisionVector c, NativeSourceGroundPlane&);
    // Optional bounded building authority adapter. Defaults remain Unsupported.
    // Unique nearest hits do not need order; equal entity fractions do.
    static NativeSourceGroundResult QueryBuildings(const NativeSourceGroundSnapshot&,
        NativeCollisionVector storedPosition, uint64_t worldGeneration, uint64_t metadataRevision);
};
