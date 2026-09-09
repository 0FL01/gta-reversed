// ShoreShot: R6ac coastal composition slice (round 31).
// Composes ONE frame from the existing StreamPager world path (IPL/DFF/TXD
// bytes around a fixed shore center) and the existing WaterLevel path
// (water.dat quads as flat WaterRGBA triangles) in a SINGLE shared-depth
// CPU call (TexSample_RenderDuo: world meshes = actor 0, water meshes =
// actor 1). No frame stitching, no procedural meshes/quads: every world
// vertex comes from DFF bytes via StreamPager, every water vertex comes
// from water.dat bytes via WaterLevel, the water color comes from the
// matching data/timecyc.dat row (--hour, EXTRASUNNY_LA).
//
// Shore point (verified on site, NOT the brief's x range): the brief
// suggested x~-2700..-2900 y~-1600..-600, but that rectangle holds only
// LOD*/interior!=0 records (countryS/seabed: lod_vbg_fir_co,
// lod_redwoodgrp, lodseabed*, all filtered by the pager) plus open-sea
// water quads with no loadable world nearby. The real Santa Monica Pier
// shore with pier-in-water is at x~800 y~-1860: IPL `gaz_pier2`
// (836.31,-1866.76, LAw.ipl, interior 0) sits INSIDE water.dat quad
// x[736,904] y[-1896,-1864] (center 820,-1880, distance 21m), with
// `gaz_pier1`, `Beach01_LAw2`, roads/canals/trees around. Hence
// shoreCenter=836,-1866 (logged verbatim).
#pragma once

#include <cstddef>
#include <cstdint>

#include "app/platform/linux/StreamPager.h"
#include "app/platform/linux/WorldShot.h"

struct ShoreShotStats {
    float centerX = 0.0f; // fixed pager center (shoreCenter, world units)
    float centerY = 0.0f;
    int models = 0; // pager-placed instances (M, want >= 4)
    int mtris = 0; // pager triangles (T, want > 2000)
    int waterRows = 0; // water.dat data rows parsed (want 307)
    int waterQuads = 0; // water.dat quad rows (want 301, Qw > 0 gate)
    int waterTris = 0; // visible water triangles merged (want 604)
    int worldMeshes = 0; // leading meshes in the merged scene (actor split)
    float eye[3] = {}; // fixed shore camera eye (world)
    float target[3] = {}; // fixed shore camera target (world)
    char waterFile[64] = {}; // always "data/water.dat" this round
    int hour = 12; // requested clock hour 0-23 (water color only)
    uint8_t waterRGBA[4] = {}; // WaterRGBA bytes of the timecyc row
    float bboxMin[3] = {};
    float bboxMax[3] = {};
};

// Loads the shore composition relative to gameDir (e.g. "/game"): pager
// world around the fixed center + water.dat sea colored by --hour. Merges
// world meshes first, then water meshes (image indices rebased; water has
// no images so world indices are unchanged). On failure returns false with
// a human-readable message in err (never prints shore-ok).
bool ShoreShot_Init(const char* gameDir, int hour, WorldShotScene& scene, ShoreShotStats& stats,
                    E2ELoadInfo& loadInfo, E2EPagerFrame& pagerFrame, char* err,
                    std::size_t errSize);
void ShoreShot_Shutdown();
