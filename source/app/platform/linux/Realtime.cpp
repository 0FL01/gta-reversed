// SDL owns the window and EGL context on Wayland. Real DFF triangles/TXD
// textures are rasterized by GL, not by TexSample's offline CPU renderer.
#include "app/platform/linux/Realtime.h"
#include "app/platform/linux/StreamPager.h"
#include "app/platform/linux/RealtimeEnvironment.h"
#include "app/platform/linux/RealtimeGameplay.h"

#include <SDL3/SDL.h>
#include <EGL/egl.h>
#include <GL/gl.h>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <string>

namespace {
struct Window {
    SDL_Window* window = nullptr;
    SDL_GLContext context = nullptr;

    ~Window() {
        if (context) {
            SDL_GL_DestroyContext(context);
        }
        if (window) {
            SDL_DestroyWindow(window);
        }
        SDL_Quit();
    }
};

struct Pager {
    ~Pager() { StreamPager_Shutdown(); }
};

static GLint WrapMode(uint32_t mode) {
    if (mode == 2) {
        return GL_MIRRORED_REPEAT;
    }
    return mode == 3 || mode == 4 ? GL_CLAMP_TO_EDGE : GL_REPEAT;
}

// Compatibility GL is already a native-track dependency. Compile static
// geometry once per pager update; no per-frame asset IO or texture upload.
struct GpuScene {
    GLuint list = 0;
    std::vector<GLuint> textures;

    ~GpuScene() { Clear(); }

    void Clear() {
        if (list) {
            glDeleteLists(list, 1);
            list = 0;
        }
        glDeleteTextures(static_cast<GLsizei>(textures.size()), textures.data());
        textures.clear();
    }

    bool UploadTextures(const WorldShotScene& scene) {
        Clear();
        textures.resize(scene.images.size());
        glGenTextures(static_cast<GLsizei>(textures.size()), textures.data());
        for (size_t i = 0; i < scene.images.size(); ++i) {
            const auto& image = scene.images[i];
            if (image.w <= 0 || image.h <= 0 ||
                image.rgba.size() != static_cast<size_t>(image.w) * image.h * 4) {
                std::printf("play-fail invalid texture %s\n", image.name);
                return false;
            }
            glBindTexture(GL_TEXTURE_2D, textures[i]);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, WrapMode((image.filter >> 8) & 15));
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, WrapMode((image.filter >> 12) & 15));
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, image.w, image.h, 0,
                         GL_RGBA, GL_UNSIGNED_BYTE, image.rgba.data());
        }
        return glGetError() == GL_NO_ERROR;
    }

    bool Upload(const WorldShotScene& scene) {
        if (!UploadTextures(scene)) {
            return false;
        }
        list = glGenLists(1);
        if (!list) {
            std::printf("play-fail GL display list allocation\n");
            return false;
        }
        glNewList(list, GL_COMPILE);
        Draw(scene);
        glEndList();
        const auto error = glGetError();
        if (error != GL_NO_ERROR) {
            std::printf("play-fail upload GL=0x%x\n", error);
            return false;
        }
        return true;
    }

    // Dynamic actors retain textures; only posed vertices change each tick.
    void Draw(const WorldShotScene& scene) const {
        for (const auto& mesh : scene.meshes) {
            assert(mesh.pos.size() == static_cast<size_t>(mesh.tris) * 9);
            assert(mesh.nrm.size() == mesh.pos.size());
            const bool hasUV = mesh.uv.size() == static_cast<size_t>(mesh.tris) * 6;
            const bool hasImages = mesh.triImg.size() == static_cast<size_t>(mesh.tris);
            const bool hasColors = mesh.triCol.size() == static_cast<size_t>(mesh.tris) * 3;
            const bool hasSurfaces = mesh.surfaces.size() == static_cast<size_t>(mesh.tris);
            const bool hasDay = mesh.dayColors.size() == static_cast<size_t>(mesh.tris) * 12;
            const bool hasNight = mesh.nightColors.size() == mesh.dayColors.size() && hasDay;
            int previous = -3;
            for (int t = 0; t < mesh.tris; ++t) {
                const int image = hasImages && hasUV ? mesh.triImg[t] : -1;
                assert(image < static_cast<int>(textures.size()));
                if (image != previous) {
                    if (t) {
                        glEnd();
                    }
                    if (image >= 0) {
                        glEnable(GL_TEXTURE_2D);
                        glBindTexture(GL_TEXTURE_2D, textures[image]);
                    } else {
                        glDisable(GL_TEXTURE_2D);
                    }
                    glBegin(GL_TRIANGLES);
                    previous = image;
                }
                const float* color = hasColors ? &mesh.triCol[t * 3] : mesh.color;
                const auto* surface = hasSurfaces ? &mesh.surfaces[t] : nullptr;
                glMultiTexCoord2f(GL_TEXTURE3, surface ? surface->ambient : 1.0f,
                                  surface ? surface->diffuse : 1.0f);
                for (int k = 0; k < 3; ++k) {
                    const size_t v = static_cast<size_t>(t) * 3 + k;
                    const float* n = &mesh.nrm[v * 3];
                    glNormal3fv(n);
                    const uint8_t black[]{0, 0, 0, 255};
                    const auto* day = hasDay ? &mesh.dayColors[v * 4] : black;
                    const auto* night = hasNight ? &mesh.nightColors[v * 4] : day;
                    glMultiTexCoord4f(GL_TEXTURE1, day[0] / 255.0f, day[1] / 255.0f,
                                      day[2] / 255.0f, day[3] / 255.0f);
                    glMultiTexCoord4f(GL_TEXTURE2, night[0] / 255.0f, night[1] / 255.0f,
                                      night[2] / 255.0f, night[3] / 255.0f);
                    // Preserve the authored material (including car paint)
                    // even when its texture is absent. The diagnostic grey
                    // belongs only to legacy scenes without material metadata.
                    if (surface) {
                        glColor4fv(surface->color.data());
                    } else if (image == -2) {
                        glColor3f(0.5f, 0.5f, 0.5f);
                    } else {
                        glColor3f(color[0], color[1], color[2]);
                    }
                    if (hasUV) {
                        glTexCoord2fv(&mesh.uv[v * 2]);
                    }
                    glVertex3fv(&mesh.pos[v * 3]);
                }
            }
            if (mesh.tris) {
                glEnd();
            }
        }
        glDisable(GL_TEXTURE_2D);
    }
};

struct Camera {
    float x = 1600.0f, y = -1700.0f, z = 70.0f;
    float yaw = -1.43f, pitch = -0.20f;

    void Update(float dt, const bool* keys, bool demo) {
        yaw += (keys[SDL_SCANCODE_LEFT] - keys[SDL_SCANCODE_RIGHT]) * dt;
        pitch = std::clamp(pitch + (keys[SDL_SCANCODE_UP] - keys[SDL_SCANCODE_DOWN]) * dt,
                           -1.4f, 1.4f);
        const float speed = keys[SDL_SCANCODE_LSHIFT] ? 90.0f : 25.0f;
        const float forward = demo ? 8.0f : (keys[SDL_SCANCODE_W] - keys[SDL_SCANCODE_S]) * speed;
        const float side = (keys[SDL_SCANCODE_D] - keys[SDL_SCANCODE_A]) * speed;
        x += (std::cos(yaw) * forward + std::sin(yaw) * side) * dt;
        y += (std::sin(yaw) * forward - std::cos(yaw) * side) * dt;
        z += (keys[SDL_SCANCODE_E] - keys[SDL_SCANCODE_Q]) * speed * dt;
    }

    void Apply(int width, int height, float farPlane) const {
        glViewport(0, 0, width, height);
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        const double top = 0.1 * std::tan(3.14159265 / 6.0);
        const double right = top * width / height;
        glFrustum(-right, right, -top, top, 0.1, farPlane);
        const float cy = std::cos(yaw), sy = std::sin(yaw);
        const float cp = std::cos(pitch), sp = std::sin(pitch);
        // Z-up look-at, column-major OpenGL view matrix.
        const float view[] = {
            sy, -cy * sp, -cy * cp, 0,
            -cy, -sy * sp, -sy * cp, 0,
            0, cp, -sp, 0,
            0, 0, 0, 1
        };
        glMatrixMode(GL_MODELVIEW);
        glLoadMatrixf(view);
        glTranslatef(-x, -y, -z);
    }
};
} // namespace

int Realtime_Run(int argc, char** argv, const char* gameDir) {
    double seconds = 0;
    bool demo = false;
    bool demoCurb = false;
    bool freecam = false;
    bool freezeTime = false;
    float hour = 12.0f;
    const char* weather = "EXTRASUNNY_LA";
    Camera camera;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--seconds") == 0) {
            char* end = nullptr;
            if (++i == argc) {
                std::printf("play-fail --seconds needs a positive number\n");
                return 1;
            }
            seconds = std::strtod(argv[i], &end);
            if (end == argv[i] || *end || !std::isfinite(seconds) || seconds <= 0) {
                std::printf("play-fail --seconds needs a positive number\n");
                return 1;
            }
        } else if (std::strcmp(argv[i], "--demo") == 0) {
            demo = true;
        } else if (std::strcmp(argv[i], "--demo-curb") == 0) {
            demoCurb = true;
        } else if (std::strcmp(argv[i], "--freecam") == 0) {
            freecam = true;
        } else if (std::strcmp(argv[i], "--freeze-time") == 0) {
            freezeTime = true;
        } else if (std::strcmp(argv[i], "--hour") == 0) {
            char* end = nullptr;
            if (++i == argc) {
                std::printf("play-fail --hour needs a number in [0,24)\n");
                return 1;
            }
            hour = std::strtof(argv[i], &end);
            if (end == argv[i] || *end || !std::isfinite(hour) || hour < 0 || hour >= 24) {
                std::printf("play-fail invalid --hour\n");
                return 1;
            }
        } else if (std::strcmp(argv[i], "--weather") == 0) {
            if (++i == argc) {
                std::printf("play-fail --weather needs a timecycle section\n");
                return 1;
            }
            weather = argv[i];
        } else if (std::strcmp(argv[i], "--cam") == 0) {
            int used = 0;
            if (++i == argc || std::sscanf(argv[i], "%f,%f,%f%n", &camera.x, &camera.y, &camera.z, &used) != 3 ||
                argv[i][used] || !std::isfinite(camera.x) || !std::isfinite(camera.y) || !std::isfinite(camera.z) ||
                std::abs(camera.x) > 6000 || std::abs(camera.y) > 6000 || std::abs(camera.z) > 2000) {
                std::printf("play-fail --cam needs finite map coordinates x,y,z\n");
                return 1;
            }
        }
    }
    if (demoCurb) {
        if (freecam || demo) {
            std::printf("play-fail --demo-curb requires gameplay without --demo/--freecam\n");
            return 1;
        }
        // Measured real 17 cm curb, not generated terrain or a pose fixture.
        camera.x = 1540.0f;
        camera.y = -1736.0f;
        camera.z = 20.0f;
        camera.yaw = -1.57079637f;
    }
    Window window;
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::printf("play-fail SDL init: %s\n", SDL_GetError());
        return 1;
    }
    // OpenGL 2.1 compatibility supports the native track's fixed pipeline.
    if (!SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2) ||
        !SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1) ||
        !SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1) ||
        !SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24)) {
        std::printf("play-fail GL attributes: %s\n", SDL_GetError());
        return 1;
    }
    window.window = SDL_CreateWindow("mad-sa | OpenGL gameplay | WASD / arrows / Space / F / Tab / Esc",
        1280, 720, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!window.window || !(window.context = SDL_GL_CreateContext(window.window)) ||
        !SDL_GL_MakeCurrent(window.window, window.context)) {
        std::printf("play-fail GL window/context: %s\n", SDL_GetError());
        return 1;
    }
    if (!SDL_GL_SetSwapInterval(1)) {
        std::printf("play-note vsync unavailable: %s (60 Hz timer remains enabled)\n", SDL_GetError());
    }
    std::printf("play-video driver=%s EGL=%s GL=%s renderer=%s\n", SDL_GetCurrentVideoDriver(),
        eglGetCurrentContext() != EGL_NO_CONTEXT ? "yes" : "no",
        glGetString(GL_VERSION), glGetString(GL_RENDERER));
    std::printf("play-controls WASD move/drive; arrows look; Shift sprint; Space jump/handbrake; F enter/exit; "
                "Tab free camera; Esc quit; demo=%d freecam=%d\n", demo, freecam);
    std::fflush(stdout);

    Pager pager;
    E2ELoadInfo load{};
    char error[512] = {};
    const StreamPagerOptions options{ .includeStreamed = true, .radius = 900.0f, .maxInstances = 4096 };
    if (!StreamPager_Init(gameDir, load, error, sizeof(error), options)) {
        std::printf("play-fail pager init: %s\n", error);
        return 1;
    }
    std::printf("play-load textAndBinary=%d outdoor=%d binaryFiles=%d binaryInstances=%d radius=%.0f cap=%d\n",
        load.iplTotal, load.iplKept, load.binaryIplFiles, load.binaryInstances, options.radius, options.maxInstances);
    GpuScene gpu;
    GpuScene actorGpu;
    RealtimeEnvironment environment;
    if (!environment.Load(gameDir, error, sizeof(error), hour, weather) ||
        !environment.Upload(error, sizeof(error))) {
        std::printf("play-fail environment: %s\n", error);
        return 1;
    }
    std::printf("play-environment hour=%.2f weather=%s waterTris=%d clock=%s\n", hour, weather,
                environment.GetWaterTriangleCount(), freezeTime ? "frozen" : "one-minute-per-second");
    const bool gameplayEnabled = !freecam;
    RealtimeGameplay gameplay;
    RealtimeGameplayWorld collision;
    std::string gameplayError;
    WorldShotScene world;
    float loadedX = camera.x, loadedY = camera.y;
    int updates = 0;
    auto updateWorld = [&]() {
        E2EPagerFrame frame{};
        if (!StreamPager_Update(camera.x, camera.y, camera.z, world, frame, error, sizeof(error))) {
            std::printf("play-fail pager update: %s\n", error);
            return false;
        }
        if (!gpu.Upload(world)) {
            return false;
        }
        if (gameplayEnabled && !collision.Rebuild(world, gameplayError)) {
            std::printf("play-fail collision: %s\n", gameplayError.c_str());
            return false;
        }
        loadedX = camera.x;
        loadedY = camera.y;
        ++updates;
        std::printf("play-scene update=%d instances=%d tris=%d textures=%zu cam=%.1f,%.1f,%.1f\n",
            updates, frame.instances, frame.tris, world.images.size(), camera.x, camera.y, camera.z);
        std::fflush(stdout);
        return true;
    };
    if (!updateWorld()) {
        return 1;
    }
    if (gameplayEnabled) {
        const auto initStart = SDL_GetTicksNS();
        if (!gameplay.Initialize(gameDir, gameplayError) ||
            !gameplay.Spawn(collision, camera.x, camera.y, camera.z, camera.yaw, gameplayError) ||
            !actorGpu.UploadTextures(gameplay.Actors())) {
            std::printf("play-fail gameplay: %s\n", gameplayError.c_str());
            return 1;
        }
        std::printf("play-gameplay-init seconds=%.3f collisionTris=%zu actorTris=%d textures=%zu\n",
            static_cast<double>(SDL_GetTicksNS() - initStart) / 1e9, collision.TriangleCount(),
            gameplay.Actors().stats.triangles, gameplay.Actors().images.size());
    }
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_ALPHA_TEST);
    glAlphaFunc(GL_GREATER, 0.5f); // TXD cutouts (foliage/fences), no opaque rectangles.
    glDisable(GL_CULL_FACE);
    glClearColor(0.38f, 0.55f, 0.72f, 1.0f);

    const Uint64 start = SDL_GetTicksNS();
    Uint64 previous = start, report = start;
    uint64_t frames = 0, reportFrames = 0;
    bool running = true;
    bool demoJumped = false;
    bool demoEntered = false;
    bool demoExited = false;
    while (running) {
        const Uint64 now = SDL_GetTicksNS();
        if (seconds > 0 && static_cast<double>(now - start) / 1e9 >= seconds) {
            break;
        }
        const float dt = std::min(static_cast<float>(now - previous) / 1e9f, 0.1f);
        previous = now;
        SDL_Event event{};
        RealtimeGameplayInput input{};
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED ||
                (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE)) {
                running = false;
            }
            if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat) {
                input.Jump |= event.key.key == SDLK_SPACE;
                input.Interact |= event.key.key == SDLK_F;
                if (event.key.key == SDLK_TAB && gameplayEnabled) {
                    freecam = !freecam;
                }
            }
        }
        if (!running) {
            break;
        }
        int width = 0, height = 0;
        if (!SDL_GetWindowSizeInPixels(window.window, &width, &height)) {
            std::printf("play-fail window size: %s\n", SDL_GetError());
            return 1;
        }
        if (width <= 0 || height <= 0 || (SDL_GetWindowFlags(window.window) & SDL_WINDOW_MINIMIZED)) {
            SDL_Delay(50);
            continue;
        }
        const bool* keys = SDL_GetKeyboardState(nullptr);
        if (freecam) {
            camera.Update(dt, keys, demo);
        }
        if (gameplayEnabled) {
            if (!freecam) {
                input.Forward = keys[SDL_SCANCODE_W] - keys[SDL_SCANCODE_S];
                input.Side = keys[SDL_SCANCODE_D] - keys[SDL_SCANCODE_A];
                input.Sprint = keys[SDL_SCANCODE_LSHIFT];
                input.Brake = keys[SDL_SCANCODE_LCTRL];
                input.Handbrake = keys[SDL_SCANCODE_SPACE];
                input.LookYaw = (keys[SDL_SCANCODE_LEFT] - keys[SDL_SCANCODE_RIGHT]) * dt;
                input.LookPitch = (keys[SDL_SCANCODE_UP] - keys[SDL_SCANCODE_DOWN]) * dt;
                if (demo) {
                    // Use the interactive input/state path, never teleports
                    // or forced vehicle ownership to make a demo appear green.
                    input = {};
                    const double time = gameplay.State().SimulatedSeconds;
                    if (!demoJumped && time >= 0.5) {
                        input.Jump = demoJumped = true;
                    }
                    if (!demoEntered && time >= 2.0) {
                        input.Interact = demoEntered = true;
                    }
                    input.Forward = (time >= 2.2 && time < 6.0) || time >= 10.0 ? 1.0f : 0.0f;
                    input.Brake = time >= 6.0 && time < 9.0;
                    if (!demoExited && time >= 9.0) {
                        input.Interact = demoExited = true;
                    }
                }
                if (demoCurb) {
                    input = {};
                    const double time = gameplay.State().SimulatedSeconds;
                    if (time >= 0.5 && time < 2.5) {
                        input.Forward = 1.0f;
                    } else if (time >= 2.5 && time < 4.5) {
                        input.Forward = -1.0f;
                    } else if (time >= 5.0 && time < 6.4) {
                        input.Forward = time < 5.7 ? 1.0f : -1.0f;
                        input.Sprint = true;
                    }
                }
            } else {
                input = {};
            }
            gameplay.Tick(dt, input, collision);
            if (!freecam) {
                const auto& view = gameplay.Camera();
                camera.x = view.Position.X;
                camera.y = view.Position.Y;
                camera.z = view.Position.Z;
                camera.yaw = view.Yaw;
                camera.pitch = view.Pitch;
            }
        }
        if (std::hypot(camera.x - loadedX, camera.y - loadedY) >= 40.0f && !updateWorld()) {
            return 1;
        }
        camera.Apply(width, height, std::max(1600.0f, environment.GetParams().farClip));
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        if (!freezeTime) {
            hour = std::fmod(hour + dt / 60.0f, 24.0f);
            environment.SetHour(hour);
        }
        environment.DrawSky(camera.x, camera.y, camera.z);
        environment.BeginWorld();
        glCallList(gpu.list);
        environment.EndWorld();
        if (gameplayEnabled) {
            environment.BeginObjects();
            actorGpu.Draw(gameplay.Actors());
            environment.EndWorld();
        }
        environment.DrawWater();
        const auto glError = glGetError();
        if (glError != GL_NO_ERROR || !SDL_GL_SwapWindow(window.window)) {
            std::printf("play-fail present GL=0x%x SDL=%s\n", glError, SDL_GetError());
            return 1;
        }
        ++frames;
        // Always pace: swap interval may be ignored/overridden by the driver
        // or overlay. No busy-spin and no batch-waypoint pseudo-FPS.
        const Uint64 elapsed = SDL_GetTicksNS() - now;
        constexpr Uint64 frameNs = 1'000'000'000 / 60;
        if (elapsed < frameNs) {
            SDL_DelayNS(frameNs - elapsed);
        }
        if (now - report >= 1'000'000'000) {
            std::printf("play-frame swaps=%llu fps=%.1f drawable=%dx%d cam=%.1f,%.1f,%.1f\n",
                static_cast<unsigned long long>(frames),
                static_cast<double>(frames - reportFrames) * 1e9 / (now - report),
                width, height, camera.x, camera.y, camera.z);
            std::fflush(stdout);
            if (gameplayEnabled) {
                const auto& state = gameplay.State();
                std::printf("play-state ticks=%llu mode=%s grounded=%d ped=%.2f,%.2f,%.2f car=%.2f,%.2f,%.2f "
                    "speed=%.2f walk=%.2f drive=%.2f jumps=%llu landings=%llu entries=%llu exits=%llu anim=%s "
                    "blocked=%llu phase=%.4f moveBlend=%.3f runBlend=%.3f airBlend=%.3f\n",
                    static_cast<unsigned long long>(state.Ticks), state.InVehicle ? "car" : "foot", state.Grounded,
                    state.Ped.X, state.Ped.Y, state.Ped.Z, state.Car.X, state.Car.Y, state.Car.Z,
                    state.Speed, state.WalkDistance, state.DriveDistance,
                    static_cast<unsigned long long>(state.Jumps), static_cast<unsigned long long>(state.Landings),
                    static_cast<unsigned long long>(state.Entries), static_cast<unsigned long long>(state.Exits), state.Animation,
                    static_cast<unsigned long long>(state.BlockedSteps), state.LocomotionPhase,
                    state.LocomotionBlend, state.RunBlend, state.AirBlend);
                std::fflush(stdout);
            }
            report = now;
            reportFrames = frames;
        }
    }
    std::printf("play-ok swaps=%llu seconds=%.3f sceneUpdates=%d\n",
        static_cast<unsigned long long>(frames), static_cast<double>(SDL_GetTicksNS() - start) / 1e9, updates);
    return 0;
}
