// RadarMap implementation: 3x3 radar-TXD mosaic -> circular north-up disc.
//
// See RadarMap.h for the contract and the spec grounding. Integer math and
// fixed traversal order everywhere (deterministic); the only float ops are
// the world<->tile floor/ceil (exact on these magnitudes) and the per-pixel
// world reconstruction (same formula every run).

#include "app/platform/linux/RadarMap.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "app/platform/linux/MenuShot.h"
#include "app/platform/linux/TexSample.h"

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

constexpr int kFbW = 640;
constexpr int kFbH = 480;
constexpr int kDiscCx = 320;
constexpr int kDiscCy = 240; // top-down center
constexpr int kDiscR = 190;
constexpr int kDiscRangeM = 350; // world meters at the disc rim (m_radarRange)
constexpr int kMapTiles = 12;
constexpr float kTileM = 500.0f;
constexpr float kMapMin = -3000.0f;

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

int StrCaseCmp(const char* a, const char* b) {
    while (*a && *b) {
        char ca = *a >= 'A' && *a <= 'Z' ? static_cast<char>(*a + 32) : *a;
        char cb = *b >= 'A' && *b <= 'Z' ? static_cast<char>(*b + 32) : *b;
        if (ca != cb) {
            return ca < cb ? -1 : 1;
        }
        ++a;
        ++b;
    }
    return *a == *b ? 0 : (*a ? 1 : -1);
}

// VER2 IMG entry read (same layout as WorldShot.cpp ImgReadEntry, local copy:
// that helper is TU-local there). Sector-padded tail is kept: the TXD chunk
// parser stops at the declared chunk sizes, trailing zeros are inert.
bool ImgReadEntryLocal(const char* imgPath, const char* wantName, std::vector<uint8>& out) {
    void* file = nullptr;
    if (OS_FileOpen(FILE_DATA_AREA_DEFAULT, &file, imgPath, FILE_ACCESS_READ) != 0 || !file) {
        return false;
    }
    char magic[4] = {};
    uint32 count = 0;
    bool ok = OS_FileRead(file, magic, 4) == 0 && OS_FileRead(file, &count, 4) == 0;
    if (!ok || std::memcmp(magic, "VER2", 4) != 0 || count == 0 || count > 300000) {
        OS_FileClose(file);
        return false;
    }
    uint32 foundOff = 0;
    uint32 foundSize = 0;
    for (uint32 i = 0; i < count; ++i) {
        uint32 off = 0;
        uint32 size = 0;
        char name[24] = {};
        if (OS_FileRead(file, &off, 4) != 0 || OS_FileRead(file, &size, 4) != 0 ||
            OS_FileRead(file, name, 24) != 0) {
            OS_FileClose(file);
            return false;
        }
        name[23] = '\0';
        if (StrCaseCmp(name, wantName) == 0) {
            foundOff = off;
            foundSize = size & 0x7FFFu;
            break;
        }
    }
    if (foundSize == 0) {
        OS_FileClose(file);
        return false;
    }
    OS_FileSetPosition(file, static_cast<int32>(foundOff * 2048u));
    out.resize(static_cast<size_t>(foundSize) * 2048u);
    ok = OS_FileRead(file, out.data(), static_cast<int32>(out.size())) == 0;
    OS_FileClose(file);
    return ok;
}

bool s_radarRwInit = false;

bool RadarRwInit() {
    if (s_radarRwInit) {
        return true;
    }
    if (rw::Engine::state != rw::Engine::Dead) {
        rw::Texture::setLoadTextures(false);
        s_radarRwInit = true;
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
    s_radarRwInit = true;
    return true;
}

int ClampTile(int v) {
    if (v < 0) {
        return 0;
    }
    if (v >= kMapTiles) {
        return kMapTiles - 1;
    }
    return v;
}

} // namespace

bool RadarMap_Render(const char* gameDir, double worldX, double worldY,
                     std::vector<uint8_t>& outRGBA, RadarMapStats& stats, char* err,
                     std::size_t errSize) {
    stats = RadarMapStats{};
    outRGBA.clear();
    if (!gameDir || !gameDir[0]) {
        SetErr(err, errSize, "no game dir");
        return false;
    }
    if (!std::isfinite(worldX) || !std::isfinite(worldY)) {
        SetErr(err, errSize, "bad world coords (want finite --x/--y)");
        return false;
    }
    stats.worldX = worldX;
    stats.worldY = worldY;
    stats.discR = kDiscR;
    stats.discRange = kDiscRangeM;
    (void)std::snprintf(stats.radarSrc, sizeof(stats.radarSrc), "models/gta3.img");
    (void)std::snprintf(stats.tileFmt, sizeof(stats.tileFmt), "radar%%02d idx=y*12+x");
    (void)std::snprintf(stats.fontTex, sizeof(stats.fontTex), "font2");
    (void)std::snprintf(stats.arrowName, sizeof(stats.arrowName), "arrow");

    // World -> center tile (CRadar::StreamRadarSections/DrawRadarMap).
    const int ctx = static_cast<int>(std::floor((worldX + 3000.0) / 500.0));
    const int cty = static_cast<int>(std::ceil(11.0 - (worldY + 3000.0) / 500.0));
    stats.tileX = ClampTile(ctx);
    stats.tileY = ClampTile(cty);

    if (!RadarRwInit()) {
        SetErr(err, errSize, "librw Engine::init failed");
        return false;
    }
    OS_SetFilePathOffset(gameDir);

    // 3x3 mosaic around the center tile (CRadar::DrawRadarMap order):
    // row 0 = north (ty-1), col 0 = west (tx-1). Edge tiles clamp (same as
    // ClipRadarTileCoords); names dedupe so an off-map request loads each
    // TXD once and reports an honest tiles<9.
    struct TileEntry {
        int tx = 0;
        int ty = 0;
        char name[32] = {};
    };
    TileEntry want[9];
    int nWant = 0;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            const int nx = ClampTile(stats.tileX + dx);
            const int ny = ClampTile(stats.tileY + dy);
            char nm[32] = {};
            (void)std::snprintf(nm, sizeof(nm), "radar%02d.txd", ny * kMapTiles + nx);
            bool dup = false;
            for (int k = 0; k < nWant; ++k) {
                if (std::strcmp(want[k].name, nm) == 0) {
                    dup = true;
                    break;
                }
            }
            if (dup) {
                continue;
            }
            want[nWant].tx = nx;
            want[nWant].ty = ny;
            (void)std::snprintf(want[nWant].name, sizeof(want[nWant].name), "%s", nm);
            ++nWant;
        }
    }

    // Mosaic slots in fixed north->south, west->east order (9 slots; clamped
    // edges alias the same decoded tile, so every slot always resolves).
    TexImage mosaic[3][3];
    bool haveMosaic[3][3] = {};
    int tileW = 0;
    int tileH = 0;
    long tilePx = 0;
    int unique = 0;
    char centerInternal[32] = {};
    for (int s = 0; s < nWant; ++s) {
        std::vector<uint8> txdBytes;
        if (!ImgReadEntryLocal("models/gta3.img", want[s].name, txdBytes) || txdBytes.empty()) {
            char msg[128];
            (void)std::snprintf(msg, sizeof(msg), "tile missing in gta3.img: %s", want[s].name);
            SetErr(err, errSize, msg);
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
            char msg[128];
            (void)std::snprintf(msg, sizeof(msg), "tile TXD parse failed: %s", want[s].name);
            SetErr(err, errSize, msg);
            return false;
        }
        TexImage img;
        char internal[32] = {};
        bool decoded = false;
        FORLIST(link, txd->textures) {
            rw::Texture* t = LLLinkGetData(link, rw::Texture, inDict);
            TexImage cand;
            if (TexSample_Decode(t, cand) && !cand.rgba.empty()) {
                img = std::move(cand);
                (void)std::snprintf(internal, sizeof(internal), "%s", t->name);
                decoded = true;
                break; // first texture = the tile (GetFirstTexture)
            }
        }
        txd->destroy();
        if (!decoded) {
            char msg[128];
            (void)std::snprintf(msg, sizeof(msg), "tile has no decodable raster: %s", want[s].name);
            SetErr(err, errSize, msg);
            return false;
        }
        if (tileW == 0) {
            tileW = img.w;
            tileH = img.h;
        } else if (img.w != tileW || img.h != tileH) {
            SetErr(err, errSize, "radar tiles have mixed sizes");
            return false;
        }
        tilePx += static_cast<long>(img.w) * img.h;
        ++unique;
        if (want[s].tx == stats.tileX && want[s].ty == stats.tileY) {
            (void)std::snprintf(centerInternal, sizeof(centerInternal), "%s", internal);
        }
        // Publish into every mosaic slot this tile covers (edge aliasing).
        for (int row = 0; row < 3; ++row) {
            for (int col = 0; col < 3; ++col) {
                const int sx = ClampTile(stats.tileX + (col - 1));
                const int sy = ClampTile(stats.tileY + (row - 1));
                if (sx == want[s].tx && sy == want[s].ty && !haveMosaic[row][col]) {
                    mosaic[row][col] = img;
                    haveMosaic[row][col] = true;
                }
            }
        }
    }
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            if (!haveMosaic[row][col]) {
                SetErr(err, errSize, "mosaic slot uncovered (internal error)");
                return false;
            }
        }
    }
    stats.tiles = unique;
    stats.tileW = tileW;
    stats.tileH = tileH;
    stats.tilePx = tilePx;
    (void)std::snprintf(stats.firstTile, sizeof(stats.firstTile), "%s", centerInternal);
    // Mosaic order log: row 0 = north.
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            const int sx = ClampTile(stats.tileX + (col - 1));
            const int sy = ClampTile(stats.tileY + (row - 1));
            (void)std::snprintf(stats.tileNames[row * 3 + col],
                                sizeof(stats.tileNames[row * 3 + col]), "radar%02d",
                                sy * kMapTiles + sx);
        }
    }
    if (unique != 9) {
        char msg[160];
        (void)std::snprintf(msg, sizeof(msg),
                            "only %d/9 unique tiles at map edge (x=%.0f y=%.0f)", unique,
                            worldX, worldY);
        SetErr(err, errSize, msg);
        return false;
    }

    // Mosaic world bounds (GetTextureCorners over the 3x3 block).
    const double west = 500.0 * (static_cast<double>(stats.tileX - 1) - 6.0);
    const double north = 500.0 * (6.0 - static_cast<double>(stats.tileY - 1));
    const double mosW = 3.0 * tileW;
    const double mosH = 3.0 * tileH;

    // Frame: dark surround + circular north-up disc of nearest-texel samples.
    outRGBA.assign(static_cast<std::size_t>(kFbW) * kFbH * 4, 0);
    for (std::size_t i = 0; i < outRGBA.size(); i += 4) {
        outRGBA[i] = 12;
        outRGBA[i + 1] = 14;
        outRGBA[i + 2] = 22;
        outRGBA[i + 3] = 255;
    }
    const double mPerPx = static_cast<double>(kDiscRangeM) / static_cast<double>(kDiscR);
    long discPixels = 0;
    long oob = 0;
    for (int fy = 0; fy < kFbH; ++fy) {
        const int by = kFbH - 1 - fy; // bottom-up store, top-down loop
        const int dy = fy - kDiscCy;
        for (int fx = 0; fx < kFbW; ++fx) {
            const int dx = fx - kDiscCx;
            if (dx * dx + dy * dy > kDiscR * kDiscR) {
                continue;
            }
            ++discPixels;
            const double wx = worldX + static_cast<double>(dx) * mPerPx;
            const double wy = worldY - static_cast<double>(dy) * mPerPx; // up = north
            const double mx = (wx - west) / 1500.0 * mosW;
            const double my = (north - wy) / 1500.0 * mosH;
            const int mxi = static_cast<int>(std::floor(mx));
            const int myi = static_cast<int>(std::floor(my));
            if (mxi < 0 || myi < 0 || mxi >= static_cast<int>(mosW) ||
                myi >= static_cast<int>(mosH)) {
                ++oob;
                continue;
            }
            int col = mxi / tileW;
            int row = myi / tileH;
            if (col < 0) {
                col = 0;
            }
            if (col > 2) {
                col = 2;
            }
            if (row < 0) {
                row = 0;
            }
            if (row > 2) {
                row = 2;
            }
            int sx = mxi - col * tileW;
            int sy = myi - row * tileH;
            if (sx < 0) {
                sx = 0;
            }
            if (sx >= tileW) {
                sx = tileW - 1;
            }
            if (sy < 0) {
                sy = 0;
            }
            if (sy >= tileH) {
                sy = tileH - 1;
            }
            const TexImage& t = mosaic[row][col];
            const uint8* src =
                t.rgba.data() + (static_cast<std::size_t>(sy) * t.w + sx) * 4;
            uint8* dst = outRGBA.data() + (static_cast<std::size_t>(by) * kFbW + fx) * 4;
            dst[0] = src[0];
            dst[1] = src[1];
            dst[2] = src[2];
            dst[3] = 255;
        }
    }
    stats.discPixels = discPixels;
    stats.oob = oob;
    if (oob != 0) {
        char msg[160];
        (void)std::snprintf(msg, sizeof(msg), "disc reaches outside the 3x3 mosaic oob=%ld",
                            oob);
        SetErr(err, errSize, msg);
        return false;
    }

    // Player marker: models/hud.txd `arrow`, alpha-tested, centered on the
    // disc center, north-up (no heading headless).
    TexImage arrow;
    {
        std::vector<uint8> hudBytes;
        if (!ReadWholeFile("models/hud.txd", hudBytes) || hudBytes.empty()) {
            SetErr(err, errSize, "cannot read models/hud.txd");
            return false;
        }
        rw::StreamMemory stream;
        stream.open(hudBytes.data(), static_cast<uint32>(hudBytes.size()));
        rw::TexDictionary* txd = nil;
        if (rw::findChunk(&stream, rw::ID_TEXDICTIONARY, nil, nil)) {
            txd = rw::TexDictionary::streamRead(&stream);
        }
        stream.close();
        if (!txd) {
            SetErr(err, errSize, "hud.txd parse failed");
            return false;
        }
        bool found = false;
        FORLIST(link, txd->textures) {
            rw::Texture* t = LLLinkGetData(link, rw::Texture, inDict);
            char low[32] = {};
            for (int i = 0; i < 32 && t->name[i]; ++i) {
                const char c = t->name[i];
                low[i] = static_cast<char>(c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c);
            }
            if (std::strcmp(low, "arrow") == 0) {
                TexImage cand;
                if (TexSample_Decode(t, cand) && !cand.rgba.empty()) {
                    arrow = std::move(cand);
                    found = true;
                }
                break;
            }
        }
        txd->destroy();
        if (!found) {
            SetErr(err, errSize, "hud.txd has no decodable `arrow` sprite");
            return false;
        }
    }
    stats.arrowW = arrow.w;
    stats.arrowH = arrow.h;
    long markerPixels = 0;
    {
        const int x0 = kDiscCx - arrow.w / 2;
        const int y0 = kDiscCy - arrow.h / 2;
        for (int ay = 0; ay < arrow.h; ++ay) {
            const int fy = y0 + ay;
            if (fy < 0 || fy >= kFbH) {
                continue;
            }
            const int by = kFbH - 1 - fy;
            for (int ax = 0; ax < arrow.w; ++ax) {
                const int fx = x0 + ax;
                if (fx < 0 || fx >= kFbW) {
                    continue;
                }
                const uint8* src =
                    arrow.rgba.data() + (static_cast<std::size_t>(ay) * arrow.w + ax) * 4;
                if (src[3] < 128) {
                    continue;
                }
                uint8* dst = outRGBA.data() + (static_cast<std::size_t>(by) * kFbW + fx) * 4;
                dst[0] = src[0];
                dst[1] = src[1];
                dst[2] = src[2];
                dst[3] = 255;
                ++markerPixels;
            }
        }
    }
    stats.markerPixels = markerPixels;
    if (markerPixels <= 50) {
        char msg[128];
        (void)std::snprintf(msg, sizeof(msg), "arrow marker invisible markerPixels=%ld",
                            markerPixels);
        SetErr(err, errSize, msg);
        return false;
    }

    // North tag: font2 glyph `N` above the marker, inside the disc rim.
    {
        MenuHudFont font;
        char ferr[512] = {};
        if (!MenuShot_LoadHudFont(gameDir, font, ferr, sizeof(ferr))) {
            char msg[640];
            (void)std::snprintf(msg, sizeof(msg), "radar font: %s", ferr);
            SetErr(err, errSize, msg);
            return false;
        }
        stats.glyphs += MenuShot_DrawTextRight(outRGBA, kFbW, kFbH, font, "N",
                                               kDiscCx + kDiscR - 14,
                                               kDiscCy - kDiscR + 10, 22, 0, 0, 0);
        stats.glyphs += MenuShot_DrawTextRight(outRGBA, kFbW, kFbH, font, "N",
                                               kDiscCx + kDiscR - 15,
                                               kDiscCy - kDiscR + 9, 22, 255, 255, 255);
        // DrawTextRight counts both passes (shadow + ink): halve to glyphs.
        stats.glyphs = stats.glyphs / 2;
    }
    if (stats.glyphs < 1) {
        SetErr(err, errSize, "north glyph `N` did not draw");
        return false;
    }
    if (stats.tilePx <= 10000) {
        SetErr(err, errSize, "tile texel budget too small (want >10000)");
        return false;
    }
    return true;
}

void RadarMap_Shutdown() {
    // No pager/scene state held; the shared librw engine stays up for the
    // process lifetime like the other 2D slices.
}
