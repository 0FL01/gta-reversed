// Actual offscreen compatibility-GL draw, owned atlas/metrics oracle. No captures.
// Reuse the existing HUD probe's GL-state/readback helpers without changing it.
// Its renamed, uncalled main relied on main's implicit return; its historical
// aggregate fixtures also predate the script/help fields. Scope diagnostics to it.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wreturn-type"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#define main RealtimeHudOriginalProbeMain
#include "app/platform/linux/RealtimeHudProbe.cpp"
#undef main
#pragma GCC diagnostic pop

static void Camera(int width, int height) {
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();
    glMatrixMode(GL_PROJECTION); glLoadIdentity();
    const double top = .1 * std::tan(std::numbers::pi / 6);
    glFrustum(-top * width / height, top * width / height, -top, top, .1, 1600);
}

static NativeScriptPropertyLabel Label(float x, float y, float z, std::string text = "$30000", int alpha = 163) {
    NativeScriptPropertyLabel label;
    label.Position = {x, y, z}; label.Text = std::move(text); label.Alpha = alpha;
    return label;
}

static void Clear() {
    glPushAttrib(GL_ALL_ATTRIB_BITS);
    glDisable(GL_SCISSOR_TEST); glColorMask(1, 1, 1, 1); glDepthMask(1); glStencilMask(255);
    glClearColor(0, 0, 0, 0); glClearDepth(.375); glClearStencil(42);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    glPopAttrib();
}

static std::vector<uint8_t> Prices(const RealtimeHud& hud, std::span<const NativeScriptPropertyLabel> labels, int width, int height) {
    Clear();
    const auto before = Snapshot();
    hud.DrawPropertyPrices(labels, width, height, .1f, 100, 60);
    Require(before == Snapshot(), "price restores hostile attributes/program/matrices/texture units");
    std::vector<uint8_t> rgba(width * height * 4), stencil(width * height);
    std::vector<float> depth(width * height);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    glReadPixels(0, 0, width, height, GL_DEPTH_COMPONENT, GL_FLOAT, depth.data());
    glReadPixels(0, 0, width, height, GL_STENCIL_INDEX, GL_UNSIGNED_BYTE, stencil.data());
    for (std::size_t i = 0; i < depth.size(); ++i) Require(Near(depth[i], .375f) && stencil[i] == 42, "price never writes depth/stencil");
    Require(glGetError() == GL_NO_ERROR, "price GL error");
    return rgba;
}

static void ProjectionProbe(const RealtimeHud& hud) {
    Camera(640, 448);
    auto view = RealtimeHud::CapturePriceView(.1f, 100, 60);
    std::vector<NativeScriptPropertyLabel> labels;
    // More than16 rejected candidates must not starve the later visible ones.
    for (int i = 0; i < 24; ++i) labels.push_back(Label(0, 0, i % 3 == 0 ? -1.1f : i % 3 == 1 ? -100.f : 10.f));
    for (int i = 0; i < 17; ++i) labels.push_back(Label(float(i % 4 - 2) * 4, float(i / 4 - 2) * 3, -20));
    const auto projected = RealtimeHud::ProjectPropertyPrices(labels, view, 640, 448);
    Require(projected.size() == 16 && projected.front().Candidate == 24 && projected.back().Candidate == 39,
        "pool-order cap16 after near/far/behind rejection");
    Require(Near(projected[0].Depth, 20) && Near(projected[0].Width, 37.333333f) &&
        Near(projected[0].Height, 26.133333f) && Near(projected[0].ScaleX, 1) && Near(projected[0].ScaleY, .871111f),
        "source depth/FOV dimensions (not projected geometry or NDC depth)");
    const auto full = Prices(hud, labels, 640, 448);
    Require(full == Prices(hud, std::span(labels).subspan(24, 16), 640, 448), "GPU admits later16 exactly");
    labels[24].Position.X = 10000;
    labels[24].Alpha = 0;
    const auto outside = RealtimeHud::ProjectPropertyPrices(labels, view, 640, 448);
    Require(outside.size() == 16 && outside.front().Candidate == 24 && outside.front().X > 640 && outside.back().Candidate == 39,
        "offscreen XY and zero-alpha still consume source slots");
    glMatrixMode(GL_MODELVIEW); glLoadIdentity(); glRotatef(90, 0, 1, 0); glTranslatef(-10, -20, -30);
    const auto rotatedView = RealtimeHud::CapturePriceView(.1f, 100, 60);
    const std::array rotated{Label(20, 20, 30)};
    const auto p = RealtimeHud::ProjectPropertyPrices(rotated, rotatedView, 640, 448);
    Require(p.size() == 1 && Near(p[0].X, 320) && Near(p[0].Y, 224) && Near(p[0].Depth, 10), "live translated/rotated world matrix projection");
    std::puts("sale-projection PASS near<=1.1 far>=100 behind rejected24 accepted16 pool-order XY/alpha consume rotated depth20 W=37.333333 H=26.133333 scale=1,0.871111");
}

static unsigned OracleGlyph(unsigned char ch) {
    if (ch == '$') return 93;
    if (ch >= '0' && ch <= '9') return 144 + ch - '0';
    if (ch >= 'A' && ch <= 'Z') return 155 + ch - 'A';
    if (ch >= 'a' && ch <= 'z') return 155 + ch - 'a';
    Require(false, "oracle alphabet"); return 0;
}

static void PriceOracle(const RealtimeHud& hud, const MenuHudFont& font, int width, int height, float depth, int alpha, const char* text) {
    Camera(width, height);
    const std::array labels{Label(0, 0, -depth, text, alpha)};
    const auto actual = Prices(hud, labels, width, height);
    const double sx = std::min(width / 640., width * 70. / (depth * 60 * 30));
    const double sy = std::min(width / 640., height * 70. / (depth * 60 * 30));
    double advance = 0;
    for (const unsigned char ch : labels[0].Text) advance += font.prop[OracleGlyph(ch)] * sx;
    int samples = 0, bad = 0, ink = 0;
    for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
        std::array<double, 4> expected{};
        double left = width / 2. - advance / 2;
        for (const unsigned char ch : labels[0].Text) {
            const auto glyph = OracleGlyph(ch);
            const double localX = x + .5 - left, localY = y + .5 - height / 2.;
            if (localX >= 0 && localX < 32 * sx && localY >= 0 && localY < 20 * sy) {
                const auto texel = Sample(font.rgba, font.w, font.h,
                    (glyph % 16 * 32 + .5 + localX / (32 * sx) * 31) / 512,
                    (glyph / 16 * 40 + .5 + localY / (20 * sy) * 39) / 512);
                const double a = texel[3] / 255. * alpha / 255.;
                for (int c = 0; c < 3; ++c) expected[c] = texel[c] * labels[0].Color[c] / 255. * a + expected[c] * (1 - a);
                expected[3] = 255 * a + expected[3] * (1 - a);
            }
            left += font.prop[glyph] * sx;
        }
        const auto* pixel = &actual[((height - 1 - y) * width + x) * 4];
        for (int c = 0; c < 4; ++c) bad += std::abs(pixel[c] - expected[c]) > 2.5;
        ++samples; ink += expected[3] > 20;
    }
    std::printf("sale-font %dx%d text=%s depth=%g alpha=%d scale=%.9f,%.9f proportionalWidth=%.9f samples=%d ink=%d badRGBA=%d\n",
        width, height, text, depth, alpha, sx, sy, advance, samples, ink, bad);
    Require(bad == 0 && (alpha ? ink > 40 : ink == 0), "actual GPU font1 glyphs/width/centering/scale/RGB/alpha/no background oracle");
}

static void RadarOracle(const RealtimeHud& hud, const WorldShotImage& image, int sprite) {
    RealtimeHudState state;
    state.playerX = 2495; state.playerY = -1685; state.hour = 12; state.minute = 34;
    const RealtimeHudView view{kPi / 2, true, false};
    const auto base = Render(hud, view, state, 640, 448);
    NativeScriptRadarBlip blip;
    blip.Position = {2585, -1685, 0}; blip.Sprite = sprite; blip.Active = true;
    const std::array blips{blip}; state.scriptBlips = blips;
    const auto actual = Render(hud, view, state, 640, 448);
    int bad = 0, ink = 0;
    for (int y = 374; y < 390; ++y) for (int x = 103; x < 118; ++x) {
        const auto texel = Sample(image.rgba, image.w, image.h, (x + .5 - 102.5) / 16, (y + .5 - 374) / 16);
        const auto offset = ((448 - 1 - y) * 640 + x) * 4;
        const double a = texel[3] / 255.;
        for (int c = 0; c < 4; ++c) {
            const double expected = c == 3 ? texel[3] + base[offset + c] * (1 - a) : texel[c] * a + base[offset + c] * (1 - a);
            bad += std::abs(actual[offset + c] - expected) > 2.5;
        }
        ink += texel[3] > 100;
    }
    std::printf("sale-radar sprite=%d size=%dx%d ink=%d badRGBA=%d\n", sprite, image.w, image.h, ink, bad);
    Require(bad == 0 && ink > 20, "actual sprite31/32 owned RGBA compositing");
}

int main(int argc, char** argv) {
    const char* game = argc > 1 ? argv[1] : "/game";
    char error[512]{}; std::string message;
    RealtimeHud hud; MenuHudFont font; WorldShotImage green, red;
    RadarMapAssets radar;
    Require(hud.Load(game, error, sizeof(error)), error);
    Require(RadarMap_LoadAssets(game, radar, error, sizeof(error)), error);
    Require(MenuShot_LoadPricedownFont(game, font, error, sizeof(error)), error);
    Require(NativeScriptEntities_LoadRadar(game, green, message, 31), message.c_str());
    Require(NativeScriptEntities_LoadRadar(game, red, message, 32), message.c_str());
    Require(green.rgba != red.rgba, "green and red are distinct owned sprites");
    for (unsigned char ch : std::string("$0123456789Buy")) {
        const auto glyph = OracleGlyph(ch);
        Require(PricedownGlyph(ch) == glyph, "source pricedown mapping");
        int maxAlpha = 0;
        for (int y = 0; y < 40; ++y) for (int x = 0; x < 32; ++x)
            maxAlpha = std::max(maxAlpha, int(font.rgba[((glyph / 16 * 40 + y) * font.w + glyph % 16 * 32 + x) * 4 + 3]));
        std::printf("sale-owned-font ch=%c glyph=%u width=%d maxAlpha=%d\n", ch, glyph, font.prop[glyph], maxAlpha);
    }
    // Parser handoff: no asset/RW reads below here.
    const auto getDisplay = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
    Require(getDisplay, "EGL entry point");
    const auto display = getDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
    Require(eglInitialize(display, nullptr, nullptr) && eglBindAPI(EGL_OPENGL_API), "EGL initialize");
    const EGLint attributes[]{EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8, EGL_NONE};
    EGLConfig config{}; EGLint count{};
    Require(eglChooseConfig(display, attributes, &config, 1, &count) && count, "EGL config");
    const EGLint size[]{EGL_WIDTH, 1280, EGL_HEIGHT, 896, EGL_NONE};
    const auto surface = eglCreatePbufferSurface(display, config, size);
    const auto context = eglCreateContext(display, config, EGL_NO_CONTEXT, nullptr);
    Require(eglMakeCurrent(display, surface, surface, context), "EGL context");
    std::printf("sale-GL renderer=%s\n", glGetString(GL_RENDERER));
    const auto program = HostileState();
    const auto before = Snapshot();
    Require(hud.Upload(error, sizeof(error)), error);
    Require(before == Snapshot(), "sale Upload state restored");
    ProjectionProbe(hud);
    PriceOracle(hud, font, 640, 448, 20, 163, "$30000");
    PriceOracle(hud, font, 640, 448, 50, 255, "$1234567890");
    PriceOracle(hud, font, 1280, 720, 20, 128, "$30000");
    PriceOracle(hud, font, 640, 448, 10, 255, "Buy");
    PriceOracle(hud, font, 640, 448, 20, 0, "$30000");
    RadarOracle(hud, green, 31); RadarOracle(hud, red, 32);
    GeometryProbe();
    RealtimeHudState state;
    state.playerX = 2495; state.playerY = -1685; state.hour = 23; state.minute = 59;
    const RealtimeHudView view{kPi / 2};
    const auto legacy = Render(hud, view, state, 640, 448);
    FontOracle(legacy, font, 23, 59, 640, 448);
    MapOracle(legacy, radar, view, state, 640, 448);
    MarkerOracle(legacy, radar, view, state, 640, 448);
    hud.ReleaseGpu(); glUseProgram(0); glDeleteProgram(program);
    Require(glGetError() == GL_NO_ERROR, "sale cleanup GL");
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(display, context); eglDestroySurface(display, surface); eglTerminate(display);
    std::puts("realtime-hud-sale-probe PASS");
}
