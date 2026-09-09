// GameShot: R6af game screen slice (round 34).
// Composes ONE 640x480 frame from the three EXISTING game-byte paths, by
// CALLING them (no copy-paste of their internals):
//   1. ShoreShot+HudShot path: HudShot_Render(gameDir,H,A) renders the fixed
//      pier shore base (hour 12, center 836,-1866, same camera) + health/armour
//      bars + H/A digits + clock from models/hud.txd:radardisc texels (tinted
//      by HudColours) and models/fonts.txd:font2 glyphs.
//   2. RadarMap path: RadarMap_Render(gameDir,836,-1866) renders the 3x3
//      radar-TXD mosaic disc (r=190, analytic circle mask) + hud.txd `arrow`
//      marker + font2 `N` from models/gta3.img radar tiles.
// The final frame starts as a copy of the HudShot frame, then the radar disc
// (same texels, 1:1, no resampling, no procedural pixels) is blitted into the
// left-bottom corner over it.
//
// Spec grounding for the corner (read-only, NOT linked):
// - game_sa/Radar.cpp TransformRadarPointToScreenSpace (in=0 = player center):
//     x = STRETCH_X(94)/2 + STRETCH_X(40)
//     y = STRETCH_FROM_BOTTOM(104) + STRETCH_Y(76)/2
//   At the 640x480 HUD frame STRETCH_X is identity (640/640) and STRETCH_Y =
//   480/448, so cx = 47+40 = 87, cy = (480-104*480/448) + (76*480/448)/2 =
//   368.571429+40.714286 = 409.285714 -> 409 (top-down, origin top-left).
//   This is the CRadar origin drawn by CHud::DrawRadar (game_sa/Hud.cpp:
//   rects around SPRITE_RADAR_DISC at STRETCH_X(36..87) / FROM_BOTTOM(108..66)
//   frame the same center; CRadar::DrawMap centers the masked map there).
//   Hence radarPos=87,409 (top-down 640x480 coords), logged verbatim.
// - Source disc geometry is RadarMap's (RadarMap.cpp kDiscCx=320, kDiscCy=240,
//   kDiscR=190, kDiscRangeM=350): the blit copies source offset (dx,dy) with
//   dx*dx+dy*dy<=R*R from (320+dx,240+dy) to (87+dx,409+dy), clipped to the
//   frame. Every copied byte comes from the RadarMap output (radar TXD texels
//   + arrow texels already alpha-tested there); the dark surround (12,14,22)
//   is never copied (outside-circle skip), so the world shows through.
// - N glyph: lives in the RadarMap full frame outside the circle (rim tag);
//   the game frame carries disc+arrow (Rp proof); Cr is checksummed on the
//   FULL RadarMap frame (N included), so the radarMatchesRadar proof covers it.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "app/platform/linux/HudShot.h"
#include "app/platform/linux/RadarMap.h"

struct GameShotStats {
    HudShotStats hud{}; // base+overlay from the HudShot path
    RadarMapStats radar{}; // full radar frame from the RadarMap path
    int radarDstCx = 0; // game-layout disc center, top-down (want 87)
    int radarDstCy = 0; // game-layout disc center, top-down (want 409)
    int radarR = 0; // blitted radius, px (want 190, == RadarMap discR)
    long radarPixels = 0; // disc pixels copied into the game frame (want >20000)
};

// Bit-identical etalons the composition MUST reproduce (gates, not inputs):
// shore base (round 31), HUD overlay at default 137/60 (round 32), radar disc
// at the pier 836,-1866 (round 33).
constexpr uint64_t kGameShoreEtalon = 3518993618115791197ULL;
constexpr uint64_t kGameHudEtalon = 10922981183498037905ULL;
constexpr uint64_t kGameRadarEtalon = 12130037637705462246ULL;

// Renders base + HUD (via HudShot_Render) + radar disc (via RadarMap_Render)
// into gamePixels. basePixels/hudPixels/radarFull are returned for the
// checksum proofs (C0/Ch/Cr); gamePixels is hudPixels + radar disc.
// Returns false with err set (no game-ok) on any failure; never invents
// texels (all bytes come from the two called paths).
bool GameShot_Render(const char* gameDir, int health, int armor, std::vector<uint8_t>& basePixels,
                     std::vector<uint8_t>& hudPixels, std::vector<uint8_t>& radarFull,
                     std::vector<uint8_t>& gamePixels, GameShotStats& stats, char* err,
                     std::size_t errSize);
void GameShot_Shutdown();
