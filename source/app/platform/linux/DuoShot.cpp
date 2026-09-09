// DuoShot implementation: car+ped meeting in one shared-depth frame.
// See DuoShot.h for the contract. Own nothing: librw engine ownership
// stays inside CarPose/IfpAnim (their inits are idempotent NULL-platform
// inits, their shutdowns only drop their own TXD dicts); the merged scene
// owns decoded RGBA copies, so sequential inits are safe.

#include "app/platform/linux/DuoShot.h"

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

bool DuoShot_Init(const char* gameDir, const char* carModel, const char* pedModel,
                  WorldShotScene& scene, DuoShotStats& stats, CarPoseStats& carStats,
                  IfpAnimStats& pedStats, char* err, std::size_t errSize) {
    stats = DuoShotStats{};
    carStats = CarPoseStats{};
    pedStats = IfpAnimStats{};
    scene.meshes.clear();
    scene.images.clear();
    if (!gameDir || !gameDir[0]) {
        SetErr(err, errSize, "no game dir");
        return false;
    }
    std::string wantCar = carModel && carModel[0] ? carModel : "landstal";
    std::string wantPed = pedModel && pedModel[0] ? pedModel : "andre";

    // Fixed placement this round (logged verbatim, no raycast).
    stats.pedOffset[0] = -2.2f;
    stats.pedOffset[1] = 0.5f;
    stats.pedOffset[2] = 0.0f;
    // Fixed duo camera: due west of the pair on the ped side, looking east
    // so the ped (x~-2.2) stands between the eye and the car (x~0) and
    // partially occludes it in the shared z-buffer.
    stats.eye[0] = -9.0f;
    stats.eye[1] = 0.5f;
    stats.eye[2] = 1.8f;
    stats.target[0] = 0.5f;
    stats.target[1] = 0.3f;
    stats.target[2] = 0.0f;

    // 1. Car via the existing R6l path (steer=spin=0, origin yaw=0).
    WorldShotScene carScene{};
    CarPoseAudit audit{};
    char carErr[512] = {};
    if (!CarPose_Init(gameDir, wantCar.c_str(), 0.0, 0.0, carScene, carStats, audit, carErr,
                       sizeof(carErr))) {
        char msg[640];
        (void)std::snprintf(msg, sizeof(msg), "car part: %s", carErr);
        SetErr(err, errSize, msg);
        return false;
    }
    if (!carStats.chassisSame) {
        CarPose_Shutdown();
        SetErr(err, errSize, "car part moved body during wheel pose");
        return false;
    }

    // 2. Ped via the existing R6j path (IDLE_stance@0.5, legacy single key).
    WorldShotScene pedScene{};
    char pedErr[512] = {};
    if (!IfpAnim_Init(gameDir, wantPed.c_str(), "IDLE_stance", 0.5, pedScene, pedStats, pedErr,
                       sizeof(pedErr), false)) {
        char msg[640];
        (void)std::snprintf(msg, sizeof(msg), "ped part: %s", pedErr);
        SetErr(err, errSize, msg);
        CarPose_Shutdown();
        IfpAnim_Shutdown();
        return false;
    }

    // 3. Translate the ped by pedOffset (car space == world, yaw=0).
    for (WorldShotMesh& mesh : pedScene.meshes) {
        for (size_t i = 0; i < mesh.pos.size(); i += 3) {
            mesh.pos[i] += stats.pedOffset[0];
            mesh.pos[i + 1] += stats.pedOffset[1];
            mesh.pos[i + 2] += stats.pedOffset[2];
        }
    }
    pedScene.bboxMin[0] += stats.pedOffset[0];
    pedScene.bboxMin[1] += stats.pedOffset[1];
    pedScene.bboxMin[2] += stats.pedOffset[2];
    pedScene.bboxMax[0] += stats.pedOffset[0];
    pedScene.bboxMax[1] += stats.pedOffset[1];
    pedScene.bboxMax[2] += stats.pedOffset[2];

    // 4. Merge: car meshes first (actor 0), ped meshes after (actor 1);
    // ped image indices rebased by the car image count.
    const int carImgs = static_cast<int>(carScene.images.size());
    scene.images.reserve(carScene.images.size() + pedScene.images.size());
    for (WorldShotImage& im : carScene.images) {
        scene.images.push_back(std::move(im));
    }
    for (WorldShotImage& im : pedScene.images) {
        scene.images.push_back(std::move(im));
    }
    scene.meshes.reserve(carScene.meshes.size() + pedScene.meshes.size());
    for (WorldShotMesh& m : carScene.meshes) {
        scene.meshes.push_back(std::move(m));
    }
    const int carMeshCount = static_cast<int>(scene.meshes.size());
    for (WorldShotMesh& m : pedScene.meshes) {
        for (int& ti : m.triImg) {
            if (ti >= 0) {
                ti += carImgs;
            }
        }
        scene.meshes.push_back(std::move(m));
    }

    // 5. Combined bbox = union (for the log; the camera is explicit).
    bool first = true;
    auto grow = [&](const float* mn, const float* mx) {
        if (first) {
            for (int c = 0; c < 3; ++c) {
                scene.bboxMin[c] = mn[c];
                scene.bboxMax[c] = mx[c];
            }
            first = false;
            return;
        }
        for (int c = 0; c < 3; ++c) {
            if (mn[c] < scene.bboxMin[c]) {
                scene.bboxMin[c] = mn[c];
            }
            if (mx[c] > scene.bboxMax[c]) {
                scene.bboxMax[c] = mx[c];
            }
        }
    };
    grow(carScene.bboxMin, carScene.bboxMax);
    grow(pedScene.bboxMin, pedScene.bboxMax);

    (void)std::snprintf(stats.car, sizeof(stats.car), "%s", carStats.model);
    (void)std::snprintf(stats.ped, sizeof(stats.ped), "%s", pedStats.model);
    (void)std::snprintf(stats.carSrc, sizeof(stats.carSrc), "%s", carStats.src);
    (void)std::snprintf(stats.pedSrc, sizeof(stats.pedSrc), "%s", pedStats.src);
    stats.carTris = carStats.tris;
    stats.pedTris = pedStats.tris;
    stats.carMeshes = carMeshCount;
    for (int c = 0; c < 3; ++c) {
        stats.bboxMin[c] = scene.bboxMin[c];
        stats.bboxMax[c] = scene.bboxMax[c];
    }
    (void)std::snprintf(scene.stats.dffName, sizeof(scene.stats.dffName), "duo:%s+%s", stats.car,
                         stats.ped);
    scene.stats.triangles = stats.carTris + stats.pedTris;
    scene.stats.vertices = scene.stats.triangles * 3;
    return true;
}

void DuoShot_Shutdown() {
    CarPose_Shutdown();
    IfpAnim_Shutdown();
}
