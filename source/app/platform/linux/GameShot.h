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

// Round 36 (R6ah): zone label over the game frame (--show-zone).
// Resolves the frame-center zone (fixed pier 836,-1866, same center as the
// shore/radar paths) through the EXISTING ZoneInfo path (data/info.zon
// smallest-zone rule) + the EXISTING GXT MAIN path (english/american.gxt
// display string), then blits the display string with the EXISTING font2
// glyph path (MenuShot centered blit, white ink + black drop shadow) at the
// fixed game-layout zone-name position (bottom-center, lower third).
// zonePos is fixed (kZoneCx,kZoneYTop) and logged verbatim; glyphs is the
// display-string length in characters (including the inter-word space), so
// the pier string counts 12; labelPixels counts frame pixels differing from
// the input frame after both shadow+ink passes. No hardcoded district
// string anywhere on this path (every byte comes from info.zon/GXT/font2).
// Without this call GameShot_Render output is untouched bit-for-bit.
struct ZoneLabelStats {
    char key[16] = {};  // display GXT key from info.zon (e.g. file bytes)
    char text[256] = {}; // display string from GXT MAIN (e.g. TDAT bytes)
    int level = 0;      // zone record level column
    int glyphs = 0;     // display-string characters incl. space (want 12)
    long labelPixels = 0; // frame pixels changed by the label (want >500)
    int posX = 0;       // fixed label center-x, top-down (want 320)
    int posY = 0;       // fixed label top-y, top-down (want 360)
    int cellH = 0;      // fixed glyph cell height px (want 40)
    int inkDrawn = 0;   // font2 ink glyphs drawn (excl. space)
};

// Fixed game-layout zone-name geometry at 640x480 (top-down): center-x 320
// (frame middle, as the retail district intro), top-y 360 (lower third
// 320..480, above the bottom edge with the 40px label ending at 400).
constexpr int kZoneLabelCx = 320;
constexpr int kZoneLabelYTop = 360;
constexpr int kZoneLabelCellH = 40;

// Overlays the zone label onto gamePixels in place (must be a 640x480
// bottom-up RGBA frame from GameShot_Render). Returns false with err set on
// any failure (zone/GXT/font); never modifies gamePixels on failure.
bool GameShot_ApplyZoneLabel(const char* gameDir, std::vector<uint8_t>& gamePixels,
                             ZoneLabelStats& out, char* err, std::size_t errSize);

// Round 37 (R6ai): wanted stars over the game frame (--wanted N).
// There is NO star sprite in any shipped TXD: models/hud.txd holds 69
// textures (radardisc/skipicon/siterocket/siteM16/radarRingPlane/fist/arrow +
// 62 radar_* icons, enumerated by the HudShot path), and models/misc.txd,
// models/particle.txd (only "coronastar", a corona effect, not HUD),
// models/fonts.txd (font1/font2 only), models/fronten*.txd,
// models/effectsPC.txd hold no star-named texture either (byte-level TXD
// field scan, see round log). The game draws wanted stars as FONT GLYPHS:
// re3 DrawWantedLevel (read-only spec, NOT linked) prints "]" per star with
// the heading/gothic font, and SA's models/fonts.txd:texture "font2" cell for
// byte 0x5D (col 13, row 3 of the 16x13 grid) is a 5-pointed star (direct
// DXT3 decode + per-cell ASCII audit; font1's 0x5D cell is a "]" bracket, so
// the star is font2-only). Confirmed by the modding community as well
// ("Wanted stars - fonts.txd, they are actually text characters").
// Hence this path blits N copies of the REAL font2 0x5D texels through the
// EXISTING MenuShot right-aligned glyph path (fonts.dat advances, black
// drop shadow + gold ink), right-justified to the bars' right edge above
// the health bar (top-right corner, as in the retail HUD). No procedural
// pixels: every star byte is a fonts.txd texel; an empty/missing glyph
// fails honestly (drawn != wanted, no game-ok).
struct WantedStats {
    int wanted = 0;     // requested star count (0..6)
    char starTex[32] = {}; // always "font2"
    char starSrc[64] = {}; // always "models/fonts.txd"
    int glyph = 0x5D;   // star glyph byte ("]" cell holds the star)
    int posRight = 0;   // fixed row right edge, top-down (want 608)
    int posTop = 0;     // fixed row top, top-down (want 0)
    int cellH = 0;      // fixed glyph cell height px (want 24)
    int drawn = 0;      // font2 ink glyphs drawn (want == wanted)
    long starPixels = 0; // frame pixels changed by the overlay (want >300 at 3)
};

// Fixed game-layout wanted-star geometry at 640x480 (top-down): right edge
// 608 (same STRETCH_FROM_RIGHT(32) edge as the health/armour bars and the
// clock), top 0 (the strip above the health bar at y=22; the star ink sits
// in the top ~2/3 of its cell, clear of the bar), cell height 24 (same
// order as the HUD digits). Ink is the re3 wanted-lit gold (193,164,120),
// shadow is black at (+1,+1) like the other HUD text passes.
constexpr int kWantedRight = 608;
constexpr int kWantedTop = 0;
constexpr int kWantedCellH = 24;
constexpr int kWantedInkR = 193;
constexpr int kWantedInkG = 164;
constexpr int kWantedInkB = 120;
constexpr char kWantedStarTex[] = "font2";
constexpr char kWantedStarSrc[] = "models/fonts.txd";
constexpr int kWantedStarGlyph = 0x5D;

// Overlays wanted stars onto gamePixels in place (must be a 640x480
// bottom-up RGBA frame from GameShot_Render). wanted==0 leaves the frame
// untouched (returns true with starPixels=0). Returns false with err set on
// any failure; never modifies gamePixels on failure.
bool GameShot_ApplyWanted(const char* gameDir, std::vector<uint8_t>& gamePixels, int wanted,
                          WantedStats& out, char* err, std::size_t errSize);
