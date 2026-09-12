#include "NativeDiagnosticActors.h"
#include "NativePlayerAssets.h"

#include <bit>
#include <cmath>
#include <cstdio>

bool NativeDiagnosticActors::Initialize(const char* gameDir, std::string& error) {
    if (m_Gameplay) { error = "diagnostic actors already initialized"; return false; }
    WorldShotScene floor{};
    WorldShotMesh mesh{};
    mesh.tris = 2;
    mesh.pos = {-100,-100,0, 100,-100,0, 100,100,0,
                -100,-100,0, 100,100,0, -100,100,0};
    floor.meshes.push_back(std::move(mesh));
    if (!m_World.Rebuild(floor, error)) return false;
    auto gameplay = std::make_unique<RealtimeGameplay>();
    const auto clothes = NativePlayerClothes_Startup();
    if (!gameplay->Initialize(gameDir, error, &clothes) || !gameplay->Spawn(m_World, 0, 0, 10, 0, error)) return false;
    m_Topology = std::make_shared<const WorldShotScene>(gameplay->Actors());
    m_Gameplay = std::move(gameplay);
    m_Previous = m_Current = Capture();
    error.clear();
    return true;
}

NativeDiagnosticActorPose NativeDiagnosticActors::Capture() const {
    NativeDiagnosticActorPose pose;
    const auto& state = m_Gameplay->State();
    pose.Tick = state.Ticks;
    pose.InVehicle = state.InVehicle;
    pose.CarSpeed = state.Speed;
    // Canonical numeric bits: no locale/decimal tolerance or Godot formatting.
    const auto bits = [](float v) { return std::bit_cast<std::uint32_t>(v); };
    char trace[384];
    std::snprintf(trace, sizeof(trace), "diagnostic-actors-v1 tick=%llu time=%016llx ped=%08x,%08x,%08x car=%08x,%08x,%08x phase=%08x speed=%08x vehicle=%u",
        static_cast<unsigned long long>(state.Ticks),
        static_cast<unsigned long long>(std::bit_cast<std::uint64_t>(state.SimulatedSeconds)),
        bits(state.Ped.X), bits(state.Ped.Y), bits(state.Ped.Z), bits(state.Car.X), bits(state.Car.Y), bits(state.Car.Z),
        bits(state.LocomotionPhase), bits(state.Speed), unsigned(state.InVehicle));
    pose.Trace = trace;
    for (const auto& mesh : m_Gameplay->Actors().meshes) {
        pose.Positions.push_back(mesh.pos);
        pose.Normals.push_back(mesh.nrm);
        pose.Triangles.push_back(mesh.tris);
    }
    return pose;
}

bool NativeDiagnosticActors::Tick(double seconds, const RealtimeGameplayInput& input, std::string& error) {
    if (!m_Gameplay || !std::isfinite(seconds) || seconds < 0 || seconds > 0.25 ||
        !std::isfinite(input.Forward) || !std::isfinite(input.Side) || std::abs(input.Forward) > 1 || std::abs(input.Side) > 1 ||
        !std::isfinite(input.LookYaw) || !std::isfinite(input.LookPitch)) {
        error = "invalid diagnostic actor tick"; return false;
    }
    m_Gameplay->Tick(seconds, input, m_World);
    auto next = Capture();
    m_Previous = std::move(m_Current);
    m_Current = std::move(next);
    error.clear();
    return true;
}

bool NativeDiagnosticActors::Present(double alpha, NativeDiagnosticActorPose& out, std::string& error) const {
    if (!m_Gameplay || !std::isfinite(alpha) || alpha < 0 || alpha > 1) {
        error = "invalid diagnostic presentation alpha"; return false;
    }
    auto pose = m_Current;
    for (std::size_t i = 0; i < pose.Positions.size(); ++i) {
        // Visibility transitions snap to current; never interpolate a hidden ped
        // into/out of a car. Geometry topology itself is fixed by native init.
        if (m_Previous.Triangles[i] != m_Current.Triangles[i]) continue;
        for (std::size_t j = 0; j < pose.Positions[i].size(); ++j)
            pose.Positions[i][j] = float(double(m_Previous.Positions[i][j]) * (1 - alpha) + double(m_Current.Positions[i][j]) * alpha);
        for (std::size_t j = 0; j < pose.Normals[i].size(); ++j)
            pose.Normals[i][j] = float(double(m_Previous.Normals[i][j]) * (1 - alpha) + double(m_Current.Normals[i][j]) * alpha);
    }
    out = std::move(pose);
    error.clear();
    return true;
}
