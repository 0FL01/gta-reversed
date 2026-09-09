// StreamPager: R6b streaming pager over the parsed IPL world.
// Parses IDE/IPL lists from data/*.dat (same files/order as SceneShot),
// indexes filtered static instances (interior==0, no LOD*, no anim/skinned)
// in a deterministic x/y grid (cell 300m), and serves a bounded camera
// window: instances within radius R=300m, cells evicted past R+hysteresis.
// Every rendered triangle still comes from DFF bytes via librw (CachedModel);
// the pager only bounds *which* IPL records are resident. No GL here.
#pragma once

#include <cstddef>

#include "app/platform/linux/WorldShot.h"

struct E2ELoadInfo {
    int binaryIplFiles = 0;
    int binaryInstances = 0;
    int iplTotal = 0; // all parsed inst records (unfiltered)
    int iplKept = 0; // static outdoor instances entering the grid
    int ideModels = 0;
    int ideFiles = 0;
    int iplFiles = 0;
};

struct StreamPagerOptions {
    // Offline fixtures deliberately retain their original small text-IPL slice.
    bool includeStreamed = false;
    float radius = 300.0f;
    int maxInstances = 80;
};

struct E2EPagerFrame {
    int instances = 0; // placed instances in the returned scene
    int modelsUnique = 0; // distinct DFF models referenced by them
    int tris = 0;
    int verts = 0;
    int activeCells = 0;
    int loadedCells = 0; // cells paged in on this update
    int evictedCells = 0; // cells paged out on this update
    int cacheModels = 0; // resident DFF models after update
    int texDicts = 0; // resident non-empty TXDs after update
    int fallback = 0; // 1 when the R-window was too sparse and widened
    int evictedShown = 0; // entries filled below (<= 16)
    int evictedCX[16];
    int evictedCY[16];
    int evictedDist[16]; // rect distance (m) that crossed R+hysteresis
};

// Loads and indexes the world relative to gameDir. False => err message.
bool StreamPager_Init(const char* gameDir, E2ELoadInfo& info, char* err, std::size_t errSize,
                      const StreamPagerOptions& options = {});
// Rebuilds `scene` (world-space soup, deterministic order) around the
// camera ground position. False => err message (never silent world-ok).
bool StreamPager_Update(float camX, float camY, float camZ, WorldShotScene& scene, E2EPagerFrame& frame,
                        char* err, std::size_t errSize);
void StreamPager_Counters(int& sectorsLoaded, int& sectorsEvicted, int& modelsPeak, int& trisPeak);
void StreamPager_Shutdown();
