// ShoreShot implementation: pager world + water.dat sea in one scene.
// See ShoreShot.h for the contract and the shore-point derivation.

#include "app/platform/linux/ShoreShot.h"

#include <cstdio>
#include <cstring>
#include <utility>

#include "app/platform/linux/TimeCycle.h"
#include "app/platform/linux/WaterLevel.h"

namespace {

// Fixed shore center: gaz_pier2 IPL position (LAw.ipl:
// "6188, gaz_pier2, 0, 836.3125, -1866.757813, ..."), rounded to integers
// for a stable log. It lies inside water.dat quad x[736,904] y[-1896,-1864]
// (row "736.0 -1896.0 ... 904.0 -1896.0 ... 736.0 -1864.0 ...", center
// 820,-1880, 21m away), so the pager window (R=300) and the sea overlap.
constexpr float kShoreCenterX = 836.0f;
constexpr float kShoreCenterY = -1866.0f;

// Fixed shore camera: eye over the sea SW of the pier (water quad
// x[592,976] y[-2112,-1896] covers 746,-1986), looking NE-down at the pier.
// Foreground = WaterRGBA sea, background = pier + beach world, one pass.
constexpr float kEye[3] = { 746.0f, -1986.0f, 55.0f };
constexpr float kTarget[3] = { 836.0f, -1866.0f, 0.0f };

void SetErr(char* err, std::size_t errSize, const char* msg) {
    if (!err || errSize == 0) {
        return;
    }
    (void)std::snprintf(err, errSize, "%s", msg ? msg : "unknown");
}

} // namespace

bool ShoreShot_Init(const char* gameDir, int hour, WorldShotScene& scene, ShoreShotStats& stats,
                    E2ELoadInfo& loadInfo, E2EPagerFrame& pagerFrame, char* err,
                    std::size_t errSize) {
    return ShoreShot_InitWeather(gameDir, "EXTRASUNNY_LA", hour, scene, stats, loadInfo,
                                 pagerFrame, err, errSize);
}

bool ShoreShot_InitWeather(const char* gameDir, const char* weather, int hour,
                           WorldShotScene& scene, ShoreShotStats& stats, E2ELoadInfo& loadInfo,
                           E2EPagerFrame& pagerFrame, char* err, std::size_t errSize) {
    stats = ShoreShotStats{};
    loadInfo = E2ELoadInfo{};
    pagerFrame = E2EPagerFrame{};
    scene.meshes.clear();
    scene.images.clear();
    if (!gameDir || !gameDir[0]) {
        SetErr(err, errSize, "no game dir");
        return false;
    }
    if (hour < 0 || hour > 23) {
        SetErr(err, errSize, "bad hour (want 0-23)");
        return false;
    }
    stats.centerX = kShoreCenterX;
    stats.centerY = kShoreCenterY;
    stats.hour = hour;
    for (int c = 0; c < 3; ++c) {
        stats.eye[c] = kEye[c];
        stats.target[c] = kTarget[c];
    }
    (void)std::snprintf(stats.waterFile, sizeof(stats.waterFile), "data/water.dat");

    // 1. World: existing pager path centered on the pier.
    WorldShotScene world{};
    {
        char perr[512] = {};
        if (!StreamPager_Init(gameDir, loadInfo, perr, sizeof(perr))) {
            char msg[640];
            (void)std::snprintf(msg, sizeof(msg), "pager-init: %s", perr);
            SetErr(err, errSize, msg);
            StreamPager_Shutdown();
            return false;
        }
        if (!StreamPager_Update(stats.centerX, stats.centerY, 0.0f, world, pagerFrame, perr,
                                sizeof(perr))) {
            char msg[640];
            (void)std::snprintf(msg, sizeof(msg), "pager-update: %s", perr);
            SetErr(err, errSize, msg);
            StreamPager_Shutdown();
            return false;
        }
    }
    stats.models = pagerFrame.instances;
    stats.mtris = pagerFrame.tris;

    // 2. Water color: existing timecyc path (named section, --hour).
    // R6am: the section row comes from TimeCycle_LoadWeatherHour (the same
    // exact-token path as --shot-scene --weather W --hour H); every RGB
    // below is still timecyc bytes only.
    TimeCycleParams tcp{};
    {
        char terr[256] = {};
        if (!TimeCycle_LoadWeatherHour(gameDir, weather ? weather : "EXTRASUNNY_LA", hour,
                                       tcp, terr, sizeof(terr))) {
            char msg[384];
            (void)std::snprintf(msg, sizeof(msg), "timecyc: %s", terr);
            SetErr(err, errSize, msg);
            StreamPager_Shutdown();
            return false;
        }
    }
    for (int c = 0; c < 4; ++c) {
        stats.waterRGBA[c] = tcp.water[c];
    }
    // Round 42 (R6an): keep the whole row for the fog env (timecyc bytes
    // only; the render call sites build TexTimeEnv from here).
    for (int c = 0; c < 3; ++c) {
        stats.tcAmb[c] = tcp.amb[c];
        stats.tcDir[c] = tcp.dir[c];
        stats.tcSkyTop[c] = tcp.skyTop[c];
        stats.tcSkyBot[c] = tcp.skyBot[c];
    }
    stats.tcFarClp = tcp.farClp;
    stats.tcFogSt = tcp.fogSt;

    // 3. Water geometry: existing WaterLevel path (water.dat only).
    WaterLevelData water{};
    WorldShotScene waterScene{};
    int waterTris = 0;
    {
        char werr[512] = {};
        if (!WaterLevel_Load(gameDir, stats.waterFile, water, werr, sizeof(werr))) {
            char msg[640];
            (void)std::snprintf(msg, sizeof(msg), "water-load: %s", werr);
            SetErr(err, errSize, msg);
            StreamPager_Shutdown();
            WaterLevel_Shutdown();
            return false;
        }
        if (!WaterLevel_BuildScene(water, tcp.water, waterScene, waterTris, werr,
                                   sizeof(werr))) {
            char msg[640];
            (void)std::snprintf(msg, sizeof(msg), "water-build: %s", werr);
            SetErr(err, errSize, msg);
            StreamPager_Shutdown();
            WaterLevel_Shutdown();
            return false;
        }
    }
    stats.waterRows = water.rows;
    stats.waterQuads = water.quads;
    stats.waterTris = waterTris;

    // 4. Merge: world meshes first (actor 0), then water meshes (actor 1).
    // Water carries no images (flat triImg=-1), so world image indices are
    // unchanged; the rebase loop is still applied for honesty.
    const int worldImages = static_cast<int>(world.images.size());
    for (WorldShotImage& im : world.images) {
        scene.images.push_back(std::move(im));
    }
    for (WorldShotImage& im : waterScene.images) {
        scene.images.push_back(std::move(im));
    }
    for (WorldShotMesh& m : world.meshes) {
        for (int& ti : m.triImg) {
            if (ti >= 0) {
                ti += 0; // world block starts at 0; kept explicit
            }
        }
        scene.meshes.push_back(std::move(m));
    }
    stats.worldMeshes = static_cast<int>(scene.meshes.size());
    for (WorldShotMesh& m : waterScene.meshes) {
        for (int& ti : m.triImg) {
            if (ti >= 0) {
                ti += worldImages;
            }
        }
        scene.meshes.push_back(std::move(m));
    }

    // Combined bbox = union (for the log; the camera is explicit).
    bool first = true;
    for (const WorldShotScene* part : { &world, &waterScene }) {
        if (first) {
            for (int c = 0; c < 3; ++c) {
                scene.bboxMin[c] = part->bboxMin[c];
                scene.bboxMax[c] = part->bboxMax[c];
            }
            first = false;
            continue;
        }
        for (int c = 0; c < 3; ++c) {
            if (part->bboxMin[c] < scene.bboxMin[c]) {
                scene.bboxMin[c] = part->bboxMin[c];
            }
            if (part->bboxMax[c] > scene.bboxMax[c]) {
                scene.bboxMax[c] = part->bboxMax[c];
            }
        }
    }
    for (int c = 0; c < 3; ++c) {
        stats.bboxMin[c] = scene.bboxMin[c];
        stats.bboxMax[c] = scene.bboxMax[c];
    }
    (void)std::snprintf(scene.stats.dffName, sizeof(scene.stats.dffName), "shore:pager+water.dat");
    (void)std::snprintf(scene.stats.txdName, sizeof(scene.stats.txdName), "multi+WaterRGBA");
    scene.stats.atomics = stats.models;
    scene.stats.triangles = stats.mtris + stats.waterTris;
    scene.stats.vertices = scene.stats.triangles * 3;
    scene.stats.textures = 0;
    scene.stats.firstTexture[0] = '\0';
    scene.stats.firstTexW = 0;
    scene.stats.firstTexH = 0;
    return true;
}

void ShoreShot_Shutdown() {
    StreamPager_Shutdown();
    WaterLevel_Shutdown();
}
