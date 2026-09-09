// CsDuoShot: R6z cutscene-dialogue slice (round 28).
// Composes TWO hi-poly cutscene actors in their ANPK dialogue poses —
// default cssmokevest@csplay@0.5 (the R6w pose, C=12593717684848869509) and
// cssweet@cssweet@0.5 (the R6y pose, C=6076501667886367754) — each through
// the existing CsAnim_Init path (DFF/TXD from models/cutscene.img only,
// motion from anim/cuts.img ANPK bytes only; low-poly stand-ins forbidden)
// into a single world-space triangle soup rendered in ONE shared-depth CPU
// call (TexSample_RenderCrowd, the N-actor crowd path: per-actor coverage
// bits + depth-winner owner + overlap metrics). No frame stitching, no
// procedural meshes: every vertex comes from cutscene DFF bytes via the
// existing CsAnim sampler (lerp+slerp); the only new numbers are the
// per-actor X offsets and the fixed duo camera, both logged verbatim.
//
// Placement (this round, no raycast): actors translated by
// csOffsets=(-1.5,0,0)/(+1.5,0,0) in world — a symmetric pair about the
// shared origin (the DuoShot car-at-origin convention); all Z on the flat
// z=0 plane (ground=flat, as in R6t/R6u). Camera keeps the proven crowd
// relative geometry (9 west, +2.4 Y off-axis, +1.8 above target, fov 60)
// translated to the CS cluster centre Y~=2.6 (probe AABBs this round:
// smoke [-4.20,-0.21,-3.74]-[3.00,4.08,3.17], sweet [-4.14,2.95,-2.65]-
// [3.76,5.43,2.26]): the west actor stands between the eye and the east
// actor and partially occludes it in the shared z-buffer (overlap>0 proof).
#pragma once

#include <cstddef>

#include "app/platform/linux/CsAnim.h"
#include "app/platform/linux/WorldShot.h"

struct CsDuoShotStats {
    char models[2][64]; // resolved DFF bases ("cssmokevest", "cssweet")
    char src[2][160]; // e.g. "cutscene.img:cssmokevest.dff"
    char anims[2][64]; // resolved animation names as stored
    double times[2]; // requested fractional times T (0.5 each)
    int tris[2]; // actor triangles (2704 smoke, 2088 sweet)
    int mapped[2]; // DFF bones driven by a CS sequence (56 each)
    int unmapped[2]; // bones kept at bind (5 each: tag5022..5026)
    int bones[2]; // DFF HAnim bone count (61 each)
    float offsets[2][3]; // world offsets, fixed (-1.5/0,0 / +1.5,0,0)
    int meshEnd0 = 0; // actor split: meshes [0,end0) = actor 0, rest = actor 1
    float eye[3]; // fixed duo camera eye (world)
    float target[3]; // fixed duo camera target (world)
    float bboxMin[3];
    float bboxMax[3];
};

// Loads both CS actors relative to gameDir and merges them into `scene`
// (actor meshes in order, image indices rebased). On failure returns false
// with a message in err (never csduo-ok).
bool CsDuoShot_Init(const char* gameDir, WorldShotScene& scene, CsDuoShotStats& stats,
                    CsAnimStats csStats[2], char* err, std::size_t errSize);
void CsDuoShot_Shutdown();
