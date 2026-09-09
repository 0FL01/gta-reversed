// SDL owns the window and EGL context on Wayland. Real DFF triangles/TXD
// textures are rasterized by GL, not by TexSample's offline CPU renderer.
#include "app/platform/linux/Realtime.h"
#include "app/platform/linux/StreamPager.h"

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

    bool Upload(const WorldShotScene& scene) {
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
        list = glGenLists(1);
        if (!list) {
            std::printf("play-fail GL display list allocation\n");
            return false;
        }
        glNewList(list, GL_COMPILE);
        for (const auto& mesh : scene.meshes) {
            assert(mesh.pos.size() == static_cast<size_t>(mesh.tris) * 9);
            assert(mesh.nrm.size() == mesh.pos.size());
            const bool hasUV = mesh.uv.size() == static_cast<size_t>(mesh.tris) * 6;
            const bool hasImages = mesh.triImg.size() == static_cast<size_t>(mesh.tris);
            const bool hasColors = mesh.triCol.size() == static_cast<size_t>(mesh.tris) * 3;
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
                for (int k = 0; k < 3; ++k) {
                    const size_t v = static_cast<size_t>(t) * 3 + k;
                    const float* n = &mesh.nrm[v * 3];
                    const float shade = 0.4f + 0.6f * std::max(0.0f,
                        n[0] * 0.2673f + n[1] * 0.5345f + n[2] * 0.8018f);
                    // Missing textures remain visibly grey, never invented.
                    if (image == -2) {
                        glColor3f(shade * 0.5f, shade * 0.5f, shade * 0.5f);
                    } else {
                        glColor3f(shade * color[0], shade * color[1], shade * color[2]);
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
        glEndList();
        const auto error = glGetError();
        if (error != GL_NO_ERROR) {
            std::printf("play-fail upload GL=0x%x\n", error);
            return false;
        }
        return true;
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

    void Apply(int width, int height) const {
        glViewport(0, 0, width, height);
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        const double top = 0.1 * std::tan(3.14159265 / 6.0);
        const double right = top * width / height;
        glFrustum(-right, right, -top, top, 0.1, 1600.0);
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
        }
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
    window.window = SDL_CreateWindow("mad-sa | OpenGL world | WASD / arrows / Q E / Esc",
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
    std::printf("play-controls WASD move; Q/E vertical; arrows look; Shift fast; Esc quit; demo=%d\n", demo);
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
    Camera camera;
    float loadedX = camera.x, loadedY = camera.y;
    int updates = 0;
    auto updateWorld = [&]() {
        WorldShotScene scene{};
        E2EPagerFrame frame{};
        if (!StreamPager_Update(camera.x, camera.y, camera.z, scene, frame, error, sizeof(error))) {
            std::printf("play-fail pager update: %s\n", error);
            return false;
        }
        if (!gpu.Upload(scene)) {
            return false;
        }
        loadedX = camera.x;
        loadedY = camera.y;
        ++updates;
        std::printf("play-scene update=%d instances=%d tris=%d textures=%zu cam=%.1f,%.1f,%.1f\n",
            updates, frame.instances, frame.tris, scene.images.size(), camera.x, camera.y, camera.z);
        std::fflush(stdout);
        return true;
    };
    if (!updateWorld()) {
        return 1;
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
    while (running) {
        const Uint64 now = SDL_GetTicksNS();
        if (seconds > 0 && static_cast<double>(now - start) / 1e9 >= seconds) {
            break;
        }
        const float dt = std::min(static_cast<float>(now - previous) / 1e9f, 0.1f);
        previous = now;
        SDL_Event event{};
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED ||
                (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE)) {
                running = false;
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
        camera.Update(dt, SDL_GetKeyboardState(nullptr), demo);
        if (std::hypot(camera.x - loadedX, camera.y - loadedY) >= 40.0f && !updateWorld()) {
            return 1;
        }
        camera.Apply(width, height);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glCallList(gpu.list);
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
            report = now;
            reportFrames = frames;
        }
    }
    std::printf("play-ok swaps=%llu seconds=%.3f sceneUpdates=%d\n",
        static_cast<unsigned long long>(frames), static_cast<double>(SDL_GetTicksNS() - start) / 1e9, updates);
    return 0;
}
