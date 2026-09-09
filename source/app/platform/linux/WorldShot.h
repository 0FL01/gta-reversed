// WorldShot: minimal SA-world rasterizer slice (R5 remainder).
// Loads one DFF (+ one TXD for stats) from /game via librw streams, flattens
// the clump to CPU triangles, and exposes them for GL drawing by the caller.
// Parsing only: no game_sa/ headers, no Win/D3D9, no GL calls here (the
// caller owns the GL context; this TU must not include <GL/gl.h> because
// librw's GL3 backend header pulls in glad instead).
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>
#include <array>

// Optional native GPU metadata. Offline renderers continue using triCol.
struct WorldShotSurface {
    std::array<float, 4> color{1, 1, 1, 1};
    float ambient = 1.0f;
    float diffuse = 1.0f;
    int vehicleColorIndex = -1; // carcols index when a paint marker was resolved
    bool vehicleAlpha = false; // realtime car: authored material/vertex/texture alpha
};

struct WorldShotStats {
    char dffName[128];
    char txdName[128];
    char firstTexture[40];
    int atomics;
    int triangles;
    int vertices;
    int textures;
    int firstTexW;
    int firstTexH;
};

struct WorldShotMesh {
    // Flat triangle soup: 9 floats per triangle (pos), same layout (nrm).
    std::vector<float> pos;
    std::vector<float> nrm;
    // Per-vertex UVs (6 floats per triangle); empty when geometry has none.
    std::vector<float> uv;
    // Per-triangle texture image index into WorldShotScene::images:
    // >=0 decoded TXD texels, -1 untextured by design, -2 wanted but missing.
    std::vector<int> triImg;
    // Per-triangle material color (3 floats, 0..1).
    std::vector<float> triCol;
    float color[3];
    int tris;
    // Triangle-soup RGBA8, 12 bytes/triangle; night empty means use day.
    // Missing prelight is black for lit geometry, white for unlit geometry
    // (RenderWare semantics), never a replacement authored color.
    std::vector<uint8_t> dayColors, nightColors;
    std::vector<WorldShotSurface> surfaces; // one per triangle, or empty
};

// One decoded TXD texture: RGBA8 bytes straight from TXD raster bytes.
struct WorldShotImage {
    char name[32];
    int w;
    int h;
    uint32_t filter; // DFF material filterAddressing (wrap modes)
    std::vector<uint8_t> rgba; // w*h*4, top row first (librw lock order)
};

struct WorldShotScene {
    std::vector<WorldShotMesh> meshes;
    std::vector<WorldShotImage> images; // decoded TXD texels (flatten fills)
    float bboxMin[3];
    float bboxMax[3];
    WorldShotStats stats;
};

// Loads assets relative to gameDir (e.g. "/game"). On failure returns false
// with a human-readable message in err (never prints worldshot-ok then).
bool WorldShot_Init(const char* gameDir, WorldShotScene& scene, char* err, std::size_t errSize);
void WorldShot_Shutdown();
