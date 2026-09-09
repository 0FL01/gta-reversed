// Separate TU exposes the actual runtime uploader/draw path without Realtime_Run.
#include "app/platform/linux/Realtime.cpp"
#include <EGL/eglext.h>

static void RequireGpu(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "vehicle-texture GPU FAIL %s\n", message); std::exit(1); }
}

void VehicleTextureGpuProbe(const char* game, const WorldShotScene& offline, const WorldShotScene& realtime) {
    const auto getDisplay = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
    RequireGpu(getDisplay, "surfaceless EGL entry point");
    const auto display = getDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
    RequireGpu(display != EGL_NO_DISPLAY && eglInitialize(display, nullptr, nullptr), "EGL initialize");
    RequireGpu(eglBindAPI(EGL_OPENGL_API), "OpenGL API");
    const EGLint attributes[]{EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_DEPTH_SIZE, 24, EGL_NONE};
    EGLConfig config{}; EGLint count{};
    RequireGpu(eglChooseConfig(display, attributes, &config, 1, &count) && count, "EGL config");
    constexpr int width = 640, height = 360;
    const EGLint size[]{EGL_WIDTH, width, EGL_HEIGHT, height, EGL_NONE};
    const auto surface = eglCreatePbufferSurface(display, config, size);
    const auto context = eglCreateContext(display, config, EGL_NO_CONTEXT, nullptr);
    RequireGpu(surface != EGL_NO_SURFACE && context != EGL_NO_CONTEXT &&
        eglMakeCurrent(display, surface, surface, context), "EGL context");
    std::printf("vehicle-gpu renderer=%s version=%s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));
    {
        RealtimeEnvironment environment;
        char error[512]{};
        RequireGpu(environment.Load(game, error, sizeof(error)) && environment.Upload(error, sizeof(error)), error);
        Camera camera;
        camera.x = 4; camera.y = -5; camera.z = 2.5f;
        camera.yaw = std::atan2(5.0f, -4.0f); camera.pitch = -0.2f;
        glEnable(GL_DEPTH_TEST);
        std::vector<uint8_t> before;
        for (int pass = 0; pass < 2; ++pass) {
            const auto& scene = pass ? realtime : offline;
            GpuScene gpu;
            RequireGpu(gpu.UploadTextures(scene), "runtime texture upload");
            for (size_t i = 0; i < scene.images.size(); ++i) {
                const auto& image = scene.images[i];
                std::vector<uint8_t> readback(image.rgba.size());
                glBindTexture(GL_TEXTURE_2D, gpu.textures[i]);
                glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, readback.data());
                RequireGpu(readback == image.rgba, "runtime GPU RGBA/alpha exact readback");
            }
            glClearColor(0.1f, 0.12f, 0.15f, 1);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            camera.Apply(width, height, 1000);
            environment.BeginObjects();
            gpu.Draw(scene);
            environment.EndWorld();
            std::vector<uint8_t> pixels(width * height * 3);
            glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, pixels.data());
            RequireGpu(glGetError() == GL_NO_ERROR, "runtime draw/readback GL errors");
            const char* path = pass ? "artifacts/graphics/vehicle-texture-after.ppm" : "artifacts/graphics/vehicle-texture-before.ppm";
            FILE* file = std::fopen(path, "wb");
            RequireGpu(file, "screenshot open");
            std::fprintf(file, "P6\n%d %d\n255\n", width, height);
            for (int y = height - 1; y >= 0; --y) RequireGpu(std::fwrite(pixels.data() + y * width * 3, 3, width, file) == width, "screenshot write");
            RequireGpu(std::fclose(file) == 0, "screenshot close");
            if (!pass) before = pixels;
            else {
                int changed = 0;
                for (size_t i = 0; i < pixels.size(); i += 3) {
                    changed += pixels[i] != before[i] || pixels[i + 1] != before[i + 1] || pixels[i + 2] != before[i + 2];
                }
                RequireGpu(changed > 1000, "visible shared-texture regression control");
                std::printf("vehicle-gpu PASS uploaded=%zu exactRGBA=1 changedPixels=%d screenshot=%s\n", scene.images.size(), changed, path);
            }
        }
    }
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(display, context);
    eglDestroySurface(display, surface);
    eglTerminate(display);
}
