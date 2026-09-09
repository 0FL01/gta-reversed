// CrowdShot: R6u crowd slice (round 23).
// Composes THREE skinned peds (default andre + wmybmx + ballas1, the first
// archive-order skinned trio of gta3.img with a full 32-bone skeleton and
// passing pose gates: andre #14662, army #14664 skipped on its 976-tri gate,
// ballas1 #14666 picked) each in its own IFP pose (default
// andre=IDLE_stance@0.5 legacy single key, wmybmx=WALK_civi@T=0.2 with
// lerp+slerp interp, ballas1=IDLE_stance@0.9 legacy single key: three
// DIFFERENT poses) into a single world-space triangle soup rendered in ONE
// shared-depth CPU call (TexSample_RenderCrowd). No frame stitching, no
// procedural meshes: every vertex comes from DFF bytes via the existing
// IfpAnim_Init path; the only new numbers are the per-actor X offsets and
// the fixed crowd camera, both logged verbatim.
//
// Placement (this round, no raycast): actors translated by
// offsets=(-2.5/0/+2.5,0,0) in world; all Z on the flat z=0 plane
// (ground=flat, as in R6t). Camera is fixed roughly along the row (+X) so
// the actors occlude each other in the shared z-buffer (overlap>0 proof).
#pragma once

#include <cstddef>

#include "app/platform/linux/IfpAnim.h"
#include "app/platform/linux/WorldShot.h"

struct CrowdShotStats {
    char models[3][64]; // resolved DFF bases (e.g. "andre")
    char src[3][160]; // e.g. "gta3.img:andre.dff"
    char anims[3][64]; // resolved animation names as stored
    double times[3]; // requested fractional times T
    int interp[3]; // 1 when sampled with lerp+slerp, 0 legacy single key
    int tris[3]; // ped triangles (andre 1544, wmybmx 1199, ballas1 1230)
    int mapped[3]; // DFF bones driven by an IFP sequence (32 each)
    int unmapped[3]; // bones kept at bind (0 each)
    int bones[3]; // DFF HAnim bone count (32 each)
    float offsets[3][3]; // world offsets, fixed (-2.5/0/+2.5,0,0)
    int meshEnd0 = 0; // actor split: meshes [0,end0) = actor 0
    int meshEnd1 = 0; // meshes [end0,end1) = actor 1, rest = actor 2
    float eye[3]; // fixed crowd camera eye (world)
    float target[3]; // fixed crowd camera target (world)
    float bboxMin[3];
    float bboxMax[3];
};

// Loads three peds relative to gameDir and merges them into `scene` (actor
// meshes in order, image indices rebased). On failure returns false with a
// message in err (never crowd-ok).
bool CrowdShot_Init(const char* gameDir, WorldShotScene& scene, CrowdShotStats& stats,
                    IfpAnimStats pedStats[3], char* err, std::size_t errSize);
void CrowdShot_Shutdown();
