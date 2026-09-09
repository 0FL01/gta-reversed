// GameShot implementation: HudShot path + RadarMap path + 1:1 disc blit.
// See GameShot.h for the contract and the spec grounding.

#include "app/platform/linux/GameShot.h"

#include <cstdio>
#include <cstring>
#include <vector>

namespace {

constexpr int kFbW = 640;
constexpr int kFbH = 480;
// Destination: game-layout radar center, top-down (see GameShot.h derivation:
// CHud::DrawRadar / TransformRadarPointToScreenSpace at 640x480).
constexpr int kDstCx = 87;
constexpr int kDstCy = 409;
// Source: RadarMap disc center, top-down (RadarMap.cpp kDiscCx/kDiscCy).
constexpr int kSrcCx = 320;
constexpr int kSrcCy = 240;
// Fixed pier world center (same as --shot-hud default and --shot-radar
// default: Santa Monica pier gaz_pier2 836,-1866).
constexpr double kPierX = 836.0;
constexpr double kPierY = -1866.0;

void SetErr(char* err, std::size_t errSize, const char* msg) {
    if (!err || errSize == 0) {
        return;
    }
    (void)std::snprintf(err, errSize, "%s", msg ? msg : "unknown");
}

} // namespace

bool GameShot_Render(const char* gameDir, int health, int armor, std::vector<uint8_t>& basePixels,
                     std::vector<uint8_t>& hudPixels, std::vector<uint8_t>& radarFull,
                     std::vector<uint8_t>& gamePixels, GameShotStats& stats, char* err,
                     std::size_t errSize) {
    stats = GameShotStats{};
    basePixels.clear();
    hudPixels.clear();
    radarFull.clear();
    gamePixels.clear();
    if (!gameDir || !gameDir[0]) {
        SetErr(err, errSize, "no game dir");
        return false;
    }
    if (health < 0 || health > 255 || armor < 0 || armor > 255) {
        SetErr(err, errSize, "bad health/armor (want 0-255)");
        return false;
    }

    // 1. Shore base + HUD overlay, through the existing HudShot path.
    {
        char herr[768] = {};
        if (!HudShot_Render(gameDir, health, armor, basePixels, hudPixels, stats.hud, herr,
                            sizeof(herr))) {
            char msg[896];
            (void)std::snprintf(msg, sizeof(msg), "hud-path: %s", herr);
            SetErr(err, errSize, msg);
            GameShot_Shutdown();
            return false;
        }
    }
    if (basePixels.size() != static_cast<std::size_t>(kFbW) * kFbH * 4 ||
        hudPixels.size() != static_cast<std::size_t>(kFbW) * kFbH * 4) {
        SetErr(err, errSize, "hud path returned bad frame size");
        GameShot_Shutdown();
        return false;
    }

    // 2. Radar disc, through the existing RadarMap path (same pier center).
    {
        char rerr[768] = {};
        if (!RadarMap_Render(gameDir, kPierX, kPierY, radarFull, stats.radar, rerr,
                             sizeof(rerr))) {
            char msg[896];
            (void)std::snprintf(msg, sizeof(msg), "radar-path: %s", rerr);
            SetErr(err, errSize, msg);
            GameShot_Shutdown();
            return false;
        }
    }
    if (radarFull.size() != static_cast<std::size_t>(kFbW) * kFbH * 4) {
        SetErr(err, errSize, "radar path returned bad frame size");
        GameShot_Shutdown();
        return false;
    }
    if (stats.radar.discR <= 0) {
        SetErr(err, errSize, "radar path returned bad discR");
        GameShot_Shutdown();
        return false;
    }

    // 3. Composition: hud frame + 1:1 disc blit into the left-bottom corner.
    // Radius is the RadarMap disc radius (R6ae r=190); source/dest centers as
    // documented in GameShot.h. Integer math, fixed traversal order.
    const int r = stats.radar.discR;
    stats.radarDstCx = kDstCx;
    stats.radarDstCy = kDstCy;
    stats.radarR = r;
    gamePixels = hudPixels;
    long copied = 0;
    const int r2 = r * r;
    for (int dy = -r; dy <= r; ++dy) {
        for (int dx = -r; dx <= r; ++dx) {
            if (dx * dx + dy * dy > r2) {
                continue;
            }
            const int sx = kSrcCx + dx;
            const int sy = kSrcCy + dy;
            const int dxp = kDstCx + dx;
            const int dyp = kDstCy + dy;
            if (sx < 0 || sx >= kFbW || sy < 0 || sy >= kFbH) {
                continue;
            }
            if (dxp < 0 || dxp >= kFbW || dyp < 0 || dyp >= kFbH) {
                continue;
            }
            const int sby = kFbH - 1 - sy;
            const int dby = kFbH - 1 - dyp;
            const uint8_t* src =
                radarFull.data() + (static_cast<std::size_t>(sby) * kFbW + sx) * 4;
            uint8_t* dst =
                gamePixels.data() + (static_cast<std::size_t>(dby) * kFbW + dxp) * 4;
            dst[0] = src[0];
            dst[1] = src[1];
            dst[2] = src[2];
            dst[3] = 255;
            ++copied;
        }
    }
    stats.radarPixels = copied;
    return true;
}

void GameShot_Shutdown() {
    HudShot_Shutdown();
    RadarMap_Shutdown();
}
