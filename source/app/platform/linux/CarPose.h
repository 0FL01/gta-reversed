// CarPose: steerable/spinnable car wheels slice (R6l, round 14).
// Loads ONE car DFF (+ its per-model TXD) out of the IMG archives, finds the
// wheel dummy frames by their stored hierarchy names (`wheel_lf_dummy`,
// `wheel_rf_dummy`, `wheel_lb_dummy`, `wheel_rb_dummy` — the exact names the
// retail car DFFs carry, verified by a byte-level framelist dump of
// landstal/elegy/zr350), and poses them the way the game does (read-only
// reference, NOT linked: `CVehicleModelInfo::PreprocessHierarchy` clones the
// single stored `wheel` atomic onto every wheel frame, `CAutomobile`
// steers the front pair by `m_fSteerAngle` and rolls every wheel by
// `m_wheelRotation`, which integrates `m_wheelSpeed`):
//   - front dummies (lf/rf): local yaw about Z by --steer degrees, then
//     local roll about the X axle by --spin degrees (both PRECONCAT, i.e.
//     extra local matrices pivoting about the dummy center over the DFF
//     transforms, librw degrees);
//   - rear dummies (lb/rb): local roll about X by --spin degrees.
// The body (every atomic outside the wheel subtrees) flattens through its
// untouched frame LTMs, so the body provably does not move. Geometry comes
// only from DFF bytes (the stored wheel mesh is instanced onto the four
// dummies exactly like the retail `RpAtomicClone` step); angles come only
// from argv. Procedural car bodies or whole-body rotations are forbidden.
// librw keeps no frame names on rw::Frame, so the names are parsed from the
// raw DFF framelist (NodeName extension 0x253F2FE) and paired to librw
// frames by hierarchy shape (children append order is deterministic).
#pragma once

#include <cstddef>
#include <array>

#include "app/platform/linux/WorldShot.h"

struct CarPoseStats {
    char model[64]; // resolved DFF base (e.g. "landstal")
    char requested[64]; // --model value as passed (default "landstal")
    char src[160]; // e.g. "gta3.img:landstal.dff"
    char txd[160]; // e.g. "gta3.img:landstal.txd"
    int verts = 0; // flattened verts (tris*3, body + 4 wheel instances)
    int tris = 0;
    int bodyTris = 0; // body-only triangles (non-wheel atomics)
    int wheelTris = 0; // one wheel-kit instance (per dummy)
    int geoms = 0; // non-wheel geometries flattened
    int frames = 0; // clump frame-hierarchy size
    int wheels = 0; // wheel dummy frames found by name (4 on landstal)
    char wheelNames[4][32]; // in hierarchy (raw-index) order
    int fronts = 0; // front (steered) dummies among them
    int textures = 0; // loaded model + shared dictionary entries
    int sharedTextures = 0; // textures loaded from models/generic/vehicle.txd
    int damagedAtomicsSkipped = 0;
    int lodAtomicsSkipped = 0;
    int nonRenderAtomicsSkipped = 0;
    int extrasAvailable = 0;
    int extrasSelected = 0;
    double steerDeg = 0.0; // --steer as passed
    double spinDeg = 0.0; // --spin as passed
    int chassisSame = 0; // 1 when the body audit frame LTM is bit-identical
};

// Human-readable wheel audit: posed wheel-dummy LTM before/after plus the
// body-frame comparison proof. Valid after a successful CarPose_Init.
struct CarPoseAudit {
    char wheel[32]; // audited wheel dummy name (first in hierarchy order)
    float before[12]; // its bind LTM (right,up,at,pos)
    float after[12]; // its posed LTM
    char body[32]; // audited body frame name ("chassis" or root fallback)
    int bodySame = 0; // 1 when body LTM is bit-identical before/after
};

enum class CarPoseTextures { ModelOnly, RealtimeVehicle };
enum class CarPoseGeometry { FromTextureMode, StoredAtomics, PristineNear };
struct CarPoseComponents {
    CarPoseGeometry geometry = CarPoseGeometry::FromTextureMode;
    // Forced instance choices, like CVehicleModelInfo::ms_compsToUse: -1 is
    // none, 0..5 index the available extra1..extra6 atomics in descriptor order
    // (missing frames do not consume indices). No weather/RNG selection here.
    std::array<int, 2> extras{-1, -1};
};
// ModelOnly preserves the historical offline fixtures. RealtimeVehicle requires
// the common vehicle TXD and uses upstream common-before-model name resolution.
// FromTextureMode selects StoredAtomics for ModelOnly, PristineNear otherwise.
// Bind/spin/steer caches MUST use identical component options and texture mode:
// selection changes geoms (body count), followed by four wheel-kit groups.
// scene owns decoded RGBA; later Init/Shutdown calls cannot invalidate its images.
// Loads the car relative to gameDir (e.g. "/game") and poses the wheels by
// steerDeg/spinDeg. On failure returns false with a message in err (never
// car-ok). audit is filled on success (even for steer=spin=0).
bool CarPose_Init(const char* gameDir, const char* model, double steerDeg, double spinDeg,
                  WorldShotScene& scene, CarPoseStats& stats, CarPoseAudit& audit, char* err,
                  std::size_t errSize, CarPoseTextures textures = CarPoseTextures::ModelOnly,
                  CarPoseComponents components = {});
void CarPose_Shutdown();

// DFF-derived kinematic constants for the drive slice (R6n): wheel radius
// from the stored wheel-mesh extents, wheelbase from the dummy Y positions,
// clearance = -minZ of the bound wheel instances (car origin height that
// puts the wheel bottoms on z=0). All values come from DFF bytes only.
struct CarPoseMeasure {
    char model[64];
    char src[160];
    double wheelR = 0.0;
    double wheelbase = 0.0;
    double clearance = 0.0;
    double frontY = 0.0;
    double rearY = 0.0;
    int wheels = 0;
};

bool CarPose_Measure(const char* gameDir, const char* model, CarPoseMeasure& out, char* err,
                     std::size_t errSize);
