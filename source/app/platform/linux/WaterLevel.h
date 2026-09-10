// WaterLevel: real sea water from data/water.dat / data/water1.dat for the
// Linux native track (R6aa round 29, R6ab round 30). Parses the shipped
// water mesh through OS_File* and renders the visible polys as flat
// WaterRGBA triangles from the matching data/timecyc.dat row (same --hour,
// EXTRASUNNY_LA). No game_sa/ linkage, no procedural water: every vertex
// comes from water file bytes, the color comes from timecyc bytes.
//
// File semantics (read-only reference, NOT linked):
// source/game_sa/WaterLevel.cpp CWaterLevel::LoadDataFile reads the first
// line ("processed" fails the first-vertex parse and is skipped), then per
// line 3-4 vertices of 7 floats (x y z flowX flowY bigWaves smallWaves) +
// an optional trailing flag; 4 vertices -> AddWaterLevelQuad, 3 ->
// AddWaterLevelTriangle. source/game_sa/WaterLevel.h CWaterPolygon stores
// bInvisible=(Flags&1)==0, bLimitedDepth=(Flags&2)!=0, and only
// non-invisible polys are marked to be rendered. The shipped water.dat
// holds 307 rows: 301 quads + 6 triangles; flags 1:284, 3:21, 0:2 (the two
// flag-0 rows sit at z=5.27 and are invisible).
//
// Round-30 finding (honest format correction, verified by field count):
// data/water1.dat holds 267 rows (261 quads + 6 triangles) and carries NO
// trailing flag column on ANY row (every row is exactly 4x7=28 or 3x7=21
// floats; water.dat rows are 29/22 fields). The game code (`liness >>
// flags` on the empty remainder) would leave flags=0, i.e. the whole file
// would count as invisible. For the render slice a flagless row of
// water1.dat counts as visible (flags default 1 for that file only): the
// vertices are still 100% file bytes, only the absent visibility bit
// defaults to on. This is logged loudly via WaterLevelData::noflag (0 for
// water.dat, 267 for water1.dat), so the two files are provably distinct.
// water.dat never takes the default path (307/307 rows carry flags), hence
// its pixels stay bit-identical.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "app/platform/linux/WorldShot.h"

struct WaterVert {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    // Authored water.dat parameters; the legacy flat renderer ignores these.
    float flowX = 0.0f, flowY = 0.0f;
    float bigWaves = 0.0f, smallWaves = 0.0f;
};

struct WaterPoly {
    WaterVert v[4];
    int nverts = 0; // 3 (triangle row) or 4 (quad row)
    uint32_t flags = 0; // trailing flag (file default when absent, see below)
    bool hasFlag = false; // true when the row carried a trailing flag token
    bool Visible() const { return (flags & 1u) != 0u; }
};

struct WaterLevelData {
    std::vector<WaterPoly> polys; // every parsed data row, file order
    int rows = 0; // data rows parsed (== polys.size())
    int quads = 0; // rows with 4 vertices
    int tris = 0; // rows with 3 vertices
    int invis = 0; // rows with (flags&1)==0 (never rendered, game-faithful)
    int skipped = 0; // unparseable lines skipped (expected 0 on shipped file)
    int noflag = 0; // rows without a trailing flag token (0 on water.dat)
    char file[64] = {}; // loaded file label, e.g. "data/water.dat"
    float bboxMin[3] = {};
    float bboxMax[3] = {};
};

// Loads data/water.dat or data/water1.dat relative to gameDir (sets the
// OS_File path offset itself, like SceneShot/TimeCycle). waterFile must be
// exactly "data/water.dat" or "data/water1.dat" (any other value fails
// honestly). Returns false with a message in err on any failure (missing
// file, bad header, zero rows, bad waterFile name). Flagless rows keep the
// game parse (3-4 verts x 7 floats) and take the per-file default flag: 0
// for water.dat (game-faithful; never taken on the shipped file), 1 for
// water1.dat (the file omits the column; counted in noflag).
bool WaterLevel_Load(const char* gameDir, const char* waterFile, WaterLevelData& out,
                       char* err, std::size_t errSize);

// Builds a flat-shaded WorldShotScene from the VISIBLE polys only: quad
// rows are sorted by (y,x) like the game's DoVtxSortAndGetRange and split
// as a strip (s0,s1,s2)+(s1,s2,s3) — a fan over the raw file order would
// overlap itself on the file's BL,BR,TL,TR "Z" order; triangle rows become
// 1 triangle (order-independent coverage).
// Normals are straight up (0,0,1); UVs stay empty and triImg is -1 so the
// proven TexSample flat path paints material color (flatTri/flatPixels).
// triCol carries the WaterRGBA bytes PRE-DIVIDED by the legacy flat shade
// for an up normal (WaterLevel_LegacyShadeUp, same float ops as the
// rasterizer's own light), so the pipeline reproduces the exact timecyc
// bytes on screen; WaterLevel_CountExact then proves it per pixel.
// Returns false with a message in err when nothing visible was found.
bool WaterLevel_BuildScene(const WaterLevelData& data, const uint8_t waterRGBA[4],
                           WorldShotScene& scene, int& trisOut, char* err,
                           std::size_t errSize);

// The exact legacy flat-shade factor the TexSample CPU rasterizer applies
// to an up-facing triangle with a null timecyc env: light =
// normalize(0.45,-0.55,0.70) (RenderScene), shade = 0.32+0.68*max(NdotL,0)
// (SubmitTri). Replicated here with identical float ops for the triCol
// calibration above; the on-screen proof is the exact-byte counter, not
// this value.
float WaterLevel_LegacyShadeUp();

// Counts pixels exactly equal to (r,g,b) in a bottom-up RGBA frame.
long WaterLevel_CountExact(const std::vector<uint8_t>& rgba, uint8_t r, uint8_t g,
                           uint8_t b);

void WaterLevel_Shutdown();
