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

static bool Fail(char* err, std::size_t errSize, const char* message) {
    if (err && errSize) {
        std::snprintf(err, errSize, "%s", message);
    }
    return false;
}

static bool LoadWaterTexture(WorldShotImage& image, char* err, std::size_t errSize) {
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
    assert(!m_WaterTexture && !m_LightingProgram && "ReleaseGpu before reloading environment");
    m_Loaded = false;
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
    if (!WaterLevel_Load(gameDir, "data/water.dat", m_Water, err, errSize)) {
        return false;
    }
    if (!LoadWaterTexture(m_WaterImage, err, errSize)) return false;
    m_WaterState = {};
    // OriginalWeatherConstants wind table, read-only retail 94D510; CWeather
    // Update 75EA21/75ECE2 clips wind then computes min(WindClipped+.3,1).
    constexpr const char* names[]{"EXTRASUNNY_LA", "SUNNY_LA", "EXTRASUNNY_SMOG_LA", "SUNNY_SMOG_LA", "CLOUDY_LA",
        "SUNNY_SF", "EXTRASUNNY_SF", "CLOUDY_SF", "RAINY_SF", "FOGGY_SF", "SUNNY_VEGAS", "EXTRASUNNY_VEGAS",
        "CLOUDY_VEGAS", "EXTRASUNNY_COUNTRYSIDE", "SUNNY_COUNTRYSIDE", "CLOUDY_COUNTRYSIDE", "RAINY_COUNTRYSIDE",
        "EXTRASUNNY_DESERT", "SUNNY_DESERT", "SANDSTORM_DESERT", "UNDERWATER", "EXTRACOLOURS_1", "EXTRACOLOURS_2"};
    constexpr float wind[]{0, .25f, 0, .2f, .7f, .25f, 0, .7f, 1, 0, .2f, 0, .4f, 0, .3f, .7f, 1, 0, .3f, 1.5f, 0, 0, 0};
    bool known = false;
    for (size_t i = 0; i < std::size(names); ++i) {
        if (std::strcmp(weather, names[i])) continue;
        m_WaterState.wavyness = std::min(std::min(wind[i], 1.0f) + .3f, 1.0f);
        known = true;
    }
    if (!known) return Fail(err, errSize, "water weather needs OriginalWeatherConstants mapping");
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
    glGenTextures(1, &m_WaterTexture);
    glBindTexture(GL_TEXTURE_2D, m_WaterTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, m_WaterImage.w, m_WaterImage.h, 0, GL_RGBA, GL_UNSIGNED_BYTE, m_WaterImage.rgba.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
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
            // RenderWater retail 728DA7..728E8E: x87 distance and scales, float
            // accumulator stores, then ONE subtraction iff >1 (not >=1, and
            // deliberately no negative wrap). The caller supplies the source
            // simulation time step; integer milliseconds cannot reconstruct it.
            for (int axis = 0; axis < 2; ++axis) {
                const double distance = double(state.flowTimeStep) * double(.04f) * double(state.currentFlow[axis]);
                auto advance = [distance](float& uv, float scale) {
                    uv = float(double(uv) + distance * double(scale));
                    if (uv > 1.0f) uv -= 1.0f;
                };
                advance(m_WaterState.firstFlowUV[axis], .08f);
                advance(m_WaterState.secondFlowUV[axis], .04f);
            }
        }
    }
    return true;
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
    glMatrixMode(GL_TEXTURE);
    glPopMatrix();
    glMatrixMode(matrixMode);
    glActiveTexture(static_cast<GLenum>(activeTexture));
    glPopAttrib();
    glUseProgram(static_cast<GLuint>(program));
}
