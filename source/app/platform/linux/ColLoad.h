// ColLoad: SA world collision slice (R6g, ground under feet).
// Finds every world COL blob (loose `models/coll/*.col` files plus every
// `*.col` entry of the world IMG archives, all through OS_File*), parses
// the COL chunks (COLL/COL2/COL3/COL4 layout per game_sa/Collision as the
// format spec, reimplemented standalone: no game_sa/ linkage), binds COL
// models to text-IPL instances by model name (same DAT order, same
// interior==0 / no-LOD rules and the same CMatrix::SetRotate(quat) basis
// as SceneShot), and answers vertical ground probes. Every reported
// height comes from COL bytes on disk; there is no fallback constant.
#pragma once

#include <cstddef>

struct ColProbeHit {
    double x = 0.0;
    double y = 0.0;
    double h = -50.0; // world ground height, or -50.0 when nothing is hit
    char model[32]; // COL model name of the hit, or "-" on miss
    char prim[8]; // "sphere", "box", "tri", or "none"
    char near[32]; // nearest bound IPL instance model name
    double nearDist = 0.0; // XY distance (m) to that instance
};

struct ColLoadStats {
    int files = 0; // parsed .col blobs (loose + IMG entries)
    int models = 0; // COL models kept (finite bounds, usable volumes)
    long spheres = 0;
    long boxes = 0;
    long verts = 0;
    long tris = 0;
    int instances = 0; // bound IPL instances placed in the world
};

// Loads and binds the world relative to gameDir (e.g. "/game"). False =>
// human-readable message in err (never prints coll-ok).
bool ColLoad_Init(const char* gameDir, ColLoadStats& stats, char* err, std::size_t errSize);
// Vertical raycast at (x, y) from z=+500 down to z=-50. Always succeeds;
// a miss reports h=-50.0 with prim="none". Deterministic: instances are
// scanned in IPL DAT order with strict best-z replacement.
void ColLoad_Probe(double x, double y, ColProbeHit& hit);
void ColLoad_Shutdown();
