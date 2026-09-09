// GameShot implementation: HudShot path + RadarMap path + 1:1 disc blit.
// See GameShot.h for the contract and the spec grounding.

#include "app/platform/linux/GameShot.h"

#include <cstdio>
#include <cstring>
#include <vector>

#include "app/platform/linux/GxtText.h"
#include "app/platform/linux/MenuShot.h"
#include "app/platform/linux/ZoneInfo.h"

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
    return GameShot_RenderHour(gameDir, health, armor, 12, basePixels, hudPixels, radarFull,
                               gamePixels, stats, err, errSize);
}

bool GameShot_RenderHour(const char* gameDir, int health, int armor, int hour,
                         std::vector<uint8_t>& basePixels, std::vector<uint8_t>& hudPixels,
                         std::vector<uint8_t>& radarFull, std::vector<uint8_t>& gamePixels,
                         GameShotStats& stats, char* err, std::size_t errSize) {
    return GameShot_RenderWeatherHour(gameDir, health, armor, "EXTRASUNNY_LA", hour, basePixels,
                                      hudPixels, radarFull, gamePixels, stats, err, errSize);
}

bool GameShot_RenderWeatherHour(const char* gameDir, int health, int armor, const char* weather,
                                int hour, std::vector<uint8_t>& basePixels,
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
    if (hour < 0 || hour > 23) {
        SetErr(err, errSize, "bad hour (want 0-23)");
        return false;
    }

    // 1. Shore base + HUD overlay, through the existing HudShot path.
    // R6al: hour selects the timecyc row (same path as --shot-shore --hour).
    // R6am: weather selects the timecyc section (same exact-token path as
    // --shot-scene --weather W --hour H).
    {
        char herr[768] = {};
        if (!HudShot_RenderWeatherHour(gameDir, health, armor, weather ? weather : "EXTRASUNNY_LA",
                                       hour, basePixels, hudPixels, stats.hud, herr,
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

bool GameShot_ApplyZoneLabel(const char* gameDir, std::vector<uint8_t>& gamePixels,
                             ZoneLabelStats& out, char* err, std::size_t errSize) {
    out = ZoneLabelStats{};
    if (!gameDir || !gameDir[0]) {
        SetErr(err, errSize, "no game dir");
        return false;
    }
    if (gamePixels.size() != static_cast<std::size_t>(kFbW) * kFbH * 4) {
        SetErr(err, errSize, "game frame has bad size");
        return false;
    }
    // 1. Zone rects from data/info.zon bytes (existing ZoneInfo path).
    ZoneData zones;
    {
        char zerr[512] = {};
        if (!ZoneInfo_Load(gameDir, zones, zerr, sizeof(zerr))) {
            char msg[640];
            (void)std::snprintf(msg, sizeof(msg), "zone-load: %s", zerr);
            SetErr(err, errSize, msg);
            return false;
        }
    }
    const int best = ZoneInfo_FindSmallest(zones, kPierX, kPierY);
    if (best < 0) {
        SetErr(err, errSize, "no zone at frame center");
        return false;
    }
    const ZoneRect& win = zones.zones[static_cast<std::size_t>(best)];
    // 2. Display string from text/american.gxt MAIN (existing GxtText path).
    std::string text;
    {
        GxtTable table;
        char gerr[512] = {};
        if (!GxtText_Load(gameDir, "english", table, gerr, sizeof(gerr))) {
            char msg[640];
            (void)std::snprintf(msg, sizeof(msg), "gxt-load: %s", gerr);
            SetErr(err, errSize, msg);
            return false;
        }
        if (!GxtText_Find(table, win.key.c_str(), text) || text.empty()) {
            char msg[128];
            (void)std::snprintf(msg, sizeof(msg), "no gxt for key '%s'", win.key.c_str());
            SetErr(err, errSize, msg);
            return false;
        }
    }
    // 3. font2 glyphs (existing MenuShot HUD-font path).
    MenuHudFont font;
    {
        char ferr[512] = {};
        if (!MenuShot_LoadHudFont(gameDir, font, ferr, sizeof(ferr))) {
            char msg[640];
            (void)std::snprintf(msg, sizeof(msg), "zone font: %s", ferr);
            SetErr(err, errSize, msg);
            return false;
        }
    }
    // 4. Centered blit at the fixed game-layout position: black drop shadow
    // (+1,+1) first, then white ink. Integer math, fixed traversal order.
    out.posX = kZoneLabelCx;
    out.posY = kZoneLabelYTop;
    out.cellH = kZoneLabelCellH;
    (void)std::snprintf(out.key, sizeof(out.key), "%s", win.key.c_str());
    (void)std::snprintf(out.text, sizeof(out.text), "%s", text.c_str());
    out.level = win.level;
    out.glyphs = static_cast<int>(text.size());
    const std::vector<uint8_t> before = gamePixels;
    (void)MenuShot_DrawTextCentered(gamePixels, kFbW, kFbH, font, text.c_str(),
                                    kZoneLabelCx + 1, kZoneLabelYTop + 1, kZoneLabelCellH,
                                    0, 0, 0);
    out.inkDrawn = MenuShot_DrawTextCentered(gamePixels, kFbW, kFbH, font, text.c_str(),
                                             kZoneLabelCx, kZoneLabelYTop, kZoneLabelCellH,
                                             255, 255, 255);
    if (out.inkDrawn <= 0) {
        gamePixels = before;
        SetErr(err, errSize, "zone label drew no ink");
        return false;
    }
    long changed = 0;
    for (std::size_t i = 0; i < before.size(); i += 4) {
        if (gamePixels[i] != before[i] || gamePixels[i + 1] != before[i + 1] ||
            gamePixels[i + 2] != before[i + 2]) {
            ++changed;
        }
    }
    out.labelPixels = changed;
    if (changed <= 500) {
        gamePixels = before;
        char msg[128];
        (void)std::snprintf(msg, sizeof(msg), "zone label too thin labelPixels=%ld", changed);
        SetErr(err, errSize, msg);
        return false;
    }
    return true;
}

bool GameShot_ApplyWanted(const char* gameDir, std::vector<uint8_t>& gamePixels, int wanted,
                          WantedStats& out, char* err, std::size_t errSize) {
    out = WantedStats{};
    out.wanted = wanted;
    (void)std::snprintf(out.starTex, sizeof(out.starTex), "%s", kWantedStarTex);
    (void)std::snprintf(out.starSrc, sizeof(out.starSrc), "%s", kWantedStarSrc);
    out.glyph = kWantedStarGlyph;
    out.posRight = kWantedRight;
    out.posTop = kWantedTop;
    out.cellH = kWantedCellH;
    if (!gameDir || !gameDir[0]) {
        SetErr(err, errSize, "no game dir");
        return false;
    }
    if (gamePixels.size() != static_cast<std::size_t>(kFbW) * kFbH * 4) {
        SetErr(err, errSize, "game frame has bad size");
        return false;
    }
    if (wanted < 0 || wanted > 6) {
        SetErr(err, errSize, "bad wanted (want 0-6)");
        return false;
    }
    if (wanted == 0) {
        out.starPixels = 0;
        out.drawn = 0;
        return true; // no overlay: frame stays bit-identical
    }
    // font2 glyphs (existing MenuShot HUD-font path: fonts.txd + fonts.dat).
    MenuHudFont font;
    {
        char ferr[512] = {};
        if (!MenuShot_LoadHudFont(gameDir, font, ferr, sizeof(ferr))) {
            char msg[640];
            (void)std::snprintf(msg, sizeof(msg), "wanted font: %s", ferr);
            SetErr(err, errSize, msg);
            return false;
        }
    }
    char text[8] = {};
    for (int i = 0; i < wanted; ++i) {
        text[i] = static_cast<char>(kWantedStarGlyph);
    }
    const std::vector<uint8_t> before = gamePixels;
    // Black drop shadow (+1,+1) first, then gold ink: same two-pass order as
    // the HUD digits and the zone label. Integer math, fixed traversal order.
    const int shadowDrawn = MenuShot_DrawTextRight(gamePixels, kFbW, kFbH, font, text,
                                                   kWantedRight + 1, kWantedTop + 1,
                                                   kWantedCellH, 0, 0, 0);
    out.drawn = MenuShot_DrawTextRight(gamePixels, kFbW, kFbH, font, text, kWantedRight,
                                       kWantedTop, kWantedCellH, kWantedInkR, kWantedInkG,
                                       kWantedInkB);
    if (shadowDrawn != wanted || out.drawn != wanted) {
        gamePixels = before;
        char msg[128];
        (void)std::snprintf(msg, sizeof(msg), "wanted star glyph empty shadow=%d ink=%d want=%d",
                            shadowDrawn, out.drawn, wanted);
        SetErr(err, errSize, msg);
        return false;
    }
    long changed = 0;
    for (std::size_t i = 0; i < before.size(); i += 4) {
        if (gamePixels[i] != before[i] || gamePixels[i + 1] != before[i + 1] ||
            gamePixels[i + 2] != before[i + 2]) {
            ++changed;
        }
    }
    out.starPixels = changed;
    return true;
}

bool GameShot_ApplyMoney(const char* gameDir, std::vector<uint8_t>& gamePixels, int money,
                         MoneyStats& out, char* err, std::size_t errSize) {
    out = MoneyStats{};
    out.money = money;
    (void)std::snprintf(out.fmt, sizeof(out.fmt), "$%%08d/-$%%07d");
    out.posRight = kMoneyRight;
    out.posTop = kMoneyTop;
    out.cellH = kMoneyCellH;
    if (!gameDir || !gameDir[0]) {
        SetErr(err, errSize, "no game dir");
        return false;
    }
    if (gamePixels.size() != static_cast<std::size_t>(kFbW) * kFbH * 4) {
        SetErr(err, errSize, "game frame has bad size");
        return false;
    }
    if (money < kMoneyMin || money > kMoneyMax) {
        SetErr(err, errSize, "bad money (want -999999..9999999)");
        return false;
    }
    // DrawMoney format (game_sa/Hud.cpp, read-only spec): "$%08d" of abs at
    // >=0, "-$%07d" of abs below; 9 chars max either way.
    char text[16] = {};
    if (money < 0) {
        (void)std::snprintf(text, sizeof(text), "-$%07d", -money);
        out.inkR = kMoneyRedR;
        out.inkG = kMoneyRedG;
        out.inkB = kMoneyRedB;
    } else {
        (void)std::snprintf(text, sizeof(text), "$%08d", money);
        out.inkR = kMoneyGreenR;
        out.inkG = kMoneyGreenG;
        out.inkB = kMoneyGreenB;
    }
    (void)std::snprintf(out.text, sizeof(out.text), "%s", text);
    // font2 glyphs (existing MenuShot HUD-font path: fonts.txd + fonts.dat).
    MenuHudFont font;
    {
        char ferr[512] = {};
        if (!MenuShot_LoadHudFont(gameDir, font, ferr, sizeof(ferr))) {
            char msg[640];
            (void)std::snprintf(msg, sizeof(msg), "money font: %s", ferr);
            SetErr(err, errSize, msg);
            return false;
        }
    }
    const std::vector<uint8_t> before = gamePixels;
    // Black drop shadow (+1,+1) first, then green/red ink: same two-pass
    // order as the HUD digits, the clock, the zone label and the stars.
    // Integer math, fixed traversal order.
    const int want = static_cast<int>(std::strlen(text));
    const int shadowDrawn = MenuShot_DrawTextRight(gamePixels, kFbW, kFbH, font, text,
                                                   kMoneyRight + 1, kMoneyTop + 1,
                                                   kMoneyCellH, 0, 0, 0);
    out.digits = MenuShot_DrawTextRight(gamePixels, kFbW, kFbH, font, text, kMoneyRight,
                                        kMoneyTop, kMoneyCellH,
                                        static_cast<uint8_t>(out.inkR),
                                        static_cast<uint8_t>(out.inkG),
                                        static_cast<uint8_t>(out.inkB));
    if (shadowDrawn != want || out.digits != want) {
        gamePixels = before;
        char msg[128];
        (void)std::snprintf(msg, sizeof(msg), "money glyph empty shadow=%d ink=%d want=%d",
                            shadowDrawn, out.digits, want);
        SetErr(err, errSize, msg);
        return false;
    }
    long changed = 0;
    for (std::size_t i = 0; i < before.size(); i += 4) {
        if (gamePixels[i] != before[i] || gamePixels[i + 1] != before[i + 1] ||
            gamePixels[i + 2] != before[i + 2]) {
            ++changed;
        }
    }
    out.moneyPixels = changed;
    return true;
}
