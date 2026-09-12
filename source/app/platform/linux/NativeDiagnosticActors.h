// P2-A06 diagnostic approximation ONLY. Reuses the existing native controller,
// skinning and car pose; never substitutes for source ped/Automobile gameplay.
#pragma once

#include "RealtimeGameplay.h"
#include <vector>

struct NativeDiagnosticActorPose {
    std::uint64_t Tick = 0;
    bool InVehicle = false;
    float CarSpeed = 0;
    std::string Trace;
    std::vector<std::vector<float>> Positions, Normals;
    std::vector<int> Triangles;
};

class NativeDiagnosticActors {
public:
    // Caller has initialized StreamPager and exclusively owns parser access.
    // This explicit studio fixture uses a SYNTHETIC flat floor, not world COL.
    bool Initialize(const char* gameDir, std::string& error);
    bool Tick(double seconds, const RealtimeGameplayInput& input, std::string& error);
    // Presentation-only vertex interpolation, never fed back to the controller.
    // Invalid alpha rejects without changing out or simulation state.
    bool Present(double alpha, NativeDiagnosticActorPose& out, std::string& error) const;
    const WorldShotScene& Topology() const { return *m_Topology; }
    const NativeDiagnosticActorPose& Current() const { return m_Current; }

private:
    NativeDiagnosticActorPose Capture() const;
    std::unique_ptr<RealtimeGameplay> m_Gameplay;
    RealtimeGameplayWorld m_World;
    std::shared_ptr<const WorldShotScene> m_Topology;
    NativeDiagnosticActorPose m_Previous, m_Current;
};
