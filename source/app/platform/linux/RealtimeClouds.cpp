#include "app/platform/linux/RealtimeClouds.h"

#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>

#include "app/platform/linux/TexSample.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <span>
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
constexpr std::array<std::array<float, 3>, 12> kLowCloudCoordinates{{
    { 1.0f,  0.0f, 0.0f}, { 0.7f, -0.7f, 1.0f}, { 0.0f, -1.0f, 0.5f},
    {-0.7f, -0.7f, 0.0f}, {-1.0f,  0.0f, 1.0f}, {-0.7f,  0.7f, 0.3f},
    { 0.0f,  1.0f, 0.9f}, { 0.7f,  0.7f, 0.4f}, { 0.8f,  0.4f, 1.3f},
    {-0.8f,  0.4f, 1.4f}, { 0.4f, -0.8f, 1.2f}, {-0.4f, -0.8f, 1.7f},
}};

static bool Fail(char* err, std::size_t errSize, const char* message) {
    if (err && errSize) std::snprintf(err, errSize, "%s", message);
    return false;
}

static bool StartParser(char* err, std::size_t errSize) {
    if (rw::Engine::state != rw::Engine::Dead) {
        return rw::Engine::state == rw::Engine::Started || Fail(err, errSize, "cloud parser is not started");
    }
    if (!rw::Engine::init(nullptr)) return Fail(err, errSize, "cloud librw init");
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
    if (!rw::Engine::open(nullptr) || !rw::Engine::start()) return Fail(err, errSize, "cloud librw start");
    rw::Texture::setLoadTextures(false);
    return true;
}

struct CloudFile {
    void* Handle{};
    ~CloudFile() { if (Handle) OS_FileClose(Handle); }
};

struct CloudDictionary {
    rw::TexDictionary* Previous = rw::TexDictionary::getCurrent();
    rw::TexDictionary* Value{};
    ~CloudDictionary() {
        rw::TexDictionary::setCurrent(Previous);
        if (Value) Value->destroy();
    }
};

static GLenum TextureFilter(uint32 filter) {
    switch (filter & 0xff) {
    case rw::Texture::NEAREST:
    case rw::Texture::MIPNEAREST:
    case rw::Texture::LINEARMIPNEAREST: return GL_NEAREST;
    case rw::Texture::LINEAR:
    case rw::Texture::MIPLINEAR:
    case rw::Texture::LINEARMIPLINEAR: return GL_LINEAR;
    default: return 0;
    }
}

static GLenum TextureAddress(uint32 address) {
    switch (address) {
    case rw::Texture::WRAP: return GL_REPEAT;
    case rw::Texture::MIRROR: return GL_MIRRORED_REPEAT;
    case rw::Texture::CLAMP: return GL_CLAMP_TO_EDGE;
    case rw::Texture::BORDER: return GL_CLAMP_TO_BORDER;
    default: return 0;
    }
}

static std::array<float, 4> Transform(const std::array<float, 16>& matrix,
                                      const std::array<float, 4>& point) {
    std::array<float, 4> out{};
    for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 4; ++column) {
            out[row] += matrix[column * 4 + row] * point[column];
        }
    }
    return out;
}

template<std::size_t N>
static bool Finite(const std::array<float, N>& values) {
    return std::ranges::all_of(values, [](float value) { return std::isfinite(value); });
}

struct CloudDrawState {
    GLint Program{}, ActiveTexture{}, MatrixMode{};
    GLboolean ColourSum{};

    CloudDrawState(int width, int height) {
        glGetIntegerv(GL_CURRENT_PROGRAM, &Program);
        glGetIntegerv(GL_ACTIVE_TEXTURE, &ActiveTexture);
        glGetIntegerv(GL_MATRIX_MODE, &MatrixMode);
        ColourSum = glIsEnabled(GL_COLOR_SUM);
        glPushAttrib(GL_ALL_ATTRIB_BITS);
        glUseProgram(0);
        GLint units{};
        glGetIntegerv(GL_MAX_TEXTURE_UNITS, &units);
        for (int unit = 0; unit < units; ++unit) {
            glActiveTexture(GL_TEXTURE0 + unit);
            glDisable(GL_TEXTURE_1D);
            glDisable(GL_TEXTURE_2D);
            glDisable(GL_TEXTURE_3D);
            glDisable(GL_TEXTURE_CUBE_MAP);
            glDisable(GL_TEXTURE_RECTANGLE);
            glDisable(GL_TEXTURE_GEN_S);
            glDisable(GL_TEXTURE_GEN_T);
            glDisable(GL_TEXTURE_GEN_R);
            glDisable(GL_TEXTURE_GEN_Q);
        }
        glActiveTexture(GL_TEXTURE0);
        glMatrixMode(GL_TEXTURE);
        glPushMatrix();
        glLoadIdentity();
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
        glMatrixMode(GL_PROJECTION);
        glPushMatrix();
        glLoadIdentity();
        glOrtho(0, width, height, 0, -1, 1);
        glMatrixMode(GL_MODELVIEW);
        glPushMatrix();
        glLoadIdentity();
        glViewport(0, 0, width, height);
        glDisable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
        glDisable(GL_STENCIL_TEST);
        glDisable(GL_ALPHA_TEST);
        glDisable(GL_FOG);
        glDisable(GL_LIGHTING);
        glDisable(GL_CULL_FACE);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_COLOR_LOGIC_OP);
        glDisable(GL_POLYGON_STIPPLE);
        glDisable(GL_POLYGON_SMOOTH);
        glDisable(GL_COLOR_SUM);
        glDisable(GL_SAMPLE_COVERAGE);
        glDisable(GL_SAMPLE_ALPHA_TO_COVERAGE);
        glDisable(GL_SAMPLE_ALPHA_TO_ONE);
        GLint planes{};
        glGetIntegerv(GL_MAX_CLIP_PLANES, &planes);
        for (int plane = 0; plane < planes; ++plane) glDisable(GL_CLIP_PLANE0 + plane);
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glEnable(GL_BLEND);
        glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
        glBlendFuncSeparate(GL_ONE, GL_ONE, GL_ONE, GL_ONE);
    }

    ~CloudDrawState() {
        glMatrixMode(GL_MODELVIEW);
        glPopMatrix();
        glMatrixMode(GL_PROJECTION);
        glPopMatrix();
        glMatrixMode(GL_TEXTURE);
        glPopMatrix();
        glPopAttrib();
        if (ColourSum) glEnable(GL_COLOR_SUM); else glDisable(GL_COLOR_SUM);
        glUseProgram(Program);
        glActiveTexture(ActiveTexture);
        glMatrixMode(MatrixMode);
    }
};
} // namespace

RealtimeClouds::RealtimeClouds() : m_Owner(std::this_thread::get_id()) {
}

RealtimeClouds::~RealtimeClouds() {
    ReleaseGpu();
}

bool RealtimeClouds::Load(const char* gameDir, char* err, std::size_t errSize) {
    assert(m_Owner == std::this_thread::get_id());
    assert(!m_Texture && "ReleaseGpu before reloading clouds");
    m_Loaded = false;
    m_CloudImage = {};
    if (!gameDir || !gameDir[0]) return Fail(err, errSize, "cloud load needs a game dir");
    if (!StartParser(err, errSize)) return false;
    OS_SetFilePathOffset(gameDir);
    CloudFile file;
    if (OS_FileOpen(FILE_DATA_AREA_DEFAULT, &file.Handle, "models/particle.txd", FILE_ACCESS_READ) != 0 || !file.Handle) {
        return Fail(err, errSize, "cloud models/particle.txd open");
    }
    const auto size = OS_FileSize(file.Handle);
    if (size < 12) return Fail(err, errSize, "cloud models/particle.txd is invalid");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    if (OS_FileRead(file.Handle, bytes.data(), size) != 0) return Fail(err, errSize, "cloud models/particle.txd read");
    OS_FileClose(file.Handle);
    file.Handle = nullptr;

    rw::StreamMemory stream;
    stream.open(bytes.data(), static_cast<uint32>(bytes.size()));
    CloudDictionary dictionary;
    if (rw::findChunk(&stream, rw::ID_TEXDICTIONARY, nullptr, nullptr)) {
        dictionary.Value = rw::TexDictionary::streamRead(&stream);
    }
    stream.close();
    if (!dictionary.Value) return Fail(err, errSize, "cloud models/particle.txd parse");
    auto* texture = dictionary.Value->find("cloud1");
    WorldShotImage decoded{};
    if (!texture || !TexSample_Decode(texture, decoded) || decoded.rgba.empty()) {
        return Fail(err, errSize, "cloud particle:cloud1 decode");
    }
    decoded.filter = texture->filterAddressing;
    if (!TextureFilter(decoded.filter) || !TextureAddress((decoded.filter >> 8) & 15) ||
        !TextureAddress((decoded.filter >> 12) & 15)) {
        return Fail(err, errSize, "cloud1 has unsupported filter/addressing");
    }
    m_CloudImage = std::move(decoded);
    m_Loaded = true;
    return true;
}

bool RealtimeClouds::Upload(char* err, std::size_t errSize) {
    assert(m_Owner == std::this_thread::get_id());
    assert(m_Loaded && !m_Texture);
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
    for (const auto scale : {GL_RED_SCALE, GL_GREEN_SCALE, GL_BLUE_SCALE, GL_ALPHA_SCALE}) glPixelTransferf(scale, 1.0f);
    for (const auto bias : {GL_RED_BIAS, GL_GREEN_BIAS, GL_BLUE_BIAS, GL_ALPHA_BIAS}) glPixelTransferf(bias, 0.0f);
    glGenTextures(1, &m_Texture);
    glBindTexture(GL_TEXTURE_2D, m_Texture);
    const auto filter = TextureFilter(m_CloudImage.filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, TextureAddress((m_CloudImage.filter >> 8) & 15));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, TextureAddress((m_CloudImage.filter >> 12) & 15));
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, m_CloudImage.w, m_CloudImage.h, 0,
        GL_RGBA, GL_UNSIGNED_BYTE, m_CloudImage.rgba.data());
    glPopClientAttrib();
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, unpackBuffer);
    glPopAttrib();
    glActiveTexture(active);
    const auto error = glGetError();
    if (error != GL_NO_ERROR) {
        ReleaseGpu();
        char message[128];
        std::snprintf(message, sizeof(message), "cloud texture upload GL=0x%x", error);
        return Fail(err, errSize, message);
    }
    return true;
}

void RealtimeClouds::ReleaseGpu() {
    assert(m_Owner == std::this_thread::get_id());
    if (m_Texture) glDeleteTextures(1, &m_Texture);
    m_Texture = 0;
}

RealtimeCloudCamera RealtimeClouds::CaptureCamera(std::array<float, 3> position,
    int width, int height, float nearClip, float farClip, float fov, float roll) {
    RealtimeCloudCamera camera;
    glGetFloatv(GL_MODELVIEW_MATRIX, camera.ModelView.data());
    glGetFloatv(GL_PROJECTION_MATRIX, camera.Projection.data());
    camera.Position = position;
    camera.Width = width;
    camera.Height = height;
    camera.NearClip = nearClip;
    camera.FarClip = farClip;
    camera.Fov = fov;
    camera.Roll = roll;
    return camera;
}

RealtimeCloudResult RealtimeClouds::BuildLowClouds(const RealtimeCloudCamera& camera,
    const RealtimeCloudState& state, RealtimeCloudGeometry& out) {
    out = {};
    if (!state.HasExteriorVisibility) return RealtimeCloudResult::Unsupported;
    if (!state.CanSeeOutside) return RealtimeCloudResult::Hidden;
    const auto weatherInRange = [](float value) {
        return std::isfinite(value) && value >= 0.0f && value <= 1.0f;
    };
    if (!state.HasWeather || !state.HasClock || !state.HasLowCloudColours ||
        !weatherInRange(state.Foggyness) || !weatherInRange(state.CloudCoverage) ||
        !weatherInRange(state.ExtraSunnyness) || !std::isfinite(state.Wind) ||
        !std::isfinite(state.Hour) || state.Hour < 0.0f || state.Hour >= 24.0f ||
        camera.Width <= 0 || camera.Height <= 0 || !Finite(camera.Position) ||
        !Finite(camera.ModelView) || !Finite(camera.Projection) ||
        !std::isfinite(camera.NearClip) || camera.NearClip < 0.0f ||
        !std::isfinite(camera.FarClip) || camera.FarClip <= camera.NearClip + 1.0f ||
        !std::isfinite(camera.Fov) || camera.Fov <= 0.0f || !std::isfinite(camera.Roll)) {
        return RealtimeCloudResult::Unsupported;
    }

    const auto balance = std::max({state.Foggyness, state.CloudCoverage, state.ExtraSunnyness});
    std::array<std::uint8_t, 4> colour{};
    for (std::size_t channel = 0; channel < 3; ++channel) {
        const auto balanced = static_cast<std::uint8_t>(state.LowCloudColours[channel] * (1.0f - balance));
        colour[channel] = static_cast<std::uint8_t>((static_cast<unsigned>(balanced) * 255u) >> 8u);
    }
    colour[3] = 255;

    const auto cosine = std::cos(camera.Roll);
    const auto sine = std::sin(camera.Roll);
    constexpr std::array<std::size_t, 6> indices{0, 1, 2, 3, 0, 2};
    constexpr std::array<std::array<float, 2>, 4> uv{{{0, 0}, {0, 1}, {1, 1}, {1, 0}}};
    for (const auto& offset : kLowCloudCoordinates) {
        const std::array world{
            camera.Position[0] + offset[0] * 800.0f,
            camera.Position[1] + offset[1] * 800.0f,
            offset[2] * 60.0f + 40.0f,
            1.0f,
        };
        const auto eye = Transform(camera.ModelView, world);
        const auto depth = -eye[2];
        if (!std::isfinite(depth)) return RealtimeCloudResult::Unsupported;
        if (depth <= camera.NearClip + 1.0f) continue;
        const auto clip = Transform(camera.Projection, eye);
        if (!Finite(clip) || clip[3] == 0.0f) return RealtimeCloudResult::Unsupported;
        const auto centreX = (clip[0] / clip[3] + 1.0f) * camera.Width * 0.5f;
        const auto centreY = (1.0f - clip[1] / clip[3]) * camera.Height * 0.5f;
        const auto scaleX = camera.Width / depth / camera.Fov * 70.0f;
        const auto scaleY = camera.Height / depth / camera.Fov * 70.0f;
        const auto halfWidth = scaleX * 320.0f;
        const auto halfHeight = scaleY * 40.0f;
        if (!std::isfinite(centreX) || !std::isfinite(centreY) ||
            !std::isfinite(halfWidth) || !std::isfinite(halfHeight)) {
            out = {};
            return RealtimeCloudResult::Unsupported;
        }
        const std::array<std::array<float, 2>, 4> corners{{
            {centreX - cosine * halfWidth - sine * halfHeight, centreY - cosine * halfHeight + sine * halfWidth},
            {centreX - cosine * halfWidth + sine * halfHeight, centreY + cosine * halfHeight + sine * halfWidth},
            {centreX + cosine * halfWidth + sine * halfHeight, centreY + cosine * halfHeight - sine * halfWidth},
            {centreX + cosine * halfWidth - sine * halfHeight, centreY - cosine * halfHeight - sine * halfWidth},
        }};
        // CalcScreenCoors(..., false, true) proves only the near rejection.
        // Submit off-screen sprites too and leave viewport clipping to GL;
        // the unreversed 0x70EAB0 callback provides no CPU-cull source proof.
        for (const auto index : indices) {
            assert(out.Size < out.Vertices.size());
            out.Vertices[out.Size++] = {
                corners[index][0], corners[index][1], depth, 1.0f / depth,
                uv[index][0], uv[index][1], colour,
            };
        }
        ++out.SpriteCount;
    }
    return RealtimeCloudResult::Ready;
}

RealtimeCloudResult RealtimeClouds::Draw(const RealtimeCloudCamera& camera,
    const RealtimeCloudState& state) const {
    assert(m_Owner == std::this_thread::get_id());
    if (!m_Texture) return RealtimeCloudResult::Unsupported;
    RealtimeCloudGeometry geometry;
    const auto result = BuildLowClouds(camera, state, geometry);
    if (result != RealtimeCloudResult::Ready || !geometry.Size) return result;
    const CloudDrawState drawState(camera.Width, camera.Height);
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, m_Texture);
    glBegin(GL_TRIANGLES);
    for (const auto& vertex : std::span{geometry.Vertices}.first(geometry.Size)) {
        glColor4ub(vertex.Colour[0], vertex.Colour[1], vertex.Colour[2], vertex.Colour[3]);
        glTexCoord2f(vertex.U, vertex.V);
        glVertex2f(vertex.X, vertex.Y);
    }
    glEnd();
    return result;
}
