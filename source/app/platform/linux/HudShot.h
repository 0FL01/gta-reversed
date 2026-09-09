// HudShot: R6ad HUD overlay slice (round 32).
// Renders the EXISTING --shot-shore frame (same fixed pier center/camera/hour
// via ShoreShot_Init, same TexSample_RenderDuo call) into basePixels, then
// CPU-blits a 2D HUD overlay on a copy: health + armour bars from REAL
// models/hud.txd texels (tinted by the game HUD colours) and H/A digits via
// the existing font2 glyph blit (MenuShot_DrawTextRight). Wanted stars are
// off this round (stars=off, logged).
//
// Spec grounding (read-only, NOT linked into the binary):
// - game_sa/Hud.cpp CHud::Initialise binds hud.txd sprites fist/siteM16/
//   siterocket/radardisc/radarRingPlane/SkipIcon: there are NO named
//   healthbar/armourbar sprites, so the bar pattern comes from a background
//   sprite (radardisc preferred) + honest tint, logged as barTex.
// - game_sa/Hud.cpp RenderHealthBar/RenderArmorBar + Sprite2d::DrawBarChart:
//   health progress = m_fHealth*100/m_nMaxHealth over totalWidth =
//   109px*m_nMaxHealth/STAT_MOD (==109px at max 100); armour progress =
//   m_fArmour*100/m_nMaxArmour over 62px; both height (uint8)STRETCH_Y(9px),
//   black border, dimmed (color/2) background. At the 640x480 HUD frame
//   STRETCH_X is identity, so barW = clamp(H,0,100)*109/100 (rounded) and
//   barW2 = clamp(A,0,100)*62/100 (rounded); H/A arrive in game units 0..100
//   (m_fHealth/m_fArmour), values above max clamp to a full bar.
// - game_sa/HudColours.cpp: HUD_COLOUR_RED = (180,25,29) for health,
//   HUD_COLOUR_LIGHT_GRAY = (225,225,225) for armour.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "app/platform/linux/ShoreShot.h"
#include "app/platform/linux/TexSample.h"

struct HudTexInfo {
    char name[32] = {};
    int w = 0;
    int h = 0;
};

struct HudShotStats {
    // Base shore frame (identical inputs to --shot-shore, hour fixed 12).
    ShoreShotStats shore{};
    E2ELoadInfo loadInfo{};
    E2EPagerFrame pagerFrame{};
    TexFrameStats texStats{};
    TexDuoStats duo{};
    // HUD texture evidence.
    std::vector<HudTexInfo> sprites; // every texture parsed from hud.txd
    char barTex[32] = {}; // sprite backing the bars (fallback documented)
    int barTexW = 0;
    int barTexH = 0;
    char fontTex[32] = {}; // always "font2"
    // HUD values and geometry (game units, see barW formula above).
    int health = 137;
    int armor = 60;
    int barMax1 = 109;
    int barMax2 = 62;
    int barH = 9;
    int barW1 = 0; // fill width of the health bar (px)
    int barW2 = 0; // fill width of the armour bar (px)
    int digits = 0; // font2 glyphs drawn for the H/A values + clock
    char clockText[16] = {}; // "%02d:00" of the fixed shore hour (DrawClock spec)
    long hudPixels = 0; // pixels differing from the base frame
};

// Bit-identical shore etalon (round 31): the base frame checksum MUST equal
// this, otherwise the world path was disturbed and the run fails honestly.
constexpr uint64_t kHudShoreEtalon = 3518993618115791197ULL;

// Renders base (shore, hour 12) + HUD overlay for health/armor (game units,
// clamped to 0..255 on input, bar fill clamped to 0..100). basePixels is the
// untouched shore frame (for the baseMatchesShore proof), hudPixels is the
// final frame with the overlay. Returns false with err set (no hud-ok) on
// any failure; never invents texels (all pixels come from shore/hud.txd/
// fonts.txd bytes).
bool HudShot_Render(const char* gameDir, int health, int armor, std::vector<uint8_t>& basePixels,
                    std::vector<uint8_t>& hudPixels, HudShotStats& stats, char* err,
                    std::size_t errSize);
// Round 40 (R6al): hour-parameterized variant. Identical to HudShot_Render
// except the shore base is ShoreShot_Init(gameDir, hour) (same TimeCycle
// waterColor path as --shot-shore --hour H) and the clock text is
// "%02d:00" of hour via the same font2 path. hour must be 0-23; hour=12 is
// bit-identical to HudShot_Render. No hardcoded colors/digits on this path.
bool HudShot_RenderHour(const char* gameDir, int health, int armor, int hour,
                        std::vector<uint8_t>& basePixels, std::vector<uint8_t>& hudPixels,
                        HudShotStats& stats, char* err, std::size_t errSize);
// Round 41 (R6am): weather-parameterized variant. Identical to
// HudShot_RenderHour except the shore base is
// ShoreShot_InitWeather(gameDir, weather, hour) (the same exact-token
// section path as --shot-scene --weather W --hour H). weather must be a
// valid section token; "EXTRASUNNY_LA" is bit-identical to
// HudShot_RenderHour. No hardcoded colors/digits on this path.
bool HudShot_RenderWeatherHour(const char* gameDir, int health, int armor, const char* weather,
                               int hour, std::vector<uint8_t>& basePixels,
                               std::vector<uint8_t>& hudPixels, HudShotStats& stats, char* err,
                               std::size_t errSize);
// Round 42 (R6an): fog variant. Identical to HudShot_RenderWeatherHour
// except that with wantFog=true the shore base renders through
// TexSample_RenderDuoTC with the TexTimeEnv built from the same timecyc row
// (ambient+sun+sky from the row bytes, fog blend toward SkyBot by
// clamp((dist-FogSt)/(FarClp-FogSt),0,1) — the scene --fog path). The HUD
// overlay on top is unfogged (base-only order). wantFog=false is
// bit-identical to HudShot_RenderWeatherHour.
bool HudShot_RenderWeatherHourFog(const char* gameDir, int health, int armor, const char* weather,
                                  int hour, bool wantFog, std::vector<uint8_t>& basePixels,
                                  std::vector<uint8_t>& hudPixels, HudShotStats& stats, char* err,
                                  std::size_t errSize);
void HudShot_Shutdown();
