// Initial source entity classification, owned before worker startup. No RW state.
#pragma once
#include "app/platform/linux/NativeCollisionAssets.h"
#include <optional>
#include <string_view>
#include <tuple>

enum class NativeWorldKnownBool { Unknown, False, True };
enum class NativeWorldInitialClass { Unknown, Building, AnimatedBuilding, DummyObject };
enum class NativeWorldModelKind { Unknown, Atomic, TimeAtomic, Clump };
enum class NativeWorldObjectAssignment { Unknown, Unassigned, Assigned };
enum class NativeWorldInfoStatus { Ready, NotLoaded, ModelUnrepresented, IdentityMismatch, PlacementUnrepresented };

struct NativeWorldSourceRow {
    std::string Source;
    uint32_t Line{}; // One-based physical text line, not IPL record ordinal.
};
struct NativeWorldModelInfo {
    int ModelId = -1;
    std::string Name; // Authored IDE spelling; matching uses the source uppercase CRC.
    NativeWorldSourceRow Ide;
    NativeWorldModelKind Kind = NativeWorldModelKind::Unknown;
    NativeWorldKnownBool HasAnimBlend = NativeWorldKnownBool::Unknown;
    std::optional<float> DrawDistance;
    std::optional<uint32_t> IdeFlags;
    NativeWorldObjectAssignment ObjectInfo = NativeWorldObjectAssignment::Unknown;
    // Ordered successful assignments, including duplicates and default assignments.
    std::vector<NativeWorldSourceRow> ObjectRows;
    // Only the proven shortcut indices 0..3; deduplicated physics indices are not rebuilt.
    std::optional<uint8_t> DefaultObjectInfoIndex;
    NativeWorldInitialClass InitialClass = NativeWorldInitialClass::Unknown;
};
struct NativeWorldPlacementInfo {
    NativePlacementIdentity Identity;
    int ExportedInterior{}, Lod = -1;
    uint32_t ExportedFlags{};
    std::optional<uint32_t> SourceInstanceType; // Binary or source-backed runtime text export.
    std::optional<uint8_t> Area; // Low byte, including legacy imports' unmasked Interior.
    // Authored CFileObjectInstance bits, NOT current entity state. DontStream's
    // name inherits the source header's explicitly unconfirmed interpretation.
    NativeWorldKnownBool AuthoredRedundantStream = NativeWorldKnownBool::Unknown;
    NativeWorldKnownBool AuthoredDontStream = NativeWorldKnownBool::Unknown;
    NativeWorldKnownBool AuthoredUnderwater = NativeWorldKnownBool::Unknown;
    NativeWorldKnownBool AuthoredTunnel = NativeWorldKnownBool::Unknown;
    NativeWorldKnownBool AuthoredTunnelTransition = NativeWorldKnownBool::Unknown;
    NativeWorldKnownBool UsesCollision = NativeWorldKnownBool::Unknown;
    NativeWorldKnownBool IsBigBuilding = NativeWorldKnownBool::Unknown;
    NativeWorldKnownBool InNormalBuildingSector = NativeWorldKnownBool::Unknown;
    NativeWorldKnownBool InWorld = NativeWorldKnownBool::Unknown;
};
struct NativeWorldEntityMetadata {
    NativeWorldInfoStatus Status = NativeWorldInfoStatus::NotLoaded;
    const NativeWorldModelInfo* Model{};
    const NativeWorldPlacementInfo* Placement{};
    // Initial class-only mask. True is NOT a completed ground-query eligibility proof.
    NativeWorldKnownBool InitialBuildingMask() const;
};
struct NativeWorldEntitySourceText {
    std::string Source, Text;
};

class NativeWorldEntityInfo {
public:
    // Load during exclusive startup ownership; publish const afterward. Atomic on failure.
    // Reads default.dat/gta.dat, their ordered IDE declarations before first IPL, Object.dat.
    bool LoadBeforeWorker(const char* gameDir, const NativeCollisionPopulation& population, std::string& error);
    // Pure fixture/import boundary. ideSources must be the COMPLETE ordered IDE lookup
    // namespace (including cars/peds/weap/hier), not just the population's static subset.
    // Ambiguous Object.dat CRC keys are rejected rather than guessing the global search cursor.
    // IncludesStreamed certifies StreamPager's full-word text provenance; legacy
    // imports without that contract leave authored text bits explicitly Unknown.
    bool LoadSources(const NativeCollisionPopulation& population,
                     std::span<const NativeWorldEntitySourceText> ideSources,
                     const NativeWorldEntitySourceText& objectSource, std::string& error);
    // Memory-only. Results borrow this owner and survive until its next successful Load.
    // Identity comes from IPL model ID + name + source + record + binary discriminator,
    // NEVER COL HeaderId or time-shared geometry's name. Overrides may change Position.
    NativeWorldEntityMetadata Query(const NativeCollisionPlacement& placement) const;
    const NativeWorldModelInfo* FindModel(int modelId, std::string_view name) const;
    const std::map<int, NativeWorldModelInfo>& Models() const { return m_Models; }
    size_t PlacementCount() const { return m_Placements.size(); }
private:
    using PlacementKey = std::tuple<std::string, uint32_t, bool>;
    bool m_Loaded{};
    std::map<int, NativeWorldModelInfo> m_Models;
    std::map<PlacementKey, NativeWorldPlacementInfo> m_Placements;
};
