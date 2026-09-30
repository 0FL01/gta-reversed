// Owned parser output, NOT a source Loaded model, actor, animation or COL owner.
#pragma once

#include "NativePlayerAssets.h"
#include "NativeWorldEntityInfo.h"
#include "WorldShot.h"

struct NativePedAssetBone {
    std::int32_t Tag = -1, Parent = -1;
    std::uint32_t Flags = 0;
    NativePlayerMatrix Local, World;
    std::uint32_t WorldMatrixFlags = 0; // Frame LTM flags, not hierarchy readiness.
};
struct NativePedAssetVertex {
    std::array<float, 3> Position{}, Normal{};
    std::array<float, 2> UV{};
    std::array<std::uint8_t, 4> Color{255, 255, 255, 255}, Bones{};
    std::array<float, 4> Weights{}; // Authored weights; no procedural replacement.
};
struct NativePedAssetMaterial {
    WorldShotSurface Surface;
    float Specular = 0;
    std::int32_t Image = -1; // Untextured material, NOT an unresolved texture.
};
struct NativePedAssetGeometry {
    std::uint32_t Flags = 0;
    std::uint8_t AtomicFlags = 0;
    float MorphRadius = 0;
    std::uint32_t HierarchyFlags = 0;
    NativePlayerMatrix AtomicWorld;
    std::vector<NativePedAssetBone> Bones;
    std::vector<NativePlayerMatrix> InverseBind;
    std::vector<NativePedAssetVertex> Vertices;
    std::vector<NativePlayerTriangle> Triangles;
    std::vector<NativePedAssetMaterial> Materials;
};
struct NativePedAssets {
    NativeWorldPedModelInfo Model;
    NativePlayerMatrix ClumpRootLocal{};
    std::uint32_t ClumpRootLocalFlags = 0;
    std::vector<NativePedAssetGeometry> Geometries;
    std::vector<WorldShotImage> Images;
};

// Caller-exclusive Started RW engine, or the sole worker after startup. Uses
// the existing IMG/TXD/DFF readers and exact declared names. No fallback ped,
// inferred TXD name, RW pointer, archive handle or global sample escapes.
// Failure preserves out. Completion does not grant streaming Loaded state:
// animation dependencies, source SetClump/hit COL and refs are separate owners.
bool NativePedAssets_Load(const char* gameDir, const NativeWorldPedModelInfo& model,
    NativePedAssets& out, std::string& error);
