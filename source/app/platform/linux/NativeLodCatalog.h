// Authored IPL graph, BEFORE LinkLods' collision-dependent rewrites. No RW ownership.
#pragma once
#include "app/platform/linux/NativeWorldEntityInfo.h"

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
