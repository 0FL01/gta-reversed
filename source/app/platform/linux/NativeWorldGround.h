// Bounded native initial-world authority; not retail CIplStore residency parity.
#pragma once
#include "NativeSourceGround.h"
#include "NativeWorldEntityInfo.h"

enum class NativeWorldGroundIssue {
    None, PopulationIncomplete, Metadata, CollisionBinding, TextLodState,
    Transform, CommittedGeometry, InvalidCommit, StaleCommit, DuplicateIdentity
};
struct NativeWorldGroundDiagnostic {
    NativeWorldGroundIssue Issue{};
    NativePlacementIdentity Candidate;
    std::string SourceReason;
};
struct NativeWorldGroundCommit {
    uint64_t WorldGeneration{}, MetadataRevision{};
    float X{}, Y{}, Radius{}; // Committed source-COL square, not render visibility.
    std::shared_ptr<const NativeCollisionSnapshot> SourceCollision;
    // Native initial state has no ignored entity and no runtime class mutation.
    // Text LinkLods' camera multiplier is not derivable from the IPL/IDE files.
    std::optional<float> LodDistanceMultiplier;
};
struct NativeWorldGroundPublication {
    std::shared_ptr<const NativeSourceGroundSnapshot> Snapshot;
    std::shared_ptr<const NativeCollisionSnapshot> SourceCollision;
    std::vector<NativeWorldGroundDiagnostic> Diagnostics;
};

class NativeWorldGround {
public:
    // Exclusive preparation from FULL immutable StreamPager population, complete
    // COL catalog and initial Object.dat metadata. Assets::Snapshot is memory-only.
    // Retains all unbound buildings as potential candidates in every sector.
    // No COL parser, pager, filesystem or mutable metadata is used after this call.
    static std::shared_ptr<const NativeWorldGround> Prepare(
        const NativeCollisionContext&, const NativeWorldEntityInfo&, std::string& error, uint64_t metadataRevision = 0);
    // Pure import/fixture boundary: bindings may be incomplete. Their absence is
    // never proof of null COL or sector exclusion. No caller completeness bit.
    static std::shared_ptr<const NativeWorldGround> PreparePopulation(
        const NativeCollisionPopulation&, const NativeWorldEntityInfo&,
        const NativeCollisionSnapshot& bindings, std::string& error, uint64_t metadataRevision = 0);
    // Build one source 50m normal sector wholly within a committed native square.
    // Source file record order is usable only for a single unmodified binary
    // list. Cross-IPL order and override reinsertion history remain unknown.
    // Text LOD uncertainty stays per-candidate Unsupported. Binary LoadIpl::Add
    // does not invoke SetupBigBuilding, regardless of draw distance or bound size.
    NativeWorldGroundPublication Publish(const NativeWorldGroundCommit&, float x, float y) const;
    // Atomic publication helper: rejected/stale commits preserve the old owner.
    bool Publish(const NativeWorldGroundCommit&, float x, float y,
                 NativeWorldGroundPublication& current, std::string& error) const;
    static NativeSourceGroundResult Query(const NativeWorldGroundPublication&,
        NativeCollisionVector stored, uint64_t generation, uint64_t revision);
    static bool SourceTransform(const NativeCollisionPlacement&, NativeSourceGroundTransform&);
    // Source GetBoundRect's FOUR corners, intentionally not an eight-corner AABB.
    static std::array<float, 4> SourceRect(const NativeCollisionModel&, const NativeSourceGroundTransform&,
                                         const NativeCollisionPlacement* initialPlacement = nullptr);
    static int Sector(float coordinate);
    size_t PopulationCount() const { return m_Entities.size(); }
private:
    struct Entity {
        NativeCollisionPlacement Placement;
        NativeSourceGroundTarget Target;
        std::optional<float> DrawDistance;
        bool BindingMayChange{};
        NativeWorldGroundDiagnostic Diagnostic;
    };
    std::vector<Entity> m_Entities;
    bool m_CompletePopulation{};
    uint64_t m_MetadataRevision{};
};
