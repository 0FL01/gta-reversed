// MenuShot: main-menu 2D frame from real GXT strings + real TXD font texels.
// Round 7 (R6e). Menu contents follow game_sa/Frontend/FrontendScreensPC.h
// screen 34 (Main Menu, used as spec only): title key FEM_MM, items FEP_STG
// (Start Game) / FEP_OPT (Options) / FEP_QUI (Quit Game). Glyphs come from
// models/fonts.txd texture "font2" (CFont::Initialise binds Sprite[0]=font2,
// and FONT_MENU selects texture 0 via SetFontStyle) decoded with the existing
// TexSample decoder. Glyph mapping is the ASCII-order tile grid verified
// per-glyph against the decoded texture: 16 columns x 13 rows (= 208 cells,
// exactly the 208 fonts.dat prop entries) of 32x40 px cells, col = byte&15,
// row = (byte>>4)-2 (bytes 0x20-0xFF); rows hold space/punct, digits,
// capitals, lowercase top-down, each rendered glyph ('A','O','S','t','M','a',
// 'i','n','u','Q','p','o','s','G','e',...) is legible in its cell. No
// procedural glyphs, no hardcoded strings: every letter pixel is a font2
// texel, every word is GXT TDAT bytes. Pure CPU 2D blit (nearest-neighbor +
// alpha "over", source width clipped to the fonts.dat advance so wide glyphs
// never drag the neighbour's edge in), no GL textures needed. Output is
// bottom-up RGBA (glReadPixels layout) so the shared TGA writer applies
// unchanged. Deterministic: integer math only.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

struct MenuShotStats {
    char lang[32] = {};
    char gxtFile[64] = {};
    int gxtKeys = 0; // MAIN TKEY entry count
    char itemKey[4][16] = {};
    char itemText[4][256] = {};
    int strings = 0; // menu strings found in the table (want 4)
    char fontName[32] = {};
    int fontW = 0;
    int fontH = 0;
    long solidTexels = 0; // font texels with alpha>=128 (glyph ink present)
    int glyphs = 0;       // non-space glyphs drawn (want >= 30)
    uint32_t missing[16] = {};
    int missingCount = 0; // distinct non-space bytes with empty tiles
};

bool MenuShot_Render(const char* gameDir, const char* lang, std::vector<uint8_t>& outRGBA,
                      MenuShotStats& stats, char* err, std::size_t errSize);
// Round 8 (R6f): same frame with an explicit highlight index (0..2 selects
// Start Game / Options / Quit Game). selectedIx==0 is bit-identical to
// MenuShot_Render (static --shot-menu etalon path).
bool MenuShot_RenderSelected(const char* gameDir, const char* lang, int selectedIx,
                             std::vector<uint8_t>& outRGBA, MenuShotStats& stats, char* err,
                             std::size_t errSize);
// Round 32 (R6ad): HUD digit support on the SAME glyph path. Loads font2
// texels + fonts.dat metrics without rendering a menu frame, then draws
// right-aligned ASCII strings (same cells/tint/alpha-over as the menu).
// Additive only: MenuShot_Render/RenderSelected are untouched bit-for-bit.
struct MenuHudFont {
    char name[32] = {};
    int w = 0;
    int h = 0;
    int prop[208] = {};
    int unprop = 27;
    int space = 10;
    std::vector<uint8_t> rgba; // font texels, top row first (font.w*font.h*4)
    int texW = 0;
    int texH = 0;
    bool ok = false;
};
bool MenuShot_LoadHudFont(const char* gameDir, MenuHudFont& font, char* err, std::size_t errSize);
// Draws ASCII text right-aligned (last glyph ends at xRight, exclusive) in
// top-down framebuffer coords on bottom-up RGBA. Returns drawn glyph count.
int MenuShot_DrawTextRight(std::vector<uint8_t>& px, int fbW, int fbH, const MenuHudFont& font,
                           const char* text, int xRight, int yTop, int cellDstH, uint8_t cr,
                           uint8_t cg, uint8_t cb);
void MenuShot_Shutdown();
