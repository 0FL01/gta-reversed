// CrowdShot implementation: three peds in one shared-depth frame.
// See CrowdShot.h for the contract. Owns nothing: librw engine ownership
// stays inside IfpAnim (its init is an idempotent NULL-platform init, its
// shutdown only drops its own TXD dicts); the merged scene owns decoded
// RGBA copies, so sequential inits are safe.

#include "app/platform/linux/CrowdShot.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace {

void SetErr(char* err, std::size_t errSize, const char* msg) {
    if (!err || errSize == 0) {
        return;
    }
    (void)std::snprintf(err, errSize, "%s", msg ? msg : "unknown");
}

} // namespace

bool CrowdShot_Init(const char* gameDir, WorldShotScene& scene, CrowdShotStats& stats,
                    IfpAnimStats pedStats[3], char* err, std::size_t errSize) {
    stats = CrowdShotStats{};
    for (int i = 0; i < 3; ++i) {
        pedStats[i] = IfpAnimStats{};
    }
    scene.meshes.clear();
    scene.images.clear();
    if (!gameDir || !gameDir[0]) {
        SetErr(err, errSize, "no game dir");
        return false;
    }
    // Fixed trio this round (archive-order scan, logged verbatim).
    const char* wantModel[3] = { "andre", "wmybmx", "ballas1" };
    const char* wantAnim[3] = { "IDLE_stance", "WALK_civi", "IDLE_stance" };
    const double wantTime[3] = { 0.5, 0.2, 0.9 };
    const bool wantInterp[3] = { false, true, false };
    // Fixed placement this round (row along X, logged verbatim, no raycast).
    const float wantOff[3][3] = { { -2.5f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f }, { 2.5f, 0.0f, 0.0f } };
    // Fixed crowd camera: due west of the row, slightly off-axis (+Y) and
    // above (+Z), looking east down the row so actor 0 (x=-2.5) stands
    // between the eye and actors 1/2 and partially occludes them in the
    // shared z-buffer. Slight off-axis angle keeps slivers of the back
    // actors visible (each Pi>500) while guaranteeing overlap>0.
    // Correct skinning no longer stretches hands/limbs into the silhouettes.
    // Frame the actual narrower bodies closer; retain all pixel/overlap gates.
    stats.eye[0] = -8.0f;
    stats.eye[1] = 3.0f;
    stats.eye[2] = 2.0f;
    stats.target[0] = 0.0f;
    stats.target[1] = 0.0f;
    stats.target[2] = 0.2f;
    for (int i = 0; i < 3; ++i) {
        for (int c = 0; c < 3; ++c) {
            stats.offsets[i][c] = wantOff[i][c];
        }
    }

    WorldShotScene part[3]{};
    char partErr[512] = {};
    for (int i = 0; i < 3; ++i) {
        if (!IfpAnim_Init(gameDir, wantModel[i], wantAnim[i], wantTime[i], part[i], pedStats[i],
                          partErr, sizeof(partErr), wantInterp[i])) {
            char msg[640];
            (void)std::snprintf(msg, sizeof(msg), "ped%d part (%s): %s", i, wantModel[i],
                                partErr);
            SetErr(err, errSize, msg);
            IfpAnim_Shutdown();
            return false;
        }
        // Translate the actor by its row offset (world == DFF space here).
        for (WorldShotMesh& mesh : part[i].meshes) {
            for (size_t v = 0; v < mesh.pos.size(); v += 3) {
                mesh.pos[v] += stats.offsets[i][0];
                mesh.pos[v + 1] += stats.offsets[i][1];
                mesh.pos[v + 2] += stats.offsets[i][2];
            }
        }
        part[i].bboxMin[0] += stats.offsets[i][0];
        part[i].bboxMin[1] += stats.offsets[i][1];
        part[i].bboxMin[2] += stats.offsets[i][2];
        part[i].bboxMax[0] += stats.offsets[i][0];
        part[i].bboxMax[1] += stats.offsets[i][1];
        part[i].bboxMax[2] += stats.offsets[i][2];
    }

    // Merge: actor meshes in order with image indices rebased per part.
    // Image counts are captured BEFORE the move (moved-from vectors must
    // not be sized afterwards).
    int imgCounts[3] = { 0, 0, 0 };
    for (int i = 0; i < 3; ++i) {
        imgCounts[i] = static_cast<int>(part[i].images.size());
    }
    int imgBase = 0;
    int meshCount = 0;
    for (int i = 0; i < 3; ++i) {
        for (WorldShotImage& im : part[i].images) {
            scene.images.push_back(std::move(im));
        }
    }
    for (int i = 0; i < 3; ++i) {
        for (WorldShotMesh& m : part[i].meshes) {
            for (int& ti : m.triImg) {
                if (ti >= 0) {
                    ti += imgBase;
                }
            }
            scene.meshes.push_back(std::move(m));
        }
        meshCount = static_cast<int>(scene.meshes.size());
        if (i == 0) {
            stats.meshEnd0 = meshCount;
        } else if (i == 1) {
            stats.meshEnd1 = meshCount;
        }
        imgBase += imgCounts[i];
    }

    // Combined bbox = union (for the log; the camera is explicit).
    bool first = true;
    for (int i = 0; i < 3; ++i) {
        if (first) {
            for (int c = 0; c < 3; ++c) {
                scene.bboxMin[c] = part[i].bboxMin[c];
                scene.bboxMax[c] = part[i].bboxMax[c];
            }
            first = false;
            continue;
        }
        for (int c = 0; c < 3; ++c) {
            if (part[i].bboxMin[c] < scene.bboxMin[c]) {
                scene.bboxMin[c] = part[i].bboxMin[c];
            }
            if (part[i].bboxMax[c] > scene.bboxMax[c]) {
                scene.bboxMax[c] = part[i].bboxMax[c];
            }
        }
    }

    for (int i = 0; i < 3; ++i) {
        (void)std::snprintf(stats.models[i], sizeof(stats.models[i]), "%s", pedStats[i].model);
        (void)std::snprintf(stats.src[i], sizeof(stats.src[i]), "%s", pedStats[i].src);
        (void)std::snprintf(stats.anims[i], sizeof(stats.anims[i]), "%s", pedStats[i].anim);
        stats.times[i] = wantTime[i];
        stats.interp[i] = wantInterp[i] ? 1 : 0;
        stats.tris[i] = pedStats[i].tris;
        stats.mapped[i] = pedStats[i].mapped;
        stats.unmapped[i] = pedStats[i].unmapped;
        stats.bones[i] = pedStats[i].bones;
    }
    for (int c = 0; c < 3; ++c) {
        stats.bboxMin[c] = scene.bboxMin[c];
        stats.bboxMax[c] = scene.bboxMax[c];
    }
    (void)std::snprintf(scene.stats.dffName, sizeof(scene.stats.dffName), "crowd:%s+%s+%s",
                        stats.models[0], stats.models[1], stats.models[2]);
    scene.stats.triangles = stats.tris[0] + stats.tris[1] + stats.tris[2];
    scene.stats.vertices = scene.stats.triangles * 3;
    return true;
}

void CrowdShot_Shutdown() {
    IfpAnim_Shutdown();
}
