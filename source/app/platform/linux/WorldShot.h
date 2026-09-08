// WorldShot: minimal SA-world rasterizer slice (R5 remainder).
// Loads one DFF (+ one TXD for stats) from /game via librw streams, flattens
// the clump to CPU triangles, and exposes them for GL drawing by the caller.
// Parsing only: no game_sa/ headers, no Win/D3D9, no GL calls here (the
// caller owns the GL context; this TU must not include <GL/gl.h> because
// librw's GL3 backend header pulls in glad instead).
#pragma once

#include <cstddef>
#include <vector>

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
    float color[3];
    int tris;
};

struct WorldShotScene {
    std::vector<WorldShotMesh> meshes;
    float bboxMin[3];
    float bboxMax[3];
    WorldShotStats stats;
};

// Loads assets relative to gameDir (e.g. "/game"). On failure returns false
// with a human-readable message in err (never prints worldshot-ok then).
bool WorldShot_Init(const char* gameDir, WorldShotScene& scene, char* err, std::size_t errSize);
void WorldShot_Shutdown();
