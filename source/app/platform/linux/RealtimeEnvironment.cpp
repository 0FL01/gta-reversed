#include "app/platform/linux/RealtimeEnvironment.h"

#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include "app/platform/linux/TexSample.h"
using int32 = int32_t;
using uint32 = uint32_t;
using int64 = int64_t;
using uint64 = uint64_t;
#ifndef __stdcall
#define __stdcall
#endif
#include "oswrapper/oswrapper.h"
#include <rw.h>

namespace {
// CTimeCycle::Update's sample times, not synthetic asset data.
constexpr std::array<int, 9> kSampleHours{0, 5, 6, 7, 12, 19, 20, 22, 24};
constexpr float kPi = 3.14159265358979323846f;

struct FixedWeather {
    const char* name;
    float wind;
    bool foggy, cloudy, extraSunny;
    int clearCounterpart = -1;
};
// Exact eWeatherType order (0..22). OriginalWeatherConstants wind: retail
// 94D510; Update 75EA21/75ECE2. Fixed Old==New factors from CWeather::Update
// static evidence (round8-clouds); notably SANDSTORM is foggy and extra-colour
// IDs are cloudy. These are source IDs, never weather-name classifiers.
constexpr std::array<FixedWeather, 23> kFixedWeather{{
    {"EXTRASUNNY_LA",          0,    false, false, true},
    {"SUNNY_LA",               .25f, false, false, false},
    {"EXTRASUNNY_SMOG_LA",     0,    false, false, true, 0},
    {"SUNNY_SMOG_LA",          .2f,  false, false, false, 1},
    {"CLOUDY_LA",              .7f,  false, true,  false},
    {"SUNNY_SF",               .25f, false, false, false},
    {"EXTRASUNNY_SF",          0,    false, false, true},
    {"CLOUDY_SF",              .7f,  false, true,  false},
    {"RAINY_SF",               1,    false, true,  false},
    {"FOGGY_SF",               0,    true,  true,  false},
    {"SUNNY_VEGAS",            .2f,  false, false, false},
    {"EXTRASUNNY_VEGAS",       0,    false, false, true},
    {"CLOUDY_VEGAS",           .4f,  false, true,  false},
    {"EXTRASUNNY_COUNTRYSIDE", 0,    false, false, true},
    {"SUNNY_COUNTRYSIDE",      .3f,  false, false, false},
    {"CLOUDY_COUNTRYSIDE",     .7f,  false, true,  false},
    {"RAINY_COUNTRYSIDE",      1,    false, true,  false},
    {"EXTRASUNNY_DESERT",      0,    false, false, true},
    {"SUNNY_DESERT",           .3f,  false, false, false},
    {"SANDSTORM_DESERT",       1.5f, true,  true,  false},
    {"UNDERWATER",             0,    false, true,  false},
    {"EXTRACOLOURS_1",         0,    false, true,  false},
    {"EXTRACOLOURS_2",         0,    false, true,  false},
}};

static bool Fail(char* err, std::size_t errSize, const char* message) {
    if (err && errSize) {
        std::snprintf(err, errSize, "%s", message);
    }
    return false;
}

static void AdvanceFlowUV(RealtimeWaterState& water) {
    // RenderWater, retail 728DA7..728E8E: x87 arithmetic rounded at stores.
    for (int axis = 0; axis < 2; ++axis) {
        const double distance = double(water.flowTimeStep) * double(.04f) * double(water.currentFlow[axis]);
        water.firstFlowUV[axis] = float(double(water.firstFlowUV[axis]) + distance * double(.08f));
        water.secondFlowUV[axis] = float(double(water.secondFlowUV[axis]) + distance * double(.04f));
        if (water.firstFlowUV[axis] > 1.0f) water.firstFlowUV[axis] -= 1.0f;
        if (water.secondFlowUV[axis] > 1.0f) water.secondFlowUV[axis] -= 1.0f;
    }
}

static bool LoadWaterTexture(WorldShotImage& image, WorldShotImage& seaBed, char* err, std::size_t errSize) {
    // Same NULL-platform plugin set as the other native asset loaders. Borrow
    // the shared engine; never shut it down or retain a dictionary across Load.
    if (rw::Engine::state == rw::Engine::Dead) {
        if (!rw::Engine::init(nullptr)) return Fail(err, errSize, "water librw init");
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
        if (!rw::Engine::open(nullptr) || !rw::Engine::start()) return Fail(err, errSize, "water librw start");
        rw::Texture::setLoadTextures(false);
    }
    assert(rw::Engine::state == rw::Engine::Started);
    auto* saved = rw::TexDictionary::getCurrent();
    struct DictionaryGuard {
        rw::TexDictionary* saved;
        rw::TexDictionary* owned = nullptr;
        ~DictionaryGuard() {
            if (owned) owned->destroy();
            rw::TexDictionary::setCurrent(saved);
        }
    } guard{saved};
    void* file = nullptr;
    if (OS_FileOpen(FILE_DATA_AREA_DEFAULT, &file, "models/particle.txd", FILE_ACCESS_READ) != 0 || !file)
        return Fail(err, errSize, "water models/particle.txd open");
    const int32 size = OS_FileSize(file);
    std::vector<uint8_t> bytes(std::max(size, 0));
    const bool read = size >= 12 && OS_FileRead(file, bytes.data(), size) == 0;
    OS_FileClose(file);
    if (!read) return Fail(err, errSize, "water particle.txd read");
    rw::StreamMemory stream;
    stream.open(bytes.data(), static_cast<uint32>(bytes.size()));
    if (rw::findChunk(&stream, rw::ID_TEXDICTIONARY, nullptr, nullptr))
        guard.owned = rw::TexDictionary::streamRead(&stream);
    stream.close();
    if (!guard.owned) return Fail(err, errSize, "water particle.txd parse");
    auto* texture = guard.owned->find("waterclear256");
    if (!texture || !TexSample_Decode(texture, image))
        return Fail(err, errSize, "water particle:waterclear256 decode");
    texture = guard.owned->find("seabd32"); // CWaterLevel::LoadTextures, not "seab32"
    if (!texture || !TexSample_Decode(texture, seaBed))
        return Fail(err, errSize, "water particle:seabd32 decode");
    return true;
}

// CMaths::InitMaths/GetSinFast, not continuous sin in a shader. Constants from
// original faWaveMultipliersX/Y (8D38C8/8D38E8; owned retail 94BE28/94BE48).
constexpr std::array<float, 8> kWaveX{1, .85f, .73f, .77f, .75f, .8f, .73f, .8f};
constexpr std::array<float, 8> kWaveY{.75f, .9f, .95f, .82f, .7f, .75f, .9f, 1};
static float SinFast(float phase) {
    static const auto table = [] {
        std::array<float, 256> values{};
        for (size_t i = 0; i < values.size(); ++i) values[i] = std::sin(float(i) * (2 * kPi / 256));
        return values;
    }();
    return table[static_cast<uint32_t>(phase / (2 * kPi / 256)) % 256];
}
} // namespace

RealtimeEnvironment::~RealtimeEnvironment() {
    ReleaseGpu();
}

bool RealtimeEnvironment::Load(const char* gameDir, char* err, std::size_t errSize,
                               float hour, const char* weather) {
    assert(!m_WaterTexture && !m_SeaBedTexture && !m_LightingProgram && "ReleaseGpu before reloading environment");
    m_Loaded = false;
    m_HasLowCloudParams = false;
    m_FixedWeather = -1;
    m_WaterTriangles = 0;
    if (!std::isfinite(hour) || hour < 0.0f || hour >= 24.0f) {
        return Fail(err, errSize, "environment hour must be finite in [0,24)");
    }
    for (std::size_t i = 0; i < m_Samples.size(); ++i) {
        if (!TimeCycle_LoadWeatherHour(gameDir, weather, kSampleHours[i], m_Samples[i], err, errSize)) {
            return false;
        }
        // The legacy loader tolerates short sections; interpolation needs all
        // eight real anchors, so never silently repeat a missing row here.
        if (m_Samples[i].sampleIdx != static_cast<int>(i)) {
            return Fail(err, errSize, "environment weather needs eight timecyc samples");
        }
    }
    for (size_t i = 0; i < kFixedWeather.size(); ++i) {
        if (!std::strcmp(weather, kFixedWeather[i].name)) m_FixedWeather = static_cast<int>(i);
    }
    if (m_FixedWeather < 0) return Fail(err, errSize, "water weather needs OriginalWeatherConstants mapping");
    const auto& fixed = kFixedWeather[m_FixedWeather];
    m_HasLowCloudParams = std::all_of(m_Samples.begin(), m_Samples.end(),
        [](const auto& sample) { return sample.hasLowCloudColours; });
    if (fixed.clearCounterpart >= 0) {
        // Startup-only OS_File work, before the asset worker starts. A missing
        // optional counterpart disables clouds without breaking legacy water.
        for (size_t i = 0; i < m_ClearSamples.size(); ++i) {
            char cloudErr[128]{};
            if (!TimeCycle_LoadWeatherHour(gameDir, kFixedWeather[fixed.clearCounterpart].name,
                    kSampleHours[i], m_ClearSamples[i], cloudErr, sizeof(cloudErr)) ||
                m_ClearSamples[i].sampleIdx != static_cast<int>(i) || !m_ClearSamples[i].hasLowCloudColours) {
                m_HasLowCloudParams = false;
            }
        }
    }
    if (!WaterLevel_Load(gameDir, "data/water.dat", m_Water, err, errSize)) {
        return false;
    }
    if (!LoadWaterTexture(m_WaterImage, m_SeaBedImage, err, errSize)) return false;
    m_WaterState = {};
    m_WaterFlow.Initialise(m_Water);
    m_WaterState.wavyness = std::min(std::min(fixed.wind, 1.0f) + .3f, 1.0f);
    for (const auto& poly : m_Water.polys) if (poly.Visible()) m_WaterTriangles += poly.nverts - 2;
    const auto missing = std::count_if(m_Samples.begin(), m_Samples.end(),
        [](const auto& sample) { return !sample.hasDirectionalMult; });
    if (missing) {
        std::printf("environment-note weather=%s missing-DirMult-rows=%td directional-disabled-for-missing-rows\n",
                    weather, missing);
    }
    m_Loaded = true;
    return SetHour(hour);
}

bool RealtimeEnvironment::SetHour(float hour) {
    if (!m_Loaded || !std::isfinite(hour) || hour < 0.0f || hour >= 24.0f) {
        return false;
    }
    std::size_t sample = 0;
    while (sample + 1 < m_Samples.size() && hour >= kSampleHours[sample + 1]) {
        ++sample;
    }
    const auto& a = m_Samples[sample];
    const auto& b = m_Samples[(sample + 1) % m_Samples.size()];
    const float t = (hour - kSampleHours[sample]) /
                   static_cast<float>(kSampleHours[sample + 1] - kSampleHours[sample]);
    auto color = [t](auto& dst, const auto& first, const auto& second) {
        for (std::size_t c = 0; c < dst.size(); ++c) {
            dst[c] = std::lerp(static_cast<float>(first[c]), static_cast<float>(second[c]), t) / 255.0f;
        }
    };
    m_Params.hour = hour;
    color(m_Params.ambient, a.amb, b.amb);
    color(m_Params.ambientObjects, a.ambObjects, b.ambObjects);
    // app_light.cpp SetLightsWithTimeOfDayColour: Dir RGB is unused by SA.
    m_Params.directional.fill(std::lerp(a.directionalMult, b.directionalMult, t) * 0.99609375f);
    // CCustomBuildingRenderer::UpdateDayNightBalanceParam (0x5D7F80).
    m_Params.nightBalance = hour < 6 ? 1 : hour < 7 ? 7 - hour : hour < 20 ? 0 : hour < 21 ? hour - 20 : 1;
    color(m_Params.skyTop, a.skyTop, b.skyTop);
    color(m_Params.skyBottom, a.skyBot, b.skyBot);
    color(m_Params.water, a.water, b.water);
    m_Params.farClip = std::lerp(a.farClp, b.farClp, t);
    m_Params.fogStart = std::lerp(a.fogSt, b.fogSt, t);
    // CTimeCycle::Initialise m_vecDirnLightToSun; the animated sun sprite
    // orbit is a different vector, not the light used by app_light.cpp.
    m_Params.sunDirection = {-0.5f, -0.5f, std::sqrt(0.5f)};
    return true;
}

bool RealtimeEnvironment::GetFixedWeatherLowCloudParams(float cameraZ, RealtimeLowCloudParams& out) const {
    if (!m_Loaded || !m_HasLowCloudParams || !std::isfinite(cameraZ)) return false;
    assert(m_FixedWeather >= 0 && m_FixedWeather < static_cast<int>(kFixedWeather.size()));
    const auto& fixed = kFixedWeather[m_FixedWeather];
    // CTimeCycle::CalcColoursForPoint 282..342; CColourSet::Interpolate
    // 110..112 truncates the nonnegative 0..255 sum on EVERY interpolation.
    const float hour = std::min(m_Params.hour, 23.999f);
    size_t sample = 0;
    while (sample + 1 < m_Samples.size() && hour >= kSampleHours[sample + 1]) ++sample;
    const size_t next = (sample + 1) % m_Samples.size();
    const float t = (hour - kSampleHours[sample]) / float(kSampleHours[sample + 1] - kSampleHours[sample]);
    const float altitude = std::clamp((cameraZ - 20.0f) / 200.0f, 0.0f, 1.0f);
    const auto blend = [](uint8_t a, uint8_t b, float fraction) {
        // Explicit source multiplication/addition order (not std::lerp/FMA).
        const volatile float first = a * (1.0f - fraction), second = b * fraction;
        return static_cast<uint8_t>(first + second);
    };
    RealtimeLowCloudParams params;
    params.hour = m_Params.hour;
    params.gameMs = m_WaterState.gameMs;
    for (size_t c = 0; c < params.colours.size(); ++c) {
        uint8_t a = m_Samples[sample].lowCloudColours[c], b = m_Samples[next].lowCloudColours[c];
        if (fixed.clearCounterpart >= 0) {
            a = blend(a, m_ClearSamples[sample].lowCloudColours[c], altitude);
            b = blend(b, m_ClearSamples[next].lowCloudColours[c], altitude);
        }
        params.colours[c] = blend(a, b, t);
    }
    params.foggyness = fixed.foggy ? 1.0f : 0.0f;
    params.cloudCoverage = fixed.cloudy ? 1.0f : 0.0f;
    params.extraSunnyness = fixed.extraSunny ? 1.0f : 0.0f;
    params.wind = fixed.wind;
    out = params;
    return true;
}

bool RealtimeEnvironment::Upload(char* err, std::size_t errSize) {
    assert(m_Loaded);
    ReleaseGpu();
    // Vertex-only GLSL keeps compatibility texture/alpha/fog processing.
    // Formula follows librw src/gl/shaders/default.vert: ADD prelight and
    // surface-scaled lighting, clamp, THEN modulate material and texture.
    const char* source = R"GLSL(#version 120
uniform float nightBalance;
uniform float objects;
void main() {
    vec4 eye = gl_ModelViewMatrix * gl_Vertex;
    gl_Position = gl_ProjectionMatrix * eye;
    vec4 prelit = mix(gl_MultiTexCoord1, gl_MultiTexCoord2, nightBalance);
    vec2 surface = gl_MultiTexCoord3.xy;
    vec3 normal = normalize(gl_NormalMatrix * gl_Normal);
    float lambert = max(dot(normal, normalize(gl_LightSource[0].position.xyz)), 0.0);
    vec3 light = gl_LightModel.ambient.rgb * surface.x
               + objects * gl_LightSource[0].diffuse.rgb * surface.y * lambert;
    gl_FrontColor = vec4(clamp(prelit.rgb + light, 0.0, 1.0), prelit.a) * gl_Color;
    gl_BackColor = gl_FrontColor;
    gl_TexCoord[0] = gl_MultiTexCoord0;
    gl_FogFragCoord = abs(eye.z);
}
)GLSL";
    const GLuint shader = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint success = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        if (err && errSize) {
            glGetShaderInfoLog(shader, static_cast<GLsizei>(errSize), nullptr, err);
        }
        glDeleteShader(shader);
        return false;
    }
    m_LightingProgram = glCreateProgram();
    glAttachShader(m_LightingProgram, shader);
    glLinkProgram(m_LightingProgram);
    glDeleteShader(shader);
    glGetProgramiv(m_LightingProgram, GL_LINK_STATUS, &success);
    if (!success) {
        if (err && errSize) {
            glGetProgramInfoLog(m_LightingProgram, static_cast<GLsizei>(errSize), nullptr, err);
        }
        ReleaseGpu();
        return false;
    }
    glPushAttrib(GL_TEXTURE_BIT | GL_PIXEL_MODE_BIT);
    glPushClientAttrib(GL_CLIENT_PIXEL_STORE_BIT);
    for (GLenum scale : {GL_RED_SCALE, GL_GREEN_SCALE, GL_BLUE_SCALE, GL_ALPHA_SCALE}) glPixelTransferf(scale, 1);
    for (GLenum bias : {GL_RED_BIAS, GL_GREEN_BIAS, GL_BLUE_BIAS, GL_ALPHA_BIAS}) glPixelTransferf(bias, 0);
    glPixelTransferi(GL_MAP_COLOR, GL_FALSE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    GLint unpackBuffer = 0;
    glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &unpackBuffer);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    const auto upload = [](GLuint& texture, const WorldShotImage& image) {
        glGenTextures(1, &texture);
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, image.w, image.h, 0, GL_RGBA, GL_UNSIGNED_BYTE, image.rgba.data());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    };
    upload(m_WaterTexture, m_WaterImage);
    upload(m_SeaBedTexture, m_SeaBedImage);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, static_cast<GLuint>(unpackBuffer));
    glPopClientAttrib();
    glPopAttrib();
    const GLenum error = glGetError();
    if (error != GL_NO_ERROR) {
        ReleaseGpu();
        char message[128];
        std::snprintf(message, sizeof(message), "environment water upload GL=0x%x", error);
        return Fail(err, errSize, message);
    }
    return true;
}

void RealtimeEnvironment::ReleaseGpu() {
    assert(!m_LightingActive);
    if (m_LightingProgram) {
        glDeleteProgram(m_LightingProgram);
        m_LightingProgram = 0;
    }
    if (m_WaterTexture) {
        glDeleteTextures(1, &m_WaterTexture);
        m_WaterTexture = 0;
    }
    if (m_SeaBedTexture) {
        glDeleteTextures(1, &m_SeaBedTexture);
        m_SeaBedTexture = 0;
    }
}

bool RealtimeEnvironment::WaterQueryHeightAllowed(uint32_t flags, float height, float z) {
    assert(std::isfinite(height) && std::isfinite(z));
    // Retail quad 720456/720559, triangle 7207A1/7208BA. The limited-depth
    // bit is bit 2 in CWaterPolygon, bit 1 in water.dat (AddWaterLevelQuad).
    return !((flags & 2) && double(height) - 6.0 > double(z)) && double(height) + 20.0 >= double(z);
}

bool RealtimeEnvironment::BuildSeaBed(std::span<const RealtimeWaterBlock> blocks,
    float cameraX, float cameraY, int area, RealtimeSeaBedGeometry& out) {
    if (blocks.size() > 70 || !std::isfinite(cameraX) || !std::isfinite(cameraY)) return false;
    out.size = 0;
    if (area != 0 && area != 5) return true; // CGame::CanSeeWater
    for (const auto block : blocks) {
        // RenderWater retail 728B89..728D2A. Detail compares distance to the
        // WHOLE block centre, even when only its 20m border strip is emitted.
        const float dx = cameraX - float((double(block.x) + .5) * 500.0 - 3000.0);
        const float dy = cameraY - float((double(block.y) + .5) * 500.0 - 3000.0);
        const bool detailed = std::sqrt(float(double(dx) * dx + double(dy) * dy)) < 600.0f;
        const auto segment = [&](float x0, float x1, float y0, float y1) {
            // RenderSeaBedSegment 720EF0; RenderDetailedSeaBedSegment 7210A0.
            // Fractional block bounds, NOT metres. Detailed step count is *2,
            // not the water surface's 2m grid. Float stores match the x87 path.
            const int nx = detailed ? std::max(1, int((double(x1) - x0) * 2.0)) : 1;
            const int ny = detailed ? std::max(1, int((double(y1) - y0) * 2.0)) : 1;
            const auto at = [](float a, float b, int i, int n) {
                return float(double(i) * (double(b) - a) / double(n) + double(a));
            };
            for (int x = 0; x < nx; ++x) for (int y = 0; y < ny; ++y) {
                const float xs[]{at(x0,x1,x,nx), at(x0,x1,x+1,nx)};
                const float ys[]{at(y0,y1,y,ny), at(y0,y1,y+1,ny)};
                for (int corner = 0; corner < 4; ++corner) {
                    const float u = xs[corner / 2], v = ys[corner % 2];
                    assert(out.size < out.vertices.size());
                    out.vertices[out.size++] = {
                        float((double(block.x) + u) * 500.0 - 3000.0),
                        float((double(block.y) + v) * 500.0 - 3000.0), -70.f,
                        float(double(u) * 8.0), float(double(v) * 8.0)};
                }
            }
        };
        if (block.x < 0 || block.x >= 12 || block.y < 0 || block.y >= 12) {
            segment(0,1,0,1);
        } else {
            // Corner blocks draw TWO FULL strips; their 20x20 overlap is in the
            // source too. Do not shrink the second strip to "repair" overlap.
            if (block.x == 0) segment(0,.04f,0,1);
            else if (block.x == 11) segment(.96f,1,0,1);
            if (block.y == 0) segment(0,1,0,.04f);
            else if (block.y == 11) segment(0,1,.96f,1);
        }
    }
    return true;
}

void RealtimeEnvironment::DrawSeaBed(const RealtimeSeaBedGeometry& geometry) const {
    assert(m_Loaded && m_SeaBedTexture);
    assert(geometry.size <= geometry.vertices.size() && geometry.size % 4 == 0);
    if (!geometry.size) return;
    GLint program = 0, active = 0, mode = 0, units = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &program);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
    glGetIntegerv(GL_MATRIX_MODE, &mode);
    glGetIntegerv(GL_MAX_TEXTURE_UNITS, &units);
    glPushAttrib(GL_ALL_ATTRIB_BITS);
    glUseProgram(0);
    for (int unit = 0; unit < units; ++unit) {
        glActiveTexture(GL_TEXTURE0 + unit);
        for (GLenum target : {GL_TEXTURE_1D, GL_TEXTURE_2D, GL_TEXTURE_3D, GL_TEXTURE_CUBE_MAP}) glDisable(target);
    }
    glActiveTexture(GL_TEXTURE0);
    glMatrixMode(GL_TEXTURE); glPushMatrix(); glLoadIdentity();
    for (GLenum coord : {GL_TEXTURE_GEN_S, GL_TEXTURE_GEN_T, GL_TEXTURE_GEN_R, GL_TEXTURE_GEN_Q}) glDisable(coord);
    glEnable(GL_TEXTURE_2D); glBindTexture(GL_TEXTURE_2D, m_SeaBedTexture);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    glDisable(GL_LIGHTING); glDisable(GL_CULL_FACE);
    glEnable(GL_DEPTH_TEST); glDepthFunc(GL_LEQUAL); glDepthMask(GL_TRUE);
    glEnable(GL_BLEND); glBlendEquation(GL_FUNC_ADD); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_ALPHA_TEST); glAlphaFunc(GL_GREATER, 0); // DefinedState + RenderWater ref=0
    glShadeModel(GL_SMOOTH);
    ApplyFog();
    glColor4ub(80,80,80,255);
    glBegin(GL_TRIANGLES);
    for (size_t base = 0; base < geometry.size; base += 4) for (int corner : {0,1,2,3,1,2}) {
        const auto& v = geometry.vertices[base + corner];
        glTexCoord2f(v.u, v.v); glVertex3f(v.x, v.y, v.z);
    }
    glEnd();
    glPopMatrix(); glMatrixMode(mode);
    glPopAttrib(); glActiveTexture(active); glUseProgram(program);
}

bool RealtimeEnvironment::CaptureWaterScanPoints(float cameraX, float cameraY, RealtimeWaterScanPoints& points) {
    // CCamera::GetFrustumPoints -> ScanThroughBlocks: far-plane corners then
    // camera origin. GL's camera looks along -Z instead of RW's +Z.
    GLfloat p[16], m[16];
    glGetFloatv(GL_PROJECTION_MATRIX, p); glGetFloatv(GL_MODELVIEW_MATRIX, m);
    if (p[0] <= 0 || p[5] <= 0 || p[11] != -1 || p[15] != 0 || p[8] != 0 || p[9] != 0) return false;
    const float far = float(double(p[14]) / (double(p[10]) + 1));
    if (!std::isfinite(far) || far <= 0 || !std::isfinite(cameraX) || !std::isfinite(cameraY)) return false;
    // Native bridge retains the installed projection distance (including its
    // float readback), not timecyc farClip. Retail 721395 loads RW farPlane;
    // viewWindow is a separate float, multiplied by far at 7213AD..7213F8.
    return BuildWaterScanPoints({far,{1.f/p[0],1.f/p[5]},
        {m[0],m[4]},{m[1],m[5]},{-m[2],-m[6]},{cameraX,cameraY}},points);
}

bool RealtimeEnvironment::BuildWaterScanPoints(const RealtimeWaterFrustum& camera, RealtimeWaterScanPoints& points) {
    if (!std::isfinite(camera.farClip) || camera.farClip<=0 || camera.viewWindow[0]<=0 || camera.viewWindow[1]<=0) return false;
    for (const auto v : {camera.viewWindow,camera.right,camera.up,camera.at,camera.position})
        for (float f : v) if (!std::isfinite(f)) return false;
    RealtimeWaterScanPoints out{};
    for (size_t i = 0; i < 4; ++i) {
        const float x = (i == 0 || i == 3 ? -camera.viewWindow[0] : camera.viewWindow[0])*camera.farClip;
        const float y = (i < 2 ? camera.viewWindow[1] : -camera.viewWindow[1])*camera.farClip;
        // RW's two SSE transforms 8418A0/841C70 have identical arithmetic:
        // (z*at + y*up) + (x*right + position), MULPS/ADDPS per operation.
        // Inverse rigid GL view supplies right/up/-at as its first three rows.
        for (size_t axis=0;axis<2;++axis) {
            // Force the source rounding points even on an FMA-capable build.
            const volatile float zAt=camera.farClip*camera.at[axis],yUp=y*camera.up[axis],xRight=x*camera.right[axis];
            const volatile float vertical=zAt+yUp,horizontal=xRight+camera.position[axis];
            const float world=vertical+horizontal;
            out[i][axis]=world/500.f+6.f; // PC24 DIV then ADD, retail 721419..721491
        }
    }
    out[4] = {camera.position[0]/500.f+6.f,camera.position[1]/500.f+6.f};
    for (const auto& point : out) for (float v : point)
        if (!std::isfinite(v) || v < -32767 || v > 32766) return false;
    points=out;
    return true;
}

bool RealtimeEnvironment::ScanOutsideWaterBlocks(float cameraX, float cameraY,
    std::array<RealtimeWaterBlock,70>& blocks, size_t& count) {
    count = 0;
    RealtimeWaterScanPoints points;
    return CaptureWaterScanPoints(cameraX,cameraY,points) && ScanWaterBlocks(points,blocks,count);
}

bool RealtimeEnvironment::ScanWaterBlocks(RealtimeWaterScanPoints points,
    std::array<RealtimeWaterBlock,70>& blocks, size_t& count) {
    for (const auto& p : points) for (float v : p)
        if (!std::isfinite(v) || v < -32767 || v > 32766) return false;
    count = 0;
    // Empty-extra-list water domain: retail D0D270 starts in zero-filled BSS.
    // Its only increment is in SetExtraRectangleToScan (75FD80), which has no
    // direct call or absolute address reference in the owned retail .text.
    // Other references are ScanWorld's removal/reset. ScanThroughBlocks never
    // installs an extra rectangle. This is not a pending-extra general scanner.
    // Retail 75F1B8: stable exact duplicate removal, including signed zero.
    size_t n = points.size();
    for (size_t i=0;i+1<n;++i) for (size_t j=i+1;j<n;++j) if (points[i]==points[j]) {
        std::move(points.begin()+j+1,points.begin()+n,points.begin()+j); --n; --j;
    }
    // A non-collapsed perspective frustum projects to an area in XY. Reject
    // precision-collapsed input rather than enter the original undefined hull.
    if (n<3) return false;
    const auto angle=[](float x,float y) {
        // CGeneral::GetATanOfXY (retail 54CBD0). Ratio and atan result each
        // store to float before the quadrant addition/subtraction. Do not
        // substitute atan2(y,x): rounded angle ties decide hull membership.
        const float ax=std::abs(x), ay=std::abs(y);
        const bool steep=ax<ay;
        const float ratio=steep ? ax/ay : ay/ax;
        const float a=float(std::atan(double(ratio)));
        constexpr double pi=3.1415927410125732421875, half=1.57079637050628662109375;
        constexpr double threeHalf=4.7123889923095703125, tau=6.283185482025146484375;
        if (steep) return float(y>0 ? (x>0 ? half-a : half+a) : (x>0 ? threeHalf+a : threeHalf-a));
        return float(y>0 ? (x>0 ? double(a) : pi-a) : (x>0 ? tau-a : pi+a));
    };
    std::array<std::array<float,2>,5> hull{};
    std::array<bool,5> used{};
    size_t at=0, length=1;
    for (size_t i=1;i<n;++i) if (points[i][1]<points[at][1]) at=i;
    hull[0]=points[at]; used[at]=true;
    float direction=0;
    while (true) {
        float best=99999.8984375f; size_t chosen=0;
        for (size_t i=0;i<n;++i) if (i!=at) {
            float turn=float(angle(points[i][0]-points[at][0],points[i][1]-points[at][1])-direction);
            constexpr double tau=6.283185482025146484375;
            while (turn<=0) turn=float(double(turn)+tau);
            while (turn>=tau) turn=float(double(turn)-tau);
            if (turn<best) { best=turn; chosen=i; } // STRICT: first equal-angle candidate wins
        }
        if (used[chosen]) break; // source stops on ANY visited point
        hull[length++]=points[chosen]; used[chosen]=true; at=chosen;
        direction=float(direction+best);
    }
    if (length<3) return true; // source emits nothing for a collapsed hull
    const auto floor=[](float v) { return int(std::floor(double(v))); };
    size_t start=0;
    float ymax=hull[0][1];
    for (size_t i=1;i<length;++i) {
        if (hull[i][1]<hull[start][1]) start=i;
        else if (hull[i][1]>=ymax) ymax=hull[i][1];
    }
    int row=floor(hull[start][1]); const int last=floor(ymax);
    struct Edge { size_t next; float step{}, x{}; int bound; bool right; };
    Edge left{start,0,0,9999,false},right{start,0,0,-9999,true};
    const auto advance=[&](size_t i,bool forward) { return forward ? (i+1)%length : (i+length-1)%length; };
    const auto extend=[](Edge& e,int x) { e.bound=e.right ? std::max(e.bound,x) : std::min(e.bound,x); };
    const auto setLine=[&](Edge& e,size_t from) {
        const auto a=hull[from],b=hull[e.next];
        // RW device-open 82B138 and camera-begin 82D777 call 89F404/89F432:
        // clear CW precision bits 0x300 -> 24-bit x87 arithmetic. Each FPU
        // operation rounds, not just FSTP. Double operands do not mean PC53.
        e.step=float(b[0]-a[0])/float(b[1]-a[1]);
        const float fraction=float(std::ceil(a[1])-a[1]);
        const volatile float product=fraction*e.step;
        e.x=product+a[0];
    };
    const auto outward=[](const Edge& e) { return e.right ? e.step>=0 : e.step<0; };
    for (Edge* e : {&left,&right}) {
        size_t from=start;
        for (size_t i=0;i<length;++i) {
            from=e->next; e->next=advance(from,e->right);
            extend(*e,floor(hull[from][0]));
            if (floor(hull[from][1])!=floor(hull[e->next][1])) break;
        }
        if (row!=last) { setLine(*e,from); if (outward(*e)) extend(*e,floor(e->x)); }
    }
    // Retail 75F830 callback order is Y ascending, X ascending, inclusive.
    // BlockHit 721310 stores the first 70 edge/outside cells in signed shorts.
    for (;;) {
        for (int x=left.bound;x<=right.bound;++x) if (x<=0 || x>=11 || row<=0 || row>=11) {
            blocks[count++]={int16_t(x),int16_t(row)};
            if (count==blocks.size()) return true;
        }
        if (row==last) return true;
        ++row;
        for (Edge* e : {&left,&right}) {
            e->x=float(e->x+e->step); // original accumulated float store, not direct edge evaluation
            if (row!=floor(hull[e->next][1])) {
                e->bound=floor(outward(*e) ? e->x : float(e->x-e->step));
            } else if (row==last) {
                if (!outward(*e)) e->bound=floor(float(e->x-e->step));
                else do { // final row follows the top plateau by floored X, NOT Y
                    e->bound=floor(hull[e->next][0]); e->next=advance(e->next,e->right);
                } while (e->right ? e->bound<floor(hull[e->next][0]) : e->bound>floor(hull[e->next][0]));
            } else {
                e->bound=floor(outward(*e) ? hull[e->next][0] : float(e->x-e->step));
                size_t from;
                do { from=e->next; e->next=advance(from,e->right); extend(*e,floor(hull[from][0])); }
                while (row==floor(hull[e->next][1]));
                setLine(*e,from);
                if (outward(*e)) extend(*e,floor(e->x));
            }
        }
    }
}

bool RealtimeEnvironment::DrawSeaBed(float cameraX, float cameraY, int area) const {
    if (area != 0 && area != 5) return true;
    std::array<RealtimeWaterBlock,70> blocks{};
    size_t count = 0;
    if (!ScanOutsideWaterBlocks(cameraX,cameraY,blocks,count)) return false;
    RealtimeSeaBedGeometry geometry;
    if (!BuildSeaBed(std::span{blocks.data(),count}, cameraX, cameraY, area, geometry)) return false;
    DrawSeaBed(geometry);
    return true;
}

void RealtimeEnvironment::ApplyFog() const {
    const GLfloat fog[]{m_Params.skyBottom[0], m_Params.skyBottom[1], m_Params.skyBottom[2], 1.0f};
    glEnable(GL_FOG);
    glFogi(GL_FOG_MODE, GL_LINEAR);
    glFogfv(GL_FOG_COLOR, fog);
    glFogf(GL_FOG_START, m_Params.fogStart);
    glFogf(GL_FOG_END, m_Params.farClip);
    glHint(GL_FOG_HINT, GL_NICEST);
}

void RealtimeEnvironment::BeginWorld() const {
    BeginLighting(false);
}

void RealtimeEnvironment::BeginObjects() const {
    BeginLighting(true);
}

void RealtimeEnvironment::BeginLighting(bool objects) const {
    assert(m_Loaded && m_LightingProgram && !m_LightingActive);
    m_LightingActive = true;
    glPushAttrib(GL_ALL_ATTRIB_BITS);
    glGetIntegerv(GL_CURRENT_PROGRAM, &m_PreviousProgram);
    glUseProgram(m_LightingProgram);
    glUniform1f(glGetUniformLocation(m_LightingProgram, "nightBalance"), m_Params.nightBalance);
    glUniform1f(glGetUniformLocation(m_LightingProgram, "objects"), objects ? 1.0f : 0.0f);
    // Safe defaults for clients without authored vertex metadata.
    glMultiTexCoord4f(GL_TEXTURE1, 0, 0, 0, 1);
    glMultiTexCoord4f(GL_TEXTURE2, 0, 0, 0, 1);
    glMultiTexCoord2f(GL_TEXTURE3, 1, 1);
    ApplyFog();
    glEnable(GL_LIGHTING);
    for (GLenum light = GL_LIGHT1; light <= GL_LIGHT7; ++light) {
        glDisable(light);
    }
    glEnable(GL_LIGHT0);
    glEnable(GL_NORMALIZE);
    glEnable(GL_COLOR_MATERIAL);
    glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);
    glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, GL_FALSE);
    const auto& amb = objects ? m_Params.ambientObjects : m_Params.ambient;
    const GLfloat ambient[]{amb[0], amb[1], amb[2], 1.0f};
    const GLfloat diffuse[]{m_Params.directional[0], m_Params.directional[1], m_Params.directional[2], 1.0f};
    const GLfloat direction[]{m_Params.sunDirection[0], m_Params.sunDirection[1], m_Params.sunDirection[2], 0.0f};
    const GLfloat black[]{0.0f, 0.0f, 0.0f, 1.0f};
    glLightModelfv(GL_LIGHT_MODEL_AMBIENT, ambient);
    glLightfv(GL_LIGHT0, GL_AMBIENT, black);
    glLightfv(GL_LIGHT0, GL_DIFFUSE, diffuse);
    glLightfv(GL_LIGHT0, GL_SPECULAR, black);
    // GL transforms a w=0 direction by the current camera view (no translation).
    glLightfv(GL_LIGHT0, GL_POSITION, direction);
    glMaterialfv(GL_FRONT_AND_BACK, GL_EMISSION, black);
    glMaterialfv(GL_FRONT_AND_BACK, GL_SPECULAR, black);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
}

void RealtimeEnvironment::EndWorld() const {
    assert(m_LightingActive);
    glUseProgram(static_cast<GLuint>(m_PreviousProgram));
    glPopAttrib();
    m_LightingActive = false;
}

void RealtimeEnvironment::DrawSky(float cameraX, float cameraY, float cameraZ) const {
    assert(m_Loaded);
    GLint program = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &program);
    glUseProgram(0);
    glPushAttrib(GL_ALL_ATTRIB_BITS);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_LIGHTING);
    glDisable(GL_FOG);
    glDisable(GL_ALPHA_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glShadeModel(GL_SMOOTH);
    GLint matrixMode = GL_MODELVIEW;
    glGetIntegerv(GL_MATRIX_MODE, &matrixMode);
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glTranslatef(cameraX, cameraY, cameraZ);
    // Camera-centered sky shell: world up keeps the horizon stable while the
    // camera pitches. Lower hemisphere is SkyBot, upper grades to SkyTop.
    // Geometry is a rendering envelope; all sky colors are timecyc values.
    const float radius = m_Params.farClip * 0.9f;
    constexpr int bands = 16, slices = 64;
    for (int band = 0; band < bands; ++band) {
        glBegin(GL_QUAD_STRIP);
        for (int slice = 0; slice <= slices; ++slice) {
            const float azimuth = 2.0f * kPi * slice / slices;
            for (int edge = 0; edge < 2; ++edge) {
                const float elevation = -kPi * 0.5f + kPi * (band + edge) / bands;
                const float z = std::sin(elevation);
                const float xy = std::cos(elevation);
                const float t = std::max(0.0f, z);
                glColor3f(std::lerp(m_Params.skyBottom[0], m_Params.skyTop[0], t),
                          std::lerp(m_Params.skyBottom[1], m_Params.skyTop[1], t),
                          std::lerp(m_Params.skyBottom[2], m_Params.skyTop[2], t));
                glVertex3f(radius * xy * std::cos(azimuth), radius * xy * std::sin(azimuth), radius * z);
            }
        }
        glEnd();
    }
    glPopMatrix();
    glMatrixMode(matrixMode);
    glPopAttrib();
    glUseProgram(static_cast<GLuint>(program));
}

void RealtimeEnvironment::DrawWater() const {
    // Compatibility caller: inverse rigid view translation, no hidden clock.
    GLfloat view[16];
    glGetFloatv(GL_MODELVIEW_MATRIX, view);
    DrawWater(-(view[0] * view[12] + view[1] * view[13] + view[2] * view[14]),
              -(view[4] * view[12] + view[5] * view[13] + view[6] * view[14]));
}

bool RealtimeEnvironment::SetWaterState(const RealtimeWaterState& state) {
    if (!std::isfinite(state.wavyness) || state.wavyness < 0 || state.wavyness > 1 ||
        !std::isfinite(state.sunGlare) || state.sunGlare < 0 || state.sunGlare > 1) return false;
    if (!std::isfinite(state.flowTimeStep) || state.flowTimeStep < 0 || state.flowTimeStep > 3) return false;
    for (const auto& uv : {state.firstFlowUV, state.secondFlowUV, state.currentFlow})
        for (float v : uv) if (!std::isfinite(v)) return false;
    const auto previous = m_WaterState;
    m_WaterState = state;
    if (state.accumulateFlow && previous.accumulateFlow) {
        m_WaterState.firstFlowUV = previous.firstFlowUV;
        m_WaterState.secondFlowUV = previous.secondFlowUV;
        if (state.gameMs != previous.gameMs) {
            AdvanceFlowUV(m_WaterState);
        }
    }
    return true;
}

void RealtimeWaterFlow::Initialise(const WaterLevelData& data) {
    m_Vertices.clear();
    m_Quads.clear();
    m_Selection = {};
    m_HasTick = false;
    for (size_t p = 0; p < data.polys.size(); ++p) {
        const auto& poly = data.polys[p];
        assert(poly.nverts == 3 || poly.nverts == 4);
        auto vertices = std::to_array(poly.v);
        for (int i = 0; i < poly.nverts; ++i) {
            auto& v = vertices[i];
            // LoadTextures converts XY to int32 and flow to signed 1/64 bytes.
            assert(std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z));
            v.x = std::trunc(v.x);
            v.y = std::trunc(v.y);
        }
        const auto end = vertices.begin() + poly.nverts;
        if (std::all_of(vertices.begin(), end, [&](const auto& v) { return v.x == vertices[0].x; }) ||
            std::all_of(vertices.begin(), end, [&](const auto& v) { return v.y == vertices[0].y; })) continue;
        Quad quad{{}, static_cast<int>(p)};
        for (int i = 0; i < poly.nverts; ++i) {
            auto v = vertices[i];
            // AddWaterLevelVertex 6E5A40: clamp then clear ALL RenPar fields,
            // share by XYZ only, and let the first authored vertex own metadata.
            if (v.x < -3000 || v.x > 3000 || v.y < -3000 || v.y > 3000) {
                v = {std::clamp(v.x, -3000.0f, 3000.0f), std::clamp(v.y, -3000.0f, 3000.0f), 0};
            } else {
                assert(std::isfinite(v.flowX) && std::isfinite(v.flowY));
                v.flowX = float(static_cast<int8_t>(int(v.flowX * 64.0f))) / 64.0f;
                v.flowY = float(static_cast<int8_t>(int(v.flowY * 64.0f))) / 64.0f;
            }
            const auto found = std::find_if(m_Vertices.begin(), m_Vertices.end(), [&](const auto& other) {
                return v.x == other.x && v.y == other.y && v.z == other.z;
            });
            quad.vertices[i] = static_cast<size_t>(found - m_Vertices.begin());
            if (found == m_Vertices.end()) m_Vertices.push_back(v);
        }
        if (poly.nverts != 4) continue;
        std::sort(quad.vertices.begin(), quad.vertices.end(), [&](auto a, auto b) {
            const auto& x = m_Vertices[a];
            const auto& y = m_Vertices[b];
            return x.y == y.y ? x.x < y.x : x.y < y.y;
        });
        m_Quads.push_back(quad);
    }
}

RealtimeWaterFlowSelection RealtimeWaterFlow::FindNearest(float x, float y,
    std::array<float, 2> previousDesired) const {
    assert(std::isfinite(x) && std::isfinite(y));
    RealtimeWaterFlowSelection out;
    out.desired = previousDesired;
    // FindNearestWaterAndItsFlow 6E9D70 (retail 724300..7247C9).
    // The boundary itself is OUTSIDE; no flags, interior, Z or player test.
    if (x <= -3000 || x >= 3000 || y <= -3000 || y >= 3000) {
        out.result = RealtimeWaterFlowSelection::Result::OutsideWorld;
        out.desired = {};
        out.nearestWavyDistance = 0;
        return out;
    }
    float best = 10000000.0f;
    for (const auto& q : m_Quads) {
        const auto& lo = m_Vertices[q.vertices[0]];
        const auto& right = m_Vertices[q.vertices[1]];
        const auto& top = m_Vertices[q.vertices[2]];
        const float dx = std::max({lo.x - x, x - right.x, 0.0f});
        const float dy = std::max({lo.y - y, y - top.y, 0.0f});
        const float distance = std::sqrt(float(double(dx) * dx + double(dy) * dy));
        if (distance < out.nearestWavyDistance && std::any_of(q.vertices.begin(), q.vertices.end(), [&](auto i) {
            return m_Vertices[i].bigWaves != 0 || m_Vertices[i].smallWaves != 0;
        })) {
            out.nearestWavyDistance = distance;
            out.nearestWavyHeight = lo.z; // first vertex, NOT interpolated level
        }
        if (distance >= best) continue; // first quad wins equal AABB distances
        best = distance;
        float nearest = INFINITY;
        for (int i = 0; i < 4; ++i) {
            const auto& v = m_Vertices[q.vertices[i]];
            const double vx = double(v.x) - x, vy = double(v.y) - y;
            const float squared = float(vx * vx + vy * vy); // four x87 float stores before comparisons
            if (squared > nearest) continue; // last corner wins equal distances
            nearest = squared;
            out.desired = {v.flowX, v.flowY};
            out.corner = i;
        }
        out.polygon = q.polygon;
        out.result = RealtimeWaterFlowSelection::Result::SelectedQuad;
    }
    return out; // no quads inside world: retain desired, distance=1e7, height=0
}

bool RealtimeWaterFlow::Advance(const RealtimeWaterFlowTick& tick, RealtimeWaterState& water) {
    if (water.accumulateFlow || !std::isfinite(tick.cameraX) || !std::isfinite(tick.cameraY) ||
        !std::isfinite(tick.timeStep) || tick.timeStep < 0 || tick.timeStep > 3) return false;
    for (const auto& pair : {water.currentFlow, water.firstFlowUV, water.secondFlowUV})
        for (float v : pair) if (!std::isfinite(v)) return false;
    if (m_HasTick) {
        if (tick.frame == m_LastTick.frame) return tick == m_LastTick;
        if (tick.frame != m_LastTick.frame + uint32_t{1}) return false;
    }
    m_HasTick = true;
    m_LastTick = tick;
    if (tick.suspended || !tick.canSeeWater) return true;
    // PreRenderWater -> UpdateFlow; select only at the original 29/32 phase.
    if (tick.frame % 32 == 29) m_Selection = FindNearest(tick.cameraX, tick.cameraY, m_Selection.desired);
    const float step = tick.timeStep / 1000.0f;
    for (int axis = 0; axis < 2; ++axis) {
        const float delta = m_Selection.desired[axis] - water.currentFlow[axis];
        water.currentFlow[axis] = std::abs(delta) < step ? m_Selection.desired[axis] :
            water.currentFlow[axis] + std::copysign(step, delta);
    }
    water.flowTimeStep = tick.timeStep;
    AdvanceFlowUV(water);
    return true;
}

bool RealtimeEnvironment::AdvanceWaterFlow(const RealtimeWaterFlowTick& tick) {
    return m_Loaded && m_WaterFlow.Advance(tick, m_WaterState);
}

RealtimeWaterSample RealtimeEnvironment::SampleWater(int x, int y, float z, float big, float small) const {
    x = std::abs(x);
    y = std::abs(y);
    RealtimeWaterSample out;
    out.z = z;
    const float mult = kWaveX[(x / 2) % 8] * kWaveY[(y / 2) % 8] * m_WaterState.wavyness;
    auto wave = [&](uint32_t period, float fx, float fy, float amplitude) {
        const float wx = 2 * kPi * fx, wy = 2 * kPi * fy;
        const auto step = (m_WaterState.gameMs - m_WaterState.waterTimeOffset) % period;
        const float phase = step * (2 * kPi / float(period)) + float(x) * wx + float(y) * wy;
        const float sine = SinFast(phase), cosine = SinFast(phase + kPi / 2);
        out.z += sine * mult * amplitude;
        const float derivative = period == 5000 ? -cosine * mult * amplitude * wx :
            cosine * mult * amplitude * (period == 3500 ? wx : kPi / 10);
        out.normal[0] += derivative;
        if (period != 3000) out.normal[1] += derivative;
    };
    wave(5000, 1.0f / 64, 1.0f / 64, 2 * big);
    wave(3500, 1.0f / 26, 1.0f / 52, small);
    wave(3000, 0, 1.0f / 20, .5f * small);
    // CVector::NormaliseAndMag multiplies by the reciprocal, not n / length.
    const float reciprocal = 1.0f / std::sqrt(out.normal[0] * out.normal[0] + out.normal[1] * out.normal[1] + 1);
    for (auto& n : out.normal) n *= reciprocal;
    const float glareLevel = (out.normal[0] + out.normal[1] + out.normal[2]) * .577f;
    out.colorMult = std::max(glareLevel, 0.0f) * .65f + .27f;
    out.glare = std::clamp(8 * glareLevel - 5, 0.0f, .99f) * m_WaterState.sunGlare;
    return out;
}

std::array<float, 2> RealtimeEnvironment::WaterTextureShift(int layer) const {
    assert(layer == 0 || layer == 1);
    // RenderWater retail 728E64..728F4A uses CRT sin/cos (double arguments),
    // unlike CalculateWavesOnlyForCoordinate's 256-entry LUT.
    if (layer == 0) {
        const double phase = (m_WaterState.gameMs & 4095) * .0015339808305725455;
        return {float(m_WaterState.firstFlowUV[0] + std::sin(phase) * double(.08f) * m_WaterState.wavyness),
                float(m_WaterState.firstFlowUV[1] + std::cos(phase) * double(.08f) * m_WaterState.wavyness)};
    }
    const double phase = (m_WaterState.gameMs & 8191) * .0007669904152862728;
    return {m_WaterState.secondFlowUV[0], float(m_WaterState.secondFlowUV[1] + std::cos(phase) * double(.04f) * double(.6f))};
}

void RealtimeEnvironment::DrawWater(float cameraX, float cameraY, bool interior) const {
    std::array<RealtimeWaterBlock,70> blocks{};
    size_t count = 0;
    if (!ScanOutsideWaterBlocks(cameraX,cameraY,blocks,count)) {
        GLfloat projection[16]; glGetFloatv(GL_PROJECTION_MATRIX,projection);
        assert(projection[11] == 0 && "invalid source perspective water camera");
        // Legacy orthographic probes have no source RW perspective frustum.
        // Explicit-list overload below can replay ocean blocks in those views.
    }
    DrawWater(cameraX,cameraY,interior,std::span{blocks.data(),count});
}

void RealtimeEnvironment::DrawWater(float cameraX, float cameraY, bool interior,
    std::span<const RealtimeWaterBlock> outsideBlocks) const {
    assert(outsideBlocks.size() <= 70);
    assert(m_Loaded && m_WaterTexture && std::isfinite(cameraX) && std::isfinite(cameraY));
    GLint program = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &program);
    glUseProgram(0);
    glPushAttrib(GL_ALL_ATTRIB_BITS);
    GLint activeTexture = 0, matrixMode = 0, units = 0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &activeTexture);
    glGetIntegerv(GL_MATRIX_MODE, &matrixMode);
    glGetIntegerv(GL_MAX_TEXTURE_UNITS, &units);
    for (int i = 0; i < units; ++i) {
        glActiveTexture(GL_TEXTURE0 + i);
        glDisable(GL_TEXTURE_1D);
        glDisable(GL_TEXTURE_2D);
        glDisable(GL_TEXTURE_3D);
        glDisable(GL_TEXTURE_CUBE_MAP);
    }
    glActiveTexture(GL_TEXTURE0);
    glMatrixMode(GL_TEXTURE);
    glPushMatrix();
    glLoadIdentity();
    glDisable(GL_TEXTURE_GEN_S);
    glDisable(GL_TEXTURE_GEN_T);
    glDisable(GL_TEXTURE_GEN_R);
    glDisable(GL_TEXTURE_GEN_Q);
    glBindTexture(GL_TEXTURE_2D, m_WaterTexture);
    glEnable(GL_TEXTURE_2D);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_FALSE);
    glDisable(GL_LIGHTING);
    glDisable(GL_ALPHA_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glShadeModel(GL_SMOOTH);
    ApplyFog();
    glFogi(GL_FOG_COORDINATE_SOURCE, GL_FRAGMENT_DEPTH);
    // UpdateCameraRange (0x6E9C80): round the 48-unit detail square out to 2m.
    const float minX = 2 * std::floor((cameraX - 48) / 2), maxX = 2 * std::ceil((cameraX + 48) / 2);
    const float minY = 2 * std::floor((cameraY - 48) / 2), maxY = 2 * std::ceil((cameraY + 48) / 2);
    std::array<uint8_t, 3> rgb{};
    for (size_t c = 0; c < 3; ++c) rgb[c] = static_cast<uint8_t>(m_Params.water[c] * 255);
    // RenderWater 729063..729097: layer coverage composes to timecyc alpha.
    const int secondAlpha = static_cast<int>(m_Params.water[3] * 255 * .5f);
    const std::array<int, 2> alpha{std::min(255, (secondAlpha << 8) / (256 - secondAlpha)), secondAlpha};
    const std::array<std::array<float, 2>, 2> shifts{WaterTextureShift(0), WaterTextureShift(1)};
    auto color = [&](float mult, int layer) {
        glColor4ub(static_cast<uint8_t>(rgb[0] * mult), static_cast<uint8_t>(rgb[1] * mult),
                   static_cast<uint8_t>(rgb[2] * mult), static_cast<uint8_t>(alpha[layer]));
    };
    auto lerp = [](WaterVert a, WaterVert b, float t) {
        // CRenPar lerp clears flow (only z/big/small are interpolated).
        return WaterVert{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t,
            0, 0, a.bigWaves + (b.bigWaves - a.bigWaves) * t, a.smallWaves + (b.smallWaves - a.smallWaves) * t};
    };
    // Rectangle corner order here is BL, BR, TL, TR, matching sorted file rows.
    auto rectangle = [&](auto&& self, std::array<WaterVert, 4> v) -> void {
        const float x0 = v[0].x, x1 = v[1].x, y0 = v[0].y, y1 = v[2].y;
        if (x1 < cameraX - m_Params.farClip || x0 > cameraX + m_Params.farClip ||
            y1 < cameraY - m_Params.farClip || y0 > cameraY + m_Params.farClip) return;
        const bool detailed = !(x0 >= maxX || x1 <= minX || y0 >= maxY || y1 <= minY);
        float sx = 0, sy = 0;
        bool splitX = false, splitY = false;
        if (detailed && (x0 < minX || x1 > maxX)) { sx = x0 < minX ? minX : maxX; splitX = true; }
        else if (detailed && (y0 < minY || y1 > maxY)) { sy = y0 < minY ? minY : maxY; splitY = true; }
        else if (!detailed && x1 - x0 > 168) { sx = float(int(x0 + x1) / 2); splitX = true; }
        else if (!detailed && y1 - y0 > 168) { sy = float(int(y0 + y1) / 2); splitY = true; }
        const int nx = std::max(1, int(x1 - x0) / 2), ny = std::max(1, int(y1 - y0) / 2);
        if (detailed && !splitX && !splitY && ((nx + 1) * (ny + 1) >= 2048 || nx * ny * 6 >= 4096)) {
            if (nx > ny) { sx = float(int((x0 + x1) / 4) * 2); splitX = true; }
            else { sy = float(int((y0 + y1) / 4) * 2); splitY = true; }
        }
        if (splitX) {
            const float t = (sx - x0) / (x1 - x0);
            auto a = lerp(v[0], v[1], t), b = lerp(v[2], v[3], t);
            a.x = b.x = sx;
            self(self, {v[0], a, v[2], b}); self(self, {a, v[1], b, v[3]});
            return;
        }
        if (splitY) {
            const float t = (sy - y0) / (y1 - y0);
            auto a = lerp(v[0], v[2], t), b = lerp(v[1], v[3], t);
            a.y = b.y = sy;
            self(self, {v[0], v[1], a, b}); self(self, {a, b, v[2], v[3]});
            return;
        }
        struct Vertex { float x, y; RealtimeWaterSample sample; };
        std::array<Vertex, 2048> grid;
        const int gx = detailed ? nx : 1, gy = detailed ? ny : 1;
        for (int y = 0; y <= gy; ++y) for (int x = 0; x <= gx; ++x) {
            const float tx = float(x) / gx, ty = float(y) / gy;
            WaterVert p;
            const auto& a = tx + ty <= 1 ? v[0] : v[3];
            const auto& b = tx + ty <= 1 ? v[1] : v[2];
            const auto& c = tx + ty <= 1 ? v[2] : v[1];
            const float u = tx + ty <= 1 ? tx : 1 - tx, w = tx + ty <= 1 ? ty : 1 - ty;
            p.z = a.z + (b.z - a.z) * u + (c.z - a.z) * w;
            p.bigWaves = a.bigWaves + (b.bigWaves - a.bigWaves) * u + (c.bigWaves - a.bigWaves) * w;
            p.smallWaves = a.smallWaves + (b.smallWaves - a.smallWaves) * u + (c.smallWaves - a.smallWaves) * w;
            auto& dst = grid[y * (gx + 1) + x];
            // Original 7237E9/723805 uses integer divisions and integer grid
            // accumulation. Lerp positions can round -2 to -1.999999 and then
            // select the wrong sine-table phase when converted back to int.
            dst.x = x0 + float(x * (int(x1 - x0) / gx));
            dst.y = y0 + float(y * (int(y1 - y0) / gy));
            const float r = std::hypot(dst.x - cameraX, dst.y - cameraY) / 48;
            const float attenuation = r > 1 ? 0 : r >= .5f ? (1 - r) * 2 : 1;
            dst.sample = detailed ? SampleWater(int(dst.x), int(dst.y), p.z, p.bigWaves * attenuation, p.smallWaves * attenuation) :
                RealtimeWaterSample{p.z, .577f, 0, {0, 0, 1}};
        }
        for (int layer = 0; layer < 2; ++layer) {
            // Flat source layers: 25,12.5. High-detail original uses 12.5,25.
            const float scale = detailed ? (layer ? .04f : .08f) : (layer ? .08f : .04f);
            const float size = layer ? 12.5f : 25.0f;
            const float u = (detailed ? x0 * scale : x0 / size) + shifts[layer][0];
            const float w = (detailed ? y0 * scale : y0 / size) + shifts[layer][1];
            const float baseU = u - std::floor(u) - (detailed ? 0 : 7);
            const float baseV = w - std::floor(w) - (detailed ? 0 : 7);
            auto vertex = [&](int index) {
                const auto& p = grid[index];
                color(p.sample.colorMult, layer);
                glNormal3fv(p.sample.normal.data());
                glTexCoord2f(baseU + (detailed ? (p.x - x0) * scale : (p.x - x0) / size),
                    baseV + (detailed ? (p.y - y0) * scale : (p.y - y0) / size));
                glVertex3f(p.x, p.y, p.sample.z);
            };
            glBegin(GL_TRIANGLES);
            for (int y = 0; y < gy; ++y) for (int x = 0; x < gx; ++x) {
                const int a = y * (gx + 1) + x, b = a + 1, c = a + gx + 1, d = c + 1;
                if (detailed) { vertex(a); vertex(b); vertex(c); vertex(b); vertex(d); vertex(c); }
                else { vertex(a); vertex(b); vertex(d); vertex(d); vertex(c); vertex(a); }
            }
            glEnd();
        }
    };
    // Original triangle routing (6EE240), splitters (6ECF00/6EE5A0), and
    // high-detail triangle (6EDDC0). Retail equivalents 727C30,7269C0,727FB0,
    // 7275E0/722DF0 were inspected read-only: the readable split helpers have
    // incorrect P12/P13 assignments and an orientation-losing Y simplification.
    auto triangle = [&](auto&& self, std::array<WaterVert, 3> v) -> void {
        const auto& a = v[0]; const auto& b = v[1]; const auto& c = v[2];
        const int x0 = int(a.x), x1 = int(b.x), y0 = int(a.y), y1 = int(c.y);
        assert(a.y == b.y && x1 > x0 && y0 != y1 && (a.x == c.x || b.x == c.x));
        const int lowY = std::min(y0, y1), highY = std::max(y0, y1);
        if (x1 < cameraX - m_Params.farClip || x0 > cameraX + m_Params.farClip ||
            highY < cameraY - m_Params.farClip || lowY > cameraY + m_Params.farClip) return;
        const bool detailed = !(x0 >= maxX || x1 <= minX || lowY >= maxY || highY <= minY);
        const int n = (x1 - x0) / 2;
        const bool capacitySplit = detailed && x0 >= minX && x1 <= maxX && lowY >= minY && highY <= maxY &&
            (3 * n * n >= 4096 || (n + 1) * (n + 2) / 2 >= 2048);
        const bool splitX = (detailed && (x0 < minX || x1 > maxX)) || (!detailed && x1 - x0 > 168) || capacitySplit;
        const bool splitY = detailed && !splitX && (lowY < minY || highY > maxY);
        if (splitX || splitY) {
            // Source CRenPar lerp: t is rounded to float, 1-t and the weighted
            // sums are held by x87. Interpolate metadata on the actual edges;
            // preserve the same shared intersection for every child polygon.
            auto mix = [](const WaterVert& p, const WaterVert& q, float t) {
                auto value = [t](float p, float q) { return float(double(p) * (1.0 - double(t)) + double(q) * double(t)); };
                return WaterVert{0, 0, value(p.z, q.z), 0, 0, value(p.bigWaves, q.bigWaves), value(p.smallWaves, q.smallWaves)};
            };
            auto at = [](int x, int y, WaterVert p) { p.x = float(x); p.y = float(y); return p; };
            const bool left = a.x == c.x;
            int xs, ys;
            WaterVert base, side, diagonal;
            if (splitY) {
                ys = int(lowY < minY ? minY : maxY);
                const int dy = ys - y0, height = y1 - y0;
                const float t = float(dy) / float(height);
                xs = left ? x1 + (x0 - x1) * dy / height : x0 + (x1 - x0) * dy / height;
                // Retail 727FB0 computes these directly, not via an X split.
                base = left ? mix(b, a, t) : mix(a, b, t);
                side = mix(left ? a : b, c, t);
                diagonal = mix(left ? b : a, c, t);
            } else {
                xs = detailed && x0 < minX ? int(minX) : detailed && x1 > maxX ? int(maxX) :
                    capacitySplit ? x0 + (n / 2) * 2 : (x0 + x1) / 2;
                const int dx = xs - x0, width = x1 - x0;
                const float t = float(dx) / float(width);
                ys = left ? y1 + (y0 - y1) * dx / width : y0 + (y1 - y0) * dx / width;
                base = mix(a, b, t);
                side = left ? mix(c, a, t) : mix(b, c, t);
                diagonal = left ? mix(c, b, t) : mix(a, c, t);
            }
            assert(xs > x0 && xs < x1 && ys > lowY && ys < highY);
            const auto basePoint = at(xs, y0, base), diagonalPoint = at(xs, ys, diagonal);
            const auto sidePoint = at(left ? x0 : x1, ys, side);
            std::array<WaterVert, 4> rect = left ? std::array{a, basePoint, sidePoint, diagonalPoint} :
                std::array{basePoint, b, diagonalPoint, sidePoint};
            if (ys < y0) { std::swap(rect[0], rect[2]); std::swap(rect[1], rect[3]); }
            rectangle(rectangle, rect);
            if (left) {
                self(self, {sidePoint, diagonalPoint, c});
                self(self, {basePoint, b, diagonalPoint});
            } else {
                self(self, {a, basePoint, diagonalPoint});
                self(self, {diagonalPoint, sidePoint, c});
            }
            return;
        }
        if (!detailed) {
            for (int layer = 0; layer < 2; ++layer) {
                const float size = layer ? 12.5f : 25.0f;
                const float u = a.x / size + shifts[layer][0], w = a.y / size + shifts[layer][1];
                color(.577f, layer); glNormal3f(0, 0, 1);
                glBegin(GL_TRIANGLES);
                for (const auto& p : v) {
                    glTexCoord2f((p.x - a.x) / size + u - std::floor(u) - 7,
                        (p.y - b.y) / size + w - std::floor(w) + (y1 <= y0 ? 7 : -7));
                    glVertex3f(p.x, p.y, p.z);
                }
                glEnd();
            }
            return;
        }
        // 722DF0: start at the right-angle corner; signed INTEGER grid steps,
        // and n+1,n,...,1 vertices in successive rows. Parameters use per-step
        // float deltas, rather than a rectangle's piecewise interpolation.
        assert(n > 0);
        const auto& origin = a.x == c.x ? a : b;
        const auto& across = a.x == c.x ? b : a;
        const int dx = (int(across.x) - int(origin.x)) / n, dy = (y1 - y0) / n;
        const float reciprocal = 1.0f / float(n);
        struct Vertex { int x, y; RealtimeWaterSample sample; };
        std::array<Vertex, 2048> grid{};
        int count = 0;
        auto parameter = [reciprocal](float p, float q, float r, int x, int y) {
            const float deltaX = (q - p) * reciprocal, deltaY = (r - p) * reciprocal;
            return float(double(x) * deltaX + double(p) + double(y) * deltaY);
        };
        for (int y = 0; y <= n; ++y) for (int x = 0; x <= n - y; ++x) {
            auto& p = grid[count++];
            p.x = int(origin.x) + x * dx; p.y = y0 + y * dy;
            const float radius = std::sqrt((float(p.x) - cameraX) * (float(p.x) - cameraX) +
                (float(p.y) - cameraY) * (float(p.y) - cameraY)) / 48;
            const float attenuation = radius > 1 ? 0 : radius >= .5f ? (1 - radius) * 2 : 1;
            p.sample = SampleWater(p.x, p.y, parameter(origin.z, across.z, c.z, x, y),
                parameter(origin.bigWaves, across.bigWaves, c.bigWaves, x, y) * attenuation,
                parameter(origin.smallWaves, across.smallWaves, c.smallWaves, x, y) * attenuation);
        }
        for (int layer = 0; layer < 2; ++layer) {
            const float scale = layer ? .04f : .08f;
            const float u = origin.x * scale + shifts[layer][0], w = origin.y * scale + shifts[layer][1];
            auto vertex = [&](int i) {
                const auto& p = grid[i];
                // Retail 7232A6/72330B explicitly overrides wave colorMult with
                // .577 for triangles; rectangles retain the wave-derived color.
                color(.577f, layer); glNormal3fv(p.sample.normal.data());
                glTexCoord2f(u - std::floor(u) + (p.x - int(origin.x)) * scale,
                    w - std::floor(w) + (p.y - y0) * scale);
                glVertex3f(float(p.x), float(p.y), p.sample.z);
            };
            glBegin(GL_TRIANGLES);
            int row = n + 1;
            for (int y = 1; y <= n; ++y) {
                for (int x = 1; x <= n - y; ++x) {
                    const int i = row + x;
                    vertex(i); vertex(i - 1); vertex(i + y - n - 3);
                    vertex(i); vertex(i + y - n - 2); vertex(i + y - n - 3);
                }
                const int end = row + n - y;
                vertex(end); vertex(end + y - n - 1); vertex(end + y - n - 2);
                row += n - y + 1;
            }
            glEnd();
        }
    };
    for (const auto& poly : m_Water.polys) {
        if (!poly.Visible() || (poly.v[0].z > 950) != interior) continue;
        std::array<WaterVert, 4> v;
        std::copy_n(poly.v, poly.nverts, v.begin());
        for (int i = 0; i < poly.nverts; ++i) {
            v[i].x = static_cast<int16_t>(v[i].x); v[i].y = static_cast<int16_t>(v[i].y);
        }
        if (poly.nverts == 4) {
            std::sort(v.begin(), v.end(), [](auto a, auto b) { return a.y == b.y ? a.x < b.x : a.y < b.y; });
            rectangle(rectangle, v);
        } else {
            // AddWaterLevelTriangle keeps its equal-Y pair first, ascending X.
            if (v[0].y == v[2].y) std::swap(v[1], v[2]);
            else if (v[1].y == v[2].y) std::swap(v[0], v[2]);
            if (v[0].x > v[1].x) std::swap(v[0], v[1]);
            triangle(triangle, {v[0], v[1], v[2]});
        }
    }
    // RenderWater retail 729446..7295BA: general ocean surface accompanies the
    // seabed OUTSIDE the 12x12 world only. Edge blocks 0/11 already have authored
    // water.dat. Source RenPar is exactly {z=0,big=1,small=0,flow=0}, all corners.
    for (const auto block : outsideBlocks) {
        if (block.x >= 0 && block.x < 12 && block.y >= 0 && block.y < 12) continue;
        const float x = float(int(block.x)*500-3000), y = float(int(block.y)*500-3000);
        rectangle(rectangle, std::array<WaterVert,4>{{{x,y,0,0,0,1,0}, {x+500,y,0,0,0,1,0},
            {x,y+500,0,0,0,1,0}, {x+500,y+500,0,0,0,1,0}}});
    }
    glMatrixMode(GL_TEXTURE);
    glPopMatrix();
    glMatrixMode(matrixMode);
    glActiveTexture(static_cast<GLenum>(activeTexture));
    glPopAttrib();
    glUseProgram(static_cast<GLuint>(program));
}
