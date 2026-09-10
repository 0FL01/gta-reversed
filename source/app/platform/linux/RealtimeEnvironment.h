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

struct RealtimeWaterState {
    uint32_t gameMs = 0, waterTimeOffset = 0;
    // CWeather::Update: EXTRASUNNY_LA wind=0, Wavyness=min(WindClipped+.3,1).
    // For transitions the caller supplies the original weather result, not a
    // clock-derived approximation. SunGlare is only evaluated, not drawn yet.
    float wavyness = 0.3f, sunGlare = 0.0f;
    // Original flow accumulators BEFORE RenderWater's periodic UV oscillation.
    std::array<float, 2> firstFlowUV{}, secondFlowUV{};
    // Optional owned accumulation, once per changed gameMs. Enabling seeds it
    // from the UV fields; while enabled those fields are outputs. Disable to
    // restore an absolute snapshot. Supply the ORIGINAL smoothed m_CurrentFlow
    // and clipped CTimer::TimeStep, not a wind vector or a wall-clock delta.
    // Nearest-water selection/32-frame flow smoothing is not emulated here.
    bool accumulateFlow = false;
    std::array<float, 2> currentFlow{};
    float flowTimeStep = 0.0f;
};

struct RealtimeWaterSample {
    float z = 0.0f, colorMult = 0.0f, glare = 0.0f;
    std::array<float, 3> normal{0, 0, 1};
};

class RealtimeEnvironment {
public:
    RealtimeEnvironment() = default;
    ~RealtimeEnvironment();
    RealtimeEnvironment(const RealtimeEnvironment&) = delete;
    RealtimeEnvironment& operator=(const RealtimeEnvironment&) = delete;

    // CPU-only Load before starting the RW asset worker; owns decoded pixels
    // and restores the current dictionary. Upload/Draw/ReleaseGpu are main
    // context-thread only (GL 2.1 compatibility); Draw performs no file IO.
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
    const WorldShotImage& GetWaterImage() const { return m_WaterImage; }
    // Explicit simulation clock/weather/accumulated flow; never wall time.
    // Read-modify-write GetWaterState to retain Load's named-weather default.
    bool SetWaterState(const RealtimeWaterState& state);
    const RealtimeWaterState& GetWaterState() const { return m_WaterState; }
    // Source 0x6E6EF0, without distance attenuation (render applies it first).
    RealtimeWaterSample SampleWater(int x, int y, float z, float big, float small) const;
    std::array<float, 2> WaterTextureShift(int layer) const;

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
    void DrawWater(float cameraX, float cameraY, bool interior = false) const;

    // Scope: timecyc ambient/directional lighting, sky-color dome, linear GL
    // eye-depth fog, authored waterclear256 layers and water.dat waves/normals.
    // Not full SA parity: no clouds/sun sprites, weather transitions/boxes,
    // local object lighting/shadows, custom car env/specular lighting, underwater
    // effects, reflections/refraction, glare/wakes/foam or sorted transparency.
    // Source-sized 2m grids for rectangles and right-isosceles water triangles.
private:
    void ApplyFog() const;
    void BeginLighting(bool objects) const;

    std::array<TimeCycleParams, 8> m_Samples{};
    RealtimeEnvironmentParams m_Params{};
    WaterLevelData m_Water{};
    WorldShotImage m_WaterImage{}; // owned pixels; no RW objects survive Load
    RealtimeWaterState m_WaterState{};
    unsigned int m_WaterTexture = 0;
    unsigned int m_LightingProgram = 0;
    mutable int m_PreviousProgram = 0; // lighting scopes must not nest
    mutable bool m_LightingActive = false;
    int m_WaterTriangles = 0;
    bool m_Loaded = false;
};
