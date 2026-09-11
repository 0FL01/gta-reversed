#include "app/platform/linux/RealtimeHud.h"

#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <numbers>
#include <strings.h>
#include <vector>

using int32 = std::int32_t;
using int64 = std::int64_t;
using uint32 = std::uint32_t;
using uint64 = std::uint64_t;
#ifndef __stdcall
#define __stdcall
#endif
#include "oswrapper/oswrapper.h"
#include <rw.h>

namespace {
constexpr float kPi = std::numbers::pi_v<float>;
// CRadar::RadarBlipFileNames, 0x8D0720. Preserve source spelling and order;
// all source mask names are nullptr. TORENO=64 is outside this table.
constexpr std::array<const char*, RealtimeHud::RadarSpriteCount> kRadarNames{
    nullptr, nullptr, "radar_centre", "arrow", "radar_north", "radar_airYard",
    "radar_ammugun", "radar_barbers", "radar_BIGSMOKE", "radar_boatyard",
    "radar_burgerShot", "radar_bulldozer", "radar_CATALINAPINK", "radar_CESARVIAPANDO",
    "radar_chicken", "radar_CJ", "radar_CRASH1", "radar_diner", "radar_emmetGun",
    "radar_enemyAttack", "radar_fire", "radar_girlfriend", "radar_hostpitaL",
    "radar_LocoSyndicate", "radar_MADDOG", "radar_mafiaCasino", "radar_MCSTRAP",
    "radar_modGarage", "radar_OGLOC", "radar_pizza", "radar_police", "radar_propertyG",
    "radar_propertyR", "radar_race", "radar_RYDER", "radar_saveGame", "radar_school",
    "radar_qmark", "radar_SWEET", "radar_tattoo", "radar_THETRUTH", "radar_waypoint",
    "radar_TorenoRanch", "radar_triads", "radar_triadsCasino", "radar_tshirt",
    "radar_WOOZIE", "radar_ZERO", "radar_dateDisco", "radar_dateDrink", "radar_dateFood",
    "radar_truck", "radar_cash", "radar_flag", "radar_gym", "radar_impound",
    "radar_light", "radar_runway", "radar_gangB", "radar_gangP", "radar_gangY",
    "radar_gangN", "radar_gangG", "radar_spray"
};
struct Point { float x, y; };

static void SetError(char* err, std::size_t errSize, const char* message) {
    if (err && errSize) std::snprintf(err, errSize, "%s", message);
}

struct HudFile {
    void* Handle{};
    ~HudFile() { if (Handle) OS_FileClose(Handle); }
};

using SpriteImages = std::array<WorldShotImage, RealtimeHud::RadarSpriteCount>;
using SpriteStates = std::array<RealtimeHud::RadarSpriteState, RealtimeHud::RadarSpriteCount>;

static uint32 HudWord(std::span<const std::uint8_t> bytes, std::size_t offset) {
    assert(offset + 4 <= bytes.size());
    return uint32(bytes[offset]) | uint32(bytes[offset + 1]) << 8 |
        uint32(bytes[offset + 2]) << 16 | uint32(bytes[offset + 3]) << 24;
}

struct HudChunk { uint32 Type{}; std::span<const std::uint8_t> Data; };
static bool NextHudChunk(std::span<const std::uint8_t>& bytes, HudChunk& chunk) {
    if (bytes.size() < 12 || HudWord(bytes, 4) > bytes.size() - 12) return false;
    chunk = {HudWord(bytes, 0), bytes.subspan(12, HudWord(bytes, 4))};
    bytes = bytes.subspan(12 + chunk.Data.size());
    return true;
}

// Validate native lengths BEFORE librw's unchecked read8 into raster storage.
// Only the source's named radar entries are decoded; unrelated HUD assets are
// neither prepared nor made new runtime prerequisites. Fault probes mutate RAM.
static bool DecodeRadarSprites(std::span<const std::uint8_t> bytes, SpriteImages& images,
    SpriteStates& states, char* err, std::size_t errSize, bool decode = true) {
    using State = RealtimeHud::RadarSpriteState;
    images = {};
    for (std::size_t i = 0; i < states.size(); ++i) states[i] = kRadarNames[i] ? State::Missing : State::NoTexture;
    const auto corrupt = [&](const char* detail, int sprite = -1) {
        if (err && errSize) std::snprintf(err, errSize, "hud.txd: sprite %d %s corrupt %s", sprite,
            sprite >= 0 ? kRadarNames[sprite] : "dictionary", detail);
        return false;
    };
    HudChunk dictionary, header;
    if (!NextHudChunk(bytes, dictionary) || dictionary.Type != rw::ID_TEXDICTIONARY || !bytes.empty()) return corrupt("chunk bounds");
    auto children = dictionary.Data;
    if (!NextHudChunk(children, header) || header.Type != rw::ID_STRUCT || header.Data.size() != 4) return corrupt("header");
    const unsigned count = HudWord(header.Data, 0) & 0xffff;
    for (unsigned n = 0; n < count; ++n) {
        HudChunk native, data;
        if (!NextHudChunk(children, native) || native.Type != rw::ID_TEXTURENATIVE) return corrupt("native chunk bounds");
        auto parts = native.Data;
        if (!NextHudChunk(parts, data) || data.Type != rw::ID_STRUCT || data.Data.size() < 72) return corrupt("native header");
        auto payload = data.Data;
        if (!std::memchr(payload.data() + 8, 0, 32)) return corrupt("texture name");
        int sprite = -1;
        for (std::size_t i = 0; i < kRadarNames.size(); ++i) {
            if (kRadarNames[i] && !strcasecmp(kRadarNames[i], reinterpret_cast<const char*>(payload.data() + 8))) sprite = int(i);
        }
        if (sprite < 0) continue;
        if (states[sprite] != State::Missing) return corrupt("duplicate texture name", sprite);
        const auto platform = HudWord(payload, 0), filter = HudWord(payload, 4);
        if (platform != rw::PLATFORM_D3D8 && platform != rw::PLATFORM_D3D9) {
            states[sprite] = State::Unsupported;
            continue;
        }
        if (payload.size() < 88) return corrupt("D3D header", sprite);
        const auto format = HudWord(payload, 72), d3d = HudWord(payload, 76);
        unsigned w = unsigned(payload[80]) | unsigned(payload[81]) << 8;
        unsigned h = unsigned(payload[82]) | unsigned(payload[83]) << 8;
        const unsigned depth = payload[84], levels = payload[85], flags = payload[87];
        if (!w || !h || !levels || levels > 16) return corrupt("dimensions/levels", sprite);
        const unsigned compression = platform == rw::PLATFORM_D3D8 ? flags : (flags & 8) ? (d3d >> 24) - '0' : 0;
        const bool supported = w <= 4096 && h <= 4096 && !(format & (rw::Raster::PAL4 | rw::Raster::PAL8)) &&
            (platform != rw::PLATFORM_D3D9 || !(flags & 2)) &&
            ((compression == 1 || compression == 3) || (!compression && depth == 32 &&
                ((format & 0xf00) == rw::Raster::C8888 || (format & 0xf00) == rw::Raster::C888) &&
                (platform == rw::PLATFORM_D3D8 || d3d == 21 || d3d == 22)));
        std::size_t at = 88 + ((format & rw::Raster::PAL4) ? 128 : (format & rw::Raster::PAL8) ? 1024 : 0);
        for (unsigned level = 0; level < levels; ++level) {
            if (at > payload.size() || payload.size() - at < 4) return corrupt("mip length", sprite);
            const auto size = HudWord(payload, at);
            at += 4;
            if (size > payload.size() - at) return corrupt("truncated texels", sprite);
            if (supported) {
                const auto expected = compression ? std::max(1u, (w + 3) / 4) * std::max(1u, (h + 3) / 4) * (compression == 1 ? 8u : 16u) : w * h * 4;
                if (size != expected) return corrupt("texel length", sprite);
            }
            at += size;
            w = std::max(1u, w / 2); h = std::max(1u, h / 2);
        }
        if (at != payload.size()) return corrupt("trailing texels", sprite);
        if (!supported || (filter & 0xff) < 1 || (filter & 0xff) > 6 ||
            ((filter >> 8) & 15) < 1 || ((filter >> 8) & 15) > 4 ||
            ((filter >> 12) & 15) < 1 || ((filter >> 12) & 15) > 4) {
            states[sprite] = State::Unsupported;
            continue;
        }
        states[sprite] = State::Prepared;
        if (!decode) continue;
        assert(rw::Engine::state == rw::Engine::Started && "exclusive startup parser required");
        rw::StreamMemory stream;
        stream.open(const_cast<std::uint8_t*>(native.Data.data()), static_cast<uint32>(native.Data.size()));
        auto* texture = rw::Texture::streamReadNative(&stream);
        stream.close();
        const bool decoded = texture && TexSample_Decode(texture, images[sprite]);
        if (texture) texture->destroy();
        if (!decoded) return corrupt("supported raster decode", sprite);
        images[sprite].filter = filter;
        states[sprite] = State::Prepared;
    }
    HudChunk extension;
    if (!NextHudChunk(children, extension) || extension.Type != rw::ID_EXTENSION || !children.empty()) return corrupt("extension bounds");
    return true;
}

static bool ReadRadarSpriteBytes(const char* gameDir, std::vector<std::uint8_t>& bytes, char* err, std::size_t errSize) {
    if (!gameDir || !gameDir[0]) {
        SetError(err, errSize, "radar sprites preload needs a game dir");
        return false;
    }
    OS_SetFilePathOffset(gameDir);
    HudFile file;
    if (OS_FileOpen(FILE_DATA_AREA_DEFAULT, &file.Handle, "models/hud.txd", FILE_ACCESS_READ) != 0 || !file.Handle) {
        SetError(err, errSize, "cannot read models/hud.txd for radar sprites");
        return false;
    }
    const auto size = OS_FileSize(file.Handle);
    if (size <= 0) {
        SetError(err, errSize, "models/hud.txd is empty");
        return false;
    }
    bytes.resize(static_cast<std::size_t>(size));
    if (OS_FileRead(file.Handle, bytes.data(), size) != 0) {
        SetError(err, errSize, "cannot read models/hud.txd for radar sprites");
        return false;
    }
    return true;
}

static GLenum TextureFilter(uint32 filter) {
    switch (filter & 0xff) {
    case rw::Texture::NEAREST:
    case rw::Texture::MIPNEAREST:
    case rw::Texture::LINEARMIPNEAREST: return GL_NEAREST;
    case rw::Texture::LINEAR:
    case rw::Texture::MIPLINEAR:
    case rw::Texture::LINEARMIPLINEAR: return GL_LINEAR;
    default: assert(false && "unsupported source texture filter"); return GL_LINEAR;
    }
}

static GLenum TextureAddress(uint32 address) {
    switch (address) {
    case rw::Texture::WRAP: return GL_REPEAT;
    case rw::Texture::MIRROR: return GL_MIRRORED_REPEAT;
    case rw::Texture::CLAMP: return GL_CLAMP_TO_EDGE;
    case rw::Texture::BORDER: return GL_CLAMP_TO_BORDER;
    default: assert(false && "unsupported source texture addressing"); return GL_CLAMP_TO_EDGE;
    }
}

// DisplayThisBlip (0x583B40), priority=-99 at DrawRadarSprite, 1..3 in
// DrawBlips. Exterior means BOTH source area predicates, supplied by caller.
static bool DisplayRadarSprite(int sprite, int priority, bool exterior, const RealtimeHudView& view) {
    if (!exterior) {
        return (sprite >= 0 && sprite <= 4) || sprite == 25 || sprite == 36 ||
            sprite == 41 || sprite == 44 || sprite == 52;
    }
    if (sprite >= 0 && sprite <= 4) return true;
    switch (sprite) {
    case 5: case 6: case 7: case 9: case 10: case 11: case 14: case 17:
    case 22: case 27: case 29: case 30: case 33: case 35: case 36: case 39:
    case 45: case 48: case 49: case 50: case 51: case 52: case 53: case 54:
    case 55: case 63: return (view.locationsBlips && priority < 0) || priority == 1;
    case 8: case 12: case 13: case 15: case 16: case 18: case 21: case 23:
    case 24: case 25: case 26: case 28: case 34: case 37: case 38: case 40:
    case 42: case 43: case 44: case 46: case 47: case 58: case 59: case 60:
    case 61: case 62: return (view.contactsBlips && priority < 0) || priority == 3;
    default: return (view.otherBlips && priority < 0) || priority == 2;
    }
}

static bool RadarVisible(const NativeScriptRadarBlip& blip, float distance,
    bool playerOnMission, unsigned radarZoom, bool exterior) {
    // Contact remains the legacy contact flag; Kind distinguishes 04CE even
    // for a caller that leaves that legacy default true. Sprite traces ignore
    // Colour/Bright/Friendly/Fade/Size and authored height: white/255, 8px.
    return blip.Active && blip.Sprite > 0 && blip.Sprite < int(RealtimeHud::RadarSpriteCount) &&
        !(blip.Kind == NativeScriptBlipKind::Contact && blip.Contact && playerOnMission) &&
        (blip.Display == 2 || blip.Display == 3) && (!blip.ShortRange || (!radarZoom && distance <= 1.0f)) &&
        DisplayRadarSprite(blip.Sprite, -99, exterior, {});
}

// CRadar::CachedRotateClockwise, with GTA heading = native yaw - pi/2.
static Point WorldToRadar(Point world, const RealtimeHudView& view, const RealtimeHudState& state) {
    const float angle = view.cameraYaw - kPi / 2.0f;
    const float c = std::cos(angle), s = std::sin(angle);
    const float x = (world.x - state.playerX) / state.radarRange;
    const float y = (world.y - state.playerY) / state.radarRange;
    return {c * x + s * y, c * y - s * x};
}

static Point RadarToWorld(Point p, const RealtimeHudView& view, const RealtimeHudState& state) {
    const float angle = view.cameraYaw - kPi / 2.0f;
    const float c = std::cos(angle), s = std::sin(angle);
    return {state.playerX + state.radarRange * (c * p.x - s * p.y),
            state.playerY + state.radarRange * (s * p.x + c * p.y)};
}

// Radar.cpp 0x583480 and common.h: original reference height is 448, not 480.
static Point RadarToScreen(Point p) { return {87.0f + 47.0f * p.x, 382.0f - 38.0f * p.y}; }

static Point WorldToUv(Point p, int x, int y) {
    return {(p.x - (500.0f * x - 3000.0f)) / 500.0f,
            ((3000.0f - 500.0f * y) - p.y) / 500.0f};
}

// DrawRadarMask uses six segments per quadrant (24-sided disc). Clip geometry
// against those same chords instead of writing into the world's depth/stencil.
static std::vector<Point> ClipDisc(std::vector<Point> poly) {
    for (int edge = 0; edge < 24 && !poly.empty(); ++edge) {
        const float a = edge * kPi / 12.0f, b = (edge + 1) * kPi / 12.0f;
        const Point p{std::cos(a), std::sin(a)}, q{std::cos(b), std::sin(b)};
        const auto distance = [&](Point v) { return (q.x - p.x) * (v.y - p.y) - (q.y - p.y) * (v.x - p.x); };
        std::vector<Point> clipped;
        auto prev = poly.back();
        float d0 = distance(prev);
        for (const auto next : poly) {
            const float d1 = distance(next);
            if ((d0 >= 0.0f) != (d1 >= 0.0f)) {
                const float t = d0 / (d0 - d1);
                clipped.push_back({prev.x + t * (next.x - prev.x), prev.y + t * (next.y - prev.y)});
            }
            if (d1 >= 0.0f) clipped.push_back(next);
            prev = next;
            d0 = d1;
        }
        poly = std::move(clipped);
    }
    return poly;
}

static void Quad(float x0, float y0, float x1, float y1, float u0 = 0, float v0 = 0, float u1 = 1, float v1 = 1) {
    glBegin(GL_QUADS);
    glTexCoord2f(u0, v0); glVertex2f(x0, y0);
    glTexCoord2f(u1, v0); glVertex2f(x1, y0);
    glTexCoord2f(u1, v1); glVertex2f(x1, y1);
    glTexCoord2f(u0, v1); glVertex2f(x0, y1);
    glEnd();
}

// Font.cpp FindSubFontCharacter(style=1); its input is the GXT byte minus 32.
static unsigned PricedownGlyph(unsigned char ch) {
    assert(ch >= 32);
    const unsigned id = ch - 32;
    switch (id) {
    case 1: return 208;
    case 4: return 93;
    case 7: return 206;
    case 8: case 9: return id + 86;
    case 14: return 207;
    case 26: return 154;
    case 6: return 10;
    case 31: return 91;
    case 62: return 32;
    case 143: return 205;
    }
    if (id >= 16 && id <= 25) return id + 128;
    if (id >= 33 && id <= 58) return id + 122;
    if (id >= 65 && id <= 90) return id + 90;
    if (id >= 96 && id <= 118) return id + 85;
    if (id >= 119 && id <= 140) return id + 62;
    if (id >= 141 && id <= 142) return 204;
    return id;
}

static int PriceAdvance(const MenuHudFont& font, unsigned char ch) {
    const auto glyph = PricedownGlyph(ch);
    // tFontData::m_spaceValue immediately follows the 208 prop bytes. The
    // source maps '!' to this entry and PrintChar suppresses its geometry.
    assert(glyph <= 208);
    return glyph == 208 ? font.space : font.prop[glyph];
}

static void PriceText(const MenuHudFont& font, std::string_view text, float cx, float y, float sx, float sy, int width) {
    // SetCentreSize(SCREEN_WIDTH). Split only at spaces/newlines; strings are
    // already formatted by NativeScriptEntities (no numeric/token substitution).
    while (!text.empty()) {
        std::size_t end = 0, space = std::string_view::npos;
        float advance = 0, atSpace = 0;
        for (; end < text.size() && text[end] != '\n'; ++end) {
            if (text[end] == ' ') { space = end; atSpace = advance; }
            const float next = PriceAdvance(font, text[end]) * sx;
            if (advance + next > width && space != std::string_view::npos) {
                end = space; advance = atSpace; break;
            }
            advance += next;
        }
        float x = cx - advance / 2;
        for (const unsigned char ch : text.substr(0, end)) {
            const auto glyph = PricedownGlyph(ch);
            if (ch != ' ' && glyph != 208) {
                const float u = float(glyph % 16 * 32), v = float(glyph / 16 * 40);
                // Font1 32x40 cells, PrintChar logical height 20 (16 for >=192).
                const float cellHeight = glyph < 192 ? 40.0f : 32.0f;
                if (x >= 0 && x <= width && y >= 0) Quad(x, y, x + 32 * sx, y + cellHeight * .5f * sy,
                    (u + .5f) / 512, (v + .5f) / 512, (u + 31.5f) / 512, (v + cellHeight - .5f) / 512);
            }
            x += PriceAdvance(font, ch) * sx;
        }
        text.remove_prefix(std::min(end + 1, text.size()));
        y += 18 * sy; // CFont::GetHeight(false)
    }
}

static void ClockText(const char* text, int unprop, float dx, float dy) {
    // CFont::GetCharacterSize includes SetEdge(2) in non-proportional advance.
    const float advance = (unprop + 2) * 0.55f;
    float x = 608.0f - std::strlen(text) * advance + dx;
    for (const auto* ch = text; *ch; ++ch, x += advance) {
        const auto glyph = PricedownGlyph(*ch);
        // 32x40 authored cells; PrintChar's 20*scaleY logical height. The
        // upstream PrintChar body is incomplete/unreversed: UVs here use exact
        // decoded cell bounds, half-texel inset prevents neighbouring ink bleed.
        const float u = float(glyph % 16 * 32), v = float(glyph / 16 * 40);
        Quad(x, 22.0f + dy, x + 32.0f * 0.55f, 44.0f + dy,
             (u + 0.5f) / 512.0f, (v + 0.5f) / 512.0f,
             (u + 31.5f) / 512.0f, (v + 39.5f) / 512.0f);
    }
}

struct DrawState {
    GLint program{}, active{}, mode{};
    GLboolean colorSum{};
    DrawState(int width, int height) {
        glGetIntegerv(GL_CURRENT_PROGRAM, &program);
        glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
        glGetIntegerv(GL_MATRIX_MODE, &mode);
        colorSum = glIsEnabled(GL_COLOR_SUM);
        glPushAttrib(GL_ALL_ATTRIB_BITS);
        glUseProgram(0);
        GLint units{};
        glGetIntegerv(GL_MAX_TEXTURE_UNITS, &units);
        for (int i = 0; i < units; ++i) {
            glActiveTexture(GL_TEXTURE0 + i);
            glDisable(GL_TEXTURE_1D); glDisable(GL_TEXTURE_2D);
            glDisable(GL_TEXTURE_3D); glDisable(GL_TEXTURE_CUBE_MAP);
            glDisable(GL_TEXTURE_RECTANGLE);
            glDisable(GL_TEXTURE_GEN_S); glDisable(GL_TEXTURE_GEN_T);
            glDisable(GL_TEXTURE_GEN_R); glDisable(GL_TEXTURE_GEN_Q);
        }
        glActiveTexture(GL_TEXTURE0);
        glMatrixMode(GL_TEXTURE); glPushMatrix(); glLoadIdentity();
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
        glMatrixMode(GL_PROJECTION); glPushMatrix(); glLoadIdentity();
        glOrtho(0, 640, 448, 0, -1, 1);
        glMatrixMode(GL_MODELVIEW); glPushMatrix(); glLoadIdentity();
        glViewport(0, 0, width, height);
        glDisable(GL_DEPTH_TEST); glDepthMask(GL_FALSE);
        glDisable(GL_STENCIL_TEST); glDisable(GL_ALPHA_TEST);
        glDisable(GL_FOG); glDisable(GL_LIGHTING); glDisable(GL_CULL_FACE);
        glDisable(GL_SCISSOR_TEST); glDisable(GL_COLOR_LOGIC_OP);
        glDisable(GL_POLYGON_STIPPLE); glDisable(GL_POLYGON_SMOOTH);
        glDisable(GL_COLOR_SUM); glDisable(GL_SAMPLE_COVERAGE);
        glDisable(GL_SAMPLE_ALPHA_TO_COVERAGE); glDisable(GL_SAMPLE_ALPHA_TO_ONE);
        GLint planes{};
        glGetIntegerv(GL_MAX_CLIP_PLANES, &planes);
        for (int i = 0; i < planes; ++i) glDisable(GL_CLIP_PLANE0 + i);
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glEnable(GL_BLEND);
        glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
        glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        glColor4ub(255, 255, 255, 255);
    }
    ~DrawState() {
        glMatrixMode(GL_MODELVIEW); glPopMatrix();
        glMatrixMode(GL_PROJECTION); glPopMatrix();
        glMatrixMode(GL_TEXTURE); glPopMatrix();
        glPopAttrib();
        // Mesa compatibility does not restore this extension enable with the
        // attribute stack (covered by the hostile-state rendered probe).
        if (colorSum) glEnable(GL_COLOR_SUM); else glDisable(GL_COLOR_SUM);
        glUseProgram(program);
        glActiveTexture(active);
        glMatrixMode(mode);
    }
};
} // namespace

RealtimeHud::~RealtimeHud() { ReleaseGpu(); }

bool RealtimeHud::Load(const char* gameDir, char* err, std::size_t errSize) {
    assert(!m_Textures.Tiles[0] && "ReleaseGpu before reload");
    m_RadarSprites = {};
    m_RadarSpriteStates = {};
    m_Loaded = false;
    std::vector<std::uint8_t> bytes;
    // Reject corrupt sprite payloads before the older shared HUD dictionary
    // reader is used for the map's centre/north/disc. No RW needed for preflight.
    if (!ReadRadarSpriteBytes(gameDir, bytes, err, errSize) ||
        !DecodeRadarSprites(bytes, m_RadarSprites, m_RadarSpriteStates, err, errSize, false)) return false;
    m_Loaded = RadarMap_LoadAssets(gameDir, m_Radar, err, errSize)
        && MenuShot_LoadPricedownFont(gameDir, m_Font, err, errSize);
    if (m_Loaded) m_Loaded = DecodeRadarSprites(bytes, m_RadarSprites, m_RadarSpriteStates, err, errSize);
    return m_Loaded;
}

bool RealtimeHud::Upload(char* err, std::size_t errSize) {
    assert(m_Loaded && !m_Textures.Tiles[0]);
    GLint active{}, unpackBuffer{};
    glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
    glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &unpackBuffer);
    glPushAttrib(GL_ALL_ATTRIB_BITS);
    glPushClientAttrib(GL_CLIENT_PIXEL_STORE_BIT);
    glActiveTexture(GL_TEXTURE0);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    glPixelTransferi(GL_MAP_COLOR, GL_FALSE);
    for (const auto scale : {GL_RED_SCALE, GL_GREEN_SCALE, GL_BLUE_SCALE, GL_ALPHA_SCALE}) glPixelTransferf(scale, 1);
    for (const auto bias : {GL_RED_BIAS, GL_GREEN_BIAS, GL_BLUE_BIAS, GL_ALPHA_BIAS}) glPixelTransferf(bias, 0);
    const auto upload = [](unsigned int& texture, int width, int height,
        const std::vector<std::uint8_t>& rgba, uint32 sampler, bool sourceSampler) {
        glGenTextures(1, &texture);
        glBindTexture(GL_TEXTURE_2D, texture);
        const auto filter = sourceSampler ? TextureFilter(sampler) : GL_LINEAR;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, sourceSampler ? TextureAddress((sampler >> 8) & 15) : GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, sourceSampler ? TextureAddress((sampler >> 12) & 15) : GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    };
    const auto imageUpload = [&](unsigned int& texture, const TexImage& image, bool sourceSampler = false) {
        upload(texture, image.w, image.h, image.rgba, image.filter, sourceSampler);
    };
    for (std::size_t i = 0; i < m_Textures.Tiles.size(); ++i) imageUpload(m_Textures.Tiles[i], m_Radar.tiles[i]);
    imageUpload(m_Textures.Centre, m_Radar.centre);
    imageUpload(m_Textures.North, m_Radar.north);
    imageUpload(m_Textures.Disc, m_Radar.disc);
    upload(m_Textures.Font, m_Font.w, m_Font.h, m_Font.rgba, 0, false);
    for (std::size_t i = 0; i < m_Textures.Sprites.size(); ++i) {
        if (PreparedRadarSprite(i)) imageUpload(m_Textures.Sprites[i], m_RadarSprites[i], true);
    }
    glPopClientAttrib();
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, unpackBuffer);
    glPopAttrib();
    glActiveTexture(active);
    const auto error = glGetError();
    if (error != GL_NO_ERROR) {
        if (err && errSize) std::snprintf(err, errSize, "HUD texture upload GL=0x%x", error);
        ReleaseGpu();
        return false;
    }
    return true;
}

void RealtimeHud::ReleaseGpu() {
    bool allocated = m_Textures.Centre || m_Textures.North || m_Textures.Disc || m_Textures.Font;
    for (const auto texture : m_Textures.Tiles) allocated |= texture != 0;
    for (const auto texture : m_Textures.Sprites) allocated |= texture != 0;
    if (!allocated) return;
    glDeleteTextures(m_Textures.Tiles.size(), m_Textures.Tiles.data());
    glDeleteTextures(m_Textures.Sprites.size(), m_Textures.Sprites.data());
    for (const auto texture : {m_Textures.Centre, m_Textures.North, m_Textures.Disc, m_Textures.Font}) glDeleteTextures(1, &texture);
    m_Textures = {};
}

const WorldShotImage* RealtimeHud::PreparedRadarSprite(int sprite) const {
    if (!m_Loaded || sprite < 0 || sprite >= int(RadarSpriteCount) ||
        m_RadarSpriteStates[sprite] != RadarSpriteState::Prepared) return nullptr;
    return &m_RadarSprites[sprite];
}

bool RealtimeHud::IsRadarSpriteUploaded(int sprite) const {
    return PreparedRadarSprite(sprite) && m_Textures.Sprites[sprite] != 0;
}

const char* RealtimeHud::RadarSpriteName(int sprite) {
    return sprite >= 0 && sprite < int(RadarSpriteCount) ? kRadarNames[sprite] : nullptr;
}

RealtimeHud::RadarSpriteState RealtimeHud::GetRadarSpriteState(int sprite) const {
    if (sprite < 0 || sprite >= int(RadarSpriteCount)) return RadarSpriteState::InvalidId;
    if (!kRadarNames[sprite]) return RadarSpriteState::NoTexture;
    if (!m_Loaded) return RadarSpriteState::Unloaded;
    return IsRadarSpriteUploaded(sprite) ? RadarSpriteState::Uploaded : m_RadarSpriteStates[sprite];
}

RealtimeHudPriceView RealtimeHud::CapturePriceView(float nearClip, float farClip, float fov) {
    RealtimeHudPriceView view;
    glGetFloatv(GL_MODELVIEW_MATRIX, view.ModelView.data());
    glGetFloatv(GL_PROJECTION_MATRIX, view.Projection.data());
    view.NearClip = nearClip; view.FarClip = farClip; view.Fov = fov;
    return view;
}

std::vector<RealtimeHudProjectedPrice> RealtimeHud::ProjectPropertyPrices(
    std::span<const NativeScriptPropertyLabel> labels, const RealtimeHudPriceView& view, int width, int height) {
    assert(width > 0 && height > 0 && std::isfinite(view.Fov) && view.Fov > 0);
    assert(view.NearClip >= 0 && std::isfinite(view.FarClip) && view.FarClip > view.NearClip + 1);
    const auto transform = [](const auto& matrix, const std::array<float, 4>& p) {
        std::array<float, 4> out{};
        for (int row = 0; row < 4; ++row) for (int col = 0; col < 4; ++col) out[row] += matrix[col * 4 + row] * p[col];
        return out;
    };
    std::vector<RealtimeHudProjectedPrice> projected;
    projected.reserve(std::min(labels.size(), std::size_t{16}));
    for (std::size_t i = 0; i < labels.size() && projected.size() < 16; ++i) {
        const auto& p = labels[i].Position;
        const auto eye = transform(view.ModelView, {p.X, p.Y, p.Z, 1});
        const float depth = -eye[2]; // GL camera looks along -Z; source camera uses +Z.
        assert(std::isfinite(depth));
        if (depth <= view.NearClip + 1 || depth >= view.FarClip) continue;
        const auto clip = transform(view.Projection, eye);
        assert(std::isfinite(clip[3]) && clip[3] > 0);
        const float w = width / depth / view.Fov * 70, h = height / depth / view.Fov * 70;
        projected.push_back({i, (clip[0] / clip[3] + 1) * width / 2,
            (1 - clip[1] / clip[3]) * height / 2, depth, w, h,
            std::min(width / 640.0f, w / 30), std::min(width / 640.0f, h / 30)});
    }
    return projected;
}

void RealtimeHud::DrawPropertyPrices(std::span<const NativeScriptPropertyLabel> labels,
    int width, int height, float nearClip, float farClip, float fov) const {
    DrawPropertyPrices(labels, CapturePriceView(nearClip, farClip, fov), width, height);
}

void RealtimeHud::DrawPropertyPrices(std::span<const NativeScriptPropertyLabel> labels,
    const RealtimeHudPriceView& view, int width, int height) const {
    assert(m_Textures.Tiles[0]);
    const auto projected = ProjectPropertyPrices(labels, view, width, height);
    if (projected.empty()) return;
    const DrawState restore(width, height);
    // Pickup font scales are already in drawable pixels, unlike the HUD's
    // reference-coordinate clock/help. Do not apply screen stretching twice.
    glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0, width, height, 0, -1, 1);
    glEnable(GL_TEXTURE_2D); glBindTexture(GL_TEXTURE_2D, m_Textures.Font);
    for (const auto& p : projected) {
        const auto& label = labels[p.Candidate];
        glColor4ub(label.Color[0], label.Color[1], label.Color[2], label.Alpha);
        PriceText(m_Font, label.Text, p.X, p.Y, p.ScaleX, p.ScaleY, width);
    }
}

void RealtimeHud::Draw(const RealtimeHudView& view, const RealtimeHudState& state, int width, int height) const {
    assert(m_Textures.Tiles[0] && width > 0 && height > 0);
    assert(std::isfinite(view.cameraYaw) && std::isfinite(state.playerYaw));
    assert(std::isfinite(state.playerX) && std::isfinite(state.playerY));
    assert(std::isfinite(state.radarRange) && state.radarRange > 0 && state.radarRange <= 350);
    assert(state.hour >= 0 && state.hour < 24 && state.minute >= 0 && state.minute < 60);
    const DrawState restore(width, height);
    if (view.radar) {
        const int cx = int(std::floor((state.playerX + 3000.0f) / 500.0f));
        const int cy = int(std::ceil(11.0f - (state.playerY + 3000.0f) / 500.0f));
        for (int y = cy - 1; y <= cy + 1; ++y) {
            for (int x = cx - 1; x <= cx + 1; ++x) {
                const float west = x * 500.0f - 3000.0f, north = 3000.0f - y * 500.0f;
                auto poly = ClipDisc({WorldToRadar({west, north}, view, state),
                    WorldToRadar({west + 500, north}, view, state),
                    WorldToRadar({west + 500, north - 500}, view, state),
                    WorldToRadar({west, north - 500}, view, state)});
                if (poly.size() < 3) continue;
                if (x >= 0 && x < 12 && y >= 0 && y < 12) {
                    glEnable(GL_TEXTURE_2D); glBindTexture(GL_TEXTURE_2D, m_Textures.Tiles[y * 12 + x]);
                    glColor4ub(255, 255, 255, 255);
                } else {
                    glDisable(GL_TEXTURE_2D); glColor4ub(111, 137, 170, 255);
                }
                glBegin(GL_TRIANGLE_FAN);
                for (auto p : poly) {
                    const auto uv = WorldToUv(RadarToWorld(p, view, state), x, y);
                    const auto screen = RadarToScreen(p);
                    glTexCoord2f(uv.x, uv.y); glVertex2f(screen.x, screen.y);
                }
                glEnd();
            }
        }
        // CHud::DrawRadar stamps the same quadrant four times with flipped
        // rectangles. radardisc is authored alpha corner/rim, not bar texture.
        glEnable(GL_TEXTURE_2D); glBindTexture(GL_TEXTURE_2D, m_Textures.Disc);
        glColor4ub(0, 0, 0, 255);
        Quad(36, 340, 87, 382); Quad(36, 424, 87, 382);
        Quad(138, 340, 87, 382); Quad(138, 424, 87, 382);
        glColor4ub(255, 255, 255, 255);
        const float orientation = view.cameraYaw - kPi / 2.0f;
        const auto n = RadarToScreen({std::sin(orientation), std::cos(orientation)});
        glBindTexture(GL_TEXTURE_2D, m_Textures.North);
        Quad(n.x - 8, n.y - 8, n.x + 8, n.y + 8);
        const auto drawBlip = [&](const NativeScriptRadarBlip& blip) {
            auto p = WorldToRadar({blip.Position.X, blip.Position.Y}, view, state);
            const auto distance = std::sqrt(p.x*p.x + p.y*p.y);
            if (!RadarVisible(blip, distance, state.playerOnMission, state.radarZoom, state.exterior) ||
                !DisplayRadarSprite(blip.Sprite, -99, state.exterior, view) || !IsRadarSpriteUploaded(blip.Sprite)) return;
            glBindTexture(GL_TEXTURE_2D, m_Textures.Sprites[blip.Sprite]);
            glColor4ub(255, 255, 255, 255);
            if (distance > 1) { p.x /= distance; p.y /= distance; }
            const auto screen = RadarToScreen(p);
            const float halfWidth = std::floor(width / 640.0f * 8) * 640 / width;
            const float halfHeight = std::floor(height / 448.0f * 8) * 448 / height;
            // Native compatibility quad: source DrawRadarSprite bounds and
            // decoded full UVs. This does not claim original D3D quad pixel parity.
            Quad(screen.x - halfWidth, screen.y - halfHeight, screen.x + halfWidth, screen.y + halfHeight);
        };
        // DrawBlips' two passes: waypoint draws in both; ordinary sprites draw
        // in the second, in priority then pool order. Some interior categories
        // pass all three priorities, and their repeated alpha blend is source.
        for (const auto& blip : state.scriptBlips) if (blip.Sprite == 41) drawBlip(blip);
        for (int priority = 1; priority <= 3; ++priority) {
            for (const auto& blip : state.scriptBlips) {
                if (blip.Sprite != 41 && DisplayRadarSprite(blip.Sprite, priority, state.exterior, view)) drawBlip(blip);
            }
        }
        for (const auto& blip : state.scriptBlips) if (blip.Sprite == 41) drawBlip(blip);
        // CRadar::DrawBlips + DrawRotatingRadarSprite + CSprite2d::SetVertices:
        // UV corners 00,10,11,01 map to rotating vertices 0,1,2,3.
        glBindTexture(GL_TEXTURE_2D, m_Textures.Centre);
        const float angle = state.playerYaw - view.cameraYaw - kPi;
        glBegin(GL_QUADS);
        for (int i = 0; i < 4; ++i) {
            const float theta = i * kPi / 2.0f + angle - kPi / 4.0f;
            glTexCoord2f(i == 1 || i == 2, i >= 2);
            glVertex2f(87.0f + 8.0f * std::sin(theta), 382.0f + 8.0f * std::cos(theta));
        }
        glEnd();
    }
    if (view.clock) {
        glEnable(GL_TEXTURE_2D); glBindTexture(GL_TEXTURE_2D, m_Textures.Font);
        char text[16];
        std::snprintf(text, sizeof(text), "%02d:%02d", state.hour, state.minute);
        glColor4ub(0, 0, 0, 255);
        for (int y = -2; y <= 2; ++y) for (int x = -2; x <= 2; ++x) {
            if (x || y) ClockText(text, m_Font.unprop, float(x), float(y));
        }
        glColor4ub(225, 225, 225, 255);
        ClockText(text, m_Font.unprop, 0, 0);
    }
    if (!state.helpText.empty() && state.helpAlpha) {
        const auto lines = NativeScriptHelpLines(state.helpText, m_Font.prop);
        // CHud uses FONT_SUBTITLES=1: same font1 texture as clock, WITHOUT
        // Pricedown glyph remapping. SetAlphaFade affects text; box gets alpha directly.
        glDisable(GL_TEXTURE_2D); glColor4ub(0, 0, 0, state.helpAlpha);
        Quad(30, 24, 234, 32 + float(lines.size()) * 19.8f);
        glEnable(GL_TEXTURE_2D); glBindTexture(GL_TEXTURE_2D, m_Textures.Font);
        glColor4ub(225, 225, 225, state.helpAlpha);
        float y = 28;
        for (const auto& line : lines) {
            float x = 34;
            for (const unsigned char ch : line) {
                if (ch < 32 || ch >= 128) continue;
                const auto glyph = unsigned(ch - 32);
                const float u = float(glyph % 16 * 32), v = float(glyph / 16 * 40);
                if (ch != ' ') Quad(x, y, x + 16.64f, y + 22,
                    (u + 0.5f) / 512, (v + 0.5f) / 512, (u + 31.5f) / 512, (v + 39.5f) / 512);
                x += m_Font.prop[glyph] * 0.52f;
            }
            y += 19.8f;
        }
    }
}
