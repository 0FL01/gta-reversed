// Live HUD using owned game textures. GL 2.1 compatibility context, main thread.
#pragma once

#include "app/platform/linux/MenuShot.h"
#include "app/platform/linux/RadarMap.h"
#include "app/platform/linux/NativeScriptEntities.h"
#include <string_view>

struct RealtimeHudView {
    // Native camera yaw: atan2(forwardY, forwardX), radians, +X = zero.
    float cameraYaw = 0.0f;
    bool radar = true;
    bool clock = true;
};

struct RealtimeHudState {
    float playerX = 0.0f, playerY = 0.0f;
    // Same +X/CCW convention as cameraYaw (NOT CEntity::GetHeading).
    float playerYaw = 0.0f;
    float radarRange = 180.0f; // CRadar::RADAR_MIN_RANGE; caller owns zoom.
    int hour = 0, minute = 0; // supplied game clock, never wall clock
    std::span<const NativeScriptRadarBlip> scriptBlips;
    // Source IsPlayerOnAMission is a declared script-global ==1, NOT the VM's
    // AlreadyRunningMission storage ownership flag (mission0 startup is not a mission).
    bool playerOnMission = false, exterior = true;
    unsigned radarZoom = 0;
    std::string_view helpText; // Entities.HelpPresentation().Text (owned, timed GXT)
    std::uint8_t helpAlpha = 0; // Entities.HelpPresentation().Alpha; zero draws nothing
};

class RealtimeHud {
public:
    RealtimeHud() = default;
    ~RealtimeHud();
    RealtimeHud(const RealtimeHud&) = delete;
    RealtimeHud& operator=(const RealtimeHud&) = delete;

    // CPU parsing MUST precede streaming Worker construction/start. Uses the
    // existing OS_File*/librw readers, retains only owned top-down RGBA copies.
    bool Load(const char* gameDir, char* err, std::size_t errSize);
    // Current main-thread GL context required. Upload/Draw/ReleaseGpu never
    // call OS parsers, TexSample or librw. Release before context destruction.
    bool Upload(char* err, std::size_t errSize);
    void ReleaseGpu();
    // After world/water, before swap. Width/height are drawable PIXELS, not SDL
    // logical window size. Restores GL attributes, matrices, program and active
    // texture. Does not modify depth/stencil contents or framebuffer binding.
    void Draw(const RealtimeHudView& view, const RealtimeHudState& state, int width, int height) const;

    // Initial source-backed slice: exterior radar, player/north, clock. No
    // fabricated health/armour/money, weapon/wanted/blip or script state.
private:
    RadarMapAssets m_Radar;
    MenuHudFont m_Font;
    WorldShotImage m_PropertyRadar{};
    std::array<unsigned int, 149> m_Textures{}; // tiles, centre/north/disc/font1/propertyR
    bool m_Loaded = false;
};
