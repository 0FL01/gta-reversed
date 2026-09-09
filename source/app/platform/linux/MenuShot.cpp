// MenuShot implementation: GXT menu strings + TXD font-texel 2D blit.
// See MenuShot.h for the contract. Menu layout mirrors the real main menu
// (title on top, three centered items, first selected) at 640x480.

#include "app/platform/linux/MenuShot.h"

#include "app/platform/linux/GxtText.h"
#include "app/platform/linux/TexSample.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using int8 = int8_t;
using int16 = int16_t;
using int32 = int32_t;
using int64 = int64_t;
using uint8 = uint8_t;
using uint16 = uint16_t;
using uint32 = uint32_t;
using uint64 = uint64_t;

#ifndef __stdcall
#define __stdcall
#endif

#include "oswrapper/oswrapper.h"

#include <rw.h>

namespace {

constexpr int kWidth = 640;
constexpr int kHeight = 480;

// Main-menu screen 34 (FrontendScreensPC.h): title + 3 items.
constexpr const char* kKeys[4] = { "FEM_MM", "FEP_STG", "FEP_OPT", "FEP_QUI" };

void SetErr(char* err, std::size_t errSize, const char* msg) {
    if (!err || errSize == 0) {
        return;
    }
    (void)std::snprintf(err, errSize, "%s", msg ? msg : "unknown");
}

bool ReadWholeFile(const char* path, std::vector<uint8>& out) {
    void* file = nullptr;
    if (OS_FileOpen(FILE_DATA_AREA_DEFAULT, &file, path, FILE_ACCESS_READ) != 0 || !file) {
        return false;
    }
    int32 size = OS_FileSize(file);
    if (size < 0) {
        OS_FileClose(file);
        return false;
    }
    out.resize(static_cast<std::size_t>(size));
    bool ok = true;
    if (size > 0) {
        ok = OS_FileRead(file, out.data(), size) == 0;
    }
    OS_FileClose(file);
    return ok;
}

// --- librw engine for TXD parsing (same plugin set as WorldShot) ---

bool s_rwInit = false;
rw::TexDictionary* s_fontTxd = nullptr;

bool RwInitEngine() {
    if (s_rwInit) {
        return true;
    }
    if (!rw::Engine::init(nil)) {
        return false;
    }
    rw::ps2::registerPDSPlugin(40);
    rw::ps2::registerPluginPDSPipes();
    rw::registerMeshPlugin();
    rw::registerNativeDataPlugin();
    rw::registerAtomicRightsPlugin();
    rw::registerMaterialRightsPlugin();
    rw::xbox::registerVertexFormatPlugin();
    rw::registerSkinPlugin();
    rw::registerUserDataPlugin();
    rw::registerHAnimPlugin();
    rw::registerMatFXPlugin();
    rw::registerUVAnimPlugin();
    rw::ps2::registerADCPlugin();
    if (!rw::Engine::open(nil) || !rw::Engine::start()) {
        return false;
    }
    rw::Texture::setLoadTextures(false);
    s_rwInit = true;
    return true;
}

// --- data/fonts.dat proportional metrics (font 0 = menu texture font2) ---
// Format (cf. CFont::LoadFontValues): [TOTAL_FONTS], per font [FONT_ID] id,
// [REPLACEMENT_SPACE_CHAR] value, [PROP] 26 lines x 8 ints (208 entries),
// [UNPROP] value. Units are texture px (cell = 32px).

struct FontMetrics {
    int prop[208] = {};
    int unprop = 27;
    int space = 10;
    bool ok = false;
};

bool ParseFontsDat(const std::vector<uint8>& bytes, FontMetrics& out) {
    out = FontMetrics{};
    std::string text(bytes.begin(), bytes.end());
    // Split into lines (tolerate CRLF).
    std::vector<std::string> lines;
    {
        std::size_t pos = 0;
        while (pos <= text.size()) {
            std::size_t end = text.find('\n', pos);
            std::string line = text.substr(pos, end == std::string::npos ? end - pos : end - pos);
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
                line.pop_back();
            }
            std::size_t first = line.find_first_not_of(" \t");
            lines.push_back(first == std::string::npos ? std::string() : line.substr(first));
            if (end == std::string::npos) {
                break;
            }
            pos = end + 1;
        }
    }
    int curFont = -1;
    int propRow = -1; // >=0 while reading the 26 PROP rows of font 0
    bool wantValue = false;
    enum class Want { None, Id, Space, Unprop };
    Want want = Want::None;
    for (const std::string& line : lines) {
        if (line.empty() || line[0] == '#') {
            continue;
        }
        if (propRow >= 0) {
            int v[8] = {};
            if (std::sscanf(line.c_str(), "%d %d %d %d %d %d %d %d", &v[0], &v[1], &v[2],
                             &v[3], &v[4], &v[5], &v[6], &v[7]) != 8) {
                return false;
            }
            if (curFont == 0) {
                for (int k = 0; k < 8; ++k) {
                    out.prop[propRow * 8 + k] = v[k];
                }
            }
            if (++propRow >= 26) {
                propRow = -1;
            }
            continue;
        }
        if (want != Want::None) {
            int val = 0;
            if (std::sscanf(line.c_str(), "%d", &val) != 1) {
                return false;
            }
            if (want == Want::Id) {
                curFont = val;
            } else if (curFont == 0) {
                if (want == Want::Space) {
                    out.space = val;
                } else {
                    out.unprop = val;
                }
            }
            want = Want::None;
            (void)wantValue;
            continue;
        }
        if (line == "[FONT_ID]") {
            want = Want::Id;
        } else if (line == "[REPLACEMENT_SPACE_CHAR]") {
            want = Want::Space;
        } else if (line == "[PROP]") {
            propRow = 0;
        } else if (line == "[UNPROP]") {
            want = Want::Unprop;
        }
    }
    // Sanity: first PROP row of font 0 is "12 13 13 28 28 28 28 8".
    out.ok = (out.prop[0] == 12 && out.prop[3] == 28 && out.unprop > 0 && out.space > 0);
    return out.ok;
}

int AdvanceFor(uint8 ch, const FontMetrics& m) {
    if (ch == ' ') {
        return m.space;
    }
    // CFont::GetCharacterSize for FONT_MENU (m_FontStyle=2): the letterId
    // (byte-0x20) goes through FindSubFontCharacter(style=2) before indexing
    // gFontData[0].m_propValues (both reversed in Font.cpp). This is what
    // makes M advance 30 (measured ink width), not the direct prop entry.
    int lid = static_cast<int>(ch) - 0x20;
    int sub = lid;
    if (lid == 6) {
        sub = 10;
    } else if (lid >= 16 && lid <= 25) {
        sub = (lid - 128) & 0xFF; // uint8 wrap (digits -> 144..153)
    } else if (lid == 31) {
        sub = 91;
    } else if (lid >= 33 && lid <= 58) {
        sub = lid + 122;
    } else if (lid == 62) {
        sub = 32;
    } else if (lid >= 65 && lid <= 90) {
        sub = lid + 90;
    } else if (lid >= 96 && lid <= 118) {
        sub = lid + 85;
    } else if (lid >= 119 && lid <= 140) {
        sub = lid + 62;
    } else if (lid == 141 || lid == 142) {
        sub = 204;
    } else if (lid == 143) {
        sub = 205;
    }
    if (sub < 0 || sub > 207) {
        return m.unprop;
    }
    int w = m.prop[sub];
    return w > 0 ? w : m.unprop;
}

// --- framebuffer helpers (bottom-up RGBA, glReadPixels layout) ---

void FillRect(std::vector<uint8>& px, int x0, int y0, int x1, int y1, uint8 r, uint8 g,
              uint8 b) {
    // y0/y1 are TOP-DOWN; convert to bottom-up rows.
    if (x0 < 0) {
        x0 = 0;
    }
    if (y0 < 0) {
        y0 = 0;
    }
    if (x1 > kWidth) {
        x1 = kWidth;
    }
    if (y1 > kHeight) {
        y1 = kHeight;
    }
    for (int y = y0; y < y1; ++y) {
        int by = kHeight - 1 - y;
        for (int x = x0; x < x1; ++x) {
            uint8* dst = px.data() + (static_cast<std::size_t>(by) * kWidth + x) * 4;
            dst[0] = r;
            dst[1] = g;
            dst[2] = b;
            dst[3] = 255;
        }
    }
}

// Blits one ASCII-order cell (col=ch&15, row=(ch>>4)-2, 32x40 px) from the
// font image scaled to (cellDstW, cellDstH) px, tinted by (cr,cg,cb), alpha
// "over" blend. Source width is clipped to srcAdvW (the fonts.dat advance in
// texture px) so a wide glyph never drags the neighbour cell's edge in.
// Integer math only (deterministic). Returns false when the tile is empty.
bool BlitGlyph(std::vector<uint8>& px, const TexImage& font, int xTop, int yTop, int cellDstW,
               int cellDstH, int srcAdvW, uint8 ch, uint8 cr, uint8 cg, uint8 cb,
               bool& empty) {
    empty = false;
    const int cellW = 32;
    const int cellH = 40;
    if (ch < 0x20) {
        empty = true;
        return false;
    }
    const int col = ch & 15;
    const int row = (ch >> 4) - 2;
    if (row < 0 || row >= 13) {
        empty = true;
        return false;
    }
    const int sx0 = col * cellW;
    const int sy0 = row * cellH;
    int srcW = srcAdvW < cellW ? srcAdvW : cellW;
    if (srcW <= 0) {
        empty = true;
        return false;
    }
    // Empty-tile probe: any ink in the clipped box?
    bool ink = false;
    for (int sy = 0; sy < cellH && !ink; ++sy) {
        const uint8* texRow =
            font.rgba.data() + (static_cast<std::size_t>(sy0 + sy) * font.w + sx0) * 4;
        for (int sx = 0; sx < srcW; ++sx) {
            if (texRow[sx * 4 + 3] >= 16) {
                ink = true;
                break;
            }
        }
    }
    if (!ink) {
        empty = true;
        return false;
    }
    for (int dy = 0; dy < cellDstH; ++dy) {
        int y = yTop + dy;
        if (y < 0 || y >= kHeight) {
            continue;
        }
        int by = kHeight - 1 - y;
        int sy = sy0 + dy * cellH / cellDstH;
        for (int dx = 0; dx < cellDstW; ++dx) {
            int x = xTop + dx;
            if (x < 0 || x >= kWidth) {
                continue;
            }
            int sx = sx0 + dx * srcW / cellDstW;
            const uint8* src =
                font.rgba.data() + (static_cast<std::size_t>(sy) * font.w + sx) * 4;
            uint8 a = src[3];
            if (a == 0) {
                continue;
            }
            // Texel RGB (white ink) tinted by the pen color, then "over".
            int tr = (src[0] * cr + 127) / 255;
            int tg = (src[1] * cg + 127) / 255;
            int tb = (src[2] * cb + 127) / 255;
            uint8* dst = px.data() + (static_cast<std::size_t>(by) * kWidth + x) * 4;
            int inv = 255 - a;
            dst[0] = static_cast<uint8>((tr * a + dst[0] * inv + 127) / 255);
            dst[1] = static_cast<uint8>((tg * a + dst[1] * inv + 127) / 255);
            dst[2] = static_cast<uint8>((tb * a + dst[2] * inv + 127) / 255);
            dst[3] = 255;
        }
    }
    return true;
}

// Line width in framebuffer px for centering (tokens ~x~ skipped like CFont).
int LineWidthPx(const std::string& s, const FontMetrics& m, int cellDstH) {
    long w = 0;
    for (std::size_t i = 0; i < s.size();) {
        uint8 ch = static_cast<uint8>(s[i]);
        if (ch == '~') {
            std::size_t j = s.find('~', i + 1);
            i = (j == std::string::npos) ? s.size() : j + 1;
            continue;
        }
        w += AdvanceFor(ch, m);
        ++i;
    }
    return static_cast<int>(w * cellDstH / 40);
}

// Draws one centered string; advances come from fonts.dat. Records empty
// (missing) tiles in stats; returns the drawn-glyph count.
int DrawCentered(std::vector<uint8>& px, const TexImage& font, const std::string& s,
                 const FontMetrics& m, int yTop, int cellDstH, uint8 cr, uint8 cg, uint8 cb,
                 MenuShotStats& stats) {
    int drawn = 0;
    int wpx = LineWidthPx(s, m, cellDstH);
    int x = (kWidth - wpx) / 2;
    for (std::size_t i = 0; i < s.size();) {
        uint8 ch = static_cast<uint8>(s[i]);
        if (ch == '~') {
            std::size_t j = s.find('~', i + 1);
            i = (j == std::string::npos) ? s.size() : j + 1;
            continue;
        }
        int adv = AdvanceFor(ch, m);
        if (ch == ' ') {
            x += adv * cellDstH / 40;
            ++i;
            continue;
        }
        int advDst = adv * cellDstH / 40;
        // Draw into exactly the advance box (not the full cell): the cell
        // carries side bearings shared with neighbours; painting the whole
        // cell at pen positions would overlap consecutive glyphs.
        bool empty = false;
        if (BlitGlyph(px, font, x, yTop, advDst, cellDstH, adv, ch, cr, cg, cb, empty)) {
            ++drawn;
        } else if (empty && stats.missingCount < 16) {
            bool dup = false;
            for (int k = 0; k < stats.missingCount; ++k) {
                if (stats.missing[k] == ch) {
                    dup = true;
                    break;
                }
            }
            if (!dup) {
                stats.missing[stats.missingCount++] = ch;
            }
        }
        x += advDst;
        ++i;
    }
    return drawn;
}

} // namespace

void MenuShot_Shutdown() {
    if (s_fontTxd) {
        s_fontTxd->destroy();
        s_fontTxd = nullptr;
    }
}

static bool RenderWithSelected(const char* gameDir, const char* lang, int selectedIx,
                               std::vector<uint8_t>& outRGBA, MenuShotStats& stats, char* err,
                               std::size_t errSize) {
    stats = MenuShotStats{};
    (void)std::snprintf(stats.lang, sizeof(stats.lang), "%s", lang ? lang : "english");
    if (selectedIx < 0 || selectedIx > 2) {
        SetErr(err, errSize, "selectedIx out of range (want 0..2)");
        return false;
    }
    if (!gameDir || !gameDir[0]) {
        SetErr(err, errSize, "no game dir");
        return false;
    }
    OS_SetFilePathOffset(gameDir);

    // 1. GXT menu strings (MAIN table).
    GxtTable table;
    if (!GxtText_Load(gameDir, lang, table, err, errSize)) {
        return false;
    }
    (void)std::snprintf(stats.gxtFile, sizeof(stats.gxtFile), "%s", table.file.c_str());
    stats.gxtKeys = static_cast<int>(table.entries.size());
    std::string items[4];
    stats.strings = 0;
    for (int i = 0; i < 4; ++i) {
        (void)std::snprintf(stats.itemKey[i], sizeof(stats.itemKey[i]), "%s", kKeys[i]);
        std::string text;
        if (!GxtText_Find(table, kKeys[i], text) || text.empty()) {
            (void)std::snprintf(err, errSize, "gxt key missing: %s", kKeys[i]);
            return false;
        }
        items[i] = text;
        (void)std::snprintf(stats.itemText[i], sizeof(stats.itemText[i]), "%s", text.c_str());
        ++stats.strings;
    }

    // 2. Font texels from models/fonts.txd (texture font2, cf. CFont).
    std::vector<uint8> txdBytes;
    if (!ReadWholeFile("models/fonts.txd", txdBytes) || txdBytes.empty()) {
        SetErr(err, errSize, "cannot read models/fonts.txd");
        return false;
    }
    if (!RwInitEngine()) {
        SetErr(err, errSize, "librw Engine::init failed");
        return false;
    }
    rw::StreamMemory stream;
    stream.open(txdBytes.data(), static_cast<uint32>(txdBytes.size()));
    rw::TexDictionary* txd = nil;
    if (rw::findChunk(&stream, rw::ID_TEXDICTIONARY, nil, nil)) {
        txd = rw::TexDictionary::streamRead(&stream);
    }
    stream.close();
    if (!txd) {
        SetErr(err, errSize, "fonts.txd parse failed");
        return false;
    }
    if (s_fontTxd) {
        s_fontTxd->destroy();
        s_fontTxd = nullptr;
    }
    s_fontTxd = txd;
    const char* wantTex = "font2";
    rw::Texture* tex = txd->find(wantTex);
    if (!tex) {
        wantTex = "font1";
        tex = txd->find(wantTex);
    }
    if (!tex) {
        SetErr(err, errSize, "fonts.txd has neither font2 nor font1");
        return false;
    }
    TexImage font;
    if (!TexSample_Decode(tex, font) || font.rgba.empty()) {
        SetErr(err, errSize, "font texture decode failed");
        return false;
    }
    (void)std::snprintf(stats.fontName, sizeof(stats.fontName), "%s", wantTex);
    stats.fontW = font.w;
    stats.fontH = font.h;
    // font2 is 512x512: 16 columns x 13 rows of 32x40 px ASCII-order cells
    // (13*16 = 208 = fonts.dat prop entries; verified per-glyph).
    if (font.w != 512 || font.h != 512) {
        SetErr(err, errSize, "font texture is not 512x512");
        return false;
    }
    long solid = 0;
    for (std::size_t i = 3; i < font.rgba.size(); i += 4) {
        if (font.rgba[i] >= 128) {
            ++solid;
        }
    }
    stats.solidTexels = solid;
    if (solid == 0) {
        SetErr(err, errSize, "font texture has no ink");
        return false;
    }

    // 3. Proportional metrics from data/fonts.dat (font 0).
    std::vector<uint8> fontsDat;
    if (!ReadWholeFile("data/fonts.dat", fontsDat) || fontsDat.empty()) {
        SetErr(err, errSize, "cannot read data/fonts.dat");
        return false;
    }
    FontMetrics metrics;
    if (!ParseFontsDat(fontsDat, metrics)) {
        SetErr(err, errSize, "data/fonts.dat parse failed");
        return false;
    }

    // 4. Frame: dark background + panel, title + 3 items, first selected.
    outRGBA.resize(static_cast<std::size_t>(kWidth) * kHeight * 4);
    for (std::size_t i = 0; i < outRGBA.size(); i += 4) {
        outRGBA[i] = 10;
        outRGBA[i + 1] = 12;
        outRGBA[i + 2] = 22;
        outRGBA[i + 3] = 255;
    }
    FillRect(outRGBA, 120, 56, 520, 424, 17, 21, 36); // panel
    FillRect(outRGBA, 120, 56, 520, 58, 64, 68, 96); // panel top edge
    FillRect(outRGBA, 120, 422, 520, 424, 64, 68, 96); // panel bottom edge

    const int titleCellH = 36; // 32x40 cell -> ~29x36 px
    const int itemCellH = 28;  // 32x40 cell -> ~22x28 px
    // Title (top-down y).
    stats.glyphs += DrawCentered(outRGBA, font, items[0], metrics, 92, titleCellH, 255, 255, 255,
                                 stats);
    // Items.
    const int y0 = 208;
    const int step = 64;
    for (int row = 1; row < 4; ++row) {
        int y = y0 + (row - 1) * step;
        if (row == 1 + selectedIx) { // selected: highlight bar + gold ink
            FillRect(outRGBA, 132, y - 8, 508, y + itemCellH + 8, 141, 52, 22);
        }
        uint8 cr = 225, cg = 225, cb = 225;
        if (row == 1 + selectedIx) {
            cr = 255;
            cg = 200;
            cb = 90;
        }
        stats.glyphs +=
            DrawCentered(outRGBA, font, items[row], metrics, y, itemCellH, cr, cg, cb, stats);
    }
    return true;
}

bool MenuShot_Render(const char* gameDir, const char* lang, std::vector<uint8_t>& outRGBA,
                     MenuShotStats& stats, char* err, std::size_t errSize) {
    return RenderWithSelected(gameDir, lang, 0, outRGBA, stats, err, errSize);
}

bool MenuShot_RenderSelected(const char* gameDir, const char* lang, int selectedIx,
                             std::vector<uint8_t>& outRGBA, MenuShotStats& stats, char* err,
                             std::size_t errSize) {
    return RenderWithSelected(gameDir, lang, selectedIx, outRGBA, stats, err, errSize);
}

// --- Round 32 (R6ad) HUD helpers: same engine/plugins/glyph path, no menu. ---

static bool s_hudRwInit = false;

static bool HudRwInitEngine() {
    if (s_hudRwInit) {
        return true;
    }
    // Tolerant like CarPose: ShoreShot_Init (StreamPager) may have already
    // brought the engine up in this process; reuse it instead of failing.
    if (rw::Engine::state != rw::Engine::Dead) {
        rw::Texture::setLoadTextures(false);
        s_hudRwInit = true;
        return true;
    }
    if (!rw::Engine::init(nil)) {
        return false;
    }
    rw::ps2::registerPDSPlugin(40);
    rw::ps2::registerPluginPDSPipes();
    rw::registerMeshPlugin();
    rw::registerNativeDataPlugin();
    rw::registerAtomicRightsPlugin();
    rw::registerMaterialRightsPlugin();
    rw::xbox::registerVertexFormatPlugin();
    rw::registerSkinPlugin();
    rw::registerUserDataPlugin();
    rw::registerHAnimPlugin();
    rw::registerMatFXPlugin();
    rw::registerUVAnimPlugin();
    rw::ps2::registerADCPlugin();
    if (!rw::Engine::open(nil) || !rw::Engine::start()) {
        return false;
    }
    rw::Texture::setLoadTextures(false);
    s_hudRwInit = true;
    return true;
}

bool MenuShot_LoadHudFont(const char* gameDir, MenuHudFont& font, char* err, std::size_t errSize) {
    font = MenuHudFont{};
    if (!gameDir || !gameDir[0]) {
        SetErr(err, errSize, "no game dir");
        return false;
    }
    OS_SetFilePathOffset(gameDir);
    std::vector<uint8> txdBytes;
    if (!ReadWholeFile("models/fonts.txd", txdBytes) || txdBytes.empty()) {
        SetErr(err, errSize, "cannot read models/fonts.txd");
        return false;
    }
    if (!HudRwInitEngine()) {
        SetErr(err, errSize, "librw Engine::init failed");
        return false;
    }
    rw::StreamMemory stream;
    stream.open(txdBytes.data(), static_cast<uint32>(txdBytes.size()));
    rw::TexDictionary* txd = nil;
    if (rw::findChunk(&stream, rw::ID_TEXDICTIONARY, nil, nil)) {
        txd = rw::TexDictionary::streamRead(&stream);
    }
    stream.close();
    if (!txd) {
        SetErr(err, errSize, "fonts.txd parse failed");
        return false;
    }
    const char* wantTex = "font2";
    rw::Texture* tex = txd->find(wantTex);
    if (!tex) {
        txd->destroy();
        SetErr(err, errSize, "fonts.txd has no font2");
        return false;
    }
    TexImage decoded;
    const bool decOk = TexSample_Decode(tex, decoded);
    txd->destroy();
    if (!decOk || decoded.rgba.empty() || decoded.w != 512 || decoded.h != 512) {
        SetErr(err, errSize, "font2 decode failed (want 512x512)");
        return false;
    }
    std::vector<uint8> fontsDat;
    if (!ReadWholeFile("data/fonts.dat", fontsDat) || fontsDat.empty()) {
        SetErr(err, errSize, "cannot read data/fonts.dat");
        return false;
    }
    FontMetrics metrics;
    if (!ParseFontsDat(fontsDat, metrics)) {
        SetErr(err, errSize, "data/fonts.dat parse failed");
        return false;
    }
    (void)std::snprintf(font.name, sizeof(font.name), "%s", wantTex);
    font.w = decoded.w;
    font.h = decoded.h;
    font.texW = decoded.w;
    font.texH = decoded.h;
    font.rgba = std::move(decoded.rgba);
    for (int i = 0; i < 208; ++i) {
        font.prop[i] = metrics.prop[i];
    }
    font.unprop = metrics.unprop;
    font.space = metrics.space;
    font.ok = true;
    return true;
}

int MenuShot_DrawTextRight(std::vector<uint8_t>& px, int fbW, int fbH, const MenuHudFont& font,
                           const char* text, int xRight, int yTop, int cellDstH, uint8_t cr,
                           uint8_t cg, uint8_t cb) {
    if (!font.ok || !text || fbW != kWidth || fbH != kHeight || cellDstH <= 0) {
        return 0;
    }
    FontMetrics m;
    for (int i = 0; i < 208; ++i) {
        m.prop[i] = font.prop[i];
    }
    m.unprop = font.unprop;
    m.space = font.space;
    m.ok = true;
    TexImage img;
    (void)std::snprintf(img.name, sizeof(img.name), "%s", font.name);
    img.w = font.texW;
    img.h = font.texH;
    img.filter = 0;
    img.rgba = font.rgba; // one 1MB copy per call; HUD draws few strings
    // Total advance first (same math as LineWidthPx, no GXT tokens on HUD).
    long totalAdv = 0;
    for (const char* p = text; *p; ++p) {
        totalAdv += AdvanceFor(static_cast<uint8>(*p), m);
    }
    const int totalDst = static_cast<int>(totalAdv * cellDstH / 40);
    int x = xRight - totalDst;
    int drawn = 0;
    for (const char* p = text; *p; ++p) {
        const uint8 ch = static_cast<uint8>(*p);
        const int adv = AdvanceFor(ch, m);
        if (ch == ' ') {
            x += adv * cellDstH / 40;
            continue;
        }
        const int advDst = adv * cellDstH / 40;
        bool empty = false;
        if (BlitGlyph(px, img, x, yTop, advDst, cellDstH, adv, ch, cr, cg, cb, empty)) {
            ++drawn;
        }
        x += advDst;
    }
    return drawn;
}
