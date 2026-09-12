// Authored IPL graph, BEFORE LinkLods' collision-dependent rewrites. No RW ownership.
#pragma once
#include "app/platform/linux/NativeWorldEntityInfo.h"
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

enum class NativeLodLinkStatus {
    None, Bound, UnknownParentSource, AmbiguousParentSource, InvalidIndex, Cycle, InvalidAncestor
};
struct NativeLodSource {
    std::string Key, Name; // Exact exported IPL key; source basename without extension.
    NativeWorldSourceRow Declaration; // DAT declaration for text; InitImageList/DAT for archive.
    std::string Archive;
    uint32_t ArchiveOrder{}, DirectoryRecord{}, Sector{}, Sectors{};
    bool Binary{};
    // Raw decoded inst order, not filtered/render order. Empty IPLs are retained.
    std::vector<NativeCollisionPlacement> Records;
};
struct NativeLodNode {
    NativePlacementIdentity Identity;
    NativeCollisionPlacement Placement; // Authored position/quaternion/type word retained.
    size_t Source{}, LocalIndex{};
    NativeLodLinkStatus Link = NativeLodLinkStatus::None;
    std::optional<size_t> ParentSource, CandidateParent, Parent;
    std::vector<size_t> ParentSourceCandidates, Children;
    std::string Evidence;
    // These are deliberately NOT derived from model-name prefixes or authored edges.
    NativeWorldKnownBool RuntimeModelIsLod = NativeWorldKnownBool::Unknown;
    NativeWorldKnownBool RuntimeBigBuilding = NativeWorldKnownBool::Unknown;
    NativeWorldKnownBool RuntimeUsesCollision = NativeWorldKnownBool::Unknown;
};
// P1-A03 LinkLods inputs: explicit cache lane and camera multiplier. No
// defaults are trusted; the evaluator rejects cache/unknown/nonfinite.
struct NativeLinkLodsInputs {
    NativeLinkLodsInputs(bool cacheLoading, float lodMultiplier)
        : CacheLoading(cacheLoading), LodMultiplier(lodMultiplier) {}
    bool CacheLoading;
    float LodMultiplier;
};
// Source underwater predicate, FileLoader.cpp 1056 then 1070-1085.
// Child: authored IPL bit OR (COL Min.z + entity Pos.z < 0). Parent in this
// narrow scope is KnownAbsent (no COL), so only its authored bit applies.
// Strict source float operation: (colMinZ + positionZ) < 0.0f, no rewrite
// (e.g. no positionZ < -colMinZ) and no double promotion. Caller must have
// proven authored bits Known (DiskValidated IncludesStreamed provenance).
[[nodiscard]] inline bool NativeLodSourceChildUnderwater(bool authoredUnderwater, float colMinZ, float positionZ) {
    return authoredUnderwater || (colMinZ + positionZ < 0.0f);
}
// Owned single-chain LinkLods decision: identities, counts, link,
// collision transfer, known post-LinkLods flags, effective shared COL,
// authored draws, underwater propagation and source-anchored reasons.
// PREPARED CPU authority only; A04 render COL generation/A05 async pending.
struct NativeLodChainDecision {
    NativePlacementIdentity Child, Parent;
    size_t ChildNode = SIZE_MAX, ParentNode = SIZE_MAX;
    int ChildModelId = -1, ParentModelId = -1;
    std::string ChildModel, ParentModel;
    size_t ParentChildren{}, ChildChildren{};
    NativeLodLinkStatus Link = NativeLodLinkStatus::None;
    bool EdgeKept{};
    bool CollisionTransferred{};
    std::shared_ptr<const NativeCollisionModel> EffectiveCol;
    std::string EffectiveColLibrary;
    uint32_t EffectiveColFaces{};
    NativeWorldKnownBool ChildBigBuilding = NativeWorldKnownBool::Unknown;
    NativeWorldKnownBool ChildUsesCollision = NativeWorldKnownBool::Unknown;
    NativeWorldKnownBool ChildIsLod = NativeWorldKnownBool::Unknown;
    NativeWorldKnownBool ParentBigBuilding = NativeWorldKnownBool::Unknown;
    NativeWorldKnownBool ParentUsesCollision = NativeWorldKnownBool::Unknown;
    NativeWorldKnownBool ParentIsLod = NativeWorldKnownBool::Unknown;
    float ChildDrawDistance{}, ParentDrawDistance{};
    bool DrawUnchanged{};
    bool Underwater{}, UnderwaterPropagated{};
    std::string LinkReason, TransferReason, RelationReason;
    bool operator==(const NativeLodChainDecision&) const = default;
};
// Relation-level renderer decision on external visibility inputs. No
// near/far/frustum/residency/GPU claim.
struct NativeLodRelationDecision {
    bool ChildSubmitted{}, ParentMarked{}, ParentSuppressed{}, ParentSubmitted{};
    std::string Reason;
    bool operator==(const NativeLodRelationDecision&) const = default;
};
// P1-A07 catalog-backed selected resource residency. Pure diagnostic
// residency policy: Visible are presented, HiddenTargets are authored
// parents retained hidden (explicit lab policy, NOT source runtime LOD).
// Population is the full catalog size, ExcludedOutside is
// Population-Visible-HiddenTargets, TimeModels counts admitted TimeAtomic
// resources (visibility stays unknown until P5-A02). Source order preserved.
struct NativeCatalogResidency {
    std::vector<NativePlacementIdentity> Visible, HiddenTargets;
    size_t Population{}, ExcludedOutside{}, TimeModels{};
};
class NativeLodCatalog {
public:
    // Full runtime population required. Raw DAT/IPL/IMG decode validates every
    // identity, transform, type word and contextual index before publishing const.
    static std::shared_ptr<const NativeLodCatalog> LoadBeforeWorker(
        const char* gameDir, const NativeCollisionPopulation& population, std::string& error);
    // Pure import/probe boundary: caller supplies COMPLETE raw-decoded catalog in
    // source order. Does not certify disk provenance; LoadBeforeWorker does that.
    static std::shared_ptr<const NativeLodCatalog> Assemble(
        const NativeCollisionPopulation& population, std::vector<NativeLodSource> sources,
        NativeWorldEntityInfo metadata, std::string& error);
    std::span<const NativeLodNode> Nodes() const { return m_Nodes; }
    std::span<const NativeLodSource> Sources() const { return m_Sources; }
    const NativeLodNode* Find(const NativePlacementIdentity& identity) const;
    NativeWorldEntityMetadata Metadata(const NativeLodNode& node) const { return m_Metadata.Query(node.Placement); }
    bool DiskValidated() const { return m_DiskValidated; }
    // LinkLods 0x5B5285 condition at the INITIAL, non-cache pass. Unlinked binary
    // rows never enter that pass. Collision fixups can remove children before a
    // later entity is visited, so this is only a condition diagnostic.
    NativeWorldKnownBool InitialBigBuildingCondition(const NativeLodNode& node, float lodMultiplier) const;
    // P1-A03 prepared CPU authority: pure bounded single-child LinkLods chain
    // evaluator in the existing catalog, not a new graph authority. No
    // mutation: ALL NativeLodNode.Runtime* stay Unknown; only the returned
    // decision is known. No near/far/frustum/residency/GPU claim. A04 render
    // COL generation and A05 async remain pending.
    // FileLoader.cpp 1956-2024 first-bind then source-order mutable pass;
    // Entity.cpp 863-871 SetupBigBuilding; BaseModelInfo.cpp 143-160
    // SetColModel(...,false) clears bIsLod; BaseModelInfo.h SetOwnsColModel
    // alters the SEPARATE bDoWeOwnTheColModel bit, not bIsLod.
    // Requires BOTH ends InitialBuildingMask()==True (Building/
    // AnimatedBuilding; Dummy starts uses-collision false per
    // Building.cpp 14-18 vs Entity 108-126 and CEntity/Dummy/CDummyObject).
    // Rejects childBig || !parentBig after the 0x5B5285 computation (child
    // normal must precede big; e.g. multiplier 2 crosses 180*2>300).
    // Underwater is FileLoader 1056 + 1070-1085 via
    // NativeLodSourceChildUnderwater on Known authored bits (no manual flag
    // decode); DiskValidated restricts model IDs <20000 so no u16 range gate.
    bool EvaluateLinkLodsChain(const NativePlacementIdentity& childIdentity,
                               const NativeCollisionAssets& collisions,
                               const NativeLinkLodsInputs& inputs, NativeLodChainDecision& out,
                               std::string& error) const;
    // Relation-level renderer decision on external visibility inputs only.
    // Opaque visible child marks/suppresses the single-child parent
    // (Renderer.cpp 509-521,695-722); otherwise a visible parent is the
    // fallback (640-655,1048/1065). A visible translucent child (alpha<255)
    // is still submitted but does not mark the parent. No actual
    // near/far/frustum/residency/GPU claim. Validates the chain carries the
    // accepted post-LinkLods states (child not-big/uses-COL/bIsLod,
    // parent big/no-COL/not-bIsLod); forged/tampered chains are rejected
    // with out unchanged.
    bool EvaluateLodRelation(const NativeLodChainDecision& chain, bool childVisible, uint8_t childAlpha,
                             bool parentVisible, NativeLodRelationDecision& out,
                             std::string& error) const;
    // P1-A07 selected residency: area 0 selects the exterior XY disc
    // (Placement.Interior low byte == 0 as well as squaredDistance <= radius^2,
    // no Z cull); positive area selects the entire interior (exact
    // position ignored beyond finite validation). Bound-parent closure is
    // expanded recursively even outside the initial window. Catalog source
    // order is preserved. Any selected node referenced as parent by another
    // selected node is HiddenTargets (diagnostic, NOT source runtime LOD;
    // Runtime* fields stay Unknown), others Visible. Atomic/TimeAtomic are
    // admitted (TimeModels counted); invalid link, cross-area linkage,
    // Clump or unknown model kind inside selected fails the WHOLE candidate
    // with out unchanged. Empty selection fails. Requires DiskValidated,
    // finite x/y, finite positive radius and area 0..255. No name-prefix
    // filters, caps or skip buckets.
    bool SelectResidency(float x, float y, float radius, int area, NativeCatalogResidency& out,
                         std::string& error) const;
private:
    using Key = std::tuple<std::string, std::string, uint32_t, int, bool>;
    static std::shared_ptr<NativeLodCatalog> AssembleMutable(
        const NativeCollisionPopulation& population, std::vector<NativeLodSource> sources,
        NativeWorldEntityInfo metadata, std::string& error);
    static Key IdentityKey(const NativePlacementIdentity& identity);
    std::vector<NativeLodSource> m_Sources;
    std::vector<NativeLodNode> m_Nodes;
    std::map<Key, size_t> m_Index;
    NativeWorldEntityInfo m_Metadata;
    bool m_DiskValidated{};
};
