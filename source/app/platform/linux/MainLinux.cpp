// mad-sa Linux native entry point (R1 skeleton, R2 SDL3 video, R3 headless data, R4 OpenAL, R5 EGL/GL).
// Standalone `main()` for the `mad-sa-linux` ELF track. It must not depend on
// the Windows DLL/hook model (`dllmain`/`InjectHooks`) nor on Win libraries.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <SDL3/SDL.h>

#include <AL/al.h>
#include <AL/alc.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>

using int8 = int8_t;
using int16 = int16_t;
using int32 = int32_t;
using int64 = int64_t;
using uint8 = uint8_t;
using uint16 = uint16_t;
using uint32 = uint32_t;
using uint64 = uint64_t;

#ifndef __stdcall
#define __stdcall
#endif

#include "oswrapper/oswrapper.h"

namespace {
void PrintUsage(const char* prog) {
    (void)std::printf(
        "usage: %s --smoke | --smoke-video | --smoke-audio | --headless [--ticks N] | --shot <out.tga> [--frames N]\n",
        prog ? prog : "mad-sa-linux"
    );
}

bool HasArg(int argc, char** argv, const char* want) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], want) == 0) {
            return true;
        }
    }
    return false;
}

const char* ArgValue(int argc, char** argv, const char* want, const char* fallback) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], want) == 0) {
            return argv[i + 1];
        }
    }
    return fallback;
}

bool FileExists(const char* path) {
    void* file = nullptr;
    if (OS_FileOpen(FILE_DATA_AREA_DEFAULT, &file, path, FILE_ACCESS_READ) != 0 || !file) {
        return false;
    }
    OS_FileClose(file);
    return true;
}

bool LoadTextFile(const char* path, std::string& out) {
    void* file = nullptr;
    if (OS_FileOpen(FILE_DATA_AREA_DEFAULT, &file, path, FILE_ACCESS_READ) != 0 || !file) {
        return false;
    }
    int32 size = OS_FileSize(file);
    if (size < 0) {
        OS_FileClose(file);
        return false;
    }
    std::vector<char> buf(static_cast<size_t>(size) + 1, '\0');
    int32 rc = 0;
    if (size > 0) {
        rc = OS_FileRead(file, buf.data(), size);
    }
    OS_FileClose(file);
    if (rc != 0) {
        return false;
    }
    out.assign(buf.data(), static_cast<size_t>(size));
    return true;
}

struct DatStats {
    int total = 0;
    int img = 0;
    int ide = 0;
    int ipl = 0;
    int splash = 0;
};

DatStats ParseDat(const std::string& text) {
    DatStats stats{};
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        std::string line = text.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        pos = end == std::string::npos ? text.size() : end + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
            line.pop_back();
        }
        size_t first = line.find_first_not_of(" \t");
        if (first == std::string::npos || line[first] == '#') {
            continue;
        }
        line = line.substr(first);
        ++stats.total;
        if (line.rfind("IMG", 0) == 0) {
            ++stats.img;
        } else if (line.rfind("IDE", 0) == 0) {
            ++stats.ide;
        } else if (line.rfind("IPL", 0) == 0) {
            ++stats.ipl;
        } else if (line.rfind("SPLASH", 0) == 0) {
            ++stats.splash;
        }
    }
    return stats;
}

std::string ResolveGameDir(int argc, char** argv) {
    const char* override = ArgValue(argc, argv, "--game-dir", nullptr);
    if (override && override[0] != '\0') {
        return std::string(override);
    }
    OS_SetFilePathOffset("");
    const char* candidates[] = { "/game", "Grand-Theft-Auto-San-Andreas", "." };
    for (const char* candidate : candidates) {
        OS_SetFilePathOffset(candidate);
        if (FileExists("data/gta.dat")) {
            return std::string(candidate);
        }
    }
    return std::string("/game");
}

int RunSmokeVideo() {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        (void)std::printf("video-fail init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window* window = SDL_CreateWindow("mad-sa", 640, 480, 0);
    if (!window) {
        (void)std::printf("video-fail window: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    for (int i = 0; i < 10; ++i) {
        SDL_Event event = {};
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) {
                i = 10;
                break;
            }
        }
        OS_ThreadSleep(10);
    }
    SDL_DestroyWindow(window);
    SDL_Quit();
    OS_DebugOut("mad-sa-linux video smoke");
    (void)std::printf("video-ok\n");
    return 0;
}

int RunHeadless(int argc, char** argv) {
    const char* ticksArg = ArgValue(argc, argv, "--ticks", "600");
    int ticks = std::atoi(ticksArg);
    if (ticks <= 0) {
        (void)std::printf("headless-fail bad --ticks '%s'\n", ticksArg);
        return 1;
    }
    std::string gameDir = ResolveGameDir(argc, argv);
    OS_SetFilePathOffset(gameDir.c_str());

    std::string gtaDat;
    if (!LoadTextFile("data/gta.dat", gtaDat)) {
        (void)std::printf("headless-fail cannot read data/gta.dat from '%s'\n", gameDir.c_str());
        return 1;
    }
    std::string defaultDat;
    if (!LoadTextFile("data/default.dat", defaultDat)) {
        (void)std::printf("headless-fail cannot read data/default.dat from '%s'\n", gameDir.c_str());
        return 1;
    }
    DatStats gta = ParseDat(gtaDat);
    DatStats def = ParseDat(defaultDat);
    if (gta.ide <= 0 || gta.ipl <= 0 || def.ide <= 0) {
        (void)std::printf(
            "headless-fail empty dat gta(ide=%d ipl=%d) default(ide=%d)\n",
            gta.ide, gta.ipl, def.ide
        );
        return 1;
    }
    for (int i = 0; i < ticks; ++i) {
        (void)OS_TimeMS();
    }
    OS_DebugOut("mad-sa-linux headless data load");
    (void)std::printf(
        "data-ok ticks=%d gta_lines=%d ide=%d ipl=%d\n",
        ticks, gta.total, gta.ide, gta.ipl
    );
    return 0;
}

int RunSmokeAudio() {
    bool forcedNull = false;
    if (!std::getenv("ALSOFT_DRIVERS")) {
        (void)setenv("ALSOFT_DRIVERS", "null", 1);
        forcedNull = true;
    }
    ALCdevice* device = alcOpenDevice(nullptr);
    const char* backend = forcedNull ? "null" : "default";
    if (!device) {
        (void)std::printf("audio-fail open device\n");
        return 1;
    }
    ALCcontext* context = alcCreateContext(device, nullptr);
    if (!context) {
        (void)std::printf("audio-fail create context\n");
        alcCloseDevice(device);
        return 1;
    }
    if (alcMakeContextCurrent(context) == ALC_FALSE) {
        (void)std::printf("audio-fail make current\n");
        alcDestroyContext(context);
        alcCloseDevice(device);
        return 1;
    }
    ALuint buffer = 0;
    alGenBuffers(1, &buffer);
    if (alGetError() != AL_NO_ERROR || buffer == 0) {
        (void)std::printf("audio-fail gen buffer\n");
        alcMakeContextCurrent(nullptr);
        alcDestroyContext(context);
        alcCloseDevice(device);
        return 1;
    }
    alDeleteBuffers(1, &buffer);
    alcMakeContextCurrent(nullptr);
    alcDestroyContext(context);
    alcCloseDevice(device);
    OS_DebugOut("mad-sa-linux audio smoke");
    (void)std::printf("audio-ok backend=%s\n", backend);
    return 0;
}

bool WriteTga24(const char* path, int width, int height, const std::vector<uint8>& rgba) {
    assert(path && width > 0 && height > 0);
    assert(rgba.size() == static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
    void* file = nullptr;
    OS_SetFilePathOffset("");
    if (OS_FileOpen(FILE_DATA_AREA_DEFAULT, &file, path, FILE_ACCESS_WRITE) != 0 || !file) {
        return false;
    }
    uint8 header[18] = {};
    header[2] = 2;
    header[12] = static_cast<uint8>(width & 0xFF);
    header[13] = static_cast<uint8>((width >> 8) & 0xFF);
    header[14] = static_cast<uint8>(height & 0xFF);
    header[15] = static_cast<uint8>((height >> 8) & 0xFF);
    header[16] = 24;
    bool ok = OS_FileWrite(file, header, sizeof(header)) == 0;
    std::vector<uint8> row(static_cast<size_t>(width) * 3);
    for (int y = 0; ok && y < height; ++y) {
        const uint8* src = rgba.data() + static_cast<size_t>(y) * static_cast<size_t>(width) * 4;
        for (int x = 0; x < width; ++x) {
            row[static_cast<size_t>(x) * 3 + 0] = src[static_cast<size_t>(x) * 4 + 2];
            row[static_cast<size_t>(x) * 3 + 1] = src[static_cast<size_t>(x) * 4 + 1];
            row[static_cast<size_t>(x) * 3 + 2] = src[static_cast<size_t>(x) * 4 + 0];
        }
        ok = OS_FileWrite(file, row.data(), static_cast<int32>(row.size())) == 0;
    }
    OS_FileClose(file);
    return ok;
}

int RunShot(int argc, char** argv) {
    const char* outPath = ArgValue(argc, argv, "--shot", "out.tga");
    const char* framesArg = ArgValue(argc, argv, "--frames", "120");
    int frames = std::atoi(framesArg);
    if (!outPath || outPath[0] == '\0' || frames <= 0) {
        (void)std::printf("shot-fail bad args shot='%s' frames='%s'\n", outPath, framesArg);
        return 1;
    }
    const int width = 640;
    const int height = 480;
    auto getPlatformDisplay = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(
        eglGetProcAddress("eglGetPlatformDisplayEXT")
    );
    if (!getPlatformDisplay) {
        (void)std::printf("shot-fail no eglGetPlatformDisplayEXT\n");
        return 1;
    }
#ifndef EGL_PLATFORM_SURFACELESS_MESA
#define EGL_PLATFORM_SURFACELESS_MESA 0x31DD
#endif
    EGLDisplay display = getPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
    if (display == EGL_NO_DISPLAY) {
        (void)std::printf("shot-fail no surfaceless display 0x%x\n", eglGetError());
        return 1;
    }
    if (!eglInitialize(display, nullptr, nullptr)) {
        (void)std::printf("shot-fail egl init 0x%x\n", eglGetError());
        return 1;
    }
    const EGLint configAttrs[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 24,
        EGL_NONE
    };
    EGLConfig config = nullptr;
    EGLint configCount = 0;
    if (!eglChooseConfig(display, configAttrs, &config, 1, &configCount) || configCount < 1) {
        (void)std::printf("shot-fail choose config 0x%x\n", eglGetError());
        eglTerminate(display);
        return 1;
    }
    const EGLint pbufferAttrs[] = { EGL_WIDTH, width, EGL_HEIGHT, height, EGL_NONE };
    EGLSurface surface = eglCreatePbufferSurface(display, config, pbufferAttrs);
    if (surface == EGL_NO_SURFACE) {
        (void)std::printf("shot-fail pbuffer 0x%x\n", eglGetError());
        eglTerminate(display);
        return 1;
    }
    (void)eglBindAPI(EGL_OPENGL_API);
    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, nullptr);
    if (context == EGL_NO_CONTEXT) {
        (void)std::printf("shot-fail context 0x%x\n", eglGetError());
        eglDestroySurface(display, surface);
        eglTerminate(display);
        return 1;
    }
    if (!eglMakeCurrent(display, surface, surface, context)) {
        (void)std::printf("shot-fail make current 0x%x\n", eglGetError());
        eglDestroyContext(display, context);
        eglDestroySurface(display, surface);
        eglTerminate(display);
        return 1;
    }
    const char* glVersion = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    (void)std::printf("shot-gl %s\n", glVersion ? glVersion : "(null)");
    glViewport(0, 0, width, height);
    for (int i = 0; i < frames; ++i) {
        float t = frames <= 1 ? 1.0f : static_cast<float>(i) / static_cast<float>(frames - 1);
        glClearColor(0.15f + 0.55f * t, 0.35f, 0.85f - 0.35f * t, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glFinish();
        SDL_Event event = {};
        while (SDL_PollEvent(&event)) {
        }
    }
    std::vector<uint8> pixels(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    GLenum glErr = glGetError();
    if (glErr != GL_NO_ERROR) {
        (void)std::printf("shot-fail read pixels 0x%x\n", glErr);
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroyContext(display, context);
        eglDestroySurface(display, surface);
        eglTerminate(display);
        return 1;
    }
    uint64_t sumR = 0;
    uint64_t sumG = 0;
    uint64_t sumB = 0;
    uint64_t nonBlack = 0;
    uint64_t checksum = 1469598103934665603ULL;
    for (size_t i = 0; i < pixels.size(); i += 4) {
        uint64_t r = pixels[i];
        uint64_t g = pixels[i + 1];
        uint64_t b = pixels[i + 2];
        sumR += r;
        sumG += g;
        sumB += b;
        if (r + g + b > 16) {
            ++nonBlack;
        }
        checksum ^= r + (g << 8) + (b << 16);
        checksum *= 1099511628211ULL;
    }
    uint64_t total = static_cast<uint64_t>(width) * static_cast<uint64_t>(height);
    if (nonBlack == 0) {
        (void)std::printf("shot-fail black frame\n");
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroyContext(display, context);
        eglDestroySurface(display, surface);
        eglTerminate(display);
        return 1;
    }
    if (!WriteTga24(outPath, width, height, pixels)) {
        (void)std::printf("shot-fail write '%s'\n", outPath);
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroyContext(display, context);
        eglDestroySurface(display, surface);
        eglTerminate(display);
        return 1;
    }
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(display, context);
    eglDestroySurface(display, surface);
    eglTerminate(display);
    OS_DebugOut("mad-sa-linux GL shot");
    (void)std::printf(
        "shot-ok frames=%d out=%s nonblack=%llu/%llu avg=%llu,%llu,%llu checksum=%llu\n",
        frames, outPath,
        static_cast<unsigned long long>(nonBlack), static_cast<unsigned long long>(total),
        static_cast<unsigned long long>(sumR / total),
        static_cast<unsigned long long>(sumG / total),
        static_cast<unsigned long long>(sumB / total),
        static_cast<unsigned long long>(checksum)
    );
    return 0;
}
} // namespace

int main(int argc, char** argv) {
    if (HasArg(argc, argv, "--smoke")) {
        OS_DebugOut("mad-sa-linux smoke");
        uint32 ms = OS_TimeMS();
        double accurate = OS_TimeAccurate();
        (void)std::printf("smoke-ok ms=%u accurate=%.3f\n", ms, accurate);
        return 0;
    }
    if (HasArg(argc, argv, "--smoke-video")) {
        return RunSmokeVideo();
    }
    if (HasArg(argc, argv, "--smoke-audio")) {
        return RunSmokeAudio();
    }
    if (HasArg(argc, argv, "--headless")) {
        return RunHeadless(argc, argv);
    }
    if (HasArg(argc, argv, "--shot")) {
        return RunShot(argc, argv);
    }
    if (HasArg(argc, argv, "--help") || HasArg(argc, argv, "-h")) {
        PrintUsage(argc > 0 ? argv[0] : nullptr);
        return 0;
    }
    PrintUsage(argc > 0 ? argv[0] : nullptr);
    return 1;
}
