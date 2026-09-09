#include "app/platform/linux/Realtime.cpp"
#include <EGL/eglext.h>

static void RequireGpu(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "vehicle-material GPU FAIL %s\n", message); std::exit(1); }
}

static WorldShotMesh Triangle(float z, std::array<float, 4> color, bool alpha) {
    WorldShotMesh mesh{};
    mesh.tris = 1;
    mesh.pos = {-1, -1, z, 1, -1, z, 0, 1, z};
    mesh.nrm = {0, 0, 1, 0, 0, 1, 0, 0, 1};
    mesh.dayColors.assign(12, 255);
    WorldShotSurface surface;
    surface.color = color;
    surface.ambient = surface.diffuse = 0;
    surface.vehicleAlpha = alpha;
    mesh.surfaces.push_back(surface);
    return mesh;
}

void VehicleMaterialGpuProbe(const char* game, const WorldShotScene& scene) {
    const auto getDisplay = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
    RequireGpu(getDisplay, "surfaceless EGL entry point");
    const auto display = getDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
    RequireGpu(display != EGL_NO_DISPLAY && eglInitialize(display, nullptr, nullptr) && eglBindAPI(EGL_OPENGL_API), "EGL initialize");
    const EGLint attributes[]{EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_DEPTH_SIZE, 24, EGL_NONE};
    EGLConfig config{}; EGLint count{};
    RequireGpu(eglChooseConfig(display, attributes, &config, 1, &count) && count, "EGL config");
    constexpr int width = 640, height = 360;
    const EGLint size[]{EGL_WIDTH, width, EGL_HEIGHT, height, EGL_NONE};
    const auto surface = eglCreatePbufferSurface(display, config, size);
    const auto context = eglCreateContext(display, config, EGL_NO_CONTEXT, nullptr);
    RequireGpu(surface != EGL_NO_SURFACE && context != EGL_NO_CONTEXT && eglMakeCurrent(display, surface, surface, context), "EGL context");
    std::printf("material-gpu renderer=%s\n", glGetString(GL_RENDERER));
    {
        RealtimeEnvironment environment;
        char error[512]{};
        RequireGpu(environment.Load(game, error, sizeof(error)) && environment.Upload(error, sizeof(error)), error);
        glEnable(GL_DEPTH_TEST);
        glEnable(GL_ALPHA_TEST);
        glAlphaFunc(GL_GREATER, 0.5f); // actual realtime world cutout state
        glDisable(GL_BLEND);
        GpuScene gpu;
        RequireGpu(gpu.UploadTextures(scene), "runtime texture upload");
        for (int view = 0; view < 2; ++view) {
            Camera camera;
            camera.x = view ? -4 : 4; camera.y = view ? 5 : -5; camera.z = 2.5f;
            camera.yaw = std::atan2(-camera.y, -camera.x); camera.pitch = -0.2f;
            std::vector<uint8_t> original;
            for (int pass = 0; pass < 2; ++pass) {
                glClearColor(0.1f, 0.12f, 0.15f, 1);
                glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
                camera.Apply(width, height, 1000);
                environment.BeginObjects();
                if (pass) gpu.DrawActors(scene); else gpu.Draw(scene);
                environment.EndWorld();
                std::vector<uint8_t> pixels(width * height * 3);
                glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, pixels.data());
                RequireGpu(glGetError() == GL_NO_ERROR, "runtime GL errors");
                char path[160];
                std::snprintf(path, sizeof(path), "artifacts/graphics/vehicle-material-%s-%d.ppm", pass ? "after" : "before", view);
                FILE* file = std::fopen(path, "wb");
                RequireGpu(file, "screenshot open");
                std::fprintf(file, "P6\n%d %d\n255\n", width, height);
                for (int y = height - 1; y >= 0; --y) RequireGpu(std::fwrite(pixels.data() + y * width * 3, 3, width, file) == width, "screenshot write");
                RequireGpu(std::fclose(file) == 0, "screenshot close");
                if (!pass) original = pixels;
                else {
                    int changed = 0;
                    for (size_t i = 0; i < pixels.size(); i += 3) changed += pixels[i] != original[i] || pixels[i + 1] != original[i + 1] || pixels[i + 2] != original[i + 2];
                    RequireGpu(changed > 100, "visible actual-material regression control");
                    std::printf("material-gpu PASS view=%d changedPixels=%d screenshot=%s\n", view, changed, path);
                }
            }
        }
        // Independent overlap oracle: deliberately near-first input. The same
        // actual material alpha (128/255) must composite, not merely look changed.
        const float alpha = 128.0f / 255.0f;
        WorldShotScene fixture{};
        fixture.meshes = {Triangle(-2, {1, 0, 0, alpha}, true),
            Triangle(-4, {0, 0, 1, alpha}, true), Triangle(-8, {0, 1, 0, 1}, false)};
        GpuScene test;
        RequireGpu(test.UploadTextures(fixture), "fixture upload");
        glViewport(0, 0, width, height);
        glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(-1, 1, -1, 1, 0, 10);
        glMatrixMode(GL_MODELVIEW); glLoadIdentity();
        const auto render = [&](bool ordered) {
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            environment.BeginObjects();
            if (ordered) test.DrawActors(fixture); else test.Draw(fixture);
            environment.EndWorld();
            std::array<uint8_t, 3> pixel{};
            glReadPixels(width / 2, height / 2, 1, 1, GL_RGB, GL_UNSIGNED_BYTE, pixel.data());
            return pixel;
        };
        const auto before = render(false);
        const auto after = render(true);
        RequireGpu(before[0] == 255 && before[1] == 0 && before[2] == 0, "old opaque defect reproduced");
        RequireGpu(std::abs(after[0] - 128) <= 1 && std::abs(after[1] - 63) <= 1 && std::abs(after[2] - 64) <= 1, "source-alpha compositing and far-to-near ordering");
        float depth = 0;
        glReadPixels(width / 2, height / 2, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, &depth);
        RequireGpu(std::abs(depth - 0.2f) < 1e-5f, "original vehicle ZWRITE retained");
        fixture.meshes.push_back(Triangle(-1, {0, 1, 0, 1}, false));
        RequireGpu(render(true) == std::array<uint8_t, 3>{0, 255, 0}, "opaque foreground occludes glass");
        fixture.meshes.pop_back();
        std::reverse(fixture.meshes.begin(), fixture.meshes.end());
        RequireGpu(render(true) == after, "ordering independent of triangle input order");
        // Authored texture alpha multiplies material alpha; zero-alpha holes
        // must not write depth. World texture cutouts keep the caller's 0.5 ref.
        WorldShotImage image{};
        image.w = image.h = 1; image.filter = 0; image.rgba = {255, 255, 255, 64};
        fixture.images = {image};
        fixture.meshes = {Triangle(-2, {1, 0, 0, alpha}, true), Triangle(-8, {0, 1, 0, 1}, false)};
        fixture.meshes[0].uv.assign(6, 0); fixture.meshes[0].triImg = {0};
        RequireGpu(test.UploadTextures(fixture), "fractional texture upload");
        const auto textured = render(true);
        RequireGpu(std::abs(textured[0] - 32) <= 1 && std::abs(textured[1] - 223) <= 1, "texture times material alpha");
        fixture.images[0].rgba[3] = 0;
        RequireGpu(test.UploadTextures(fixture), "zero-alpha texture upload");
        RequireGpu(render(true) == std::array<uint8_t, 3>{0, 255, 0}, "zero-alpha hole");
        glReadPixels(width / 2, height / 2, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, &depth);
        RequireGpu(std::abs(depth - 0.8f) < 1e-5f, "zero-alpha hole preserves underlying depth");
        fixture.images[0].rgba[3] = 64;
        fixture.meshes[0].surfaces[0].vehicleAlpha = false;
        fixture.meshes[0].surfaces[0].color[3] = 1;
        RequireGpu(test.UploadTextures(fixture), "world cutout texture upload");
        RequireGpu(render(true) == render(false), "world foliage cutout unchanged");
        GLfloat ref = 0; GLboolean write = GL_FALSE;
        glGetFloatv(GL_ALPHA_TEST_REF, &ref); glGetBooleanv(GL_DEPTH_WRITEMASK, &write);
        RequireGpu(ref == 0.5f && write && glIsEnabled(GL_ALPHA_TEST) && !glIsEnabled(GL_BLEND), "caller GL state restored");
        RequireGpu(glGetError() == GL_NO_ERROR, "oracle GL errors");
        std::printf("vehicle-material GPU PASS compositing=%u,%u,%u depth-order=back-to-front zwrite=1 texture-alpha=multiplied cutouts=preserved state=restored\n", after[0], after[1], after[2]);
    }
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(display, context); eglDestroySurface(display, surface); eglTerminate(display);
}
