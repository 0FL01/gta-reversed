// Native compatibility-GL environment. Legacy offline rendering stays unchanged.
#pragma once

#include "app/platform/linux/TimeCycle.h"
#include "app/platform/linux/WaterLevel.h"

#include <array>
#include <cstddef>

struct RealtimeEnvironmentParams {
    float hour = 12.0f;
    std::array<float, 3> ambient{}, directional{}, skyTop{}, skyBottom{}, sunDirection{};
    std::array<float, 3> ambientObjects{};
    float nightBalance = 0.0f;
    std::array<float, 4> water{};
    float farClip = 0.0f;
    float fogStart = 0.0f;
};

class RealtimeEnvironment {
public:
    RealtimeEnvironment() = default;
    ~RealtimeEnvironment();
    RealtimeEnvironment(const RealtimeEnvironment&) = delete;
    RealtimeEnvironment& operator=(const RealtimeEnvironment&) = delete;

    // CPU-only load, then Upload with a current GL 2.1 compatibility context.
    // Declare this object AFTER the context owner: destruction/ReleaseGpu must
    // happen while that context is current. Reload requires ReleaseGpu first.
    bool Load(const char* gameDir, char* err, std::size_t errSize,
              float hour = 12.0f, const char* weather = "EXTRASUNNY_LA");
    bool Upload(char* err, std::size_t errSize);
    void ReleaseGpu();

    // Finite hours in [0,24); the caller controls clock speed and wraps days.
    // No IO or GL work. Interpolates 22->24 against the midnight row.
    bool SetHour(float hour);
    const RealtimeEnvironmentParams& GetParams() const { return m_Params; }
    const WaterLevelData& GetWaterData() const { return m_Water; }
    int GetWaterTriangleCount() const { return m_WaterTriangles; }

    // Caller installs the camera's projection/view first (far plane >= farClip),
    // clears color/depth, then DrawSky(camera), BeginWorld(), draws world lists,
    // EndWorld(), BeginObjects(), draw actors, EndWorld(), DrawWater(). Scopes
    // must not nest; they restore compatibility state and the previous program.
    // GLSL 1.20 vertex inputs: normal; unlit material RGBA in glColor; base UV
    // in texcoord0; authored day/night RGBA in texcoord1/2; material ambient /
    // diffuse coefficients in texcoord3.xy. GpuScene::Draw supplies optional
    // WorldShotMesh metadata; texture GL_MODULATE remains supported.
    void DrawSky(float cameraX, float cameraY, float cameraZ) const;
    void BeginWorld() const;
    void BeginObjects() const; // same scope; pair with EndWorld()
    void EndWorld() const;
    void DrawWater() const;

    // Scope: timecyc ambient/directional lighting, sky-color dome, linear GL
    // eye-depth fog, and visible water.dat triangles with timecyc RGBA blending.
    // Not full SA parity: no clouds/sun sprites, weather transitions/boxes,
    // local object lighting/shadows, custom car env/specular lighting, underwater effects, water texture,
    // waves/flow, reflections/refraction, or sorted intersecting transparency.
private:
    void ApplyFog() const;
    void BeginLighting(bool objects) const;

    std::array<TimeCycleParams, 8> m_Samples{};
    RealtimeEnvironmentParams m_Params{};
    WaterLevelData m_Water{};
    unsigned int m_WaterList = 0;
    unsigned int m_LightingProgram = 0;
    mutable int m_PreviousProgram = 0; // lighting scopes must not nest
    mutable bool m_LightingActive = false;
    int m_WaterTriangles = 0;
    bool m_Loaded = false;
};
