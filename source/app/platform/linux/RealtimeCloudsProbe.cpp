// Actual low-cloud module, real particle TXD and own surfaceless EGL framebuffer.
#include "app/platform/linux/RealtimeClouds.cpp"
#include "RealtimeCloudsProbe.source.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

// The production module resolves these calls to oswrapper_linux.cpp. Keeping
// the isolated probe on stdio avoids linking SDL/OpenAL-only OS services.
static std::string s_ProbeGameDir;

void OS_SetFilePathOffset(const char* path) {
    s_ProbeGameDir = path ? path : "";
}

int32 OS_FileOpen(OSFileDataArea dataArea, void** file, const char* path, OSFileAccessType access) {
    if (!file || !path || dataArea != FILE_DATA_AREA_DEFAULT || access != FILE_ACCESS_READ) return 1;
    const auto fullPath = s_ProbeGameDir.empty() ? std::string(path) : s_ProbeGameDir + "/" + path;
    *file = std::fopen(fullPath.c_str(), "rb");
    return *file ? 0 : 1;
}

int32 OS_FileSize(void* file) {
    if (!file) return -1;
    const auto position = std::ftell(static_cast<std::FILE*>(file));
    if (position < 0 || std::fseek(static_cast<std::FILE*>(file), 0, SEEK_END) != 0) return -1;
    const auto size = std::ftell(static_cast<std::FILE*>(file));
    if (std::fseek(static_cast<std::FILE*>(file), position, SEEK_SET) != 0) return -1;
    return size >= 0 && size <= INT32_MAX ? static_cast<int32>(size) : -1;
}

int32 OS_FileRead(void* file, void* destination, int32 size) {
    if (!file || !destination || size < 0) return 1;
    return std::fread(destination, 1, static_cast<std::size_t>(size), static_cast<std::FILE*>(file)) ==
        static_cast<std::size_t>(size) ? 0 : 1;
}

int32 OS_FileClose(void* file) {
    return file ? std::fclose(static_cast<std::FILE*>(file)) : 1;
}

namespace {
constexpr int kWidth = 1280, kHeight = 720;
constexpr int kSurfaceWidth = 1280, kSurfaceHeight = 1280;
constexpr float kPi = 3.14159265358979323846f;

static void Require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "cloud-probe-fail %s GL=0x%x EGL=0x%x\n", message, glGetError(), eglGetError());
        std::exit(1);
    }
}

static std::uint64_t Hash(const std::vector<std::uint8_t>& bytes) {
    std::uint64_t hash = 14695981039346656037ull;
    for (const auto byte : bytes) hash = (hash ^ byte) * 1099511628211ull;
    return hash;
}

static void StartReferenceParser() {
    if (rw::Engine::state != rw::Engine::Dead) return;
    Require(rw::Engine::init(nullptr), "reference librw init");
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
    Require(rw::Engine::open(nullptr) && rw::Engine::start(), "reference librw start");
    rw::Texture::setLoadTextures(false);
}

struct ProbeFile {
    void* Handle{};
    ~ProbeFile() { if (Handle) OS_FileClose(Handle); }
};

static std::vector<std::uint8_t> ReadAsset(const char* path) {
    ProbeFile file;
    Require(OS_FileOpen(FILE_DATA_AREA_DEFAULT, &file.Handle, path, FILE_ACCESS_READ) == 0 && file.Handle, "probe asset open");
    const auto size = OS_FileSize(file.Handle);
    Require(size > 0, "probe asset size");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    Require(OS_FileRead(file.Handle, bytes.data(), size) == 0, "probe asset read");
    return bytes;
}

static WorldShotImage ReferenceCloudImage() {
    // Walk the native RW chunks directly. No librw raster parser, lock or
    // TexSample decoder participates in this witness. Supports only the
    // explicitly enumerated D3D9 formats below; other assets fail narrowly.
    const auto bytes = ReadAsset("models/particle.txd");
    const auto le = [&](std::size_t offset, std::size_t size = 4) {
        Require(size <= 4 && offset <= bytes.size() && size <= bytes.size() - offset, "raw TXD integer bounds");
        std::uint32_t value = 0;
        for (std::size_t i = 0; i < size; ++i) value |= std::uint32_t(bytes[offset + i]) << (8 * i);
        return value;
    };
    struct Chunk { std::uint32_t Id; std::size_t Begin, End; };
    const auto chunks = [&](std::size_t start, std::size_t end) {
        std::vector<Chunk> result;
        Require(end <= bytes.size(), "raw TXD container bounds");
        while (start < end) {
            Require(end - start >= 12, "raw TXD chunk header");
            const auto size = le(start + 4);
            Require(size <= end - start - 12, "raw TXD chunk payload");
            result.push_back({le(start), start + 12, start + 12 + size});
            start += 12 + size;
        }
        return result;
    };
    for (const auto& dictionary : chunks(0, bytes.size())) {
        if (dictionary.Id != 0x16) continue;
        for (const auto& native : chunks(dictionary.Begin, dictionary.End)) {
            if (native.Id != 0x15) continue;
            for (const auto& data : chunks(native.Begin, native.End)) {
                if (data.Id != 1 || data.End - data.Begin < 92) continue;
                const auto base = data.Begin;
                if (std::memcmp(bytes.data() + base + 8, "cloud1\0", 7) != 0) continue;
                const auto format = le(base + 76);
                const auto flags = le(base + 87, 1);
                WorldShotImage image{};
                std::strcpy(image.name, "cloud1");
                image.filter = le(base + 4);
                image.w = le(base + 80, 2);
                image.h = le(base + 82, 2);
                const auto size = le(base + 88);
                Require(le(base) == 9 && image.w > 0 && image.h > 0 && size <= data.End - base - 92,
                    "raw cloud1 D3D9 mip bounds");
                Require(!(le(base + 72) & 0x6000), "raw cloud1 palette unsupported");
                image.rgba.resize(static_cast<std::size_t>(image.w) * image.h * 4);
                std::printf("cloud-raw-native platform=9 format=0x%08x flags=0x%x depth=%u levels=%u mip0=%u\n",
                    format, flags, le(base + 84, 1), le(base + 85, 1), size);
                const auto mip = base + 92;
                if (format == 21 || format == 22) { // A8R8G8B8 / X8R8G8B8, little-endian BGRA
                    Require(!(flags & 8) && size == image.rgba.size(), "raw cloud1 packed32 mip size");
                    for (std::size_t i = 0; i < size; i += 4) {
                        image.rgba[i] = bytes[mip + i + 2];
                        image.rgba[i + 1] = bytes[mip + i + 1];
                        image.rgba[i + 2] = bytes[mip + i];
                        image.rgba[i + 3] = format == 21 ? bytes[mip + i + 3] : 255;
                    }
                } else {
                    Require(false, "raw cloud1 witness supports D3D9 A8R8G8B8/X8R8G8B8 only");
                }
                return image;
            }
        }
    }
    Require(false, "raw native cloud1 not found");
    return {};
}

static std::array<std::uint8_t, 3> ReferenceLowColour(const char* weather, std::size_t row) {
    const auto bytes = ReadAsset("data/timecyc.dat");
    std::string text(bytes.begin(), bytes.end());
    std::istringstream lines(text);
    std::string line;
    bool section = false;
    std::vector<std::array<std::uint8_t, 3>> colours;
    while (std::getline(lines, line)) {
        const auto first = line.find_first_not_of(" \t\r");
        if (first == std::string::npos) continue;
        if (line[first] == '/') {
            const auto slashEnd = line.find_first_not_of('/', first);
            if (slashEnd != std::string::npos && slashEnd - first >= 4) {
                const auto nameStart = line.find_first_not_of(" \t", slashEnd);
                const auto nameEnd = line.find_first_of(" \t\r", nameStart);
                const auto name = line.substr(nameStart, nameEnd - nameStart);
                if (section && name != weather) break;
                section = name == weather;
            }
            continue;
        }
        if (!section) continue;
        std::ranges::replace(line, ',', ' ');
        std::istringstream tokens(line);
        std::vector<float> values;
        float value{};
        while (tokens >> value) values.push_back(value);
        if (values.size() < 33) continue;
        std::array<std::uint8_t, 3> colour{};
        for (std::size_t channel = 0; channel < 3; ++channel) {
            Require(std::isfinite(values[30 + channel]) && values[30 + channel] >= 0.0f && values[30 + channel] <= 255.0f,
                "timecyc low-cloud RGB range");
            colour[channel] = static_cast<std::uint8_t>(values[30 + channel]);
        }
        colours.push_back(colour);
    }
    Require(colours.size() == 8 && row < colours.size(), "timecyc named weather rows");
    return colours[row];
}

static RealtimeCloudCamera Camera(float x, float y, float z, float yaw, float pitch, float roll,
                                  float nearClip = 0.1f, float farClip = 1600.0f, float fov = 70.0f,
                                  int width = kWidth, int height = kHeight, float projectionFov = 60.0f) {
    RealtimeCloudCamera camera;
    camera.Position = {x, y, z};
    camera.Width = width;
    camera.Height = height;
    camera.NearClip = nearClip;
    camera.FarClip = farClip;
    camera.Fov = fov;
    camera.Roll = roll;
    const auto cy = std::cos(yaw), sy = std::sin(yaw), cp = std::cos(pitch), sp = std::sin(pitch);
    camera.ModelView = {
        sy, -cy * sp, -cy * cp, 0,
        -cy, -sy * sp, -sy * cp, 0,
        0, cp, -sp, 0,
        -sy * x + cy * y, cy * sp * x + sy * sp * y - cp * z,
        cy * cp * x + sy * cp * y + sp * z, 1,
    };
    const auto tangent = std::tan(projectionFov * kPi / 360.0f);
    const auto aspect = static_cast<float>(width) / height;
    camera.Projection = {
        1.0f / (tangent * aspect), 0, 0, 0,
        0, 1.0f / tangent, 0, 0,
        0, 0, -(farClip + nearClip) / (farClip - nearClip), -1,
        0, 0, -2.0f * farClip * nearClip / (farClip - nearClip), 0,
    };
    return camera;
}

static RealtimeCloudState State(std::array<std::uint8_t, 3> colour) {
    RealtimeCloudState state;
    state.HasExteriorVisibility = true;
    state.CanSeeOutside = true;
    state.HasWeather = true;
    state.Foggyness = 0.125f;
    state.CloudCoverage = 0.375f;
    state.ExtraSunnyness = 0.25f;
    state.Wind = 0.7f;
    state.HasClock = true;
    state.Hour = 12.0f;
    state.GameMs = 0x12345678u;
    state.HasLowCloudColours = true;
    state.LowCloudColours = colour;
    return state;
}

static RealtimeCloudGeometry SourceOracle(const RealtimeCloudCamera& camera, const RealtimeCloudState& state,
                                          float projectionFov = 60.0f) {
    RealtimeCloudGeometry out;
    const auto balance = std::max({state.Foggyness, state.CloudCoverage, state.ExtraSunnyness});
    std::array<std::uint8_t, 4> colour{};
    for (int channel = 0; channel < 3; ++channel) {
        const auto balanced = static_cast<std::uint8_t>(state.LowCloudColours[channel] * (1.0f - balance));
        colour[channel] = static_cast<std::uint8_t>((static_cast<unsigned>(balanced) * source_clouds::Intensity) >> 8u);
    }
    colour[3] = 255;
    const auto cy = camera.ModelView[0], minusCySp = camera.ModelView[1], minusCyCp = camera.ModelView[2];
    const auto minusCy = camera.ModelView[4], minusSySp = camera.ModelView[5], minusSyCp = camera.ModelView[6];
    const auto cp = camera.ModelView[9], minusSp = camera.ModelView[10];
    const auto cosine = std::cos(camera.Roll), sine = std::sin(camera.Roll);
    constexpr std::array<std::size_t, 6> order{0, 1, 2, 3, 0, 2};
    constexpr std::array<std::array<float, 2>, 4> uv{{{0, 0}, {0, 1}, {1, 1}, {1, 0}}};
    for (const auto& offset : source_clouds::Offsets) {
        const auto wx = camera.Position[0] + offset[0] * source_clouds::PositionScale[0];
        const auto wy = camera.Position[1] + offset[1] * source_clouds::PositionScale[1];
        const auto wz = offset[2] * source_clouds::PositionScale[2] + source_clouds::HeightOffset;
        const auto eyeX = cy * wx + minusCy * wy + camera.ModelView[12];
        const auto eyeY = minusCySp * wx + minusSySp * wy + cp * wz + camera.ModelView[13];
        const auto eyeZ = minusCyCp * wx + minusSyCp * wy + minusSp * wz + camera.ModelView[14];
        const auto depth = -eyeZ;
        if (depth <= camera.NearClip + 1.0f) continue;
        const auto centreX = (eyeX / depth / std::tan(projectionFov * kPi / 360.0f) /
            (static_cast<float>(camera.Width) / camera.Height) + 1.0f) * camera.Width * 0.5f;
        const auto centreY = (1.0f - eyeY / depth / std::tan(projectionFov * kPi / 360.0f)) * camera.Height * 0.5f;
        const auto halfWidth = camera.Width / depth / camera.Fov * source_clouds::DefaultFov * source_clouds::SpriteDimensions[0];
        const auto halfHeight = camera.Height / depth / camera.Fov * source_clouds::DefaultFov * source_clouds::SpriteDimensions[1];
        const std::array<std::array<float, 2>, 4> corners{{
            {centreX - cosine * halfWidth - sine * halfHeight, centreY - cosine * halfHeight + sine * halfWidth},
            {centreX - cosine * halfWidth + sine * halfHeight, centreY + cosine * halfHeight + sine * halfWidth},
            {centreX + cosine * halfWidth + sine * halfHeight, centreY + cosine * halfHeight - sine * halfWidth},
            {centreX + cosine * halfWidth - sine * halfHeight, centreY - cosine * halfHeight - sine * halfWidth},
        }};
        for (const auto index : order) out.Vertices[out.Size++] = {
            corners[index][0], corners[index][1], depth, 1.0f / depth, uv[index][0], uv[index][1], colour,
        };
        ++out.SpriteCount;
    }
    return out;
}

static void GeometryOracle(const std::array<std::uint8_t, 3>& colour) {
    std::size_t cases = 0, sprites = 0, offscreenSubmitted = 0;
    float maxScreenError = 0;
    for (const float yaw : {0.0f, kPi / 2, kPi, -kPi / 2}) {
        for (const float pitch : {-0.2f, 0.0f, 0.25f}) {
            for (const float roll : {-0.4f, 0.0f, 0.3f}) {
                const auto camera = Camera(1123.0f, -901.0f, 55.0f, yaw, pitch, roll, 0.1f, 100.0f);
                const auto state = State(colour);
                RealtimeCloudGeometry actual;
                Require(RealtimeClouds::BuildLowClouds(camera, state, actual) == RealtimeCloudResult::Ready,
                    "source geometry supported");
                const auto expected = SourceOracle(camera, state);
                Require(actual.Size == expected.Size && actual.SpriteCount == expected.SpriteCount,
                    "source sprite visibility/order counts");
                for (std::size_t i = 0; i < actual.Size; i += 6) {
                    const auto vertices = std::span{actual.Vertices}.subspan(i, 6);
                    // Observation, not an extra culling rule in either path.
                    offscreenSubmitted += std::ranges::all_of(vertices, [](const auto& v) { return v.X < 0; }) ||
                        std::ranges::all_of(vertices, [&](const auto& v) { return v.X > camera.Width; });
                }
                for (std::size_t i = 0; i < actual.Size; ++i) {
                    const auto& a = actual.Vertices[i];
                    const auto& b = expected.Vertices[i];
                    maxScreenError = std::max({maxScreenError, std::abs(a.X - b.X), std::abs(a.Y - b.Y)});
                    Require(std::abs(a.X - b.X) < 0.002f + std::abs(b.X) * 0.000001f &&
                        std::abs(a.Y - b.Y) < 0.002f + std::abs(b.Y) * 0.000001f &&
                        std::abs(a.Depth - b.Depth) < 0.002f && std::abs(a.ReciprocalDepth - b.ReciprocalDepth) < 0.000002f &&
                        a.U == b.U && a.V == b.V && a.Colour == b.Colour,
                        "source XYZ/depth/RHW/UV/RGBA metadata");
                }
                sprites += actual.SpriteCount;
                ++cases;
            }
        }
    }
    auto camera = Camera(0, 0, 40, 0.2f, 0, 0);
    auto state = State(colour);
    RealtimeCloudGeometry baseline, changed;
    Require(RealtimeClouds::BuildLowClouds(camera, state, baseline) == RealtimeCloudResult::Ready, "baseline geometry");
    state.Wind = -state.Wind;
    state.Hour = 23.75f;
    state.GameMs = 0xffffffffu;
    Require(RealtimeClouds::BuildLowClouds(camera, state, changed) == RealtimeCloudResult::Ready, "reversed wind/time geometry");
    Require(baseline == changed, "low layer is independent of wind/time");
    Require(offscreenSubmitted > 0, "offscreen source submissions survive to GL clipping");
    auto boundary = Camera(0, 0, 45, 0, 0, 0, std::nextafter(799.0f, 0.0f));
    Require(RealtimeClouds::BuildLowClouds(boundary, state, changed) == RealtimeCloudResult::Ready &&
        std::ranges::any_of(std::span{changed.Vertices}.first(changed.Size), [](const auto& v) { return v.Depth == 800; }),
        "source near+1 admits depth immediately beyond boundary");
    boundary.NearClip = 799.0f;
    Require(RealtimeClouds::BuildLowClouds(boundary, state, changed) == RealtimeCloudResult::Ready &&
        std::ranges::none_of(std::span{changed.Vertices}.first(changed.Size), [](const auto& v) { return v.Depth <= 800; }),
        "source near+1 rejects exact boundary inclusively");
    state.HasWeather = false;
    Require(RealtimeClouds::BuildLowClouds(camera, state, changed) == RealtimeCloudResult::Unsupported && !changed.Size,
        "unknown weather is unsupported");
    state = State(colour);
    state.CanSeeOutside = false;
    Require(RealtimeClouds::BuildLowClouds(camera, state, changed) == RealtimeCloudResult::Hidden && !changed.Size,
        "source exterior visibility guard");
    std::printf("cloud-geometry source-sha256=%s cases=%zu submitted-sprites=%zu offscreen-submitted=%zu mixed-projection=60/70 screen-tolerance=0.002+1ppm max-screen-error=%g adapter-UV/RGBA/order/roll near-boundary-inclusive far-unclipped GL-viewport-clipping\n",
        source_clouds::SourceSha256, cases, sprites, offscreenSubmitted, maxScreenError);
}

struct EglApp {
    EGLDisplay Display = EGL_NO_DISPLAY;
    EGLSurface Surface = EGL_NO_SURFACE;
    EGLContext Context = EGL_NO_CONTEXT;
    EglApp() = default;
    EglApp(const EglApp&) = delete;
    EglApp& operator=(const EglApp&) = delete;
    EglApp(EglApp&& other) noexcept : Display(other.Display), Surface(other.Surface), Context(other.Context) {
        other.Display = EGL_NO_DISPLAY;
        other.Surface = EGL_NO_SURFACE;
        other.Context = EGL_NO_CONTEXT;
    }
    ~EglApp() {
        if (Display == EGL_NO_DISPLAY) return;
        eglMakeCurrent(Display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (Context != EGL_NO_CONTEXT) eglDestroyContext(Display, Context);
        if (Surface != EGL_NO_SURFACE) eglDestroySurface(Display, Surface);
        eglTerminate(Display);
    }
};

static EglApp MakeEgl() {
    EglApp app;
    const auto getDisplay = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
    Require(getDisplay, "surfaceless EGL entry point");
    app.Display = getDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
    Require(app.Display != EGL_NO_DISPLAY && eglInitialize(app.Display, nullptr, nullptr) && eglBindAPI(EGL_OPENGL_API), "EGL initialize");
    const EGLint attributes[]{EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8, EGL_NONE};
    EGLConfig config{};
    EGLint count{};
    Require(eglChooseConfig(app.Display, attributes, &config, 1, &count) && count, "EGL config");
    const EGLint size[]{EGL_WIDTH, kSurfaceWidth, EGL_HEIGHT, kSurfaceHeight, EGL_NONE};
    app.Surface = eglCreatePbufferSurface(app.Display, config, size);
    app.Context = eglCreateContext(app.Display, config, EGL_NO_CONTEXT, nullptr);
    Require(app.Surface != EGL_NO_SURFACE && app.Context != EGL_NO_CONTEXT &&
        eglMakeCurrent(app.Display, app.Surface, app.Surface, app.Context), "EGL pbuffer context");
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    return app;
}

static std::vector<float> Snapshot() {
    const auto check = [](GLenum name) {
        const auto error = glGetError();
        if (error != GL_NO_ERROR) {
            std::fprintf(stderr, "cloud-snapshot name=0x%x error=0x%x\n", name, error);
            Require(false, "snapshot GL query");
        }
    };
    check(0);
    std::vector<float> out;
    const auto add = [&](GLenum name, int count = 1) {
        GLfloat values[16]{};
        glGetFloatv(name, values);
        check(name);
        out.insert(out.end(), values, values + count);
    };
    for (const auto name : {GL_CURRENT_PROGRAM, GL_ACTIVE_TEXTURE, GL_MATRIX_MODE,
        GL_BLEND_SRC_RGB, GL_BLEND_DST_RGB, GL_BLEND_SRC_ALPHA, GL_BLEND_DST_ALPHA,
        GL_BLEND_EQUATION_RGB, GL_BLEND_EQUATION_ALPHA, GL_ALPHA_TEST_FUNC, GL_ALPHA_TEST_REF,
        GL_DEPTH_FUNC, GL_DEPTH_WRITEMASK, GL_STENCIL_FUNC, GL_STENCIL_REF, GL_STENCIL_VALUE_MASK,
        GL_STENCIL_WRITEMASK, GL_STENCIL_FAIL, GL_STENCIL_PASS_DEPTH_FAIL, GL_STENCIL_PASS_DEPTH_PASS,
        GL_SHADE_MODEL, GL_CULL_FACE_MODE, GL_FRONT_FACE, GL_PIXEL_UNPACK_BUFFER_BINDING,
        GL_UNPACK_ALIGNMENT, GL_UNPACK_ROW_LENGTH, GL_UNPACK_SKIP_ROWS, GL_UNPACK_SKIP_PIXELS,
        GL_RED_SCALE, GL_RED_BIAS}) add(name);
    for (const auto name : {GL_VIEWPORT, GL_SCISSOR_BOX, GL_COLOR_WRITEMASK, GL_CURRENT_COLOR}) add(name, 4);
    add(GL_POLYGON_MODE, 2);
    add(GL_MODELVIEW_MATRIX, 16);
    add(GL_PROJECTION_MATRIX, 16);
    for (const auto name : {GL_DEPTH_TEST, GL_STENCIL_TEST, GL_ALPHA_TEST, GL_BLEND, GL_FOG,
        GL_LIGHTING, GL_CULL_FACE, GL_SCISSOR_TEST, GL_COLOR_LOGIC_OP, GL_POLYGON_STIPPLE,
        GL_POLYGON_SMOOTH, GL_CLIP_PLANE0, GL_COLOR_SUM, GL_SAMPLE_COVERAGE,
        GL_SAMPLE_ALPHA_TO_COVERAGE, GL_SAMPLE_ALPHA_TO_ONE}) out.push_back(glIsEnabled(name));
    GLint active{}, units{};
    glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
    glGetIntegerv(GL_MAX_TEXTURE_UNITS, &units);
    for (int unit = 0; unit < units; ++unit) {
        glActiveTexture(GL_TEXTURE0 + unit);
        for (const auto name : {GL_TEXTURE_1D, GL_TEXTURE_2D, GL_TEXTURE_3D, GL_TEXTURE_CUBE_MAP,
            GL_TEXTURE_RECTANGLE, GL_TEXTURE_GEN_S, GL_TEXTURE_GEN_T, GL_TEXTURE_GEN_R, GL_TEXTURE_GEN_Q}) {
            out.push_back(glIsEnabled(name));
        }
        add(GL_TEXTURE_BINDING_2D);
        add(GL_TEXTURE_MATRIX, 16);
        GLfloat mode{};
        glGetTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, &mode);
        out.push_back(mode);
    }
    glActiveTexture(active);
    Require(glGetError() == GL_NO_ERROR, "snapshot GL state");
    return out;
}

static GLuint HostileState() {
    Require(glGetError() == GL_NO_ERROR, "before hostile state");
    const char* vertexSource = "#version 120\nvoid main(){gl_Position=ftransform();}";
    const char* fragmentSource = "#version 120\nvoid main(){gl_FragColor=vec4(1,0,1,1);}";
    const auto program = glCreateProgram();
    for (const auto& [type, source] : {std::pair{GL_VERTEX_SHADER, vertexSource}, std::pair{GL_FRAGMENT_SHADER, fragmentSource}}) {
        const auto shader = glCreateShader(type);
        glShaderSource(shader, 1, &source, nullptr);
        glCompileShader(shader);
        GLint ok{};
        glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
        Require(ok, "hostile shader compile");
        glAttachShader(program, shader);
        glDeleteShader(shader);
    }
    glLinkProgram(program);
    GLint ok{};
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    Require(ok, "hostile program link");
    glUseProgram(program);
    Require(glGetError() == GL_NO_ERROR, "hostile shader state");
    glViewport(3, 5, 89, 73);
    glScissor(2, 4, 20, 21);
    glEnable(GL_SCISSOR_TEST);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_GREATER);
    glDepthMask(GL_TRUE);
    glEnable(GL_STENCIL_TEST);
    glStencilFunc(GL_NEVER, 7, 0x5a);
    glStencilMask(0x33);
    glStencilOp(GL_INCR, GL_DECR, GL_REPLACE);
    Require(glGetError() == GL_NO_ERROR, "hostile stencil state");
    glEnable(GL_ALPHA_TEST);
    glAlphaFunc(GL_GREATER, 0.8f);
    Require(glGetError() == GL_NO_ERROR, "hostile depth/stencil/alpha state");
    for (const auto capability : {GL_FOG, GL_LIGHTING, GL_CULL_FACE, GL_COLOR_LOGIC_OP,
        GL_POLYGON_STIPPLE, GL_CLIP_PLANE0, GL_POLYGON_SMOOTH, GL_COLOR_SUM, GL_SAMPLE_COVERAGE,
        GL_SAMPLE_ALPHA_TO_COVERAGE, GL_SAMPLE_ALPHA_TO_ONE}) {
        glEnable(capability);
        const auto error = glGetError();
        if (error != GL_NO_ERROR) {
            std::fprintf(stderr, "cloud-hostile capability=0x%x error=0x%x\n", capability, error);
            Require(false, "hostile capability");
        }
    }
    glSampleCoverage(0, GL_FALSE);
    glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
    glColorMask(GL_FALSE, GL_TRUE, GL_FALSE, GL_FALSE);
    glEnable(GL_BLEND);
    glBlendEquationSeparate(GL_FUNC_REVERSE_SUBTRACT, GL_FUNC_SUBTRACT);
    glBlendFuncSeparate(GL_ONE, GL_ZERO, GL_DST_ALPHA, GL_SRC_ALPHA);
    glColor4f(0.2f, 0.3f, 0.4f, 0.5f);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glTranslatef(11, 12, 13);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glScalef(2, 3, 4);
    for (int unit = 0; unit < 4; ++unit) {
        glActiveTexture(GL_TEXTURE0 + unit);
        glEnable(GL_TEXTURE_2D);
        glEnable(GL_TEXTURE_GEN_S);
        glEnable(GL_TEXTURE_RECTANGLE);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_ADD);
        glMatrixMode(GL_TEXTURE);
        glLoadIdentity();
        glTranslatef(unit + 1, unit + 2, unit + 3);
    }
    Require(glGetError() == GL_NO_ERROR, "hostile texture/matrix/blend state");
    return program;
}

static void ClearFramebuffer() {
    glPushAttrib(GL_ALL_ATTRIB_BITS);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);
    glStencilMask(0xff);
    glClearColor(7.0f / 255, 11.0f / 255, 17.0f / 255, 0.25f);
    glClearDepth(0.375);
    glClearStencil(42);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    glPopAttrib();
}

static std::vector<std::uint8_t> Render(const RealtimeClouds& clouds, const RealtimeCloudCamera& camera,
    const RealtimeCloudState& state, RealtimeCloudResult expected) {
    ClearFramebuffer();
    const auto before = Snapshot();
    Require(clouds.Draw(camera, state) == expected, "actual cloud draw status");
    const auto after = Snapshot();
    Require(after == before, "cloud draw restores hostile GL state");
    std::vector<std::uint8_t> pixels(camera.Width * camera.Height * 4);
    std::vector<float> depth(camera.Width * camera.Height);
    std::vector<std::uint8_t> stencil(camera.Width * camera.Height);
    glReadPixels(0, 0, camera.Width, camera.Height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    glReadPixels(0, 0, camera.Width, camera.Height, GL_DEPTH_COMPONENT, GL_FLOAT, depth.data());
    glReadPixels(0, 0, camera.Width, camera.Height, GL_STENCIL_INDEX, GL_UNSIGNED_BYTE, stencil.data());
    for (std::size_t i = 0; i < depth.size(); ++i) {
        Require(std::abs(depth[i] - 0.375f) < 0.0001f && stencil[i] == 42, "clouds leave depth/stencil contents untouched");
    }
    Require(glGetError() == GL_NO_ERROR, "cloud render/readback GL error");
    return pixels;
}

static int ChangedRgb(const std::vector<std::uint8_t>& pixels) {
    int changed = 0;
    for (std::size_t i = 0; i < pixels.size(); i += 4) {
        changed += pixels[i] != 7 || pixels[i + 1] != 11 || pixels[i + 2] != 17;
    }
    return changed;
}

static RealtimeCloudCamera CaptureProductionCamera(const EglApp& app, int width, int height,
    float farClip, float yaw = 0.35f, float pitch = 0.03f, float roll = 0.0f) {
    Require(eglGetCurrentContext() == app.Context, "production-camera context");
    Require(glGetError() == GL_NO_ERROR, "before production-camera installation");
    GLint mode{};
    glGetIntegerv(GL_MATRIX_MODE, &mode);
    glPushAttrib(GL_VIEWPORT_BIT);
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    const ProductionCamera production{1123, -901, 45, yaw, pitch};
    production.Apply(width, height, farClip);
    Require(glGetError() == GL_NO_ERROR, "actual production-camera installation");
    const auto camera = RealtimeClouds::CaptureCamera({production.x, production.y, production.z},
        width, height, 0.1f, farClip, 70.0f, roll);
    const auto analytic = Camera(production.x, production.y, production.z, yaw, pitch, roll,
        0.1f, farClip, 70.0f, width, height, 60.0f);
    for (std::size_t i = 0; i < 16; ++i) {
        Require(std::abs(camera.Projection[i] - analytic.Projection[i]) < 0.000001f &&
            std::abs(camera.ModelView[i] - analytic.ModelView[i]) < 0.0002f, "actual Camera::Apply versus analytic 60-degree camera");
    }
    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glPopAttrib();
    Require(glGetError() == GL_NO_ERROR, "production-camera matrix/viewport stacks");
    glMatrixMode(mode);
    const auto restoreError = glGetError();
    if (restoreError != GL_NO_ERROR) std::fprintf(stderr, "cloud-camera mode=0x%x error=0x%x\n", mode, restoreError);
    Require(restoreError == GL_NO_ERROR, "production-camera restore");
    return camera;
}

// Clean, unshared GL context with its own raw-witness texture and independently
// configured renderer. Does not call module geometry/upload/state/draw helpers.
struct ReferenceRenderer {
    const EglApp& App;
    EGLContext Context;
    GLuint Texture{};
    ReferenceRenderer(const EglApp& app, const WorldShotImage& image) : App(app) {
        EGLint configId{};
        Require(eglQueryContext(app.Display, app.Context, EGL_CONFIG_ID, &configId), "reference context config id");
        const EGLint attributes[]{EGL_CONFIG_ID, configId, EGL_NONE};
        EGLConfig config{};
        EGLint count{};
        Require(eglChooseConfig(app.Display, attributes, &config, 1, &count) && count, "reference context config");
        Context = eglCreateContext(app.Display, config, EGL_NO_CONTEXT, nullptr);
        Require(Context != EGL_NO_CONTEXT && eglMakeCurrent(app.Display, app.Surface, app.Surface, Context),
            "independent reference GL context");
        const auto filter = image.filter & 255;
        Require(filter == 1 || filter == 2, "raw reference supports non-mip nearest/linear filter only");
        const auto address = [](unsigned value) {
            switch (value) {
            case 1: return GL_REPEAT;
            case 2: return GL_MIRRORED_REPEAT;
            case 3: return GL_CLAMP_TO_EDGE;
            case 4: return GL_CLAMP_TO_BORDER;
            default: Require(false, "raw reference texture addressing"); return GL_REPEAT;
            }
        };
        glGenTextures(1, &Texture);
        glBindTexture(GL_TEXTURE_2D, Texture);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, image.w, image.h, 0, GL_RGBA, GL_UNSIGNED_BYTE, image.rgba.data());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter == 1 ? GL_NEAREST : GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter == 1 ? GL_NEAREST : GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, address((image.filter >> 8) & 15));
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, address((image.filter >> 12) & 15));
        glEnable(GL_TEXTURE_2D);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
        glEnable(GL_BLEND);
        glBlendEquation(GL_FUNC_ADD);
        glDisable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
        Require(glGetError() == GL_NO_ERROR, "raw reference texture upload");
        Restore();
    }
    void Restore() const {
        Require(eglMakeCurrent(App.Display, App.Surface, App.Surface, App.Context), "restore module context");
    }
    ~ReferenceRenderer() { eglDestroyContext(App.Display, Context); }
    std::vector<std::uint8_t> Pixels(const RealtimeCloudCamera& camera, const RealtimeCloudState& state,
        float projectionFov = 60.0f, bool wrongAlphaBlend = false) const {
        Require(eglMakeCurrent(App.Display, App.Surface, App.Surface, Context), "reference GL current");
        glViewport(0, 0, camera.Width, camera.Height);
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glOrtho(0, camera.Width, camera.Height, 0, -1, 1);
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
        glClearColor(7.0f / 255, 11.0f / 255, 17.0f / 255, 0.25f);
        glClear(GL_COLOR_BUFFER_BIT);
        glBlendFunc(wrongAlphaBlend ? GL_SRC_ALPHA : GL_ONE, wrongAlphaBlend ? GL_ONE_MINUS_SRC_ALPHA : GL_ONE);
        const auto geometry = SourceOracle(camera, state, projectionFov);
        glBegin(GL_TRIANGLES);
        for (std::size_t i = 0; i < geometry.Size; ++i) {
            const auto& v = geometry.Vertices[i];
            glColor4ubv(v.Colour.data());
            glTexCoord2f(v.U, v.V);
            glVertex2f(v.X, v.Y);
        }
        glEnd();
        std::vector<std::uint8_t> pixels(camera.Width * camera.Height * 4);
        glReadPixels(0, 0, camera.Width, camera.Height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        Require(glGetError() == GL_NO_ERROR, "reference source-geometry pixel readback");
        Restore();
        return pixels;
    }
};

static void ComparePixels(const std::vector<std::uint8_t>& actual, const std::vector<std::uint8_t>& expected,
    const RealtimeCloudCamera& camera, const char* label) {
    Require(actual.size() == expected.size(), "reference pixel extent");
    std::size_t different = 0, beyondOne = 0;
    unsigned maximum = 0;
    std::uint64_t total = 0;
    for (std::size_t i = 0; i < actual.size(); ++i) {
        const auto delta = static_cast<unsigned>(std::abs(int(actual[i]) - int(expected[i])));
        different += delta != 0;
        beyondOne += delta > 1;
        maximum = std::max(maximum, delta);
        total += delta;
    }
    // Same-context-format, same-driver relational check, never a baked FNV.
    // Independent projection arithmetic can shift an edge by subpixel error.
    // Permit one byte of interpolation rounding, with a tightly bounded edge
    // population and aggregate error rather than insisting on cross-driver bits.
    const auto mean = static_cast<double>(total) / actual.size();
    std::printf("cloud-reference %s size=%dx%d near=%.3f far=%.1f projection=60 spriteFov=%.1f changed=%d differing-channels=%zu max-delta=%u beyond-one=%zu mean-delta=%.9f\n",
        label, camera.Width, camera.Height, camera.NearClip, camera.FarClip, camera.Fov,
        ChangedRgb(actual), different, maximum, beyondOne, mean);
    Require(beyondOne <= actual.size() / 100000 && mean < 0.001, "independent source/raw-texture/ONE-ONE pixel tolerance");
}

static void WritePpm(const char* path, const std::vector<std::uint8_t>& pixels) {
    auto* file = std::fopen(path, "wb");
    Require(file, "capture open");
    std::fprintf(file, "P6\n%d %d\n255\n", kWidth, kHeight);
    for (int y = kHeight - 1; y >= 0; --y) {
        for (int x = 0; x < kWidth; ++x) {
            Require(std::fwrite(&pixels[(static_cast<std::size_t>(y) * kWidth + x) * 4], 1, 3, file) == 3, "capture write");
        }
    }
    Require(std::fclose(file) == 0, "capture close");
}
} // namespace

int main(int argc, char** argv) {
    const char* gameDir = argc > 1 ? argv[1] : "/game";
    const char* outputDir = argc > 2 ? argv[2] : "artifacts/graphics";
    OS_SetFilePathOffset(gameDir);
    StartReferenceParser();
    const auto referenceImage = ReferenceCloudImage();
    const auto lowColour = ReferenceLowColour("CLOUDY_LA", 4); // exact Midday row
    const auto* previousDictionary = rw::TexDictionary::getCurrent();
    char error[512]{};
    RealtimeClouds clouds;
    Require(clouds.Load(gameDir, error, sizeof(error)), error);
    Require(rw::TexDictionary::getCurrent() == previousDictionary, "cloud load restores current TXD");
    const auto* image = clouds.PreparedImage();
    Require(image && std::strcmp(image->name, "cloud1") == 0 && image->w > 0 && image->h > 0,
        "actual prepared cloud1 metadata");
    Require(image->filter == referenceImage.filter && image->w == referenceImage.w && image->h == referenceImage.h &&
        image->rgba == referenceImage.rgba, "independent raw native cloud1 RGBA/filter witness");
    std::size_t transparent = 0, partialAlpha = 0, opaque = 0;
    for (std::size_t offset = 3; offset < image->rgba.size(); offset += 4) {
        transparent += image->rgba[offset] == 0;
        partialAlpha += image->rgba[offset] != 0 && image->rgba[offset] != 255;
        opaque += image->rgba[offset] == 255;
    }
    Require(transparent + partialAlpha + opaque == static_cast<std::size_t>(image->w) * image->h,
        "cloud1 alpha metadata count");
    std::printf("cloud-source layer=low texture=%s size=%dx%d filter=0x%04x bytes=%zu fnv=%016llx lowRGB=%u,%u,%u alpha=zero:%zu,partial:%zu,opaque:%zu\n",
        image->name, image->w, image->h, image->filter, image->rgba.size(),
        static_cast<unsigned long long>(Hash(image->rgba)), lowColour[0], lowColour[1], lowColour[2],
        transparent, partialAlpha, opaque);
    GeometryOracle(lowColour);

    // Parser handoff: no asset IO or RW operation occurs below this point.
    auto egl = MakeEgl();
    std::printf("cloud-GL renderer=%s\n", glGetString(GL_RENDERER));
    const auto camera = CaptureProductionCamera(egl, kWidth, kHeight, 1600.0f);
    const ReferenceRenderer reference(egl, referenceImage);
    const auto hostileProgram = HostileState();
    GLuint unpackBuffer{};
    glGenBuffers(1, &unpackBuffer);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, unpackBuffer);
    glBufferData(GL_PIXEL_UNPACK_BUFFER, 4096, nullptr, GL_STATIC_DRAW);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 19);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 3);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 2);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 8);
    glPixelTransferf(GL_RED_SCALE, 0.3f);
    glPixelTransferf(GL_RED_BIAS, 0.2f);
    const auto uploadState = Snapshot();
    Require(clouds.Upload(error, sizeof(error)), error);
    Require(clouds.IsUploaded() && Snapshot() == uploadState, "cloud upload restores hostile GL/pixel state");
    glPixelTransferf(GL_RED_SCALE, 1.0f);
    glPixelTransferf(GL_RED_BIAS, 0.0f);

    auto state = State(lowColour);
    const auto pixels = Render(clouds, camera, state, RealtimeCloudResult::Ready);
    const auto referencePixels = reference.Pixels(camera, state);
    ComparePixels(pixels, referencePixels, camera, "actual-Camera::Apply");
    Require(reference.Pixels(camera, state, 70.0f) != referencePixels, "pixel witness rejects conflated 70/70 projection");
    auto wrongSizing = camera;
    wrongSizing.Fov = 60.0f;
    Require(reference.Pixels(wrongSizing, state) != referencePixels, "pixel witness rejects conflated 60/60 sizing");
    Require(reference.Pixels(camera, state, 60.0f, true) != referencePixels, "pixel witness rejects conventional alpha blend");
    Require(ChangedRgb(pixels) > 1000, "real cloud1 contributes pixels");
    char capture[1024];
    std::snprintf(capture, sizeof(capture), "%s/realtime-clouds-low.ppm", outputDir);
    WritePpm(capture, pixels);

    for (const auto& extent : {std::array{640, 448}, std::array{1280, 720}, std::array{720, 1280}}) {
        for (const float farClip : {100.0f, 1600.0f, 3000.0f}) {
            const auto alternate = CaptureProductionCamera(egl, extent[0], extent[1], farClip, 1.15f, -0.08f, -0.2f);
            const auto actual = Render(clouds, alternate, state, RealtimeCloudResult::Ready);
            ComparePixels(actual, reference.Pixels(alternate, state), alternate, "aspect/far/rotation");
            auto farOnly = alternate;
            farOnly.FarClip = 50.0f;
            Require(Render(clouds, farOnly, state, RealtimeCloudResult::Ready) == actual,
                "source low-cloud call disables far rejection");
        }
    }
    // Same real installed production matrix, explicit source near metadata.
    // Exercise partial near rejection (exact boundary checked by GeometryOracle).
    for (const float nearClip : {1.0f, 500.0f, 900.0f}) {
        auto nearCamera = camera;
        nearCamera.NearClip = nearClip;
        const auto actual = Render(clouds, nearCamera, state, RealtimeCloudResult::Ready);
        ComparePixels(actual, reference.Pixels(nearCamera, state), nearCamera, "source-near-metadata");
    }
    for (int dominant = 0; dominant < 3; ++dominant) {
        auto weather = state;
        weather.Foggyness = dominant == 0 ? 0.8f : 0.1f;
        weather.CloudCoverage = dominant == 1 ? 0.8f : 0.1f;
        weather.ExtraSunnyness = dominant == 2 ? 0.8f : 0.1f;
        const auto actual = Render(clouds, camera, weather, RealtimeCloudResult::Ready);
        ComparePixels(actual, reference.Pixels(camera, weather), camera, "max-weather-balance");
        Require(actual != pixels, "dominant weather balance changes additive RGB");
        RealtimeCloudGeometry geometry;
        Require(RealtimeClouds::BuildLowClouds(camera, weather, geometry) == RealtimeCloudResult::Ready && geometry.Size,
            "weather geometry");
        for (const auto& v : std::span{geometry.Vertices}.first(geometry.Size)) {
            Require(v.Colour[3] == 255, "low-cloud alpha stays 255; weather attenuates RGB");
        }
    }

    auto reversed = state;
    reversed.Wind = -state.Wind;
    reversed.Hour = 23.75f;
    reversed.GameMs = 0xffffffffu;
    Require(Render(clouds, camera, reversed, RealtimeCloudResult::Ready) == pixels,
        "low-cloud GL pixels are independent of reversed wind/time");
    auto rotatedCamera = camera;
    rotatedCamera.Roll = 0.31f;
    Require(Render(clouds, rotatedCamera, state, RealtimeCloudResult::Ready) != pixels,
        "camera roll rotates actual cloud pixels");
    rotatedCamera = Camera(0, 0, 45, 1.15f, -0.08f, -0.2f);
    Require(Render(clouds, rotatedCamera, state, RealtimeCloudResult::Ready) != pixels,
        "camera yaw/pitch changes actual cloud projection");

    auto zero = state;
    zero.CloudCoverage = 1.0f;
    Require(ChangedRgb(Render(clouds, camera, zero, RealtimeCloudResult::Ready)) == 0,
        "coverage one has zero additive RGB contribution");
    zero = state;
    zero.LowCloudColours.fill(0);
    Require(ChangedRgb(Render(clouds, camera, zero, RealtimeCloudResult::Ready)) == 0,
        "zero source low-cloud RGB has zero additive contribution");
    auto hidden = state;
    hidden.CanSeeOutside = false;
    Require(ChangedRgb(Render(clouds, camera, hidden, RealtimeCloudResult::Hidden)) == 0,
        "interior visibility suppresses cloud draw");
    auto unknown = state;
    unknown.HasWeather = false;
    Require(ChangedRgb(Render(clouds, camera, unknown, RealtimeCloudResult::Unsupported)) == 0,
        "unknown weather never enables fallback clouds");

    std::printf("cloud-pixels changed=%d fnv=%016llx wind/time-invariant camera-rotation-sensitive full-coverage/zero-RGB/interior/unknown-clear state-restored\n",
        ChangedRgb(pixels), static_cast<unsigned long long>(Hash(pixels)));
    clouds.ReleaseGpu();
    glDeleteBuffers(1, &unpackBuffer);
    glDeleteProgram(hostileProgram);
    std::puts("cloud-proof-limit caller-source plus native sprite-adapter reference; 0x70EAB0 callback not independently reversed; raw witness packed32 mip0 only; local cloud1 opaque so SRC_ALPHA/ONE equals ONE/ONE; no cross-driver pixel hash oracle");
    std::puts("realtime-clouds-probe-ok layer=low only");
    return 0;
}
