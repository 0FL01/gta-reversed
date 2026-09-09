// HudShot implementation: shore base + TXD-texel HUD overlay.
// See HudShot.h for the contract and the spec grounding.

#include "app/platform/linux/HudShot.h"

#include <cctype>
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
constexpr int kHour = 12; // same water/timecyc row as default --shot-shore
// Game HUD geometry at 640x480 (STRETCH_X identity; see HudShot.h):
// right edge at STRETCH_FROM_RIGHT(32) = 608, health bar on top, armour below.
constexpr int kRight = 608;
constexpr int kHealthMaxW = 109;
constexpr int kArmourMaxW = 62;
constexpr int kBarH = 9;
constexpr int kHealthX0 = kRight - kHealthMaxW; // 499
constexpr int kHealthY0 = 22;
constexpr int kArmourX0 = kRight - kArmourMaxW; // 546
constexpr int kArmourY0 = 35;
constexpr int kBorder = 2; // DrawBarChart black border (STRETCH_X(2)/SCALE_Y(2))
// Game HUD colours (HudColours.cpp, read as spec): health RED, armour LTGRAY.
constexpr uint8 kHealthTint[3] = { 180, 25, 29 };
constexpr uint8 kArmourTint[3] = { 225, 225, 225 };
// HUD digits: font2 cell height + right gap before the bars.
constexpr int kDigitH = 28;
constexpr int kDigitGap = 6;
// HUD clock (DrawClock spec: "%02d:%02d" right-aligned under the bars):
// hour comes from the fixed shore hour, minutes are fixed 00.
constexpr int kClockH = 24;
constexpr int kClockY0 = kArmourY0 + kBarH + 6;

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

bool s_hudShotRwInit = false;

bool HudShotRwInit() {
    if (s_hudShotRwInit) {
        return true;
    }
    if (rw::Engine::state != rw::Engine::Dead) {
        rw::Texture::setLoadTextures(false);
        s_hudShotRwInit = true;
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
    s_hudShotRwInit = true;
    return true;
}

void Lower32(const char* src, char* dst) {
    for (int i = 0; i < 32 && src[i]; ++i) {
        char c = src[i];
        dst[i] = static_cast<char>(c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c);
        if (i + 1 == 32 || src[i + 1] == '\0') {
            if (i + 1 < 32) {
                dst[i + 1] = '\0';
            }
            break;
        }
    }
    dst[31] = '\0';
}

// Opaque tinted bar blit (top-down coords, bottom-up RGBA framebuffer).
// Every pixel of the maxW*h rect is overwritten: fill part [0,fillW) with
// texel*tint (DrawBarChart progress rect), the rest with texel*(tint/2)
// (DrawBarChart dimmed background), then a black border on top. The pattern
// comes from real hud.txd texels (nearest sample across the rect); opacity
// is the overlay composition choice (game bars are opaque DrawRects).
// Integer math only (deterministic).
void BlitBar(std::vector<uint8>& px, const TexImage& bar, int x0, int y0, int maxW, int h,
             int fillW, const uint8 tint[3]) {
    if (bar.rgba.empty() || bar.w <= 0 || bar.h <= 0 || maxW <= 0 || h <= 0) {
        return;
    }
    const uint8 bg[3] = {
        static_cast<uint8>(tint[0] / 2),
        static_cast<uint8>(tint[1] / 2),
        static_cast<uint8>(tint[2] / 2),
    };
    for (int dy = 0; dy < h; ++dy) {
        const int y = y0 + dy;
        if (y < 0 || y >= kFbH) {
            continue;
        }
        const int by = kFbH - 1 - y;
        const int sy = dy * bar.h / h;
        for (int dx = 0; dx < maxW; ++dx) {
            const int x = x0 + dx;
            if (x < 0 || x >= kFbW) {
                continue;
            }
            const int sx = dx * bar.w / maxW;
            const uint8* src =
                bar.rgba.data() + (static_cast<std::size_t>(sy) * bar.w + sx) * 4;
            const uint8* tc = dx < fillW ? tint : bg;
            uint8* dst = px.data() + (static_cast<std::size_t>(by) * kFbW + x) * 4;
            dst[0] = static_cast<uint8>((src[0] * tc[0] + 127) / 255);
            dst[1] = static_cast<uint8>((src[1] * tc[1] + 127) / 255);
            dst[2] = static_cast<uint8>((src[2] * tc[2] + 127) / 255);
            dst[3] = 255;
        }
    }
    // Black border (2px, over the fill).
    for (int dy = 0; dy < h; ++dy) {
        for (int dx = 0; dx < maxW; ++dx) {
            const bool edge = dy < kBorder || dy >= h - kBorder || dx < kBorder ||
                              dx >= maxW - kBorder;
            if (!edge) {
                continue;
            }
            const int y = y0 + dy;
            const int x = x0 + dx;
            if (x < 0 || x >= kFbW || y < 0 || y >= kFbH) {
                continue;
            }
            const int by = kFbH - 1 - y;
            uint8* dst = px.data() + (static_cast<std::size_t>(by) * kFbW + x) * 4;
            dst[0] = 0;
            dst[1] = 0;
            dst[2] = 0;
            dst[3] = 255;
        }
    }
}

} // namespace

bool HudShot_Render(const char* gameDir, int health, int armor, std::vector<uint8_t>& basePixels,
                    std::vector<uint8_t>& hudPixels, HudShotStats& stats, char* err,
                    std::size_t errSize) {
    return HudShot_RenderHour(gameDir, health, armor, kHour, basePixels, hudPixels, stats, err,
                              errSize);
}

bool HudShot_RenderHour(const char* gameDir, int health, int armor, int hour,
                        std::vector<uint8_t>& basePixels, std::vector<uint8_t>& hudPixels,
                        HudShotStats& stats, char* err, std::size_t errSize) {
    return HudShot_RenderWeatherHour(gameDir, health, armor, "EXTRASUNNY_LA", hour, basePixels,
                                     hudPixels, stats, err, errSize);
}

bool HudShot_RenderWeatherHour(const char* gameDir, int health, int armor, const char* weather,
                               int hour, std::vector<uint8_t>& basePixels,
                               std::vector<uint8_t>& hudPixels, HudShotStats& stats, char* err,
                               std::size_t errSize) {
    stats = HudShotStats{};
    basePixels.clear();
    hudPixels.clear();
    if (!gameDir || !gameDir[0]) {
        SetErr(err, errSize, "no game dir");
        return false;
    }
    if (health < 0 || health > 255 || armor < 0 || armor > 255) {
        SetErr(err, errSize, "bad health/armor (want 0-255)");
        return false;
    }
    stats.health = health;
    stats.armor = armor;
    stats.barMax1 = kHealthMaxW;
    stats.barMax2 = kArmourMaxW;
    stats.barH = kBarH;
    // Bar fill widths in game units (HudShot.h): clamp to the 0..100 HUD
    // range, then scale to the bar max width with rounding.
    {
        const int hc = health > 100 ? 100 : health;
        const int ac = armor > 100 ? 100 : armor;
        stats.barW1 = (hc * kHealthMaxW + 50) / 100;
        stats.barW2 = (ac * kArmourMaxW + 50) / 100;
    }
    (void)std::snprintf(stats.fontTex, sizeof(stats.fontTex), "font2");

    // 1. Base: the existing shore composition, same inputs as --shot-shore.
    // R6al: hour selects the timecyc row (same path as --shot-shore --hour).
    // R6am: weather selects the timecyc section (same exact-token path as
    // --shot-scene --weather W --hour H).
    if (hour < 0 || hour > 23) {
        SetErr(err, errSize, "bad hour (want 0-23)");
        return false;
    }
    WorldShotScene scene{};
    {
        char serr[640] = {};
        if (!ShoreShot_InitWeather(gameDir, weather ? weather : "EXTRASUNNY_LA", hour, scene,
                                   stats.shore, stats.loadInfo, stats.pagerFrame, serr,
                                   sizeof(serr))) {
            char msg[768];
            (void)std::snprintf(msg, sizeof(msg), "shore-init: %s", serr);
            SetErr(err, errSize, msg);
            ShoreShot_Shutdown();
            return false;
        }
    }
    TexSample_RenderDuo(scene, stats.shore.worldMeshes, kFbW, kFbH, stats.shore.eye,
                        stats.shore.target, basePixels, stats.texStats, stats.duo);
    if (basePixels.size() != static_cast<std::size_t>(kFbW) * kFbH * 4) {
        SetErr(err, errSize, "shore frame has bad size");
        ShoreShot_Shutdown();
        return false;
    }

    // 2. HUD sprites: enumerate models/hud.txd through the TXD decoder.
    OS_SetFilePathOffset(gameDir);
    std::vector<uint8> txdBytes;
    if (!ReadWholeFile("models/hud.txd", txdBytes) || txdBytes.empty()) {
        SetErr(err, errSize, "cannot read models/hud.txd");
        ShoreShot_Shutdown();
        return false;
    }
    if (!HudShotRwInit()) {
        SetErr(err, errSize, "librw Engine::init failed");
        ShoreShot_Shutdown();
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
        SetErr(err, errSize, "hud.txd parse failed");
        ShoreShot_Shutdown();
        return false;
    }
    // Per-sprite: name + decoded size (0x0 when the raster won't decode —
    // logged honestly, never skipped silently).
    struct NamedImg {
        char name[32] = {};
        TexImage img;
        bool decoded = false;
    };
    std::vector<NamedImg> named;
    FORLIST(link, txd->textures) {
        rw::Texture* t = LLLinkGetData(link, rw::Texture, inDict);
        NamedImg ni;
        (void)std::snprintf(ni.name, sizeof(ni.name), "%s", t->name);
        TexImage img;
        if (TexSample_Decode(t, img) && !img.rgba.empty()) {
            ni.img = std::move(img);
            ni.decoded = true;
        }
        named.push_back(std::move(ni));
    }
    for (const NamedImg& ni : named) {
        HudTexInfo info;
        (void)std::snprintf(info.name, sizeof(info.name), "%s", ni.name);
        info.w = ni.decoded ? ni.img.w : 0;
        info.h = ni.decoded ? ni.img.h : 0;
        stats.sprites.push_back(info);
    }
    // Bar backing: named health/armour bars if the dictionary has them
    // (hud.txd ships none per CHud::Initialise — only fist/siteM16/
    // siterocket/radardisc/radarRingPlane/SkipIcon), else the radardisc
    // background sprite + tint, else the first decodable sprite.
    const char* wantBars[] = { "healthbar", "armourbar", "armorbar", "health",
                               "armour",    "armor",     "bar",      nullptr };
    const TexImage* barImg = nullptr;
    {
        const TexImage* fallback = nullptr;
        const TexImage* radardisc = nullptr;
        const char* fallbackName = nullptr;
        const char* radarName = nullptr;
        for (const NamedImg& ni : named) {
            if (!ni.decoded) {
                continue;
            }
            if (!fallback) {
                fallback = &ni.img;
                fallbackName = ni.name;
            }
            char low[32] = {};
            Lower32(ni.name, low);
            if (std::strcmp(low, "radardisc") == 0 && !radardisc) {
                radardisc = &ni.img;
                radarName = ni.name;
            }
            for (int w = 0; wantBars[w]; ++w) {
                if (std::strcmp(low, wantBars[w]) == 0) {
                    barImg = &ni.img;
                    (void)std::snprintf(stats.barTex, sizeof(stats.barTex), "%s",
                                        ni.name);
                    break;
                }
            }
            if (barImg) {
                break;
            }
        }
        if (!barImg && radardisc) {
            barImg = radardisc; // documented fallback: background sprite + tint
            (void)std::snprintf(stats.barTex, sizeof(stats.barTex), "%s", radarName);
        }
        if (!barImg && fallback) {
            barImg = fallback;
            (void)std::snprintf(stats.barTex, sizeof(stats.barTex), "%s", fallbackName);
        }
    }
    if (!barImg) {
        SetErr(err, errSize, "hud.txd has no decodable sprite for bars");
        txd->destroy();
        ShoreShot_Shutdown();
        return false;
    }
    stats.barTexW = barImg->w;
    stats.barTexH = barImg->h;
    TexImage barCopy = *barImg; // txd dies below; texels live on in the copy
    txd->destroy();

    // 3. HUD font: existing font2 glyph path (no menu frame).
    MenuHudFont font;
    {
        char ferr[512] = {};
        if (!MenuShot_LoadHudFont(gameDir, font, ferr, sizeof(ferr))) {
            char msg[640];
            (void)std::snprintf(msg, sizeof(msg), "hud font: %s", ferr);
            SetErr(err, errSize, msg);
            ShoreShot_Shutdown();
            return false;
        }
    }

    // 4. Overlay on a copy of the base frame (base stays untouched).
    hudPixels = basePixels;
    BlitBar(hudPixels, barCopy, kHealthX0, kHealthY0, kHealthMaxW, kBarH, stats.barW1,
            kHealthTint);
    BlitBar(hudPixels, barCopy, kArmourX0, kArmourY0, kArmourMaxW, kBarH, stats.barW2,
            kArmourTint);
    // H/A digits (game units, raw values) left of the bars, white with the
    // game HUD drop shadow (CFont edge): shadow pass first, then the ink.
    char hText[16] = {};
    char aText[16] = {};
    char clockText[16] = {};
    (void)std::snprintf(hText, sizeof(hText), "%d", health);
    (void)std::snprintf(aText, sizeof(aText), "%d", armor);
    (void)std::snprintf(clockText, sizeof(clockText), "%02d:00", hour);
    (void)std::snprintf(stats.clockText, sizeof(stats.clockText), "%s", clockText);
    (void)MenuShot_DrawTextRight(hudPixels, kFbW, kFbH, font, hText, kHealthX0 - kDigitGap,
                                 kHealthY0 - 6, kDigitH, 0, 0, 0);
    (void)MenuShot_DrawTextRight(hudPixels, kFbW, kFbH, font, aText, kArmourX0 - kDigitGap,
                                 kArmourY0 - 6, kDigitH, 0, 0, 0);
    (void)MenuShot_DrawTextRight(hudPixels, kFbW, kFbH, font, clockText, kRight,
                                 kClockY0, kClockH, 0, 0, 0);
    stats.digits += MenuShot_DrawTextRight(hudPixels, kFbW, kFbH, font, hText,
                                           kHealthX0 - kDigitGap - 1, kHealthY0 - 7,
                                           kDigitH, 255, 255, 255);
    stats.digits += MenuShot_DrawTextRight(hudPixels, kFbW, kFbH, font, aText,
                                           kArmourX0 - kDigitGap - 1, kArmourY0 - 7,
                                           kDigitH, 255, 255, 255);
    stats.digits += MenuShot_DrawTextRight(hudPixels, kFbW, kFbH, font, clockText, kRight - 1,
                                           kClockY0 - 1, kClockH, 255, 255, 255);

    long changed = 0;
    for (std::size_t i = 0; i < basePixels.size(); i += 4) {
        if (hudPixels[i] != basePixels[i] || hudPixels[i + 1] != basePixels[i + 1] ||
            hudPixels[i + 2] != basePixels[i + 2]) {
            ++changed;
        }
    }
    stats.hudPixels = changed;
    return true;
}

void HudShot_Shutdown() {
    ShoreShot_Shutdown();
}
