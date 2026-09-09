// WalkSim: distance-bound ped walk slice (R6o, round 17).
// Composition of the verified slices (no new physics, no wall-clock):
//   - IfpAnim : ped DFF + per-model TXD + lerp+slerp walk pose at a
//               distance phase (phase = distTravelled / strideLen mod 1);
//   - ColLoad/Collide : vertical raycast ground heights from COL bytes;
//   - StreamPager : grid pager world around the ped (bounded window);
//   - chase-cam : eye = ped - forward*D + up*H (D/H fixed, logged).
// Every length/angle/phase comes from DFF/COL/IFP/path bytes:
//   strideLen = Root travel per WALK_civi cycle (world distance of the
//   tag-0 bone between T=0 and T=1, read at runtime via IfpAnim_Seq);
//   footMinZ = phase-0 aabbAnim min-Z (lowest point of the walk pose at
//   origin); pedZ = groundH - footMinZ (feet touch ground at phase 0);
//   yawBody = yawPath - pi/2 (model +Y forward, same as the walk travel).
#pragma once

#include <cstddef>
#include <utility>
#include <vector>

#include "app/platform/linux/WorldShot.h"
#include "app/platform/linux/StreamPager.h"
#include "app/platform/linux/IfpAnim.h"

struct WalkClip {
    char model[64]; // resolved DFF base (e.g. "andre")
    char anim[64]; // resolved anim name as stored (e.g. "WALK_civi")
    char src[160]; // e.g. "gta3.img:andre.dff"
    char bankSrc[160]; // e.g. "anim/ped.ifp"
    double total = 0.0; // clip length, seconds (IFP clock)
    double strideLen = 0.0; // Root travel per cycle, meters (IFP bytes)
    float root0[3]; // tag-0 world pos at T=0
    float root1[3]; // tag-0 world pos at T=1
    float footMinZ = 0.0f; // phase-0 aabbAnim min-Z (model origin space)
    float footMaxZ = 0.0f;
    int bones = 0;
    int mapped = 0;
    double wsum = 0.0;
};

struct WalkWaypoint {
    double x = 0.0;
    double y = 0.0;
    double groundH = 0.0; // COL raycast best-z
    char groundModel[32]; // COL model of the hit
    char groundPrim[8]; // sphere/box/tri
    double pedZ = 0.0; // groundH - footMinZ (translation added to ped verts)
    double yawPath = 0.0; // path direction atan2(dy,dx), radians
    double yawBody = 0.0; // ped yaw about Z, radians (+Y forward)
    double dist = 0.0; // cumulative XY distance, meters
    double phase = 0.0; // (dist / strideLen) mod 1, IFP interp input
    double timeAbs = 0.0; // phase * clip total, seconds
};

// Clip measurement from IFP/DFF bytes (no constants). False => err.
bool WalkSim_Clip(const char* gameDir, const char* model, const char* anim, WalkClip& out, char* err,
                  std::size_t errSize);

// World init: StreamPager + ColLoad together (one call, deterministic).
bool WalkSim_InitWorld(const char* gameDir, E2ELoadInfo& load, char* err, std::size_t errSize);
void WalkSim_ShutdownWorld();

// Ground height at (x,y) from COL bytes. Returns false only on miss
// (prim == "none"); h is still set (-50.0). Never invents a height.
bool WalkSim_Ground(double x, double y, double& hOut, char* modelOut, std::size_t modelSize,
                    char* primOut, std::size_t primSize);

// Posed ped scene at distance phase via IfpAnim lerp+slerp.
// phase must be in [0,1). False => err.
bool WalkSim_Ped(const char* gameDir, const char* model, const char* anim, double phase,
                 WorldShotScene& pedScene, IfpAnimStats& pedStats, char* err,
                 std::size_t errSize);

// Paged world scene around (x,y,z) via StreamPager. False => err.
bool WalkSim_Page(double x, double y, double z, WorldShotScene& worldScene, E2EPagerFrame& pf,
                  char* err, std::size_t errSize);

// Resamples the control polyline to W waypoints uniformly by arc length
// (W == P returns the controls). Fills yawPath/yawBody/dist/phase/timeAbs
// (groundH/pedZ left for the caller, which owns the COL probe).
// strideLen/total come from the WalkClip. Returns false on degenerate
// input (need >= 2 controls, W in 1..64, zero total length, bad stride).
bool WalkSim_Sample(const std::vector<std::pair<double, double>>& ctrl, int waypoints,
                    double strideLen, double clipTotal, std::vector<WalkWaypoint>& out, char* err,
                    std::size_t errSize);

// Merge: world soup + ped soup transformed by (pedX,pedY,pedZtrans,yawBody).
// pedZtrans = groundH - footMinZ (so phase-0 feet touch ground).
// Deterministic order (world meshes first, then ped meshes).
void WalkSim_Merge(const WorldShotScene& world, const WorldShotScene& ped, double pedX,
                   double pedY, double pedZtrans, double yawBodyRad, WorldShotScene& out);

// Chase camera: eye = ped - forward*D + up*H, target = ped + up*1.0.
// forward = (cos yawPath, sin yawPath, 0). D/H are the fixed constants.
void WalkSim_Chase(double pedX, double pedY, double groundH, double yawPathRad, double camD,
                   double camH, float eye[3], float target[3]);
