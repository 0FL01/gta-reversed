// RadarMap: R6ae minimap slice (round 33).
// Renders a north-up circular minimap from the REAL radar tiles shipped in
// the game: 144 TXDs `radar00.txd`..`radar143.txd` inside `models/gta3.img`
// (VER2, 5 sectors each, 128x128 DXT1), decoded with the existing TXD path
// (librw TexDictionary::streamRead + TexSample_Decode). No procedural map,
// no hardcoded tile bytes: every map pixel is a nearest-neighbor fetch of a
// radar-TXD texel.
//
// Spec grounding (read-only, NOT linked into the binary):
// - game_sa/Radar.h: MAX_RADAR_WIDTH_TILES = 12, MAX_RADAR_HEIGHT_TILES = 12.
// - game_sa/Radar.cpp CRadar::Initialise: tile TXD slot name is
//   `radar{:02d}` with index y*12+x (std::format, minimum 2 digits, so
//   radar00..radar09, radar10.., radar100..radar143 verbatim).
// - game_sa/Radar.cpp CRadar::StreamRadarSections(const CVector&)/DrawRadarMap:
//   world->tile is x=floor((wx+3000)/500), y=ceil(11-(wy+3000)/500)
//   (tile spans 500m; map spans -3000..3000 in both axes).
// - game_sa/Radar.cpp GetTextureCorners: tile (x,y) covers world
//   x in [500*(x-6),500*(x-5)], y in [500*(5-y),500*(6-y)] (row 0 = north).
// - game_sa/Radar.cpp TransformRealWorldToTexCoordSpace: within a tile
//   u=(wx-(500x-3000))/500 west->east, v=-(wy-(500*(12-y)-3000))/500 with
//   v=0 at the north edge (texture row 0 = north, top-down).
// - game_sa/Radar.cpp CRadar::DrawRadarMap draws the 3x3 tiles around the
//   player tile (DrawRadarSection x-1..x+1, y-1..y+1); the mosaic here is
//   that same 3x3.
// - game_sa/Radar.cpp CRadar::DrawRadarMask: the round shape is an analytic
//   circle stencil (4 quarter-circle TRIFANs); game_sa/Hud.cpp CHud::DrawRadar
//   only stamps SPRITE_RADAR_DISC (models/hud.txd `radardisc`) into the 4
//   CORNERS around the masked circle. So the faithful CPU mask is an
//   analytic circle (logged as mask=circle); radardisc texels are corner
//   filler, not the disc itself.
// - Player marker: CRadar::DrawBlips draws RadarBlipSprites[RADAR_SPRITE_CENTRE]
//   (`radar_centre`, bound from the "hud" TXD slot per CRadar::LoadTextures)
//   rotated by heading. This slice uses models/hud.txd `arrow` 32x32
//   (same dictionary family, decoded, alpha-tested, north-up, unrotated —
//   heading is unknown headless; logged as marker=arrow-fixed) + the north
//   tag as a font2 glyph `N` via the existing MenuShot HUD-font blit
//   (game `radar_north` sprite is a compass N; the glyph is its text form).
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>
#include <array>
#include "app/platform/linux/TexSample.h"

struct RadarMapAssets {
    std::array<TexImage, 144> tiles; // y*12+x; top-down RGBA, no RW ownership
    TexImage centre, north, disc;
};
// Preload once before any streaming worker starts. No GL; all dictionaries
// are destroyed before returning. Draw-time consumers must only use copies.
bool RadarMap_LoadAssets(const char* gameDir, RadarMapAssets& out, char* err, std::size_t errSize);

struct RadarMapStats {
    char radarSrc[64] = {}; // always "models/gta3.img"
    char tileFmt[64] = {}; // "radar%02d idx=y*12+x"
    double worldX = 0.0; // requested center (world units)
    double worldY = 0.0;
    int tileX = 0; // center tile of (worldX,worldY)
    int tileY = 0;
    int tiles = 0; // unique decoded tiles (want 9)
    char tileNames[9][32] = {}; // mosaic order: row 0 = north
    char firstTile[32] = {}; // internal texture name of the center tile
    int tileW = 0; // uniform decoded tile size
    int tileH = 0;
    long tilePx = 0; // sum of decoded tile texels (want >10000)
    int discR = 0; // disc radius in frame px
    int discRange = 0; // world meters mapped to discR (m_radarRange)
    long discPixels = 0; // frame pixels inside the circle
    long oob = 0; // disc samples falling outside the 3x3 mosaic (want 0)
    char arrowName[32] = {}; // always "arrow"
    int arrowW = 0;
    int arrowH = 0;
    long markerPixels = 0; // opaque arrow pixels blitted (want >50)
    char fontTex[32] = {}; // always "font2"
    int glyphs = 0; // N glyphs drawn (want 1)
};

// Renders the radar disc for (worldX,worldY) into outRGBA (640x480 bottom-up
// RGBA, glReadPixels layout, so WriteTga24 applies unchanged). Returns false
// with err set (no radar-ok) on any failure: missing/short tiles, mixed tile
// sizes, undecodable arrow/font, off-map disc (oob>0), invisible marker.
bool RadarMap_Render(const char* gameDir, double worldX, double worldY,
                     std::vector<uint8_t>& outRGBA, RadarMapStats& stats, char* err,
                     std::size_t errSize);
void RadarMap_Shutdown();
