// Surfaceless real-GL probe of the actual --play coordinator, uploader, worker
// and gameplay. No desktop capture. Build: RealtimeStreamingProbe.py in the
// existing container; run its artifact executable on the host for hardware GL.
#include "app/platform/linux/Realtime.cpp"
#include <EGL/eglext.h>
#include <sys/resource.h>

static void Require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "stream-probe FAIL %s EGL=%x GL=%x\n", message, eglGetError(), glGetError());
        std::exit(1);
    }
}

static void Distribution(const char* label, std::vector<double> samples) {
    Require(!samples.empty(), "timing samples");
    std::sort(samples.begin(), samples.end());
    std::printf("stream-probe %s n=%zu p50=%.3f p95=%.3f p99=%.3f max=%.3f ms\n", label, samples.size(),
        samples[samples.size() / 2], samples[samples.size() * 95 / 100], samples[samples.size() * 99 / 100], samples.back());
}

int main(int argc, char** argv) {
    Require(argc == 2 || argc == 3, "arguments: game-dir [run|close-cpu|close-gpu|close-retire]");
    const std::string mode = argc == 3 ? argv[2] : "run";
    Require(mode == "run" || mode == "close-cpu" || mode == "close-gpu" || mode == "close-retire", "mode");
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    Require(SDL_Init(SDL_INIT_EVENTS), "SDL event queue");
    const auto display = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
    Require(display != EGL_NO_DISPLAY && eglInitialize(display, nullptr, nullptr), "EGL initialize");
    Require(eglBindAPI(EGL_OPENGL_API), "OpenGL API");
    const EGLint attributes[]{EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_DEPTH_SIZE, 24, EGL_NONE};
    EGLConfig config{};
    EGLint count{};
    Require(eglChooseConfig(display, attributes, &config, 1, &count) && count == 1, "EGL config");
    constexpr int width = 1280, height = 720;
    const EGLint size[]{EGL_WIDTH, width, EGL_HEIGHT, height, EGL_NONE};
    const auto surface = eglCreatePbufferSurface(display, config, size);
    const auto context = eglCreateContext(display, config, EGL_NO_CONTEXT, nullptr);
    Require(surface != EGL_NO_SURFACE && context != EGL_NO_CONTEXT &&
        eglMakeCurrent(display, surface, surface, context), "EGL context");
    std::printf("stream-probe renderer=%s GL=%s mode=%s radius=900 cap=4096\n",
        glGetString(GL_RENDERER), glGetString(GL_VERSION), mode.c_str());
    {
        Pager pager;
        E2ELoadInfo load{};
        char error[512]{};
        Require(StreamPager_Init(argv[1], load, error, sizeof(error), {true, 900, 4096}), error);
        RealtimeEnvironment environment;
        Require(environment.Load(argv[1], error, sizeof(error)) && environment.Upload(error, sizeof(error)), error);
        Camera camera;
        RealtimeGameplay gameplay;
        GpuScene actors;
        auto world = std::make_unique<LiveWorld>();
        Require(world->Initialize({camera.x, camera.y, camera.z}, true), "initial world");
        std::string gameplayError;
        Require(gameplay.Initialize(argv[1], gameplayError), gameplayError.c_str());
        Require(gameplay.Spawn(world->active->cpu->Collision, camera.x, camera.y, camera.z, camera.yaw, gameplayError), gameplayError.c_str());
        Require(actors.UploadTextures(gameplay.Actors()), "actor upload");
        glEnable(GL_DEPTH_TEST);
        glEnable(GL_ALPHA_TEST);
        glAlphaFunc(GL_GREATER, 0.5f);
        glDisable(GL_CULL_FACE);
        auto draw = [&](bool immediate = false) {
            camera.Apply(width, height, 1600);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            environment.DrawSky(camera.x, camera.y, camera.z);
            environment.BeginWorld();
            if (immediate) world->active->gpu.Draw(world->active->cpu->Scene);
            else world->active->gpu.Render();
            environment.EndWorld();
            environment.BeginObjects();
            actors.Draw(gameplay.Actors());
            environment.EndWorld();
            environment.DrawWater();
            glFinish(); // include actual GPU completion, not submission-only timing
            Require(glGetError() == GL_NO_ERROR, "frame GL error");
        };
        if (mode == "run") {
            // Exact material/GLSL attribute/order regression against original
            // immediate emission, before any worker exists. No framebuffer saved.
            for (float hour : {12.0f, 0.0f}) {
                environment.SetHour(hour);
                std::vector<uint8_t> direct(width * height * 4), staged(direct.size());
                draw(true);
                glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, direct.data());
                draw();
                glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, staged.data());
                Require(direct == staged, "immediate/staged pixels exact (day/night)");
            }
            for (size_t i = 0; i < world->active->cpu->Scene.images.size(); ++i) {
                const auto& image = world->active->cpu->Scene.images[i];
                std::vector<uint8_t> rgba(image.rgba.size());
                glBindTexture(GL_TEXTURE_2D, world->active->gpu.textures[i]);
                glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
                Require(rgba == image.rgba, "striped RGBA exact");
            }
            std::printf("stream-probe exactPixels=day,night exactTextures=%zu tris=%d collision=%zu\n",
                world->active->cpu->Scene.images.size(), world->active->cpu->Frame.tris,
                world->active->cpu->Collision.TriangleCount());
        }
        environment.SetHour(12);
        draw();
        world->Start(true); // no more parsers anywhere on this thread
        std::vector<double> workTimes, streamTimes, gaps;
        workTimes.reserve(2400); streamTimes.reserve(2400); gaps.reserve(2400);
        uint64_t events = 0, ticks = 0;
        int publications = 0, buildingFrames = 0, uploadingFrames = 0, retiringFrames = 0;
        double previous = realtime_streaming::Milliseconds();
        const double started = previous;
        const auto firstPose = gameplay.Actors().meshes.front().pos;
        bool stoppedAtPhase = false;
        camera.y -= 41;
        for (int frame = 0; frame < 2400; ++frame) {
            const double now = realtime_streaming::Milliseconds();
            const double dt = frame ? (now - previous) / 1000.0 : 1.0 / 60;
            if (frame) gaps.push_back(now - previous);
            previous = now;
            SDL_Event inputEvent{};
            inputEvent.type = SDL_EVENT_USER;
            inputEvent.user.code = frame;
            Require(SDL_PushEvent(&inputEvent), "push input");
            SDL_Event event{};
            bool polled = false;
            while (SDL_PollEvent(&event)) {
                if (event.type == SDL_EVENT_USER) {
                    Require(event.user.code == frame, "input processed in same frame");
                    ++events;
                    polled = true;
                }
            }
            Require(polled, "input continuity");
            const double streamStart = realtime_streaming::Milliseconds();
            bool published = false;
            Require(world->Advance({camera.x, camera.y, camera.z}, published), "advance");
            const double streamMs = realtime_streaming::Milliseconds() - streamStart;
            if (world->worker->Building()) ++buildingFrames;
            if (world->pending) ++uploadingFrames;
            if (world->retiring) ++retiringFrames;
            auto* physicsWorld = world->active.get();
            const auto generation = physicsWorld->cpu->Generation;
            const auto oldTicks = gameplay.State().Ticks;
            gameplay.Tick(std::min(dt, 0.1), {.Jump = frame == 5}, physicsWorld->cpu->Collision);
            Require(gameplay.State().Ticks > oldTicks, "physics continuity");
            ++ticks;
            Require(std::isfinite(gameplay.State().Ped.Z) && gameplay.State().Ped.Z > 0, "real ground retained");
            Require(world->active.get() == physicsWorld && world->active->cpu->Generation == generation,
                "matching render/collision publication");
            draw();
            const double workMs = realtime_streaming::Milliseconds() - now;
            workTimes.push_back(workMs);
            streamTimes.push_back(streamMs);
            if (published) {
                ++publications;
                std::printf("stream-probe publication=%d generation=%llu frames=%d events=%llu ticks=%llu ground=%.3f tris=%d\n",
                    publications, static_cast<unsigned long long>(generation), frame + 1,
                    static_cast<unsigned long long>(events), static_cast<unsigned long long>(gameplay.State().Ticks),
                    gameplay.State().Ped.Z, world->active->cpu->Frame.tris);
                // Reverse on alternate updates, and keep updating the mailbox
                // while GPU work is pending; never abandon every moving result.
                camera.y += publications % 2 ? -45.0f : 45.0f;
            } else {
                camera.x += static_cast<float>(std::min(dt, 0.1) * 0.5);
            }
            const bool closeCpu = mode == "close-cpu" && world->worker->Building();
            const bool closeGpu = mode == "close-gpu" && world->pending &&
                world->pending->gpu.uploadImage > 0 && !world->pending->gpu.complete;
            const bool closeRetire = mode == "close-retire" && world->retiring;
            if (closeCpu || closeGpu || closeRetire || (mode == "run" && publications >= 4 && !world->retiring)) {
                stoppedAtPhase = true;
                break;
            }
            const double remaining = 1000.0 / 60 - (realtime_streaming::Milliseconds() - now);
            if (remaining > 0) SDL_DelayNS(static_cast<Uint64>(remaining * 1e6));
        }
        Require(stoppedAtPhase, "bounded test completed");
        Require(events == ticks && ticks == workTimes.size(), "input/physics/render count");
        rusage usage{};
        Require(getrusage(RUSAGE_SELF, &usage) == 0, "peak memory");
        if (mode == "run") {
            Require(buildingFrames > 10 && uploadingFrames > 10 && retiringFrames > 0, "all concurrent phases exercised");
            Require(gameplay.Actors().meshes.front().pos != firstPose, "live actor animation");
            Require(gameplay.State().Jumps == 1 && gameplay.State().Landings == 1, "jump/landing across streaming");
            Distribution("frameGap", gaps);
        }
        Distribution("frameWork", workTimes);
        Distribution("streamWork", streamTimes);
        std::printf("stream-probe continuity frames=%zu input=%llu ticks=%llu buildingFrames=%d uploadingFrames=%d retiringFrames=%d publications=%d seconds=%.3f peakRssMiB=%.1f\n",
            workTimes.size(), static_cast<unsigned long long>(events), static_cast<unsigned long long>(gameplay.State().Ticks),
            buildingFrames, uploadingFrames, retiringFrames, publications, (realtime_streaming::Milliseconds() - started) / 1000,
            usage.ru_maxrss / 1024.0);
        const double closeStart = realtime_streaming::Milliseconds();
        world.reset(); // same joined shutdown as --play, context and pager alive
        Require(glGetError() == GL_NO_ERROR, "shutdown GL error");
        std::printf("stream-probe PASS mode=%s closeMs=%.3f joined=1\n", mode.c_str(), realtime_streaming::Milliseconds() - closeStart);
    }
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(display, context);
    eglDestroySurface(display, surface);
    eglTerminate(display);
    SDL_Quit();
}
