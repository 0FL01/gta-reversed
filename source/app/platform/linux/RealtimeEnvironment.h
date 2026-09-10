// Native compatibility-GL environment. Legacy offline rendering stays unchanged.
#pragma once

#include "app/platform/linux/TimeCycle.h"
#include "app/platform/linux/WaterLevel.h"

#include <array>
#include <cstddef>
#include <span>

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
    // Leave false when using AdvanceWaterFlow, which owns source flow updates.
    bool accumulateFlow = false;
    std::array<float, 2> currentFlow{};
    float flowTimeStep = 0.0f;
};

struct RealtimeWaterSample {
    float z = 0.0f, colorMult = 0.0f, glare = 0.0f;
    std::array<float, 3> normal{0, 0, 1};
};

struct RealtimeWaterBlock {
    int16_t x = 0, y = 0;
    bool operator==(const RealtimeWaterBlock&) const = default;
};

struct RealtimeSeaBedVertex {
    float x = 0, y = 0, z = -70, u = 0, v = 0;
    bool operator==(const RealtimeSeaBedVertex&) const = default;
};

struct RealtimeSeaBedGeometry {
    // BlockHit admits 70 blocks; each emits at most four independent quads.
    // Consecutive groups of four use original indices {0,1,2,3,1,2}.
    std::array<RealtimeSeaBedVertex, 70 * 4 * 4> vertices{};
    size_t size = 0;
};

struct RealtimeWaterFlowTick {
    // Original simulation frame counter, NOT the native presentation counter.
    // Deliver consecutive ticks (uint32 wrap is supported), at the caller's
    // source simulation cadence (30 Hz when emulating APP_MAX_FPS=30).
    uint32_t frame = 0, gameMs = 0;
    float timeStep = 0; // clipped CTimer::TimeStep, milliseconds * .05, <= 3
    float cameraX = 0, cameraY = 0; // TheCamera.GetPosition(), not player coords
    // Native UI suspension is an explicit freeze. To replay ORIGINAL paused
    // CTimer ticks instead, leave suspended=false: frame still increments,
    // gameMs stops, and clipped timeStep is .00001 (Timer.cpp/Game.cpp).
    bool suspended = false;
    bool canSeeWater = true; // CGame::currArea == 0 || currArea == 5
    bool operator==(const RealtimeWaterFlowTick&) const = default;
};

struct RealtimeWaterFlowSelection {
    enum class Result { NoQuad, SelectedQuad, OutsideWorld } result = Result::NoQuad;
    std::array<float, 2> desired{};
    float nearestWavyDistance = 10000000.0f, nearestWavyHeight = 0;
    int polygon = -1, corner = -1; // authored polygon index, sorted quad corner
};

// CPU-only source flow field. Builds shared/quantized source vertices once;
// triangles participate in vertex ownership but never in the nearest-quad scan.
class RealtimeWaterFlow {
public:
    void Initialise(const WaterLevelData& data);
    RealtimeWaterFlowSelection FindNearest(float cameraX, float cameraY,
        std::array<float, 2> previousDesired = {}) const;
    // False on invalid input, conflicting duplicate, skipped/out-of-order tick,
    // or mixing explicit accumulateFlow mode. Identical duplicates are no-ops.
    // Suspended/hidden-area ticks consume their counter but change no flow/UV.
    // Does not change the wave clock: SetWaterState controls presentation time.
    bool Advance(const RealtimeWaterFlowTick& tick, RealtimeWaterState& water);
    const RealtimeWaterFlowSelection& GetSelection() const { return m_Selection; }
private:
    struct Quad { std::array<size_t, 4> vertices; int polygon; };
    std::vector<WaterVert> m_Vertices;
    std::vector<Quad> m_Quads;
    RealtimeWaterFlowSelection m_Selection{};
    RealtimeWaterFlowTick m_LastTick{};
    bool m_HasTick = false;
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
    const WorldShotImage& GetSeaBedImage() const { return m_SeaBedImage; }
    // RenderWater's actual ocean floor, NOT a floor for limited-depth polygons.
    // Original seabed has no COL, clock, weather-color or wave dependency:
    // Z=-70, RGBA={80,80,80,255}, local block UV*8. Fog uses current timecyc.
    // Blocks are the ordered ScanThroughBlocks/BlockHit list, maximum 70.
    static bool BuildSeaBed(std::span<const RealtimeWaterBlock> blocks, float cameraX,
        float cameraY, int area, RealtimeSeaBedGeometry& out);
    // Read-only capture of the same presentation scanner used by both passes.
    static bool ScanOutsideWaterBlocks(float cameraX, float cameraY,
        std::array<RealtimeWaterBlock,70>& blocks, size_t& count);
    void DrawSeaBed(const RealtimeSeaBedGeometry& geometry) const;
    // Convenience path derives the five source frustum points from the current
    // rigid GL view + symmetric perspective projection, scans edge/ocean blocks
    // in row order, then builds/draws the pass. False on unsupported/invalid view.
    // Call after opaque objects, BEFORE DrawWater, once per presentation.
    bool DrawSeaBed(float cameraX, float cameraY, int area = 0) const;
    // Source water-level query's height rejection when wave parameters are
    // requested, AFTER polygon containment and base-height interpolation.
    // Authored bit 1 limits depth to six metres;
    // neither it nor the +20m query ceiling changes the rendered surface/floor.
    // This predicate is not a substitute for the full water/COL height query.
    static bool WaterQueryHeightAllowed(uint32_t authoredFlags, float baseHeight, float queryZ);
    // Explicit simulation clock/weather/accumulated flow; never wall time.
    // Read-modify-write GetWaterState to retain Load's named-weather default.
    bool SetWaterState(const RealtimeWaterState& state);
    const RealtimeWaterState& GetWaterState() const { return m_WaterState; }
    // Main simulation thread, before DrawWater. Load resets desired/current/UV
    // and cadence. Extra renders must not fabricate simulation ticks; reuse the
    // last tick verbatim or don't call. Replay every missed tick with its actual
    // camera/timeStep/area inputs; a skipped history is explicitly rejected.
    bool AdvanceWaterFlow(const RealtimeWaterFlowTick& tick);
    const RealtimeWaterFlowSelection& GetWaterFlowSelection() const { return m_WaterFlow.GetSelection(); }
    // Source 0x6E6EF0, without distance attenuation (render applies it first).
    RealtimeWaterSample SampleWater(int x, int y, float z, float big, float small) const;
    std::array<float, 2> WaterTextureShift(int layer) const;

    // Caller installs the camera's projection/view first (far plane >= farClip),
    // clears color/depth, then DrawSky(camera), BeginWorld(), draws world lists,
    // EndWorld(), BeginObjects(), draw actors, EndWorld(), DrawSeaBed(), DrawWater(). Scopes
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
    // Explicit original BlockHit list, useful for source replay/orthographic
    // probes. The three-argument presentation path scans the current frustum.
    void DrawWater(float cameraX, float cameraY, bool interior,
        std::span<const RealtimeWaterBlock> outsideBlocks) const;

    // Scope: timecyc ambient/directional lighting, sky-color dome, linear GL
    // eye-depth fog, authored waterclear256 layers, water.dat waves/normals,
    // outside-world ocean water and the source seabd32 ocean-floor pass.
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
    WorldShotImage m_SeaBedImage{};
    RealtimeWaterState m_WaterState{};
    RealtimeWaterFlow m_WaterFlow{};
    unsigned int m_WaterTexture = 0;
    unsigned int m_SeaBedTexture = 0;
    unsigned int m_LightingProgram = 0;
    mutable int m_PreviousProgram = 0; // lighting scopes must not nest
    mutable bool m_LightingActive = false;
    int m_WaterTriangles = 0;
    bool m_Loaded = false;
};
