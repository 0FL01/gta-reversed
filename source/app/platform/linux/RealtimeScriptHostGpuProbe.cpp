// Same renderer and HUD as production; surfaceless readback, no asset dumps.
#include "app/platform/linux/Realtime.cpp"
#include <EGL/eglext.h>

namespace {
std::unique_ptr<RealtimeHud> s_ProbeHud;
void CheckGpu(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "script-entities GPU FAIL %s\n", message); std::exit(2); }
}
}
void RealtimeScriptHostGpuPrepare(const char* dir) {
    s_ProbeHud = std::make_unique<RealtimeHud>();
    char error[512]{};
    CheckGpu(s_ProbeHud->Load(dir, error, sizeof(error)), error);
}
void RealtimeScriptHostGpuProbe(NativeScriptEntities& entities) {
    const auto getDisplay = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
    CheckGpu(getDisplay, "surfaceless EGL entry point");
    const auto display = getDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
    CheckGpu(display != EGL_NO_DISPLAY && eglInitialize(display, nullptr, nullptr) && eglBindAPI(EGL_OPENGL_API), "EGL initialize");
    const EGLint attributes[]{EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_DEPTH_SIZE, 24, EGL_NONE};
    EGLConfig config{}; EGLint count{};
    CheckGpu(eglChooseConfig(display, attributes, &config, 1, &count) && count, "EGL config");
    constexpr int width = 640, height = 448;
    const EGLint size[]{EGL_WIDTH, width, EGL_HEIGHT, height, EGL_NONE};
    const auto surface = eglCreatePbufferSurface(display, config, size);
    const auto context = eglCreateContext(display, config, EGL_NO_CONTEXT, nullptr);
    CheckGpu(surface != EGL_NO_SURFACE && context != EGL_NO_CONTEXT && eglMakeCurrent(display, surface, surface, context), "EGL context");
    std::printf("script-entities GPU renderer=%s\n", glGetString(GL_RENDERER));
    const auto readback = [] {
        std::vector<uint8_t> pixels(width * height * 4);
        glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        CheckGpu(glGetError() == GL_NO_ERROR, "production readback GL errors"); return pixels;
    };
    const auto difference = [](const auto& a, const auto& b) {
        std::size_t changed = 0;
        for (std::size_t i = 0; i < a.size(); i += 4) changed += a[i] != b[i] || a[i+1] != b[i+1] || a[i+2] != b[i+2];
        return changed;
    };
    const auto& pickup = entities.Pickups()[0];
    const auto p = pickup.Position;
    {
        GpuScene gpu;
        CheckGpu(gpu.UploadTextures(entities.PreparedModel()), "actual prepared pickup textures");
        Camera camera;
        camera.x = p.X + 3; camera.y = p.Y - 4; camera.z = p.Z + 2;
        camera.yaw = std::atan2(p.Y - camera.y, p.X - camera.x);
        camera.pitch = std::atan2(p.Z - camera.z, 5.0f);
        glEnable(GL_DEPTH_TEST); glClearColor(0.1f, 0.12f, 0.15f, 1);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT); camera.Apply(width, height, 1000);
        const auto clear = readback();
        gpu.Draw(entities.Actors());
        const auto initial = readback();
        const auto coverage = difference(clear, initial);
        CheckGpu(coverage > 100, "actual GpuScene property mesh visible");
        std::string frameError;
        CheckGpu(entities.AdvanceTime(512, frameError), "source rotation time advance");
        CheckGpu(entities.Actors().meshes[0].pos == pickup.Actor.meshes[0].pos, "current phase reaches visible production scene without latency");
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT); gpu.Draw(entities.Actors());
        const auto textured = readback();
        const auto rotationEffect = difference(initial, textured);
        CheckGpu(rotationEffect > 50, "source time-driven matrix changes actual GPU pixels");
        const auto unscaled = NativeScriptPropertyActor(entities.PreparedModel(), p, 1, 512);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT); gpu.Draw(unscaled);
        const auto scaleEffect = difference(textured, readback());
        CheckGpu(scaleEffect > 100, "source COL normalization changes actual GPU geometry coverage");
        auto flat = pickup.Actor;
        for (auto& mesh : flat.meshes) std::fill(mesh.triImg.begin(), mesh.triImg.end(), -1);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT); gpu.Draw(flat);
        const auto textureEffect = difference(textured, readback());
        CheckGpu(textureEffect > 30, "prepared icons4 texels affect actual runtime pixels");
        std::printf("script-entities GPU PASS model=%s tris=%d coverage=%zu textureEffect=%zu rotationEffect=%zu colScale=%.9f scaleEffect=%zu\n",
            pickup.Actor.stats.dffName, pickup.Actor.stats.triangles, coverage, textureEffect, rotationEffect, entities.PropertyGeometry().Scale, scaleEffect);
    }
    {
        char error[256]{}; CheckGpu(s_ProbeHud && s_ProbeHud->Upload(error, sizeof(error)), error);
        RealtimeHudView view; view.clock = false;
        RealtimeHudState state;
        const auto& blip = entities.Blips()[0];
        state.playerX = blip.Position.X - 70; state.playerY = blip.Position.Y;
        const auto draw = [&] { glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT); s_ProbeHud->Draw(view, state, width, height); return readback(); };
        const auto baseline = draw();
        state.scriptBlips = entities.Blips();
        const auto visible = draw();
        const auto spritePixels = difference(baseline, visible);
        CheckGpu(spritePixels > 50 && spritePixels <= 16 * 16, "real propertyR atlas stamp, not fabricated radar primitives");
        state.playerOnMission = true; CheckGpu(draw() == baseline, "contact filter uses source on-mission condition"); state.playerOnMission = false;
        state.radarZoom = 1; CheckGpu(draw() == baseline, "source short range zoom filter"); state.radarZoom = 0;
        state.exterior = false; CheckGpu(draw() == baseline, "source exterior filter"); state.exterior = true;
        CheckGpu(entities.SetBlipDisplay({{900, 1, 1}, blip.Reference, 0}).Status == NativeScriptServiceStatus::Ready, "actual display service");
        CheckGpu(draw() == baseline, "018B hides actual HUD drawable");
        CheckGpu(entities.SetBlipDisplay({{900, 2, 2}, blip.Reference, 2}).Status == NativeScriptServiceStatus::Ready, "actual display restore");
        CheckGpu(draw() == visible, "018B restores actual HUD drawable");
        state.playerX = blip.Position.X - 181;
        state.scriptBlips = {}; const auto farBaseline = draw();
        state.scriptBlips = entities.Blips().first(1); CheckGpu(draw() == farBaseline, "short range sprite outside real distance 1 suppressed");
        std::string helpError;
        CheckGpu(entities.AdvanceTime(1000, helpError) && entities.AdvanceTime(1016, helpError) && entities.AdvanceTime(1032, helpError), "real entity presentation clock");
        const auto applyHelp = [&] {
            const auto help = entities.HelpPresentation(); state.helpText = help.Text; state.helpAlpha = help.Alpha;
        };
        applyHelp(); CheckGpu(state.helpText == pickup.Message && state.helpAlpha == 200, "source proximity latch feeds owned timed presentation");
        state.scriptBlips = {}; state.helpAlpha = 0;
        const auto noHelp = draw();
        applyHelp();
        const auto fullHelp = draw();
        const auto helpPixels = difference(noHelp, fullHelp);
        std::size_t helpInk = 0;
        for (std::size_t i = 0; i < fullHelp.size(); i += 4) helpInk += fullHelp[i] > noHelp[i] && fullHelp[i+1] > noHelp[i+1];
        CheckGpu(helpInk > 100, "owned GXT help uses actual font1 glyph ink, not merely a dark box");
        const auto expire = 1033 + entities.HelpLifetimeMs();
        CheckGpu(entities.AdvanceTime(expire - 1, helpError) && entities.AdvanceTime(expire, helpError) && entities.AdvanceTime(expire + 100, helpError), "real entity fade elapsed");
        applyHelp(); CheckGpu(state.helpAlpha == 80, "source entity alpha at 100ms into fade");
        const auto fadedHelp = draw();
        CheckGpu(difference(fullHelp, fadedHelp) > 100 && difference(noHelp, fadedHelp) > 100, "presentation alpha changes actual font and box pixels");
        CheckGpu(entities.AdvanceTime(expire + 301, helpError), "real entity help expiry");
        applyHelp(); CheckGpu(state.helpText.empty() && !state.helpAlpha && draw() == noHelp, "expired zero alpha draws neither font nor box");
        entities.Tick(p, p, true, false);
        CheckGpu(entities.AdvanceTime(expire + 302, helpError), "source latch reentry frame publication");
        CheckGpu(entities.HelpRevision() == 1 && entities.HelpPresentation().Text.empty(), "expiry does not reset source once-only pickup latch");
        // Hostile caller state around the real help path, including texture unit
        // and both matrix stacks; no caller-owned framebuffer/depth writes.
        glActiveTexture(GL_TEXTURE1); glEnable(GL_TEXTURE_2D);
        glMatrixMode(GL_PROJECTION); glLoadIdentity(); glTranslatef(3, 4, 5);
        glMatrixMode(GL_MODELVIEW); glLoadIdentity(); glScalef(2, 3, 4);
        glViewport(7, 9, 113, 127); glEnable(GL_DEPTH_TEST); glDepthMask(GL_FALSE);
        glDisable(GL_BLEND); glEnable(GL_SCISSOR_TEST); glScissor(2, 3, 4, 5);
        const auto snapshot = [] {
            std::array<GLint, 12> ints{};
            glGetIntegerv(GL_ACTIVE_TEXTURE, &ints[0]); glGetIntegerv(GL_MATRIX_MODE, &ints[1]);
            glGetIntegerv(GL_VIEWPORT, &ints[2]); glGetIntegerv(GL_SCISSOR_BOX, &ints[6]);
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &ints[10]); glGetIntegerv(GL_CURRENT_PROGRAM, &ints[11]);
            std::array<GLfloat, 32> matrices{};
            glGetFloatv(GL_PROJECTION_MATRIX, matrices.data()); glGetFloatv(GL_MODELVIEW_MATRIX, matrices.data() + 16);
            GLboolean depthWrite{}; glGetBooleanv(GL_DEPTH_WRITEMASK, &depthWrite);
            return std::tuple(ints, matrices, depthWrite, glIsEnabled(GL_BLEND), glIsEnabled(GL_DEPTH_TEST), glIsEnabled(GL_SCISSOR_TEST), glIsEnabled(GL_TEXTURE_2D));
        };
        const auto caller = snapshot(); state.helpText = pickup.Message; state.helpAlpha = 80;
        s_ProbeHud->Draw(view, state, width, height);
        CheckGpu(snapshot() == caller && glGetError() == GL_NO_ERROR, "timed help restores hostile caller GL state");
        std::printf("script-entities GPU PASS sprite=%d atlas=%s changed=%zu helpPixels=%zu helpInk=%zu fade=200,80,0 callerGL=preserved filtered=mission,zoom,exterior,display,range\n",
            blip.Sprite, entities.RadarImage().name, spritePixels, helpPixels, helpInk);
        s_ProbeHud.reset();
    }
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(display, context); eglDestroySurface(display, surface); eglTerminate(display);
}
