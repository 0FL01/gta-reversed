// SceneShot: multi-model SA-world scene slice (R6a, bridge to R6).
// Parses IDE/IPL lists from the already-loaded data/*.dat files, picks K
// static hi-res instances from real IPL records, pulls their DFFs (+ per-model
// TXDs) out of the IMG archives, places them by IPL position/quaternion, and
// exposes one merged world-space triangle soup for GL drawing by the caller.
// Parsing only: no game_sa/ headers, no Win/D3D9. Every counted triangle comes
// from DFF bytes (no procedural/synthetic scene fillers).
#pragma once

#include <cstddef>

#include "app/platform/linux/WorldShot.h"

struct SceneShotStats {
    int models = 0;
    int tris = 0;
    int verts = 0;
    int textures = 0;
    int texDicts = 0;
    int missTex = 0;
    int missDff = 0;
    int skippedSkin = 0;
    // "model@x,y,z;..." for the chosen instances (report evidence).
    char list[2048];
};

// Loads the scene relative to gameDir (e.g. "/game"). On failure returns
// false with a human-readable message in err (never prints sceneshot-ok).
bool SceneShot_Init(const char* gameDir, WorldShotScene& scene, SceneShotStats& stats, char* err,
                    std::size_t errSize);
void SceneShot_Shutdown();
