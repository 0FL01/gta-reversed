// DriveSim: kinematic car drive slice (R6n, round 16).
// Composition of the verified slices (no new physics, no handling.cfg):
//   - CarPose  : car DFF + per-model TXD, wheel dummies by stored NodeName,
//                front steer (yaw Z) + all-wheel spin (roll X), stored
//                `wheel` mesh instanced onto the four dummies;
//   - ColLoad/Collide : vertical raycast ground heights from COL bytes;
//   - StreamPager : grid pager world around the car (bounded window);
//   - chase-cam : eye = car - forward*D + up*H (D/H fixed, logged).
// Kinematics only (honest slice): car Z = groundH + clearance (clearance
// from DFF wheel bottom), body yaw from the path segment, front steer from
// curvature via steerFormula=atan(wheelbase*dyaw/ds) (wheelbase from DFF
// dummy Y positions), wheel spin = travelled distance / wheelR (wheelR from
// DFF wheel-mesh extents). Every length/angle comes from DFF/COL/path bytes.
#pragma once

#include <cstddef>
#include <utility>
#include <vector>

#include "app/platform/linux/WorldShot.h"
#include "app/platform/linux/StreamPager.h"

struct DriveMeasure {
    char model[64]; // resolved DFF base
    char src[160]; // e.g. "gta3.img:landstal.dff"
    double wheelR = 0.0; // wheel radius (m) from stored wheel-mesh extents
    double wheelbase = 0.0; // frontY - rearY (m) from dummy positions
    double clearance = 0.0; // -minZ of wheel instances in bind (m)
    double frontY = 0.0;
    double rearY = 0.0;
    int wheels = 0;
};

struct DriveWaypoint {
    double x = 0.0;
    double y = 0.0;
    double groundH = 0.0; // COL raycast best-z
    char groundModel[32]; // COL model of the hit
    char groundPrim[8]; // sphere/box/tri
    double carZ = 0.0; // groundH + clearance
    double yawPath = 0.0; // path direction atan2(dy,dx), radians
    double yawBody = 0.0; // body yaw about Z, radians (+Y forward)
    double steerDeg = 0.0; // front steer, degrees
    double spinRad = 0.0; // cumulative wheel spin, radians
    double spinDeg = 0.0; // same, degrees (for CarPose)
    double dist = 0.0; // cumulative XY distance, meters
    double vel = 0.0; // R6p handling mode: sim speed at this waypoint, m/s
    double time = 0.0; // R6p handling mode: sim time at this waypoint, s
};

// R6p handling-mode sim trace sample (fixed dt=1/30 integration).
struct DriveSimTrace {
    double t = 0.0; // sim time, s
    double v = 0.0; // speed, m/s
    double s = 0.0; // arc position, m
};

// DFF-derived measurement (wheelR/wheelbase/clearance). False => err.
bool DriveSim_Measure(const char* gameDir, const char* model, DriveMeasure& out, char* err,
                      std::size_t errSize);

// World init: StreamPager + ColLoad together (one call, deterministic).
// load is the pager load info for the log; collStats* are optional text.
bool DriveSim_InitWorld(const char* gameDir, E2ELoadInfo& load, char* err, std::size_t errSize);
void DriveSim_ShutdownWorld();

// Ground height at (x,y) from COL bytes. Returns false only on miss
// (prim == "none"); h is still set (-50.0). Never invents a height.
bool DriveSim_Ground(double x, double y, double& hOut, char* modelOut, std::size_t modelSize,
                     char* primOut, std::size_t primSize);

// Posed car scene (body + 4 wheel instances) via CarPose. steerDeg/spinDeg
// come only from the path kinematics. False => err.
bool DriveSim_Car(const char* gameDir, const char* model, double steerDeg, double spinDeg,
                  WorldShotScene& carScene, char* err, std::size_t errSize);

// Paged world scene around (x,y,z) via StreamPager. False => err.
bool DriveSim_Page(double x, double y, double z, WorldShotScene& worldScene, E2EPagerFrame& pf,
                   char* err, std::size_t errSize);

// Resamples the control polyline to W waypoints uniformly by arc length
// (W == P returns the controls). Fills yawPath/yawBody/dist/steer/spin
// (groundH/carZ left for the caller, which owns the COL probe).
// wheelbase/wheelR come from the DFF measure. Returns false on degenerate
// input (need >= 2 controls, W in 1..64, zero total length).
bool DriveSim_Sample(const std::vector<std::pair<double, double>>& ctrl, int waypoints,
                     double wheelbase, double wheelR, std::vector<DriveWaypoint>& out, char* err,
                     std::size_t errSize);

// Max turn angle (degrees) over the control polyline (for the >= 20 deg gate).
double DriveSim_MaxTurnDeg(const std::vector<std::pair<double, double>>& ctrl);

// R6p handling-mode sample: constant-acceleration launch from rest along
// the same control polyline, integrated at fixed dt (1/30):
//   v(t+dt) = min(v(t) + A*dt, VMAX), v(0) = 0,
//   s(t) = integral of v (trapezoid per step, exact for the linear ramp).
// The sim runs until s reaches the geometric path length L (last step
// shortened fractionally to hit L exactly), total time T = trace.back().t.
// Waypoints are uniform in TIME (t_j = T*j/(W-1)) mapped back to arc
// positions s(t_j) — NOT uniform in distance, so the launch shows as
// v0=0.00 -> v1 -> v2 with short first legs. spin = s/wheelR (same honest
// link as the kinematic mode). vel/time are filled; groundH/carZ left for
// the caller (COL probe). trace holds every dt step (for driveok-sim and
// speedIntegralCheck). Returns false on degenerate input.
bool DriveSim_SampleHandling(const std::vector<std::pair<double, double>>& ctrl, int waypoints,
                             double wheelbase, double wheelR, double vmaxMs, double accelSi,
                             double dt, std::vector<DriveWaypoint>& out,
                             std::vector<DriveSimTrace>& traceOut, double& totalTimeOut, char* err,
                             std::size_t errSize);

// Merge: world soup + car soup transformed by (carX,carY,carZ,yawBodyRad).
// Car images are appended with re-indexed triImg. Deterministic order
// (world meshes first, then car meshes in their own order).
void DriveSim_Merge(const WorldShotScene& world, const WorldShotScene& car, double carX,
                    double carY, double carZ, double yawBodyRad, WorldShotScene& out);

// Chase camera: eye = car - forward*D + up*H, target = car center.
// forward = (cos yawPath, sin yawPath, 0). D/H are the fixed constants.
void DriveSim_Chase(double carX, double carY, double carZ, double yawPathRad, double camD,
                    double camH, float eye[3], float target[3]);
