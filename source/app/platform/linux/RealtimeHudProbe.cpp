// Isolated actual compatibility-GL overlay + independent source-coordinate oracle.
#include "app/platform/linux/RealtimeHud.cpp"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <cstdlib>

static void Require(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "hud-probe FAIL %s\n", message); std::exit(1); }
}

static bool Near(float a, float b, float epsilon = 0.002f) { return std::abs(a - b) < epsilon; }

static void GeometryProbe() {
    const RealtimeHudState state{2495, -1685, 0, 180, 12, 34};
    for (int direction = 0; direction < 4; ++direction) {
        const float yaw = direction * kPi / 2;
        const RealtimeHudView view{yaw};
        const Point ahead{state.playerX + 90 * std::cos(yaw), state.playerY + 90 * std::sin(yaw)};
        const auto p = WorldToRadar(ahead, view, state);
        Require(Near(p.x, 0) && Near(p.y, 0.5f), "camera forward must be radar up");
        const auto back = RadarToWorld(p, view, state);
        Require(Near(ahead.x, back.x) && Near(ahead.y, back.y), "world/radar inverse");
    }
    for (int y = 0; y < 12; ++y) for (int x = 0; x < 12; ++x) {
        auto nw = WorldToUv({x * 500.0f - 3000, 3000 - y * 500.0f}, x, y);
        auto se = WorldToUv({x * 500.0f - 2500, 2500 - y * 500.0f}, x, y);
        Require(nw.x == 0 && nw.y == 0 && se.x == 1 && se.y == 1, "all 144 tile north/west UV origins");
    }
    auto disc = ClipDisc({{-2, -2}, {2, -2}, {2, 2}, {-2, 2}});
    Require(disc.size() == 24, "source six chords per quadrant");
    float area = 0;
    auto prev = disc.back();
    for (auto p : disc) {
        Require(Near(p.x * p.x + p.y * p.y, 1), "mask vertex on unit circle");
        area += prev.x * p.y - prev.y * p.x;
        prev = p;
    }
    Require(Near(area / 2, 12 * std::sin(kPi / 12)), "source 24-gon area");
    Require(ClipDisc({{2, 2}, {3, 2}, {3, 3}, {2, 3}}).empty(), "outside tile clipped");
    Require(PricedownGlyph('0') == 144 && PricedownGlyph('9') == 153 && PricedownGlyph(':') == 154, "Pricedown digit/colon remap");
    std::puts("hud-geometry PASS 144 north-to-south tiles, cardinal camera transforms, inverse, 24-gon clipping, font1 subfont");
}

// Sample a decoded top-down image using OpenGL's normalized linear convention.
static std::array<float, 4> Sample(const std::vector<uint8_t>& image, int width, int height, double u, double v) {
    const double x = u * width - 0.5, y = v * height - 0.5;
    const int ix = int(std::floor(x)), iy = int(std::floor(y));
    const double fx = x - ix, fy = y - iy;
    std::array<float, 4> value{};
    for (int dy = 0; dy < 2; ++dy) for (int dx = 0; dx < 2; ++dx) {
        const int sx = std::clamp(ix + dx, 0, width - 1), sy = std::clamp(iy + dy, 0, height - 1);
        const double weight = (dx ? fx : 1 - fx) * (dy ? fy : 1 - fy);
        for (int c = 0; c < 4; ++c) value[c] += image[(sy * width + sx) * 4 + c] * weight;
    }
    return value;
}

static std::vector<float> Snapshot() {
    std::vector<float> out;
    const auto add = [&](GLenum name, int count = 1) {
        GLfloat values[16]{};
        glGetFloatv(name, values);
        out.insert(out.end(), values, values + count);
    };
    for (auto name : {GL_CURRENT_PROGRAM, GL_ACTIVE_TEXTURE, GL_MATRIX_MODE,
        GL_BLEND_SRC_RGB, GL_BLEND_DST_RGB, GL_BLEND_SRC_ALPHA, GL_BLEND_DST_ALPHA,
        GL_BLEND_EQUATION_RGB, GL_BLEND_EQUATION_ALPHA, GL_ALPHA_TEST_FUNC, GL_ALPHA_TEST_REF,
        GL_DEPTH_FUNC, GL_DEPTH_WRITEMASK, GL_STENCIL_FUNC, GL_STENCIL_REF, GL_STENCIL_VALUE_MASK,
        GL_STENCIL_WRITEMASK, GL_STENCIL_FAIL, GL_STENCIL_PASS_DEPTH_FAIL, GL_STENCIL_PASS_DEPTH_PASS,
        GL_SHADE_MODEL, GL_CULL_FACE_MODE, GL_FRONT_FACE, GL_LINE_WIDTH,
        GL_PIXEL_UNPACK_BUFFER_BINDING, GL_UNPACK_ALIGNMENT, GL_UNPACK_ROW_LENGTH,
        GL_UNPACK_SKIP_ROWS, GL_UNPACK_SKIP_PIXELS, GL_RED_SCALE, GL_RED_BIAS}) add(name);
    for (auto name : {GL_VIEWPORT, GL_SCISSOR_BOX, GL_COLOR_WRITEMASK, GL_CURRENT_COLOR}) add(name, 4);
    add(GL_POLYGON_MODE, 2);
    add(GL_MODELVIEW_MATRIX, 16); add(GL_PROJECTION_MATRIX, 16);
    for (auto name : {GL_DEPTH_TEST, GL_STENCIL_TEST, GL_ALPHA_TEST, GL_BLEND, GL_FOG,
        GL_LIGHTING, GL_CULL_FACE, GL_SCISSOR_TEST, GL_COLOR_LOGIC_OP, GL_POLYGON_STIPPLE,
        GL_POLYGON_SMOOTH, GL_CLIP_PLANE0, GL_COLOR_SUM, GL_SAMPLE_COVERAGE,
        GL_SAMPLE_ALPHA_TO_COVERAGE, GL_SAMPLE_ALPHA_TO_ONE}) out.push_back(glIsEnabled(name));
    GLint active{}, units{};
    glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
    glGetIntegerv(GL_MAX_TEXTURE_UNITS, &units);
    for (int unit = 0; unit < units; ++unit) {
        glActiveTexture(GL_TEXTURE0 + unit);
        for (auto name : {GL_TEXTURE_1D, GL_TEXTURE_2D, GL_TEXTURE_3D, GL_TEXTURE_CUBE_MAP, GL_TEXTURE_RECTANGLE,
            GL_TEXTURE_GEN_S, GL_TEXTURE_GEN_T, GL_TEXTURE_GEN_R, GL_TEXTURE_GEN_Q}) out.push_back(glIsEnabled(name));
        add(GL_TEXTURE_BINDING_2D); add(GL_TEXTURE_MATRIX, 16);
        GLfloat mode{}; glGetTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, &mode); out.push_back(mode);
    }
    glActiveTexture(active);
    Require(glGetError() == GL_NO_ERROR, "snapshot GL queries");
    return out;
}

static void WriteImage(const char* name, const std::vector<uint8_t>& pixels, int width, int height) {
    char path[256];
    std::snprintf(path, sizeof(path), "artifacts/graphics/%s.ppm", name);
    FILE* f = std::fopen(path, "wb");
    Require(f, "screenshot open");
    std::fprintf(f, "P6\n%d %d\n255\n", width, height);
    for (int y = height - 1; y >= 0; --y) {
        for (int x = 0; x < width; ++x) Require(std::fwrite(&pixels[(y * width + x) * 4], 1, 3, f) == 3, "screenshot write");
    }
    Require(std::fclose(f) == 0, "screenshot close");
}

static GLuint HostileState() {
    const char* vs = "#version 120\nvoid main(){gl_Position=ftransform();}";
    const char* fs = "#version 120\nvoid main(){gl_FragColor=vec4(1,0,1,1);}";
    GLuint program = glCreateProgram();
    for (auto pair : {std::pair{GL_VERTEX_SHADER, vs}, std::pair{GL_FRAGMENT_SHADER, fs}}) {
        GLuint shader = glCreateShader(pair.first);
        glShaderSource(shader, 1, &pair.second, nullptr); glCompileShader(shader);
        GLint ok{}; glGetShaderiv(shader, GL_COMPILE_STATUS, &ok); Require(ok, "hostile shader compile");
        glAttachShader(program, shader); glDeleteShader(shader);
    }
    glLinkProgram(program); GLint ok{}; glGetProgramiv(program, GL_LINK_STATUS, &ok); Require(ok, "hostile shader link");
    glUseProgram(program);
    glViewport(3, 5, 89, 73); glScissor(2, 4, 20, 21); glEnable(GL_SCISSOR_TEST);
    glEnable(GL_DEPTH_TEST); glDepthFunc(GL_GREATER); glDepthMask(GL_TRUE);
    glEnable(GL_STENCIL_TEST); glStencilFunc(GL_NEVER, 7, 0x5a); glStencilMask(0x33);
    glStencilOp(GL_INCR, GL_DECR, GL_REPLACE);
    glEnable(GL_ALPHA_TEST); glAlphaFunc(GL_GREATER, 0.8f);
    glEnable(GL_FOG); glEnable(GL_LIGHTING); glEnable(GL_CULL_FACE);
    glEnable(GL_COLOR_LOGIC_OP); glEnable(GL_POLYGON_STIPPLE);
    glEnable(GL_CLIP_PLANE0); glEnable(GL_POLYGON_SMOOTH);
    glEnable(GL_COLOR_SUM); glSecondaryColor3f(0.5f, 0.1f, 0.7f);
    glEnable(GL_SAMPLE_COVERAGE); glSampleCoverage(0.0f, GL_FALSE);
    glEnable(GL_SAMPLE_ALPHA_TO_COVERAGE); glEnable(GL_SAMPLE_ALPHA_TO_ONE);
    glPolygonMode(GL_FRONT_AND_BACK, GL_LINE); glColorMask(GL_FALSE, GL_TRUE, GL_FALSE, GL_FALSE);
    glEnable(GL_BLEND); glBlendEquationSeparate(GL_FUNC_REVERSE_SUBTRACT, GL_FUNC_SUBTRACT);
    glBlendFuncSeparate(GL_ONE, GL_ZERO, GL_DST_ALPHA, GL_SRC_ALPHA);
    glColor4f(0.2f, 0.3f, 0.4f, 0.5f);
    glMatrixMode(GL_MODELVIEW); glLoadIdentity(); glTranslatef(11, 12, 13);
    glMatrixMode(GL_PROJECTION); glLoadIdentity(); glScalef(2, 3, 4);
    for (int i = 0; i < 4; ++i) {
        glActiveTexture(GL_TEXTURE0 + i); glEnable(GL_TEXTURE_2D); glEnable(GL_TEXTURE_GEN_S);
        glEnable(GL_TEXTURE_RECTANGLE);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_ADD);
        glMatrixMode(GL_TEXTURE); glLoadIdentity(); glTranslatef(i + 1, i + 2, i + 3);
    }
    return program;
}

static std::vector<uint8_t> Render(const RealtimeHud& hud, const RealtimeHudView& view,
    const RealtimeHudState& state, int width, int height) {
    glPushAttrib(GL_ALL_ATTRIB_BITS);
    glDisable(GL_SCISSOR_TEST); glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE); glStencilMask(0xff);
    glClearColor(0, 0, 0, 1); glClearDepth(0.375); glClearStencil(42);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    glPopAttrib();
    const auto before = Snapshot();
    hud.Draw(view, state, width, height);
    const auto after = Snapshot();
    for (std::size_t i = 0; i < before.size(); ++i) if (before[i] != after[i]) {
        std::printf("GL-state mismatch index=%zu before=%g after=%g\n", i, before[i], after[i]);
    }
    Require(after == before, "Draw restores hostile GL state/matrices/program/all texture units");
    std::vector<uint8_t> pixels(width * height * 4);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    std::vector<float> depth(width * height);
    std::vector<uint8_t> stencil(width * height);
    glReadPixels(0, 0, width, height, GL_DEPTH_COMPONENT, GL_FLOAT, depth.data());
    glReadPixels(0, 0, width, height, GL_STENCIL_INDEX, GL_UNSIGNED_BYTE, stencil.data());
    for (std::size_t i = 0; i < depth.size(); ++i) Require(Near(depth[i], 0.375f) && stencil[i] == 42, "world depth/stencil contents untouched");
    Require(glGetError() == GL_NO_ERROR, "render/readback GL errors");
    return pixels;
}

static void MapOracle(const std::vector<uint8_t>& pixels, const RadarMapAssets& assets,
    const RealtimeHudView& view, const RealtimeHudState& state, int width, int height) {
    int sampled = 0, bad = 0, outside = 0;
    for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
        const double sx = (x + 0.5) * 640 / width, sy = (y + 0.5) * 448 / height;
        const auto* pixel = &pixels[((height - 1 - y) * width + x) * 4];
        if ((sx < 32 || sx > 142 || sy < 334 || sy > 430) && !(sx > 510 && sy < 48)) {
            outside += pixel[0] || pixel[1] || pixel[2];
        }
        const double rx = (sx - 87) / 47, ry = (382 - sy) / 38, r2 = rx * rx + ry * ry;
        if (r2 < 0.08 || r2 > 0.65) continue; // exclude authored rim/centre/north sprites
        if (std::abs(sx - (87 - 47 * std::cos(view.cameraYaw))) < 9
            && std::abs(sy - (382 - 38 * std::sin(view.cameraYaw))) < 9) continue;
        // Independent camera basis, not implementation's angle/matrix helper.
        const double wx = state.playerX + state.radarRange * (rx * std::sin(view.cameraYaw) + ry * std::cos(view.cameraYaw));
        const double wy = state.playerY + state.radarRange * (-rx * std::cos(view.cameraYaw) + ry * std::sin(view.cameraYaw));
        const int tx = int(std::floor((wx + 3000) / 500)), ty = int(std::floor((3000 - wy) / 500));
        std::array<float, 4> expected{111, 137, 170, 255};
        if (tx >= 0 && tx < 12 && ty >= 0 && ty < 12) {
            const auto& image = assets.tiles[ty * 12 + tx];
            expected = Sample(image.rgba, image.w, image.h, (wx + 3000) / 500 - tx, (3000 - wy) / 500 - ty);
        }
        bool different = false;
        for (int c = 0; c < 3; ++c) different |= std::abs(pixel[c] - expected[c]) > 2.5f;
        bad += different; ++sampled;
    }
    std::printf("hud-map %dx%d world=%.1f,%.1f yaw=%.3f samples=%d mismatch=%d outside=%d\n", width, height, state.playerX, state.playerY, view.cameraYaw, sampled, bad, outside);
    Require(sampled > 1000 && bad <= sampled / 1000 && outside == 0, "GPU map tile/orientation/resize/source-texel oracle");
}

static void FontOracle(const std::vector<uint8_t>& pixels, const MenuHudFont& font, int hour, int minute, int width, int height) {
    char text[16]; std::snprintf(text, sizeof(text), "%02d:%02d", hour, minute);
    const double advance = (font.unprop + 2) * 0.55, start = 608 - 5 * advance;
    int sampled = 0, bad = 0;
    for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
        const double px = (x + 0.5) * 640 / width, py = (y + 0.5) * 448 / height;
        if (py < 22 || py >= 44) continue;
        // Authored font1 alpha peaks at 221 on this install. Composite ink over
        // the black probe background; do not assume the atlas is opaque.
        std::array<float, 3> ink{};
        bool hasInk = false;
        for (int i = 0; i < 5; ++i) {
            const double local = px - (start + i * advance);
            if (local < 0 || local >= 17.6) continue;
            const int glyph = text[i] == ':' ? 154 : 144 + text[i] - '0';
            const double u = (glyph % 16 * 32 + 0.5 + local / 17.6 * 31) / 512;
            const double v = (glyph / 16 * 40 + 0.5 + (py - 22) / 22 * 39) / 512;
            const auto expected = Sample(font.rgba, font.w, font.h, u, v);
            const float alpha = expected[3] / 255;
            for (int c = 0; c < 3; ++c) ink[c] = expected[c] * 225 / 255 * alpha + ink[c] * (1 - alpha);
            hasInk |= expected[3] > 100;
        }
        if (!hasInk) continue;
        const auto* actual = &pixels[((height - 1 - y) * width + x) * 4];
        for (int c = 0; c < 3; ++c) bad += std::abs(actual[c] - ink[c]) > 2.5f;
        ++sampled;
    }
    std::printf("hud-font %dx%d text=%s font=%s unprop=%d inkSamples=%d badChannels=%d\n", width, height, text, font.name, font.unprop, sampled, bad);
    Require(sampled > 60 && bad == 0, "font1 atlas pixel mapping/clock state/right alignment");
}

static void MarkerOracle(const std::vector<uint8_t>& pixels, const RadarMapAssets& assets,
    const RealtimeHudView& view, const RealtimeHudState& state, int width, int height) {
    // Independent original Sprite2d corner permutation and inverse affine UV.
    // The source sprite is diagonally authored; simply rotating a screen-axis
    // arrow would introduce a 45/180-degree error despite moving with heading.
    const double gtaPlayerHeading = std::atan2(-std::cos(state.playerYaw), std::sin(state.playerYaw));
    const double gtaCameraHeading = std::atan2(-std::cos(view.cameraYaw), std::sin(view.cameraYaw));
    const double rotation = gtaPlayerHeading - gtaCameraHeading - std::numbers::pi;
    std::array<std::array<double, 2>, 4> corners{};
    for (int i = 0; i < 4; ++i) {
        const double theta = i * std::numbers::pi / 2 + rotation - std::numbers::pi / 4;
        corners[i] = {87 + 8 * std::sin(theta), 382 + 8 * std::cos(theta)};
    }
    const double ax = corners[1][0] - corners[0][0], ay = corners[1][1] - corners[0][1];
    const double bx = corners[3][0] - corners[0][0], by = corners[3][1] - corners[0][1];
    const double determinant = ax * by - ay * bx;
    int sampled = 0, bad = 0;
    for (int y = int(370.0 * height / 448); y < int(394.0 * height / 448); ++y) {
        for (int x = int(75.0 * width / 640); x < int(99.0 * width / 640); ++x) {
            const double sx = (x + 0.5) * 640 / width, sy = (y + 0.5) * 448 / height;
            const double dx = sx - corners[0][0], dy = sy - corners[0][1];
            const double u = (dx * by - dy * bx) / determinant, v = (ax * dy - ay * dx) / determinant;
            if (u <= 0.001 || u >= 0.999 || v <= 0.001 || v >= 0.999) continue;
            const auto marker = Sample(assets.centre.rgba, assets.centre.w, assets.centre.h, u, v);
            if (marker[3] < 20) continue;
            const double rx = (sx - 87) / 47, ry = (382 - sy) / 38;
            const double wx = state.playerX + state.radarRange * (rx * std::sin(view.cameraYaw) + ry * std::cos(view.cameraYaw));
            const double wy = state.playerY + state.radarRange * (-rx * std::cos(view.cameraYaw) + ry * std::sin(view.cameraYaw));
            const int tx = int(std::floor((wx + 3000) / 500)), ty = int(std::floor((3000 - wy) / 500));
            std::array<float, 4> background{111, 137, 170, 255};
            if (tx >= 0 && tx < 12 && ty >= 0 && ty < 12) {
                const auto& tile = assets.tiles[ty * 12 + tx];
                background = Sample(tile.rgba, tile.w, tile.h, (wx + 3000) / 500 - tx, (3000 - wy) / 500 - ty);
            }
            const auto* pixel = &pixels[((height - 1 - y) * width + x) * 4];
            const double alpha = marker[3] / 255;
            for (int c = 0; c < 3; ++c) bad += std::abs(pixel[c] - (marker[c] * alpha + background[c] * (1 - alpha))) > 2.5;
            ++sampled;
        }
    }
    std::printf("hud-marker %dx%d playerYaw=%.3f cameraYaw=%.3f samples=%d badChannels=%d\n", width, height, state.playerYaw, view.cameraYaw, sampled, bad);
    Require(sampled > 20 && bad == 0, "source radar_centre rotated-corner UV/heading oracle");
}

int main(int argc, char** argv) {
    GeometryProbe();
    const char* game = argc > 1 ? argv[1] : "/game";
    char error[512]{};
    RadarMapAssets reference;
    MenuHudFont font;
    RealtimeHud hud;
    Require(RadarMap_LoadAssets(game, reference, error, sizeof(error)), error);
    Require(MenuShot_LoadPricedownFont(game, font, error, sizeof(error)), error);
    for (int glyph = 144; glyph <= 154; ++glyph) {
        int maximum = 0, ink = 0;
        for (int y = glyph / 16 * 40; y < glyph / 16 * 40 + 40; ++y) for (int x = glyph % 16 * 32; x < glyph % 16 * 32 + 32; ++x) {
            const int alpha = font.rgba[(y * font.w + x) * 4 + 3];
            maximum = std::max(maximum, alpha); ink += alpha > 127;
        }
        std::printf("font-atlas glyph=%d maxAlpha=%d ink=%d\n", glyph, maximum, ink);
    }
    Require(hud.Load(game, error, sizeof(error)), error);
    // This point is the parser ownership handoff: all subsequent work is GL or
    // owned RGBA/math. Runtime integration starts its worker after Load returns.
    const auto getDisplay = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
    Require(getDisplay, "surfaceless EGL entry point");
    const auto display = getDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
    Require(eglInitialize(display, nullptr, nullptr) && eglBindAPI(EGL_OPENGL_API), "EGL initialize");
    const EGLint attributes[]{EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8, EGL_NONE};
    EGLConfig config{}; EGLint count{};
    Require(eglChooseConfig(display, attributes, &config, 1, &count) && count, "EGL config");
    const EGLint size[]{EGL_WIDTH, 1280, EGL_HEIGHT, 896, EGL_NONE};
    auto surface = eglCreatePbufferSurface(display, config, size);
    auto context = eglCreateContext(display, config, EGL_NO_CONTEXT, nullptr);
    Require(eglMakeCurrent(display, surface, surface, context), "EGL context");
    std::printf("hud-gpu renderer=%s\n", glGetString(GL_RENDERER));
    const auto program = HostileState();
    GLuint pbo{}; glGenBuffers(1, &pbo); glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbo);
    glBufferData(GL_PIXEL_UNPACK_BUFFER, 4096, nullptr, GL_STATIC_DRAW);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 19); glPixelStorei(GL_UNPACK_SKIP_ROWS, 3);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 2); glPixelStorei(GL_UNPACK_ALIGNMENT, 8);
    glPixelTransferf(GL_RED_SCALE, 0.3f); glPixelTransferf(GL_RED_BIAS, 0.2f);
    const auto before = Snapshot();
    Require(hud.Upload(error, sizeof(error)), error);
    Require(Snapshot() == before, "Upload restores PBO/pixel store/transfer/texture state");
    // Pixel transfer also affects readback; clear it after testing Upload.
    glPixelTransferf(GL_RED_SCALE, 1); glPixelTransferf(GL_RED_BIAS, 0);
    for (int test = 0; test < 6; ++test) {
        const int width = test < 4 ? 640 : 1280, height = test < 4 ? 448 : test == 4 ? 896 : 720;
        RealtimeHudState state{test == 3 ? 2999.0f : test == 2 ? 2500.0f : 2495.0f,
            test == 3 ? 2999.0f : test == 2 ? -1500.0f : -1685.0f, test * kPi / 2, 180, test * 4, test * 11};
        RealtimeHudView view{test * kPi / 2};
        const auto pixels = Render(hud, view, state, width, height);
        MapOracle(pixels, reference, view, state, width, height);
        FontOracle(pixels, font, state.hour, state.minute, width, height);
        MarkerOracle(pixels, reference, view, state, width, height);
        char name[80]; std::snprintf(name, sizeof(name), "realtime-hud-%d-%dx%d", test, width, height);
        WriteImage(name, pixels, width, height);
    }
    // Clock rollover, player-only heading, map-only camera rotation and position
    // changes must all visibly affect the true GPU output.
    RealtimeHudState state{2495, -1685, 0, 180, 23, 59};
    RealtimeHudView view{kPi / 2};
    const auto baseline = Render(hud, view, state, 640, 448);
    for (int test = 0; test < 4; ++test) {
        auto changedState = state; auto changedView = view;
        if (test == 0) changedState.playerYaw += kPi / 2;
        if (test == 1) changedView.cameraYaw += kPi / 2;
        if (test == 2) changedState.playerX += 100;
        if (test == 3) { changedState.hour = 0; changedState.minute = 0; }
        auto pixels = Render(hud, changedView, changedState, 640, 448);
        MarkerOracle(pixels, reference, changedView, changedState, 640, 448);
        int changed = 0;
        for (std::size_t i = 0; i < pixels.size(); i += 4) changed += !std::equal(pixels.begin() + i, pixels.begin() + i + 3, baseline.begin() + i);
        std::printf("hud-state case=%d changedPixels=%d\n", test, changed);
        Require(changed > (test == 0 ? 20 : 100), "supplied state changes rendered HUD");
    }
    hud.ReleaseGpu();
    glUseProgram(0); glDeleteProgram(program); glDeleteBuffers(1, &pbo);
    Require(glGetError() == GL_NO_ERROR, "release GL errors");
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(display, context); eglDestroySurface(display, surface); eglTerminate(display);
    std::puts("realtime-hud-probe PASS");
}
