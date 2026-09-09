// DuoShot: R6t meeting slice (round 22).
// Composes ONE car (CarPose DFF, steer=spin=0, origin yaw=0) and ONE ped
// (IfpAnim DFF in the exact --shot-anim pose IDLE_stance@0.5, legacy single
// key, interp=false) into a single world-space triangle soup rendered in
// ONE shared-depth CPU call (TexSample_RenderDuo). No frame stitching, no
// procedural meshes: every vertex comes from DFF bytes via the existing
// CarPose_Init / IfpAnim_Init paths; the only new numbers are the car-space
// ped offset and the fixed duo camera, both logged verbatim.
//
// Placement (this round, no raycast): car at origin yaw=0; ped translated
// by pedOffset=(-2.2,0.5,0) in car space; both Z on the flat z=0 plane
// (ground=flat). Camera is fixed so the ped partially occludes the car
// (overlap>0 via the shared z-buffer).
#pragma once

#include <cstddef>

#include "app/platform/linux/CarPose.h"
#include "app/platform/linux/IfpAnim.h"
#include "app/platform/linux/WorldShot.h"

struct DuoShotStats {
    char car[64]; // resolved car model (e.g. "landstal")
    char ped[64]; // resolved ped model (e.g. "andre")
    char carSrc[160]; // e.g. "gta3.img:landstal.dff"
    char pedSrc[160]; // e.g. "gta3.img:andre.dff"
    int carTris = 0; // car triangles (3613 = 3049 + 4x141 on landstal)
    int pedTris = 0; // ped triangles (1544 on andre)
    int carMeshes = 0; // leading meshes in the merged scene (actor split)
    float pedOffset[3]; // car-space ped offset, fixed (-2.2,0.5,0)
    float eye[3]; // fixed duo camera eye (world)
    float target[3]; // fixed duo camera target (world)
    float bboxMin[3];
    float bboxMax[3];
};

// Loads car+ped relative to gameDir and merges them into `scene` (car
// meshes first, then ped meshes with image indices rebased). On failure
// returns false with a message in err (never duo-ok).
bool DuoShot_Init(const char* gameDir, const char* carModel, const char* pedModel,
                  WorldShotScene& scene, DuoShotStats& stats, CarPoseStats& carStats,
                  IfpAnimStats& pedStats, char* err, std::size_t errSize);
void DuoShot_Shutdown();
