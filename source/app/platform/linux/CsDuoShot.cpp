// CsDuoShot implementation: two cutscene actors in one shared-depth frame.
// See CsDuoShot.h for the contract. Owns nothing: librw engine ownership
// stays inside CsAnim (its init is an idempotent NULL-platform init, its
// shutdown only drops its own TXD dicts); the merged scene owns decoded
// RGBA copies, so sequential inits are safe (same precedent as DuoShot and
// CrowdShot over CarPose/IfpAnim).

#include "app/platform/linux/CsDuoShot.h"

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

bool CsDuoShot_Init(const char* gameDir, WorldShotScene& scene, CsDuoShotStats& stats,
                    CsAnimStats csStats[2], char* err, std::size_t errSize) {
    stats = CsDuoShotStats{};
    for (int i = 0; i < 2; ++i) {
        csStats[i] = CsAnimStats{};
    }
    scene.meshes.clear();
    scene.images.clear();
    if (!gameDir || !gameDir[0]) {
        SetErr(err, errSize, "no game dir");
        return false;
    }
    // Fixed dialogue pair this round: the R6w smoke pose and the R6y sweet
    // pose, both at T=0.5 through the existing CsAnim lerp+slerp sampler.
    const char* wantModel[2] = { "cssmokevest", "cssweet" };
    const char* wantBank[2] = { "smoke1a", "smoke1a" };
    const char* wantAnim[2] = { "csplay", "cssweet" };
    const double wantTime[2] = { 0.5, 0.5 };
    // Fixed placement this round (symmetric pair about the shared origin,
    // logged verbatim as csOffsets, no raycast).
    const float wantOff[2][3] = { { -1.5f, 0.0f, 0.0f }, { 1.5f, 0.0f, 0.0f } };
    // Fixed duo camera: crowd relative geometry translated to the CS
    // cluster centre (see header for the probe-AABB derivation).
    stats.eye[0] = -9.0f;
    stats.eye[1] = 5.0f;
    stats.eye[2] = 2.0f;
    stats.target[0] = 0.0f;
    stats.target[1] = 2.6f;
    stats.target[2] = 0.2f;
    for (int i = 0; i < 2; ++i) {
        for (int c = 0; c < 3; ++c) {
            stats.offsets[i][c] = wantOff[i][c];
        }
    }

    WorldShotScene part[2]{};
    char partErr[512] = {};
    for (int i = 0; i < 2; ++i) {
        if (!CsAnim_Init(gameDir, wantModel[i], wantBank[i], wantAnim[i], wantTime[i], part[i],
                         csStats[i], partErr, sizeof(partErr))) {
            char msg[640];
            (void)std::snprintf(msg, sizeof(msg), "actor%d part (%s): %s", i, wantModel[i],
                                partErr);
            SetErr(err, errSize, msg);
            CsAnim_Shutdown();
            return false;
        }
        // Translate the actor by its row offset (anim space == world here).
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
    int imgCounts[2] = { 0, 0 };
    for (int i = 0; i < 2; ++i) {
        imgCounts[i] = static_cast<int>(part[i].images.size());
    }
    int imgBase = 0;
    int meshCount = 0;
    for (int i = 0; i < 2; ++i) {
        for (WorldShotImage& im : part[i].images) {
            scene.images.push_back(std::move(im));
        }
    }
    for (int i = 0; i < 2; ++i) {
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
        }
        imgBase += imgCounts[i];
    }

    // Combined bbox = union (for the log; the camera is explicit).
    bool first = true;
    for (int i = 0; i < 2; ++i) {
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

    for (int i = 0; i < 2; ++i) {
        (void)std::snprintf(stats.models[i], sizeof(stats.models[i]), "%s", csStats[i].model);
        (void)std::snprintf(stats.src[i], sizeof(stats.src[i]), "%s", csStats[i].src);
        (void)std::snprintf(stats.anims[i], sizeof(stats.anims[i]), "%s", csStats[i].anim);
        stats.times[i] = wantTime[i];
        stats.tris[i] = csStats[i].tris;
        stats.mapped[i] = csStats[i].mapped;
        stats.unmapped[i] = csStats[i].unmapped;
        stats.bones[i] = csStats[i].bones;
    }
    for (int c = 0; c < 3; ++c) {
        stats.bboxMin[c] = scene.bboxMin[c];
        stats.bboxMax[c] = scene.bboxMax[c];
    }
    // Precision-capped: "csduo:" + 2x60 + "+" fits dffName[128] with the
    // NUL, so no -Wformat-truncation (model bases are short in practice).
    (void)std::snprintf(scene.stats.dffName, sizeof(scene.stats.dffName), "csduo:%.60s+%.60s",
                        stats.models[0], stats.models[1]);
    scene.stats.triangles = stats.tris[0] + stats.tris[1];
    scene.stats.vertices = scene.stats.triangles * 3;
    return true;
}

void CsDuoShot_Shutdown() {
    CsAnim_Shutdown();
}
