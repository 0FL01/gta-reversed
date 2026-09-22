// Persistent native gameplay slice. No GL, SDL, Wine, or per-tick asset IO.
// Andre, base MODEL_PLAYER, or an explicit modular CJ outfit. Not game parity.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <limits>
#include <string>

#include "app/platform/linux/NativePlayerActivity.h"
#include "app/platform/linux/WorldShot.h"

struct NativePlayerClothes;
struct IfpAnimStats;
struct NativeCollisionSnapshot;
struct NativeCollisionHit;
enum class RealtimeGameplayModel { Andre, BasePlayer };

struct RealtimeVec3 {
    float X = 0.0f, Y = 0.0f, Z = 0.0f;
};

// Owns a BVH COPY of source COL triangles plus analytic spheres/oriented box
// volumes, or the historical render-scene fixture overload. No retained scene
// pointers or invented ground on a ray miss. Rebuild with exclusive ownership. A worker
// may build a separate world, then hand it to gameplay at a frame boundary.
// Queries (including const queries' counters) require single-thread ownership.
class RealtimeGameplayWorld {
public:
    RealtimeGameplayWorld();
    ~RealtimeGameplayWorld();
    RealtimeGameplayWorld(const RealtimeGameplayWorld&) = delete;
    RealtimeGameplayWorld& operator=(const RealtimeGameplayWorld&) = delete;
    bool Rebuild(const WorldShotScene& scene, std::string& error);
    // Disable volume indexing only for the exhaustive-query verification oracle.
    bool Rebuild(const NativeCollisionSnapshot& snapshot, std::string& error, bool indexVolumes = true);
    bool Ground(float x, float y, float top, float bottom, float& height, NativeCollisionHit* source = nullptr) const;
    bool Raycast(RealtimeVec3 from, RealtimeVec3 to, RealtimeVec3& hit, NativeCollisionHit* source = nullptr) const;
    bool SphereBlocked(RealtimeVec3 center, float radius, NativeCollisionHit* source = nullptr) const;
    // Continuous sphere/triangle query; fraction is in [0,1]. Walkable queries
    // require both a walkable triangle and an upward-facing contact normal.
    // Optional unit normal points from the contacted face/edge/vertex toward
    // the sphere, opposing entry; winding does not determine its orientation.
    bool SweepSphere(RealtimeVec3 from, RealtimeVec3 to, float radius,
                     float& fraction, bool walkableOnly = false,
                     float maxContactHeight = std::numeric_limits<float>::infinity(),
                     RealtimeVec3* contactNormal = nullptr, NativeCollisionHit* source = nullptr) const;
    std::size_t SphereCount() const;
    std::size_t BoxCount() const;
    std::size_t CollapsedTriangleCount() const; // retained source/box point-segment boundaries
    std::size_t TriangleCount() const;
    std::uint64_t TriangleTests() const;
    std::uint64_t VolumeTests() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_Impl;
};

struct RealtimeGameplayInput {
    float Forward = 0.0f; // -1..1, camera-relative on foot; throttle/reverse in car
    float Side = 0.0f; // -1..1, positive = right; steering when driving
    bool Sprint = false;
    bool Jump = false; // rising edge, consumed once even across substeps
    bool Interact = false; // rising edge: nearby driver's door / stationary exit
    bool Brake = false;
    bool Handbrake = false;
    float LookYaw = 0.0f; // radians of orbit delta this frame; + turns left
    float LookPitch = 0.0f; // radians of elevation delta this frame
};

struct RealtimeGameplayCamera {
    RealtimeVec3 Position;
    RealtimeVec3 Target;
    float Yaw = 0.0f; // atan2(view.y, view.x), suitable for existing GL view
    float Pitch = 0.0f; // asin(view.z)
    bool ScriptDirectlyBehind = false;
    float ScriptPedOrientation = 0.0f; // call-time atan2(forward.y, forward.x), [0,2pi)
};

struct RealtimeGameplayState {
    RealtimeVec3 Ped; // feet
    RealtimeVec3 Car; // DFF origin
    float PedHeading = 0.0f; // world forward atan2(y,x), DFF +Y forward
    float CarHeading = 0.0f;
    float Speed = 0.0f; // signed car speed m/s
    std::uint8_t Gear = 1; // source transmission, 0=reverse; original 30 FPS limiter cadence
    float Steer = 0.0f; // radians at wheels
    float WheelSpin = 0.0f; // radians, actual travelled distance / DFF radius
    float VerticalSpeed = 0.0f;
    bool Grounded = false;
    bool InVehicle = false;
    bool Ready = false; // initialized AND world-validated; authored script spawn may be airborne
    std::uint64_t Ticks = 0, Jumps = 0, Landings = 0, Entries = 0, Exits = 0;
    std::uint64_t BlockedSteps = 0;
    double SimulatedSeconds = 0.0, DroppedSeconds = 0.0;
    double WalkDistance = 0.0, DriveDistance = 0.0;
    const char* Animation = "IDLE_stance";
    float LocomotionPhase = 0.0f; // distance-driven, never reset on speed/contact changes
    float LocomotionBlend = 0.0f, RunBlend = 0.0f, AirBlend = 0.0f;
    // Script entity origin is separate from collision feet. Source ped1's
    // bbox minimum is -1 (TempColModels.cpp), not the render mesh minimum.
    RealtimeVec3 PedRoot;
    float PedCurrentRotation = 0, PedAimingRotation = 0; // source +Y heading
    bool CarPresent = true, MissionCreated = false, PlayerOnFootTask = false;
};

class RealtimeGameplay {
public:
    RealtimeGameplay();
    ~RealtimeGameplay();
    RealtimeGameplay(const RealtimeGameplay&) = delete;
    RealtimeGameplay& operator=(const RealtimeGameplay&) = delete;

    // Call after StreamPager_Init (same thread). Temporarily uses IfpAnim and
    // CarPose, restores the pager's current TXD; never stops the shared engine.
    // Optional explicit descriptor is parsed once here, before worker startup.
    bool Initialize(const char* gameDir, std::string& error);
    bool Initialize(const char* gameDir, std::string& error, const NativePlayerClothes* player);
    bool Initialize(const char* gameDir, std::string& error, RealtimeGameplayModel model);
    // Parse the exact initial MODEL_PLAYER outfit before the sole parser worker
    // starts. The prebuilt pose is hidden until the source 070D build commits.
    bool InitializeScriptPlayerAppearance(const char* gameDir, const NativePlayerClothes& clothes,
        std::string& error);
    bool RevealScriptPlayerAppearance(std::string& error);
    bool ScriptAppearancePrepared() const;
    bool ScriptAppearanceVisible() const;
    const IfpAnimStats& PlayerModelStats() const;
    // 0053 preserves authored Z; no ground snapping and no preview vehicle.
    // Source entity root = authored base + 1 (ped1 COL bounding box).
    bool SpawnScriptPlayer(const RealtimeGameplayWorld& world, RealtimeVec3 authoredBase, std::string& error);
    bool SetScriptHeading(float radians, std::string& error);
    // SCM vehicle occupancy is authoritative. Keep the rendered source-outfit
    // ped at its script-owned location; hide it while seated, never simulate a
    // second car or turn the diagnostic controller into vehicle authority.
    bool AdoptScriptPlayerPlacement(const RealtimeGameplayWorld& world, RealtimeVec3 entityRoot,
        float forwardHeading, bool seated, std::string& error);
    bool SetScriptCameraBehind(const RealtimeGameplayWorld& world, std::string& error);
    // rayTop is the ceiling for nearest ground, not a requested flat height.
    // Fails explicitly when ground/body clearance/nearby car placement is absent.
    bool Spawn(const RealtimeGameplayWorld& world, float x, float y, float rayTop,
               float heading, std::string& error);
    void Tick(double dt, const RealtimeGameplayInput& input, const RealtimeGameplayWorld& world);
    const RealtimeGameplayState& State() const;
    // Immutable view of the controller's sole mutable activity snapshot. Use
    // Activity().Revision as its owner revision; State().Ticks resets on Spawn.
    const NativePlayerActivitySnapshot& Activity() const;
    const RealtimeGameplayCamera& Camera() const;
    // Persistent owned scene. Images/UV/material topology stay fixed after init;
    // update only dynamic positions/normals on the GPU. Ped meshes are hidden
    // in vehicle by tris=0 (restored on exit), car remains visible throughout.
    const WorldShotScene& Actors() const;

private:
    bool InitializeModel(const char* gameDir, std::string& error, const NativePlayerClothes* player, RealtimeGameplayModel model);
    struct Impl;
    std::unique_ptr<Impl> m_Impl;
};
