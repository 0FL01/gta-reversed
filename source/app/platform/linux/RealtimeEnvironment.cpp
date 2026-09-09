#include "app/platform/linux/RealtimeEnvironment.h"

#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>

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
} // namespace

RealtimeEnvironment::~RealtimeEnvironment() {
    ReleaseGpu();
}

bool RealtimeEnvironment::Load(const char* gameDir, char* err, std::size_t errSize,
                               float hour, const char* weather) {
    assert(!m_WaterList && !m_LightingProgram && "ReleaseGpu before reloading environment");
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
    WorldShotScene scene{};
    // Reuse the proven visibility/quad ordering; its CPU shade compensation
    // colors must NOT go into GL. Only file-derived vertex positions are used.
    if (!WaterLevel_BuildScene(m_Water, m_Samples[0].water, scene, m_WaterTriangles, err, errSize)) {
        ReleaseGpu();
        return false;
    }
    m_WaterList = glGenLists(1);
    if (!m_WaterList) {
        ReleaseGpu();
        return Fail(err, errSize, "environment water GL list allocation failed");
    }
    glNewList(m_WaterList, GL_COMPILE);
    glBegin(GL_TRIANGLES);
    for (const auto& mesh : scene.meshes) {
        assert(mesh.pos.size() == static_cast<std::size_t>(mesh.tris) * 9);
        for (std::size_t v = 0; v < mesh.pos.size(); v += 3) {
            glVertex3fv(&mesh.pos[v]);
        }
    }
    glEnd();
    glEndList();
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
    if (m_WaterList) {
        glDeleteLists(m_WaterList, 1);
        m_WaterList = 0;
    }
    m_WaterTriangles = 0;
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
    assert(m_Loaded && m_WaterList);
    GLint program = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &program);
    glUseProgram(0);
    glPushAttrib(GL_ALL_ATTRIB_BITS);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_FALSE);
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_LIGHTING);
    glDisable(GL_ALPHA_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    ApplyFog();
    glColor4fv(m_Params.water.data());
    glCallList(m_WaterList);
    glPopAttrib();
    glUseProgram(static_cast<GLuint>(program));
}
