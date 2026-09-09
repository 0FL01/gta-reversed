// SkinPed: skinned-character slice (R6i, round 11).
// Loads ONE skinned DFF (+ its per-model TXD) out of the IMG archives,
// runs bind-pose skinning on the CPU (anim=bind: no HAnim interpolation,
// this round draws the file pose), and exposes one world-space triangle
// soup for drawing by the caller (same WorldShotScene + TexSample orbit
// rasterizer as the world slices).
//
// Skin math follows librw's own GPU path (`gl3skin.cpp uploadSkinMatrices`,
// non-LOCALSPACEMATRICES branch, which is what SA ped DFFs carry):
//   S_i = IB_i * (W_i * inv(A))
// where IB_i = inverse-bind matrix from the RpSkin chunk bytes,
// W_i = bind-pose world LTM of hierarchy bone i (frame LTMs as streamed,
// i.e. the file pose), A = atomic frame LTM. Vertex:
//   p' = sum_k w_k * (S_{idx_k} * p)
// In a well-formed bind pose every S_i is ~identity, so p' ~ p (the T-pose
// as stored); the mean |S_i - I| is logged as binddev as proof the full
// matrix path ran. Weights/indices/bones come only from DFF bytes:
// procedural weights are forbidden (round-11 acceptance gates wsum~=1.0).
#pragma once

#include <cstddef>

#include "app/platform/linux/WorldShot.h"

struct SkinPedStats {
    char model[64]; // resolved model name (e.g. "andre" after cj fallback)
    char requested[64]; // --model value as passed (default "cj")
    char src[160]; // e.g. "gta3.img:andre.dff"
    char txd[160]; // e.g. "gta3.img:andre.txd"
    int verts = 0; // flattened verts (tris*3, same convention as WorldShot)
    int tris = 0;
    int bones = 0; // skinned bone count (sum over skinned geometries)
    int geoms = 0; // skinned geometries flattened
    int frames = 0; // clump frame-hierarchy size (skeleton carrier)
    int attached = 0; // hierarchy bones attached to frames
    int tried = 0; // fallback archive entries probed (0 on direct hit)
    int textures = 0;
    double wsum = 0.0; // mean per-vertex weight sum (must be ~= 1.0)
    double binddev = 0.0; // mean |S_i - I| over bones (bind-pose proof)
    // R6v (round 24) cutscene-archive scan accounting. Filled by
    // SkinPed_InitCs; zero/empty for the classic SkinPed_Init path.
    int cutsEntries = 0; // anim/cuts.img VER2 entry count (444, zero DFFs)
    int cutsDffs = 0; // DFF entries inside anim/cuts.img (0 by construction)
    int csEntries = 0; // models/cutscene.img VER2 entry count (634)
    int csDffs = 0; // DFF entries inside models/cutscene.img (317)
    int csTried = 0; // CS DFFs parsed in the archive-order scan
    int csSkinned = 0; // skinned DFFs among the parsed ones
    int csSkippedLo = 0; // skinned but verts<=8000 (below the hi-poly bar)
    int csPassed = 0; // skinned with verts>8000 (hi-poly candidates)
    int csIndex = -1; // VER2 archive index of the picked DFF (-1 direct hit)
    int csMaxVerts = 0; // max flattened verts over all skinned CS DFFs seen
    char csArchive[64]; // picked archive label (e.g. "cutscene.img")
    char csMaxModel[64]; // base name holding csMaxVerts
};

// Worked-example vertex for the report: stored (file) position, skinned
// (bind) position, and the raw bone indices + weights from DFF bytes.
struct SkinPedVert {
    float stored[3];
    float skinned[3];
    int bones[4];
    float weights[4];
};

// Loads the character relative to gameDir (e.g. "/game"). On failure
// returns false with a human-readable message in err (never ped-ok).
bool SkinPed_Init(const char* gameDir, const char* model, WorldShotScene& scene, SkinPedStats& stats,
                  char* err, std::size_t errSize);
// R6v (round 24) cutscene close-up: same bind-pose SkinPed path, but the
// DFF/TXD come ONLY from the cutscene archives (anim/cuts.img first by
// spec, then models/cutscene.img — the archive that actually holds the
// hi-poly CS DFFs; cuts.img carries 444 ifp/cut/dat entries and zero
// DFFs, which the scan logs explicitly instead of pretending otherwise).
// model="auto"/"" runs the archive-order hi-poly scan (first skinned DFF
// with bones>=10 and flattened verts>8000 wins; full-archive accounting
// in stats.cs*); an explicit model does a direct lookup in the CS
// archives only (a low-poly gta3/player name is an honest cs-fail, never
// a silent substitution: a low-poly stand-in for a CS close-up fails the
// round by definition).
bool SkinPed_InitCs(const char* gameDir, const char* model, WorldShotScene& scene, SkinPedStats& stats,
                    char* err, std::size_t errSize);
// Copies the stored/skinned evidence vertex (index 0 of the first skinned
// geometry). Valid after a successful SkinPed_Init, until Shutdown.
bool SkinPed_SampleVert(SkinPedVert& out);
void SkinPed_Shutdown();
