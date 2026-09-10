// Owned source COL data. No RenderWare pointers, global loader or render residency.
#pragma once
#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <vector>

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
    // Preserve all source metadata (face groups, shadow mesh, planes, V4 word).
    // Shadow mesh is rendering data, never substituted for collision faces.
    std::vector<uint8_t> SourceChunk;
};
struct NativeCollisionInstance {
    NativeCollisionPlacement Placement;
    std::shared_ptr<const NativeCollisionModel> Model;
    std::array<NativeCollisionVector, 3> Basis{};
    NativeCollisionVector Min{}, Max{};
    bool TimeShared{};
};
struct NativeCollisionSnapshot {
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
                  NativeCollisionSnapshot& out, std::string& error, int interior = 0) const;
    const NativeCollisionAssetStats& Stats() const { return m_Stats; }
    // Pure, bounded parser for source-format verification. Atomic on failure.
    static bool Parse(std::span<const uint8_t> chunk, const std::string& library,
                      NativeCollisionModel& out, std::string& error);
private:
    std::map<std::string, std::shared_ptr<const NativeCollisionModel>> m_Models;
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
    bool Snapshot(float x, float y, NativeCollisionSnapshot& out, std::string& error) const {
        return Assets.Snapshot(Population, x, y, Radius, out, error);
    }
};
