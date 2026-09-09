#include "app/platform/linux/RealtimeHud.h"

#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <numbers>

namespace {
constexpr float kPi = std::numbers::pi_v<float>;
constexpr int kCentre = 144, kNorth = 145, kDisc = 146, kFont = 147;
struct Point { float x, y; };

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

// Font.cpp FindSubFontCharacter(style=1), for the clock's ASCII alphabet.
static unsigned PricedownGlyph(unsigned char ch) {
    assert((ch >= '0' && ch <= '9') || ch == ':');
    return ch == ':' ? 154u : unsigned(ch - '0') + 144u;
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
    assert(!m_Textures[0] && "ReleaseGpu before reload");
    m_Loaded = RadarMap_LoadAssets(gameDir, m_Radar, err, errSize)
        && MenuShot_LoadPricedownFont(gameDir, m_Font, err, errSize);
    return m_Loaded;
}

bool RealtimeHud::Upload(char* err, std::size_t errSize) {
    assert(m_Loaded && !m_Textures[0]);
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
    glGenTextures(m_Textures.size(), m_Textures.data());
    for (int i = 0; i < int(m_Textures.size()); ++i) {
        const TexImage* image = i < 144 ? &m_Radar.tiles[i] : i == kCentre ? &m_Radar.centre
            : i == kNorth ? &m_Radar.north : i == kDisc ? &m_Radar.disc : nullptr;
        glBindTexture(GL_TEXTURE_2D, m_Textures[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, image ? image->w : m_Font.w,
            image ? image->h : m_Font.h, 0, GL_RGBA, GL_UNSIGNED_BYTE,
            image ? image->rgba.data() : m_Font.rgba.data());
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
    if (m_Textures[0]) glDeleteTextures(m_Textures.size(), m_Textures.data());
    m_Textures.fill(0);
}

void RealtimeHud::Draw(const RealtimeHudView& view, const RealtimeHudState& state, int width, int height) const {
    assert(m_Textures[0] && width > 0 && height > 0);
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
                    glEnable(GL_TEXTURE_2D); glBindTexture(GL_TEXTURE_2D, m_Textures[y * 12 + x]);
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
        glEnable(GL_TEXTURE_2D); glBindTexture(GL_TEXTURE_2D, m_Textures[kDisc]);
        glColor4ub(0, 0, 0, 255);
        Quad(36, 340, 87, 382); Quad(36, 424, 87, 382);
        Quad(138, 340, 87, 382); Quad(138, 424, 87, 382);
        glColor4ub(255, 255, 255, 255);
        const float orientation = view.cameraYaw - kPi / 2.0f;
        const auto n = RadarToScreen({std::sin(orientation), std::cos(orientation)});
        glBindTexture(GL_TEXTURE_2D, m_Textures[kNorth]);
        Quad(n.x - 8, n.y - 8, n.x + 8, n.y + 8);
        // CRadar::DrawBlips + DrawRotatingRadarSprite + CSprite2d::SetVertices:
        // UV corners 00,10,11,01 map to rotating vertices 0,1,2,3.
        glBindTexture(GL_TEXTURE_2D, m_Textures[kCentre]);
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
        glEnable(GL_TEXTURE_2D); glBindTexture(GL_TEXTURE_2D, m_Textures[kFont]);
        char text[16];
        std::snprintf(text, sizeof(text), "%02d:%02d", state.hour, state.minute);
        glColor4ub(0, 0, 0, 255);
        for (int y = -2; y <= 2; ++y) for (int x = -2; x <= 2; ++x) {
            if (x || y) ClockText(text, m_Font.unprop, float(x), float(y));
        }
        glColor4ub(225, 225, 225, 255);
        ClockText(text, m_Font.unprop, 0, 0);
    }
}
