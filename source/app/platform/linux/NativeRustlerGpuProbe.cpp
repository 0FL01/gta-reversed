// Exercise the production GpuScene actor path without changing host/runtime code.
#include "app/platform/linux/Realtime.cpp"
#include <EGL/eglext.h>
#include <stdexcept>

namespace {
void Verify(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
}
void NativeRustlerGpuProbe(const char* game, const WorldShotScene& source, const WorldShotScene& bind, const char* captures) {
    const auto getDisplay = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
    Verify(getDisplay, "Rustler surfaceless EGL");
    const auto display = getDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
    Verify(eglInitialize(display, nullptr, nullptr) && eglBindAPI(EGL_OPENGL_API), "Rustler EGL init");
    const EGLint attributes[]{EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_DEPTH_SIZE,24,EGL_NONE};
    EGLConfig config{}; EGLint count{};
    Verify(eglChooseConfig(display, attributes, &config, 1, &count) && count, "Rustler EGL config");
    constexpr int width = 640, height = 480;
    const EGLint size[]{EGL_WIDTH,width,EGL_HEIGHT,height,EGL_NONE};
    const auto surface = eglCreatePbufferSurface(display, config, size);
    const auto context = eglCreateContext(display, config, EGL_NO_CONTEXT, nullptr);
    Verify(eglMakeCurrent(display, surface, surface, context), "Rustler EGL context");
    GLuint framebuffer{}, color{}, depthBuffer{};
    glGenFramebuffers(1, &framebuffer); glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glGenRenderbuffers(1, &color); glBindRenderbuffer(GL_RENDERBUFFER, color);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGB8, width, height);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, color);
    glGenRenderbuffers(1, &depthBuffer); glBindRenderbuffer(GL_RENDERBUFFER, depthBuffer);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, width, height);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depthBuffer);
    Verify(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "Rustler own FBO complete");
    glDrawBuffer(GL_COLOR_ATTACHMENT0); glReadBuffer(GL_COLOR_ATTACHMENT0);
    glViewport(0, 0, width, height);
    {
        RealtimeEnvironment environment;
        char error[512]{};
        Verify(environment.Load(game, error, sizeof(error)) && environment.Upload(error, sizeof(error)), error);
        glEnable(GL_DEPTH_TEST); glEnable(GL_ALPHA_TEST); glAlphaFunc(GL_GREATER, 0.5f); glDisable(GL_BLEND);
        GpuScene gpu;
        const auto render = [&](const WorldShotScene& scene, int view) {
            Verify(gpu.UploadTextures(scene), "Rustler GPU upload");
            glClearColor(0.1f, 0.12f, 0.15f, 1);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            Camera camera;
            camera.x = view ? -7 : 5; camera.y = 12; camera.z = view ? 0 : 4;
            camera.yaw = std::atan2(-camera.y, -camera.x); camera.pitch = view ? 0 : -0.25f;
            camera.Apply(width, height, 1000);
            environment.BeginObjects(); gpu.DrawActors(scene); environment.EndWorld();
            std::vector<uint8_t> pixels(width * height * 3);
            std::vector<float> depth(width * height);
            glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, pixels.data());
            glReadPixels(0, 0, width, height, GL_DEPTH_COMPONENT, GL_FLOAT, depth.data());
            Verify(glGetError() == GL_NO_ERROR, "Rustler actual GpuScene GL error");
            return std::pair{pixels, depth};
        };
        // Reinsert the actual source moving atomic, replay material alpha zero
        // with its render bit conceptually still set. Import its actual texture.
        auto replay = source;
        auto moving = bind.meshes.at(8);
        Verify(moving.tris == 8, "raw moving prop geometry");
        for (auto& index : moving.triImg) if (index >= 0) {
            const auto& image = bind.images.at(size_t(index));
            auto found = std::find_if(replay.images.begin(), replay.images.end(), [&](const auto& v) { return !std::strcmp(v.name, image.name); });
            if (found == replay.images.end()) { index = int(replay.images.size()); replay.images.push_back(image); }
            else index = int(found - replay.images.begin());
        }
        for (auto& material : moving.surfaces) { material.color[3] = 0; material.vehicleAlpha = true; }
        replay.meshes.push_back(moving);
        size_t totalAlphaChanged = 0, totalMovingChanged = 0;
        for (int view = 0; view < 2; ++view) {
            const auto actual = render(source, view);
            if (captures) {
                const auto path = std::string(captures) + "/NativeRustlerProbe-fbo-" + std::to_string(view) + ".ppm";
                auto* file = std::fopen(path.c_str(), "wb");
                Verify(file, "Rustler own FBO capture open");
                std::fprintf(file, "P6\n%d %d\n255\n", width, height);
                for (int y = height - 1; y >= 0; --y)
                    Verify(std::fwrite(actual.first.data() + y * width * 3, 3, width, file) == width, "Rustler FBO capture write");
                Verify(std::fclose(file) == 0, "Rustler FBO capture close");
            }
            Verify(render(replay, view) == actual, "alpha-zero raw moving atomic changes pixels/depth");
            std::rotate(replay.meshes.begin(), replay.meshes.end() - 1, replay.meshes.end());
            Verify(render(replay, view) == actual, "alpha-zero input-order changes pixels/depth");
            std::rotate(replay.meshes.begin(), replay.meshes.begin() + 1, replay.meshes.end());
            auto authoredAlpha = source;
            for (auto& material : authoredAlpha.meshes.at(1).surfaces) { material.color[3] = 230.0f / 255.0f; material.vehicleAlpha = true; }
            const auto oldAlpha = render(authoredAlpha, view);
            auto visibleMoving = replay;
            for (auto& material : visibleMoving.meshes.back().surfaces) material.color[3] = 1;
            const auto oldMoving = render(visibleMoving, view);
            size_t alphaChanged = 0, movingChanged = 0, occupied = 0;
            for (size_t i = 0; i < actual.first.size(); i += 3) {
                alphaChanged += !std::equal(actual.first.begin()+i, actual.first.begin()+i+3, oldAlpha.first.begin()+i);
                movingChanged += !std::equal(actual.first.begin()+i, actual.first.begin()+i+3, oldMoving.first.begin()+i);
                occupied += actual.second[i/3] < 1;
            }
            Verify(occupied > 1000, "actual Rustler rendered coverage");
            totalAlphaChanged += alphaChanged; totalMovingChanged += movingChanged;
            std::printf("rustler-gpu view=%d coverage=%zu static230Difference=%zu moving255Difference=%zu zeroAlphaPixelDepthEqual=1 orderEqual=1 ownFBO=1 renderer=%s\n",
                view, occupied, alphaChanged, movingChanged, glGetString(GL_RENDERER));
        }
        Verify(totalAlphaChanged > 0 && totalMovingChanged > 0, "Rustler actual alpha controls visible");
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteRenderbuffers(1, &depthBuffer); glDeleteRenderbuffers(1, &color); glDeleteFramebuffers(1, &framebuffer);
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(display, context); eglDestroySurface(display, surface); eglTerminate(display);
}
