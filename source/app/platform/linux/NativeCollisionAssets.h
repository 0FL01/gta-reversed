// Owned source COL data. No RenderWare pointers, global loader or render residency.
#pragma once
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <vector>
#include <utility>

using NativeCollisionVector = std::array<float, 3>;
struct NativeCollisionSurface {
    uint8_t Material{}, Flags{}, Brightness{}, Light{};
};
enum class NativeCollisionPrimitive { Triangle, Sphere, Box };
struct NativeCollisionHit {
    std::string Model, Library, Ipl;
    int ModelId = -1;
    uint16_t HeaderId{};
    uint32_t Record{}, PrimitiveIndex{};
    bool Binary{}, ValidatedHeaderId{}, TimeShared{};
    NativeCollisionPrimitive Primitive = NativeCollisionPrimitive::Triangle;
    NativeCollisionSurface Surface;
};
struct NativeCollisionPlacement {
    std::string Model, Ipl;
    int ModelId = -1, Interior{}, Lod = -1;
    uint32_t Record{}, Flags{};
    bool Binary{};
    NativeCollisionVector Position{};
    // Authored IPL quaternion (inverse rotation), conjugated exactly once at binding.
    std::array<float, 4> Quaternion{0, 0, 0, 1};
};
// Source identity, never a model-wide replacement or a render-residency index.
struct NativePlacementIdentity {
    std::string Ipl, Model;
    uint32_t Record{};
    int ModelId = -1;
    bool Binary{};
    static NativePlacementIdentity From(const NativeCollisionPlacement& p) {
        return {p.Ipl, p.Model, p.Record, p.ModelId, p.Binary};
    }
    bool Matches(const NativeCollisionPlacement& p) const {
        return Record == p.Record && ModelId == p.ModelId && Binary == p.Binary && Model == p.Model && Ipl == p.Ipl;
    }
    bool operator==(const NativePlacementIdentity&) const = default;
};
struct NativePlacementOverride {
    NativePlacementIdentity Identity;
    NativeCollisionVector Position{};
    // Already-bound world basis: do not conjugate or normalize again.
    std::array<NativeCollisionVector, 3> Basis{};
    bool CollisionEnabled = true;
};
// Owned immutable initial state. No garage, catalog, pager or RW pointers.
class NativePlacementOverrides {
public:
    explicit NativePlacementOverrides(std::vector<NativePlacementOverride> entries) : m_Entries(std::move(entries)) {
        for (size_t i = 0; i < m_Entries.size(); ++i) {
            for (size_t j = 0; j < i; ++j) assert(!(m_Entries[i].Identity == m_Entries[j].Identity));
            for (auto v : m_Entries[i].Position) { assert(std::isfinite(v)); (void)v; }
            for (const auto& axis : m_Entries[i].Basis) for (auto v : axis) { assert(std::isfinite(v)); (void)v; }
        }
    }
    std::span<const NativePlacementOverride> Entries() const { return m_Entries; }
    const NativePlacementOverride* Find(const NativeCollisionPlacement& p) const {
        for (const auto& entry : m_Entries) if (entry.Identity.Matches(p)) return &entry;
        return nullptr;
    }
private:
    const std::vector<NativePlacementOverride> m_Entries;
};
struct NativeCollisionIde {
    std::string Name;
    bool TimeModel{};
};
struct NativeCollisionPopulation {
    std::map<int, NativeCollisionIde> Models;
    std::vector<NativeCollisionPlacement> Instances;
    bool IncludesStreamed{};
};
struct NativeCollisionModel {
    struct Sphere { NativeCollisionVector Center{}; float Radius{}; NativeCollisionSurface Surface; };
    struct Box { NativeCollisionVector Min{}, Max{}; NativeCollisionSurface Surface; };
    struct Face { std::array<uint32_t, 3> Vertices{}; NativeCollisionSurface Surface; };
    struct FaceGroup { NativeCollisionVector Min{}, Max{}; uint16_t First{}, Last{}; };
    std::string Name, Library, Unsupported;
    uint16_t HeaderId{};
    uint32_t Flags{}, Version{}, ChunkOffset{};
    bool ValidatedHeaderId{}, Empty{};
    NativeCollisionVector Min{}, Max{}, BoundCenter{};
    float BoundRadius{};
    std::vector<Sphere> Spheres;
    std::vector<Box> Boxes;
    std::vector<NativeCollisionVector> Vertices;
    std::vector<Face> Faces;
    std::vector<FaceGroup> FaceGroups; // authored broadphase ranges/order; never inferred from geometry
    // Preserve all source metadata (face groups, shadow mesh, planes, V4 word).
    // Shadow mesh is rendering data, never substituted for collision faces.
    std::vector<uint8_t> SourceChunk;
};
// Narrow read-only lookup from the existing immutable COL catalog. No new
// parser or authority: Ready requires supported non-empty geometry, Empty
// preserves the authored empty flag, KnownAbsent means no chunk for the
// lowercased name, Unsupported preserves the parser failure with the
// retained model and error. TimeShared mirrors Snapshot's FirstTime
// partner preference. Before any successful Load every query is
// Unsupported, never KnownAbsent; empty/invalid input stays Unsupported.
enum class NativeCollisionModelStatus { Ready, Empty, KnownAbsent, Unsupported };
struct NativeCollisionModelLookup {
    NativeCollisionModelStatus Status = NativeCollisionModelStatus::Unsupported;
    std::shared_ptr<const NativeCollisionModel> Model;
    bool TimeShared{};
    std::string Error;
};
struct NativeCollisionInstance {
    // Identity/quaternion remain source provenance; Position is effective.
    // Basis below is the authoritative world rotation for every COL consumer.
    NativeCollisionPlacement Placement;
    std::shared_ptr<const NativeCollisionModel> Model;
    std::array<NativeCollisionVector, 3> Basis{};
    NativeCollisionVector Min{}, Max{};
    bool TimeShared{};
};
struct NativeCollisionSnapshot {
    std::shared_ptr<const NativePlacementOverrides> Overrides;
    std::vector<NativeCollisionInstance> Instances;
    // Census over the requested interior's complete population, not just the
    // window: missing assets have no trustworthy geometry bounds to window by.
    size_t MissingModels{}, EmptyModels{}, InteriorExcluded{}, TimeShared{};
    // Names absent from the complete source catalog. No invented default boxes.
    std::map<std::string, size_t> KnownAbsence;
};
struct NativeCollisionAssetStats {
    size_t Models{}, Spheres{}, Boxes{}, Triangles{}, Empty{}, Unsupported{}, HeaderNameFallback{};
    std::vector<std::string> UnsupportedModels;
};
class NativeCollisionAssets {
public:
    // Explicit absolute OS_File paths; immutable assets can be shared by workers.
    bool Load(const char* gameDir, const NativeCollisionPopulation& population, std::string& error);
    bool Snapshot(const NativeCollisionPopulation& population, float x, float y, float radius,
                  NativeCollisionSnapshot& out, std::string& error, int interior = 0,
                  std::shared_ptr<const NativePlacementOverrides> overrides = {}) const;
    const NativeCollisionAssetStats& Stats() const { return m_Stats; }
    // Pure, bounded parser for source-format verification. Atomic on failure.
    static bool Parse(std::span<const uint8_t> chunk, const std::string& library,
                      NativeCollisionModel& out, std::string& error);
    // Pure, bounded, no IO. Lowercases the query; never invents geometry.
    NativeCollisionModelLookup LookupModel(const std::string& name) const;
private:
    using ModelMap = std::map<std::string, std::shared_ptr<const NativeCollisionModel>>;
    // Shared read-only resolution: exact Snapshot keyLower + TimePartners
    // preference. No counting, errors, identity validation, files or caches.
    std::pair<ModelMap::const_iterator, bool> ResolveModel(const std::string& lowerKey) const;
    ModelMap m_Models;
    std::map<std::string, std::string> m_TimePartners;
    NativeCollisionAssetStats m_Stats;
};

// Created once after pager initialization, before the worker starts. Every
// owner shares this const context; window builds never reopen the COL catalog.
struct NativeCollisionContext {
    NativeCollisionPopulation Population;
    NativeCollisionAssets Assets;
    float Radius{};
    static std::shared_ptr<const NativeCollisionContext> LoadBeforeWorker(
        const char* gameDir, float radius, std::string& error);
    bool Snapshot(float x, float y, NativeCollisionSnapshot& out, std::string& error,
                  std::shared_ptr<const NativePlacementOverrides> overrides = {}) const {
        return Assets.Snapshot(Population, x, y, Radius, out, error, 0, std::move(overrides));
    }
};
