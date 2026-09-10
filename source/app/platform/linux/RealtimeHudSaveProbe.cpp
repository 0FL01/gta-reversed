// Actual SCM sprite33 contact rendered through the production HUD into EGL.
// Reuse the established hostile-state/readback oracles without changing them.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wreturn-type"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#define main RealtimeHudOriginalProbeMain
#include "app/platform/linux/RealtimeHudProbe.cpp"
#undef main
#pragma GCC diagnostic pop

namespace {
struct ProbeFile {
    void* Handle{};
    ~ProbeFile() { if (Handle) OS_FileClose(Handle); }
};

struct ProbeDictionary {
    rw::TexDictionary* Previous = rw::TexDictionary::getCurrent();
    rw::TexDictionary* Value{};
    ~ProbeDictionary() {
        rw::TexDictionary::setCurrent(Previous);
        if (Value) Value->destroy();
    }
};

static std::vector<std::uint8_t> ProbeRead(const char* path) {
    ProbeFile file;
    Require(OS_FileOpen(FILE_DATA_AREA_DEFAULT, &file.Handle, path, FILE_ACCESS_READ) == 0 && file.Handle, "owned probe asset open");
    const auto size = OS_FileSize(file.Handle);
    Require(size > 0, "owned probe asset size");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    Require(OS_FileRead(file.Handle, bytes.data(), size) == 0, "owned probe asset read");
    return bytes;
}

static std::uint32_t ProbeWord(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    Require(offset + 4 <= bytes.size(), "SCM word bounds");
    return std::uint32_t(bytes[offset]) | std::uint32_t(bytes[offset + 1]) << 8 |
        std::uint32_t(bytes[offset + 2]) << 16 | std::uint32_t(bytes[offset + 3]) << 24;
}

static int ActualSprite() {
    const auto scm = ProbeRead("data/script/main.scm");
    std::size_t header = 0;
    header = ProbeWord(scm, header + 3);
    header = ProbeWord(scm, header + 3);
    const auto scriptStart = ProbeWord(scm, header + 24);
    std::size_t pos = scriptStart + (205876 - 200000);
    Require(pos + 16 <= scm.size() && scm[pos] == 0x70 && scm[pos + 1] == 0x05, "actual 0570@205876 opcode");
    pos += 2;
    for (int i = 0; i < 3; ++i) {
        Require(scm[pos] == 2, "actual 0570 coordinate operand");
        pos += 3;
    }
    Require(scm[pos] == 4, "actual 0570 sprite operand type");
    const auto sprite = static_cast<std::int8_t>(scm[pos + 1]);
    pos += 2;
    Require(scm[pos] == 2 && sprite == 33, "actual 0570 sprite33/output operand");
    return sprite;
}

static WorldShotImage ReferenceImage(const char* gameDir) {
    OS_SetFilePathOffset(gameDir);
    auto bytes = ProbeRead("models/hud.txd");
    rw::StreamMemory stream;
    stream.open(bytes.data(), static_cast<uint32>(bytes.size()));
    ProbeDictionary dictionary;
    if (rw::findChunk(&stream, rw::ID_TEXDICTIONARY, nullptr, nullptr)) dictionary.Value = rw::TexDictionary::streamRead(&stream);
    stream.close();
    auto* texture = dictionary.Value ? dictionary.Value->find("radar_race") : nullptr;
    WorldShotImage image{};
    Require(texture && TexSample_Decode(texture, image), "independent hud.txd:radar_race decode");
    std::snprintf(image.name, sizeof(image.name), "%s", texture->name);
    image.filter = texture->filterAddressing;
    return image;
}

static std::vector<std::uint8_t> DrawBlip(const RealtimeHud& hud, const RealtimeHudView& view,
    RealtimeHudState state, const NativeScriptRadarBlip& blip, int width, int height) {
    const std::array blips{blip};
    state.scriptBlips = blips;
    return Render(hud, view, state, width, height);
}

static int Difference(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b) {
    Require(a.size() == b.size(), "readback size match");
    int changed = 0;
    for (std::size_t i = 0; i < a.size(); i += 4) changed += !std::equal(a.begin() + i, a.begin() + i + 4, b.begin() + i);
    return changed;
}

static void ExactSpriteOracle(const RealtimeHud& hud, const WorldShotImage& image,
    const RealtimeHudView& view, const RealtimeHudState& state, NativeScriptRadarBlip blip,
    const std::vector<std::uint8_t>& base, const char* label) {
    constexpr int width = 640, height = 448;
    const auto actual = DrawBlip(hud, view, state, blip, width, height);
    double rx = ((blip.Position.X - state.playerX) * std::sin(view.cameraYaw) -
        (blip.Position.Y - state.playerY) * std::cos(view.cameraYaw)) / state.radarRange;
    double ry = ((blip.Position.X - state.playerX) * std::cos(view.cameraYaw) +
        (blip.Position.Y - state.playerY) * std::sin(view.cameraYaw)) / state.radarRange;
    const auto distance = std::hypot(rx, ry);
    if (distance > 1) { rx /= distance; ry /= distance; }
    const double centerX = (87 + 47 * rx) * width / 640;
    const double centerY = (382 - 38 * ry) * height / 448;
    const int left = static_cast<int>(std::lround(centerX - 8));
    const int top = static_cast<int>(std::lround(centerY - 8));
    Require(std::abs(centerX - 8 - left) < .001 && std::abs(centerY - 8 - top) < .001,
        "exact oracle integer sprite bounds");
    int bad = 0, outside = 0, ink = 0, partial = 0;
    for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
        const auto offset = (std::size_t(height - 1 - y) * width + x) * 4;
        if (x < left || x >= left + 16 || y < top || y >= top + 16) {
            outside += !std::equal(actual.begin() + offset, actual.begin() + offset + 4, base.begin() + offset);
            continue;
        }
        const auto source = (std::size_t(y - top) * 16 + x - left) * 4;
        const double alpha = image.rgba[source + 3] / 255.0;
        for (int c = 0; c < 4; ++c) {
            const double expected = c == 3 ? image.rgba[source + 3] + base[offset + c] * (1 - alpha)
                : image.rgba[source + c] * alpha + base[offset + c] * (1 - alpha);
            bad += std::abs(actual[offset + c] - expected) > 2.5;
        }
        ink += image.rgba[source + 3] != 0;
        partial += image.rgba[source + 3] != 0 && image.rgba[source + 3] != 255;
    }
    std::printf("save-radar-exact %s center=%.3f,%.3f rect=%d,%d,16,16 ink=%d partialAlpha=%d badRGBA=%d outsideDelta=%d\n",
        label, centerX, centerY, left, top, ink, partial, bad, outside);
    Require(ink > 100 && ink < 256 && bad == 0 && outside == 0,
        "exact source RGBA/alpha, projection and other HUD unchanged");
}

static std::array<float, 4> FilterSample(const WorldShotImage& image, double u, double v) {
    switch (image.filter & 0xff) {
    case rw::Texture::NEAREST:
    case rw::Texture::MIPNEAREST:
    case rw::Texture::LINEARMIPNEAREST: {
        const int x = std::clamp(static_cast<int>(u * image.w), 0, image.w - 1);
        const int y = std::clamp(static_cast<int>(v * image.h), 0, image.h - 1);
        std::array<float, 4> value{};
        std::copy_n(image.rgba.begin() + (std::size_t(y) * image.w + x) * 4, 4, value.begin());
        return value;
    }
    default: return Sample(image.rgba, image.w, image.h, u, v);
    }
}

static void FilterOracle(const RealtimeHud& hud, const WorldShotImage& image,
    const RealtimeHudView& view, const RealtimeHudState& state, NativeScriptRadarBlip blip,
    const std::vector<std::uint8_t>& base) {
    constexpr int width = 1280, height = 896, left = 206, top = 748;
    blip.Position.X = state.playerX + state.radarRange * 24.0f / 47.0f;
    blip.Position.Y = state.playerY;
    const auto actual = DrawBlip(hud, view, state, blip, width, height);
    int bad = 0, ink = 0;
    for (int y = top; y < top + 32; ++y) for (int x = left; x < left + 32; ++x) {
        const auto texel = FilterSample(image, (x + .5 - left) / 32, (y + .5 - top) / 32);
        const auto offset = (std::size_t(height - 1 - y) * width + x) * 4;
        const double alpha = texel[3] / 255;
        for (int c = 0; c < 4; ++c) {
            const double expected = c == 3 ? texel[3] + base[offset + c] * (1 - alpha)
                : texel[c] * alpha + base[offset + c] * (1 - alpha);
            bad += std::abs(actual[offset + c] - expected) > 2.5;
        }
        ink += texel[3] > 20;
    }
    std::printf("save-radar-filter source=0x%04x mode=%u 16x16-to-32x32 ink=%d badRGBA=%d\n",
        image.filter, image.filter & 0xff, ink, bad);
    Require(ink > 30 && bad == 0, "source texture filter in actual GL draw");
}
} // namespace

int main(int argc, char** argv) {
    const char* game = argc > 1 ? argv[1] : "/game";
    char error[512]{};
    OS_SetFilePathOffset(game);
    const auto actualSprite = ActualSprite();
    RealtimeHud hud;
    Require(!hud.PreparedRadarSprite(actualSprite) && !hud.IsRadarSpriteUploaded(actualSprite), "unloaded readiness is false");
    Require(hud.Load(game, error, sizeof(error)), error);
    const auto reference = ReferenceImage(game);
    const auto* prepared = hud.PreparedRadarSprite(actualSprite);
    Require(prepared && std::strcmp(prepared->name, "radar_race") == 0 && prepared->w == 16 && prepared->h == 16,
        "source sprite33 name/dimensions");
    Require(prepared->filter == reference.filter && prepared->rgba == reference.rgba,
        "independent exact owned radar_race image/filter");
    Require(prepared->rgba != hud.PreparedRadarSprite(31)->rgba && prepared->rgba != hud.PreparedRadarSprite(32)->rgba,
        "sprite33 is not a property sprite fallback");
    Require(!hud.PreparedRadarSprite(35) && !hud.IsRadarSpriteUploaded(actualSprite), "only prepared numeric IDs report ready");
    RadarMapAssets radar;
    MenuHudFont font;
    Require(RadarMap_LoadAssets(game, radar, error, sizeof(error)), error);
    Require(MenuShot_LoadPricedownFont(game, font, error, sizeof(error)), error);
    std::printf("save-radar-source actual=0570@205876 sprite=%d image=%s size=%dx%d filter=0x%04x bytes=%zu\n",
        actualSprite, prepared->name, prepared->w, prepared->h, prepared->filter, prepared->rgba.size());

    // Parser handoff: no OS/RW reads occur below this point.
    const auto getDisplay = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
    Require(getDisplay, "surfaceless EGL entry point");
    const auto display = getDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
    Require(display != EGL_NO_DISPLAY && eglInitialize(display, nullptr, nullptr) && eglBindAPI(EGL_OPENGL_API), "EGL initialize");
    const EGLint attributes[]{EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8, EGL_NONE};
    EGLConfig config{}; EGLint count{};
    Require(eglChooseConfig(display, attributes, &config, 1, &count) && count, "EGL config");
    const EGLint size[]{EGL_WIDTH, 1280, EGL_HEIGHT, 896, EGL_NONE};
    const auto surface = eglCreatePbufferSurface(display, config, size);
    const auto context = eglCreateContext(display, config, EGL_NO_CONTEXT, nullptr);
    Require(surface != EGL_NO_SURFACE && context != EGL_NO_CONTEXT && eglMakeCurrent(display, surface, surface, context), "EGL context");
    std::printf("save-radar-GL renderer=%s\n", glGetString(GL_RENDERER));
    const auto program = HostileState();
    GLuint pbo{};
    glGenBuffers(1, &pbo); glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbo); glBufferData(GL_PIXEL_UNPACK_BUFFER, 4096, nullptr, GL_STATIC_DRAW);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 19); glPixelStorei(GL_UNPACK_SKIP_ROWS, 3);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 2); glPixelStorei(GL_UNPACK_ALIGNMENT, 8);
    glPixelTransferf(GL_RED_SCALE, .3f); glPixelTransferf(GL_RED_BIAS, .2f);
    const auto uploadState = Snapshot();
    Require(hud.Upload(error, sizeof(error)), error);
    Require(uploadState == Snapshot() && hud.IsRadarSpriteUploaded(actualSprite), "upload state restored and sprite33 ready");
    glPixelTransferf(GL_RED_SCALE, 1); glPixelTransferf(GL_RED_BIAS, 0);

    RealtimeHudView view{kPi / 2, true, true};
    RealtimeHudState state;
    state.playerX = 2495; state.playerY = -1685; state.radarRange = 180; state.hour = 23; state.minute = 59;
    const auto base2x = Render(hud, view, state, 1280, 896);
    const auto base1x = Render(hud, view, state, 640, 448);
    MapOracle(base2x, radar, view, state, 1280, 896);
    FontOracle(base2x, font, state.hour, state.minute, 1280, 896);
    MarkerOracle(base2x, radar, view, state, 1280, 896);

    NativeScriptRadarBlip blip;
    blip.Position = {state.playerX + state.radarRange * 24.0f / 47.0f, state.playerY, 13};
    blip.Sprite = actualSprite; blip.Display = 2; blip.Active = true; blip.ShortRange = true; blip.Contact = true;
    ExactSpriteOracle(hud, reference, view, state, blip, base1x, "midrange");
    FilterOracle(hud, reference, view, state, blip, base2x);
    auto both = blip; both.Display = 3;
    Require(DrawBlip(hud, view, state, both, 640, 448) == DrawBlip(hud, view, state, blip, 640, 448), "BOTH and BLIPONLY display sprite33 identically");

    auto hidden = blip; hidden.Active = false;
    Require(DrawBlip(hud, view, state, hidden, 640, 448) == base1x, "inactive sprite33 hidden");
    hidden = blip; hidden.Display = 0;
    Require(DrawBlip(hud, view, state, hidden, 640, 448) == base1x, "display zero sprite33 hidden");
    hidden = blip; hidden.Display = 1;
    Require(DrawBlip(hud, view, state, hidden, 640, 448) == base1x, "marker-only sprite33 hidden");
    auto gatedState = state; gatedState.playerOnMission = true;
    Require(DrawBlip(hud, view, gatedState, blip, 640, 448) == base1x, "contact sprite33 hidden on mission");
    gatedState = state; gatedState.radarZoom = 1;
    Require(DrawBlip(hud, view, gatedState, blip, 640, 448) == base1x, "short-range sprite33 hidden while zoomed");
    gatedState = state; gatedState.exterior = false;
    Require(DrawBlip(hud, view, gatedState, blip, 640, 448) == base1x, "sprite33 hidden outside exterior radar");
    hidden = blip; hidden.Position.X = state.playerX + 181;
    Require(DrawBlip(hud, view, state, hidden, 640, 448) == base1x, "short-range sprite33 uses real pre-clamp distance");

    auto rim = blip; rim.Position.X = state.playerX + 360; rim.ShortRange = false;
    ExactSpriteOracle(hud, reference, view, state, rim, base1x, "rim-clamped");
    auto nonContact = blip; nonContact.Contact = false;
    auto missionState = state; missionState.playerOnMission = true;
    Require(Difference(DrawBlip(hud, view, missionState, nonContact, 640, 448), base1x) > 30,
        "mission gate applies to contact kind, not arbitrary coordinate blips");
    auto unsupported = blip; unsupported.Sprite = 35;
    Require(DrawBlip(hud, view, state, unsupported, 640, 448) == base1x, "unprepared sprite cannot fall back to property texture");
    auto property = blip; property.Sprite = 32;
    Require(DrawBlip(hud, view, state, property, 640, 448) != DrawBlip(hud, view, state, blip, 640, 448),
        "actual sprite33 output differs from propertyR");

    hud.ReleaseGpu();
    Require(!hud.IsRadarSpriteUploaded(actualSprite) && hud.PreparedRadarSprite(actualSprite), "release clears GPU readiness only");
    glUseProgram(0); glDeleteProgram(program); glDeleteBuffers(1, &pbo);
    Require(glGetError() == GL_NO_ERROR, "save radar cleanup GL");
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(display, context); eglDestroySurface(display, surface); eglTerminate(display);
    std::puts("realtime-hud-save-probe PASS fullport=0 service-validation=unchanged");
}
