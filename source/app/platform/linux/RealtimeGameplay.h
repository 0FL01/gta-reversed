// Persistent native gameplay slice. No GL, SDL, Wine, or per-tick asset IO.
// andre is a real skinned ped stand-in, not modular CJ. This is not game parity.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <limits>
#include <string>

#include "app/platform/linux/WorldShot.h"

struct RealtimeVec3 {
    float X = 0.0f, Y = 0.0f, Z = 0.0f;
};

// Owns a BVH COPY of the actual pager triangles: no retained scene pointers,
// no text-IPL-only collision bindings, no invented ground on a ray miss.
// Rebuild only after a successful pager update, on the gameplay thread.
class RealtimeGameplayWorld {
public:
    RealtimeGameplayWorld();
    ~RealtimeGameplayWorld();
    RealtimeGameplayWorld(const RealtimeGameplayWorld&) = delete;
    RealtimeGameplayWorld& operator=(const RealtimeGameplayWorld&) = delete;
    bool Rebuild(const WorldShotScene& scene, std::string& error);
    bool Ground(float x, float y, float top, float bottom, float& height) const;
    bool Raycast(RealtimeVec3 from, RealtimeVec3 to, RealtimeVec3& hit) const;
    bool SphereBlocked(RealtimeVec3 center, float radius) const;
    // Continuous sphere/triangle query; fraction is in [0,1]. Walkable queries
    // require both a walkable triangle and an upward-facing contact normal.
    bool SweepSphere(RealtimeVec3 from, RealtimeVec3 to, float radius,
                     float& fraction, bool walkableOnly = false,
                     float maxContactHeight = std::numeric_limits<float>::infinity()) const;
    std::size_t TriangleCount() const;
    std::uint64_t TriangleTests() const;

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
};

struct RealtimeGameplayState {
    RealtimeVec3 Ped; // feet
    RealtimeVec3 Car; // DFF origin
    float PedHeading = 0.0f; // world forward atan2(y,x), DFF +Y forward
    float CarHeading = 0.0f;
    float Speed = 0.0f; // signed car speed m/s
    float Steer = 0.0f; // radians at wheels
    float WheelSpin = 0.0f; // radians, actual travelled distance / DFF radius
    float VerticalSpeed = 0.0f;
    bool Grounded = false;
    bool InVehicle = false;
    bool Ready = false; // initialized AND successfully spawned on real ground
    std::uint64_t Ticks = 0, Jumps = 0, Landings = 0, Entries = 0, Exits = 0;
    std::uint64_t BlockedSteps = 0;
    double SimulatedSeconds = 0.0, DroppedSeconds = 0.0;
    double WalkDistance = 0.0, DriveDistance = 0.0;
    const char* Animation = "IDLE_stance";
    float LocomotionPhase = 0.0f; // distance-driven, never reset on speed/contact changes
    float LocomotionBlend = 0.0f, RunBlend = 0.0f, AirBlend = 0.0f;
};

class RealtimeGameplay {
public:
    RealtimeGameplay();
    ~RealtimeGameplay();
    RealtimeGameplay(const RealtimeGameplay&) = delete;
    RealtimeGameplay& operator=(const RealtimeGameplay&) = delete;

    // Call after StreamPager_Init (same thread). Temporarily uses IfpAnim and
    // CarPose, restores the pager's current TXD; never stops the shared engine.
    bool Initialize(const char* gameDir, std::string& error);
    // rayTop is the ceiling for nearest ground, not a requested flat height.
    // Fails explicitly when ground/body clearance/nearby car placement is absent.
    bool Spawn(const RealtimeGameplayWorld& world, float x, float y, float rayTop,
               float heading, std::string& error);
    void Tick(double dt, const RealtimeGameplayInput& input, const RealtimeGameplayWorld& world);
    const RealtimeGameplayState& State() const;
    const RealtimeGameplayCamera& Camera() const;
    // Persistent owned scene. Images/UV/material topology stay fixed after init;
    // update only dynamic positions/normals on the GPU. Ped meshes are hidden
    // in vehicle by tris=0 (restored on exit), car remains visible throughout.
    const WorldShotScene& Actors() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_Impl;
};
