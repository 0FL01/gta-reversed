// Native compatibility-GL port of CClouds::Render_RenderLowClouds only.
#pragma once

#include "app/platform/linux/WorldShot.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <thread>

struct RealtimeCloudCamera {
    // Column-major world camera matrices. ModelView has no object transform.
    std::array<float, 16> ModelView{}, Projection{};
    std::array<float, 3> Position{};
    float NearClip = std::numeric_limits<float>::quiet_NaN();
    float FarClip = std::numeric_limits<float>::quiet_NaN();
    // Source CDraw::FOV for sprite size, independent of Projection (currently
    // production Camera::Apply is 60 degrees while this source value is 70).
    float Fov = std::numeric_limits<float>::quiet_NaN();
    float Roll = std::numeric_limits<float>::quiet_NaN(); // TheCamera.GetRoll(), radians
    int Width = 0, Height = 0; // drawable pixels
};

struct RealtimeCloudState {
    // Unknown metadata is never interpreted as clear weather or exterior.
    bool HasExteriorVisibility = false;
    bool CanSeeOutside = false;
    bool HasWeather = false;
    float Foggyness = std::numeric_limits<float>::quiet_NaN();
    float CloudCoverage = std::numeric_limits<float>::quiet_NaN();
    float ExtraSunnyness = std::numeric_limits<float>::quiet_NaN();
    float Wind = std::numeric_limits<float>::quiet_NaN();
    bool HasClock = false;
    float Hour = std::numeric_limits<float>::quiet_NaN();
    std::uint32_t GameMs = 0;
    bool HasLowCloudColours = false;
    // CTimeCycle::m_CurrentColours.m_nLowClouds* after interpolation/boxes.
    // m_fCloudAlpha belongs to other layers and is not consumed here.
    std::array<std::uint8_t, 3> LowCloudColours{};
};

struct RealtimeCloudVertex {
    float X = 0.0f, Y = 0.0f;
    float Depth = 0.0f, ReciprocalDepth = 0.0f;
    float U = 0.0f, V = 0.0f;
    std::array<std::uint8_t, 4> Colour{};
    bool operator==(const RealtimeCloudVertex&) const = default;
};

struct RealtimeCloudGeometry {
    // Twelve source sprites, two independent triangles per sprite.
    std::array<RealtimeCloudVertex, 12 * 6> Vertices{};
    std::size_t Size = 0;
    std::size_t SpriteCount = 0; // submitted after source near test, not visible-pixel count
    bool operator==(const RealtimeCloudGeometry&) const = default;
};

enum class RealtimeCloudResult {
    Unsupported,
    Hidden,
    Ready,
};

class RealtimeClouds {
public:
    RealtimeClouds();
    ~RealtimeClouds();
    RealtimeClouds(const RealtimeClouds&) = delete;
    RealtimeClouds& operator=(const RealtimeClouds&) = delete;

    // CPU-only startup load. Call before constructing the streaming worker.
    // Borrows the shared parser, restores its current TXD, and retains only an
    // owned top-down RGBA copy of particle.txd:cloud1. No later method does IO
    // or calls librw. Reloading requires ReleaseGpu first.
    bool Load(const char* gameDir, char* err, std::size_t errSize);
    // Current main-thread GL 2.1 compatibility context required; startup before
    // the streaming worker. ReleaseGpu/destruction precedes context teardown.
    bool Upload(char* err, std::size_t errSize);
    void ReleaseGpu();

    const WorldShotImage* PreparedImage() const { return m_Loaded ? &m_CloudImage : nullptr; }
    bool IsUploaded() const { return m_Texture != 0; }

    // Captures the already-installed world camera. Near/Far/Fov/Roll are
    // explicit source values because they cannot be recovered from GL exactly.
    static RealtimeCloudCamera CaptureCamera(std::array<float, 3> position,
        int width, int height, float nearClip, float farClip, float fov, float roll);

    // Caller-source positions, near test, sizing and colour balance. The
    // 0x70EAB0 sprite callback is unreversed: quad expansion/UV/byte intensity
    // are the native adapter, not a claim of executable-level equivalence.
    // Wind and clock are validated source inputs but do not
    // alter this layer; CClouds only applies them to other cloud layers/update.
    static RealtimeCloudResult BuildLowClouds(const RealtimeCloudCamera& camera,
        const RealtimeCloudState& state, RealtimeCloudGeometry& out);

    // Source order: after DrawSky and before opaque world geometry. Draw uses
    // additive ONE/ONE blending, no depth test/write or fog, and restores GL
    // attributes, matrices, program, active texture and viewport.
    RealtimeCloudResult Draw(const RealtimeCloudCamera& camera,
        const RealtimeCloudState& state) const;

private:
    WorldShotImage m_CloudImage{};
    unsigned int m_Texture = 0;
    bool m_Loaded = false;
    const std::thread::id m_Owner;
};
