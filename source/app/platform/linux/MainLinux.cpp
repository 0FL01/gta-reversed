// mad-sa Linux native entry point (R1 skeleton, R2 SDL3 video, R3 headless data, R4 OpenAL, R5 EGL/GL).
// Standalone `main()` for the `mad-sa-linux` ELF track. It must not depend on
// the Windows DLL/hook model (`dllmain`/`InjectHooks`) nor on Win libraries.
#include <cassert>
#include <cmath>
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
#include "app/platform/linux/WorldShot.h"
#include "app/platform/linux/SceneShot.h"
#include "app/platform/linux/StreamPager.h"
#include "app/platform/linux/TexSample.h"
#include "app/platform/linux/SfxDecode.h"
#include "app/platform/linux/GxtText.h"
#include "app/platform/linux/MenuShot.h"
#include "app/platform/linux/MenuNav.h"
#include "app/platform/linux/ColLoad.h"
#include "app/platform/linux/RadioDecode.h"
#include "app/platform/linux/SkinPed.h"
#include "app/platform/linux/IfpAnim.h"
#include "app/platform/linux/CarPose.h"
#include "app/platform/linux/TimeCycle.h"
#include "app/platform/linux/DriveSim.h"

#include <sys/resource.h>

namespace {
void PrintUsage(const char* prog) {
    (void)std::printf(
        "usage: %s --smoke | --smoke-video | --smoke-audio | --smoke-audio-real [--bank NAME] [--samples K] | --smoke-radio [--station RE] [--seconds S] | --headless [--ticks N] | --shot <out.tga> [--frames N] | --shot-scene <out.tga> [--frames N] [--cam x,y,z] [--hour H] | --shot-menu <out.tga> [--lang english] | --menu-nav <seq> [--out nav.tga] [--lang english] | --coll-probe [--count N] | --shot-ped <out.tga> [--model cj] | --shot-anim <out.tga> [--model andre] [--anim IDLE_stance] [--time 0.5] | --anim-seq <out.tga> [--model andre] [--anim WALK_civi] [--frames 6] | --shot-car <out.tga> [--model landstal] [--steer DEG] [--spin DEG] | --drive [--path Ax,Ay:Bx,By:Cx,Cy] [--waypoints W] [--frames-per-leg F] [--model landstal] [--out prefix] | --list-anims | --e2e [--path Ax,Ay,Az:Bx,By,Bz] [--waypoints W] [--frames-per-leg F] [--out prefix]\n",
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

// Round 6 (R6d): real SFX sound. Decodes K sounds from audio/sfx/<BANK>
// (default GENRL) through SfxDecode (BankLkup.dat + BankSlot-layout bank
// headers, signed PCM16 mono at the per-sound rate), uploads each to an
// OpenAL buffer on the null sink (same ALSOFT_DRIVERS trick as R4), and
// reports buffer-verified metrics. No synthesis anywhere on this path.
int RunSmokeAudioReal(int argc, char** argv) {
    const char* bankArg = ArgValue(argc, argv, "--bank", "GENRL");
    int want = std::atoi(ArgValue(argc, argv, "--samples", "16"));
    if (want <= 0) {
        (void)std::printf("audio-real-fail bad --samples '%s'\n",
                           ArgValue(argc, argv, "--samples", "16"));
        return 1;
    }
    std::string gameDir = ResolveGameDir(argc, argv);
    OS_SetFilePathOffset(gameDir.c_str());

    SfxDecodeResult decoded;
    if (!SfxDecode_PakBank(bankArg, want, decoded)) {
        (void)std::printf("audio-real-fail bank=%s reason=%s\n", bankArg,
                           decoded.failReason.c_str());
        return 1;
    }

    bool forcedNull = false;
    if (!std::getenv("ALSOFT_DRIVERS")) {
        (void)setenv("ALSOFT_DRIVERS", "null", 1);
        forcedNull = true;
    }
    const char* backend = forcedNull ? "null" : "default";
    ALCdevice* device = alcOpenDevice(nullptr);
    if (!device) {
        (void)std::printf("audio-real-fail open device\n");
        return 1;
    }
    ALCcontext* context = alcCreateContext(device, nullptr);
    if (!context || alcMakeContextCurrent(context) == ALC_FALSE) {
        (void)std::printf("audio-real-fail create context\n");
        if (context) {
            alcDestroyContext(context);
        }
        alcCloseDevice(device);
        return 1;
    }
    const size_t count = decoded.sounds.size();
    std::vector<ALuint> buffers(count, 0);
    alGenBuffers(static_cast<ALsizei>(count), buffers.data());
    if (alGetError() != AL_NO_ERROR) {
        (void)std::printf("audio-real-fail gen buffers\n");
        alcMakeContextCurrent(nullptr);
        alcDestroyContext(context);
        alcCloseDevice(device);
        return 1;
    }
    for (size_t i = 0; i < count; ++i) {
        const SfxSound& sound = decoded.sounds[i];
        alBufferData(buffers[i], AL_FORMAT_MONO16, sound.pcm.data(),
                     static_cast<ALsizei>(sound.dataSize), sound.rateHz);
        if (alGetError() != AL_NO_ERROR) {
            (void)std::printf("audio-real-fail buffer data sound=%d\n",
                               sound.soundIndex);
            alDeleteBuffers(static_cast<ALsizei>(count), buffers.data());
            alcMakeContextCurrent(nullptr);
            alcDestroyContext(context);
            alcCloseDevice(device);
            return 1;
        }
        // Verify the upload stuck: size/frequency/bits/channels round-trip.
        ALint gotSize = 0, gotFreq = 0, gotBits = 0, gotCh = 0;
        alGetBufferi(buffers[i], AL_SIZE, &gotSize);
        alGetBufferi(buffers[i], AL_FREQUENCY, &gotFreq);
        alGetBufferi(buffers[i], AL_BITS, &gotBits);
        alGetBufferi(buffers[i], AL_CHANNELS, &gotCh);
        if (alGetError() != AL_NO_ERROR || gotSize != (ALint)sound.dataSize ||
            gotFreq != (ALint)sound.rateHz || gotBits != 16 || gotCh != 1) {
            (void)std::printf("audio-real-fail buffer verify sound=%d\n",
                               sound.soundIndex);
            alDeleteBuffers(static_cast<ALsizei>(count), buffers.data());
            alcMakeContextCurrent(nullptr);
            alcDestroyContext(context);
            alcCloseDevice(device);
            return 1;
        }
    }
    // Worked example for the report: first decoded sound end to end.
    {
        const SfxSound& first = decoded.sounds.front();
        int64_t sumSq = 0;
        for (int16_t v : first.pcm) {
            sumSq += static_cast<int64_t>(v) * v;
        }
        const double firstRms = first.pcm.empty()
                                    ? 0.0
                                    : std::sqrt(static_cast<double>(sumSq) /
                                                first.pcm.size());
        (void)std::printf(
            "sfx-detail bankId=%d sound=%d rate=%u size=%u rms=%.1f\n",
            first.bankId, first.soundIndex, first.rateHz, first.dataSize,
            firstRms);
    }
    (void)std::printf(
        "sample-ok bank=%s samples=%d decodedBytes=%llu durationMs=%lld "
        "rms=%.1f peak=%d bufChecksum=%llu backend=%s skippedBanks=%d\n",
        decoded.bankName.c_str(), static_cast<int>(count),
        static_cast<unsigned long long>(decoded.decodedBytes),
        static_cast<long long>(std::llround(decoded.durationMs)),
        decoded.rms, decoded.peak,
        static_cast<unsigned long long>(decoded.bufChecksum), backend,
        decoded.skippedBanks);
    (void)std::printf("audio-real-ok\n");
    alDeleteBuffers(static_cast<ALsizei>(count), buffers.data());
    alcMakeContextCurrent(nullptr);
    alcDestroyContext(context);
    alcCloseDevice(device);
    OS_DebugOut("mad-sa-linux real sfx audio");
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

// Rasterizes real /game geometry (WorldShot triangle soup) with the CPU
// sampler in TexSample (decoded TXD texels, perspective-correct UV, CPU
// Lambert). No synthetic gradient: every non-background pixel comes from a
// DFF triangle parsed by librw, shaded by a TXD texel (or the honest grey
// fallback / flat material color). camEyeOverride (nullable) replaces the
// default orbit eye with an explicit world-space camera position.
void DrawWorldFrame(const WorldShotScene& scene, int width, int height, float angleDeg,
                    const float* camEyeOverride, std::vector<uint8>& outPixels,
                    TexFrameStats& stats) {
    TexSample_RenderOrbit(scene, width, height, angleDeg, camEyeOverride, outPixels, stats);
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
    std::string gameDir = ResolveGameDir(argc, argv);
    WorldShotScene scene{};
    char worldErr[256] = {};
    if (!WorldShot_Init(gameDir.c_str(), scene, worldErr, sizeof(worldErr))) {
        (void)std::printf("shot-fail worldshot %s (game=%s)\n", worldErr, gameDir.c_str());
        WorldShot_Shutdown();
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroyContext(display, context);
        eglDestroySurface(display, surface);
        eglTerminate(display);
        return 1;
    }
    const WorldShotStats& wst = scene.stats;
    (void)std::printf(
        "worldshot-load dff=%s atomics=%d tris=%d verts=%d txd=%s textures=%d firstTex=%s %dx%d "
        "bbox=[%.2f,%.2f,%.2f]-[%.2f,%.2f,%.2f]\n",
        wst.dffName, wst.atomics, wst.triangles, wst.vertices,
        wst.txdName, wst.textures,
        wst.firstTexture[0] ? wst.firstTexture : "-",
        wst.firstTexW, wst.firstTexH,
        scene.bboxMin[0], scene.bboxMin[1], scene.bboxMin[2],
        scene.bboxMax[0], scene.bboxMax[1], scene.bboxMax[2]
    );
    for (int i = 0; i < frames; ++i) {
        SDL_Event event = {};
        while (SDL_PollEvent(&event)) {
        }
    }
    // The captured frame is the final turntable angle (as before: only the
    // last frame was ever read back). CPU rasterizer, no GL drawing.
    std::vector<uint8> pixels;
    TexFrameStats texStats{};
    DrawWorldFrame(scene, width, height, 60.0f, nullptr, pixels, texStats);
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
        "texshot-ok tris=%d sampledTri=%d texelFetch=%ld greyFallback=%d flatTri=%d texPixels=%ld "
        "uv=[%.3f,%.3f]x[%.3f,%.3f] firstTex=%s texel=%d,%d,%d,%d pixel=%d,%d,%d render=cpu\n",
        texStats.tris, texStats.sampledTri, texStats.texelFetch, texStats.fallbackTri, texStats.flatTri,
        texStats.texPixels, texStats.haveUV ? texStats.uvMin[0] : 0.0f,
        texStats.haveUV ? texStats.uvMax[0] : 0.0f, texStats.haveUV ? texStats.uvMin[1] : 0.0f,
        texStats.haveUV ? texStats.uvMax[1] : 0.0f, texStats.haveFirst ? texStats.firstTex : "-",
        texStats.firstTexel[0], texStats.firstTexel[1], texStats.firstTexel[2],
        texStats.firstTexel[3], texStats.firstPixel[0], texStats.firstPixel[1],
        texStats.firstPixel[2]
    );
    (void)std::printf(
        "worldshot-ok dff=%s tris=%d verts=%d txd=%s textures=%d frames=%d out=%s nonblack=%llu/%llu checksum=%llu\n",
        wst.dffName, wst.triangles, wst.vertices,
        wst.txdName, wst.textures,
        frames, outPath,
        static_cast<unsigned long long>(nonBlack),
        static_cast<unsigned long long>(total),
        static_cast<unsigned long long>(checksum)
    );
    (void)std::printf(
        "shot-ok frames=%d out=%s nonblack=%llu/%llu avg=%llu,%llu,%llu checksum=%llu\n",
        frames, outPath,
        static_cast<unsigned long long>(nonBlack), static_cast<unsigned long long>(total),
        static_cast<unsigned long long>(sumR / total),
        static_cast<unsigned long long>(sumG / total),
        static_cast<unsigned long long>(sumB / total),
        static_cast<unsigned long long>(checksum)
    );
    WorldShot_Shutdown();
    return 0;
}

// R6a: multi-model world scene from real IDE/IPL records. Same EGL pbuffer
// path as RunShot; the pixel loop/checksum are identical so checksums stay
// comparable across modes (gradient vs single-DFF vs scene).
int RunShotScene(int argc, char** argv) {
    const char* outPath = ArgValue(argc, argv, "--shot-scene", "scene.tga");
    const char* framesArg = ArgValue(argc, argv, "--frames", "120");
    int frames = std::atoi(framesArg);
    if (!outPath || outPath[0] == '\0' || frames <= 0) {
        (void)std::printf("sceneshot-fail bad args shot-scene='%s' frames='%s'\n", outPath, framesArg);
        return 1;
    }
    float camEye[3] = {};
    const float* camOverride = nullptr;
    const char* camArg = ArgValue(argc, argv, "--cam", nullptr);
    if (camArg) {
        float cx = 0.0f;
        float cy = 0.0f;
        float cz = 0.0f;
        if (std::sscanf(camArg, "%f , %f , %f", &cx, &cy, &cz) != 3) {
            (void)std::printf("sceneshot-fail bad --cam '%s' (want x,y,z)\n", camArg);
            return 1;
        }
        camEye[0] = cx;
        camEye[1] = cy;
        camEye[2] = cz;
        camOverride = camEye;
    }
    // R6m: optional time-of-day. Absent --hour keeps the legacy look
    // bit-for-bit; a given H (integer 0-23) reads EXTRASUNNY_LA from
    // data/timecyc.dat and relights the frame (no interpolation: the floor
    // sample row is used as-is).
    int hour = -1;
    const char* hourArg = ArgValue(argc, argv, "--hour", nullptr);
    if (hourArg) {
        size_t len = std::strlen(hourArg);
        bool digits = len > 0 && len <= 2;
        for (size_t i = 0; digits && i < len; ++i) {
            if (hourArg[i] < '0' || hourArg[i] > '9') {
                digits = false;
            }
        }
        int h = digits ? std::atoi(hourArg) : -1;
        if (!digits || h < 0 || h > 23) {
            (void)std::printf("sceneshot-fail bad --hour '%s' (want 0-23)\n", hourArg);
            return 1;
        }
        hour = h;
    }
    const int width = 640;
    const int height = 480;
    auto getPlatformDisplay = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(
        eglGetProcAddress("eglGetPlatformDisplayEXT")
    );
    if (!getPlatformDisplay) {
        (void)std::printf("sceneshot-fail no eglGetPlatformDisplayEXT\n");
        return 1;
    }
#ifndef EGL_PLATFORM_SURFACELESS_MESA
#define EGL_PLATFORM_SURFACELESS_MESA 0x31DD
#endif
    EGLDisplay display = getPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
    if (display == EGL_NO_DISPLAY) {
        (void)std::printf("sceneshot-fail no surfaceless display 0x%x\n", eglGetError());
        return 1;
    }
    if (!eglInitialize(display, nullptr, nullptr)) {
        (void)std::printf("sceneshot-fail egl init 0x%x\n", eglGetError());
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
        (void)std::printf("sceneshot-fail choose config 0x%x\n", eglGetError());
        eglTerminate(display);
        return 1;
    }
    const EGLint pbufferAttrs[] = { EGL_WIDTH, width, EGL_HEIGHT, height, EGL_NONE };
    EGLSurface surface = eglCreatePbufferSurface(display, config, pbufferAttrs);
    if (surface == EGL_NO_SURFACE) {
        (void)std::printf("sceneshot-fail pbuffer 0x%x\n", eglGetError());
        eglTerminate(display);
        return 1;
    }
    (void)eglBindAPI(EGL_OPENGL_API);
    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, nullptr);
    if (context == EGL_NO_CONTEXT) {
        (void)std::printf("sceneshot-fail context 0x%x\n", eglGetError());
        eglDestroySurface(display, surface);
        eglTerminate(display);
        return 1;
    }
    if (!eglMakeCurrent(display, surface, surface, context)) {
        (void)std::printf("sceneshot-fail make current 0x%x\n", eglGetError());
        eglDestroyContext(display, context);
        eglDestroySurface(display, surface);
        eglTerminate(display);
        return 1;
    }
    const char* glVersion = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    (void)std::printf("sceneshot-gl %s\n", glVersion ? glVersion : "(null)");
    std::string gameDir = ResolveGameDir(argc, argv);
    WorldShotScene scene{};
    SceneShotStats sst{};
    char sceneErr[512] = {};
    if (!SceneShot_Init(gameDir.c_str(), scene, sst, sceneErr, sizeof(sceneErr))) {
        (void)std::printf("sceneshot-fail scene %s (game=%s)\n", sceneErr, gameDir.c_str());
        SceneShot_Shutdown();
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroyContext(display, context);
        eglDestroySurface(display, surface);
        eglTerminate(display);
        return 1;
    }
    (void)std::printf(
        "sceneshot-load models=%d tris=%d verts=%d texdicts=%d textures=%d missTex=%d missDff=%d "
        "skippedSkin=%d bbox=[%.2f,%.2f,%.2f]-[%.2f,%.2f,%.2f] list=%s\n",
        sst.models, sst.tris, sst.verts, sst.texDicts, sst.textures, sst.missTex, sst.missDff,
        sst.skippedSkin, scene.bboxMin[0], scene.bboxMin[1], scene.bboxMin[2], scene.bboxMax[0],
        scene.bboxMax[1], scene.bboxMax[2], sst.list
    );
    for (int i = 0; i < frames; ++i) {
        SDL_Event event = {};
        while (SDL_PollEvent(&event)) {
        }
    }
    TexTimeEnv timeEnv{};
    const TexTimeEnv* timeEnvPtr = nullptr;
    if (hour >= 0) {
        TimeCycleParams tcp{};
        char tcErr[256] = {};
        if (!TimeCycle_LoadHour(gameDir.c_str(), hour, tcp, tcErr, sizeof(tcErr))) {
            (void)std::printf("sceneshot-fail timecyc %s (game=%s hour=%d)\n", tcErr,
                              gameDir.c_str(), hour);
            SceneShot_Shutdown();
            eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            eglDestroyContext(display, context);
            eglDestroySurface(display, surface);
            eglTerminate(display);
            return 1;
        }
        for (int c = 0; c < 3; ++c) {
            timeEnv.amb[c] = tcp.amb[c] / 255.0f;
            timeEnv.sun[c] = tcp.dir[c] / 255.0f;
            timeEnv.skyTop[c] = tcp.skyTop[c];
            timeEnv.skyBot[c] = tcp.skyBot[c];
        }
        timeEnvPtr = &timeEnv;
        (void)std::printf(
            "timecyc-load weather=EXTRASUNNY_LA hour=%d amb=%d,%d,%d dir=%d,%d,%d "
            "skytop=%d,%d,%d skybot=%d,%d,%d suncore=%d,%d,%d sample=%s sunDir=fixed spec=off\n",
            hour, tcp.amb[0], tcp.amb[1], tcp.amb[2], tcp.dir[0], tcp.dir[1], tcp.dir[2],
            tcp.skyTop[0], tcp.skyTop[1], tcp.skyTop[2], tcp.skyBot[0], tcp.skyBot[1],
            tcp.skyBot[2], tcp.sunCore[0], tcp.sunCore[1], tcp.sunCore[2], tcp.sampleName
        );
    }
    std::vector<uint8> pixels;
    TexFrameStats texStats{};
    if (timeEnvPtr) {
        TexSample_RenderOrbitTC(scene, width, height, 60.0f, camOverride, *timeEnvPtr, pixels,
                                texStats);
    } else {
        DrawWorldFrame(scene, width, height, 60.0f, camOverride, pixels, texStats);
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
        (void)std::printf("sceneshot-fail black frame\n");
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroyContext(display, context);
        eglDestroySurface(display, surface);
        eglTerminate(display);
        return 1;
    }
    if (!WriteTga24(outPath, width, height, pixels)) {
        (void)std::printf("sceneshot-fail write '%s'\n", outPath);
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
    OS_DebugOut("mad-sa-linux GL scene shot");
    (void)std::printf(
        "tex-ok tris=%d sampledTri=%d texelFetch=%ld greyFallback=%d flatTri=%d texPixels=%ld "
        "uv=[%.3f,%.3f]x[%.3f,%.3f] firstTex=%s texel=%d,%d,%d,%d pixel=%d,%d,%d render=cpu\n",
        texStats.tris, texStats.sampledTri, texStats.texelFetch, texStats.fallbackTri, texStats.flatTri,
        texStats.texPixels, texStats.haveUV ? texStats.uvMin[0] : 0.0f,
        texStats.haveUV ? texStats.uvMax[0] : 0.0f, texStats.haveUV ? texStats.uvMin[1] : 0.0f,
        texStats.haveUV ? texStats.uvMax[1] : 0.0f, texStats.haveFirst ? texStats.firstTex : "-",
        texStats.firstTexel[0], texStats.firstTexel[1], texStats.firstTexel[2],
        texStats.firstTexel[3], texStats.firstPixel[0], texStats.firstPixel[1],
        texStats.firstPixel[2]
    );
    (void)std::printf(
        "sceneshot-ok models=%d tris=%d verts=%d txd=%d frames=%d out=%s nonblack=%llu/%llu "
        "avg=%llu,%llu,%llu checksum=%llu\n",
        sst.models, sst.tris, sst.verts, sst.textures, frames, outPath,
        static_cast<unsigned long long>(nonBlack), static_cast<unsigned long long>(total),
        static_cast<unsigned long long>(sumR / total), static_cast<unsigned long long>(sumG / total),
        static_cast<unsigned long long>(sumB / total), static_cast<unsigned long long>(checksum)
    );
    if (hour >= 0) {
        (void)std::printf("timeshot-ok hour=%d checksum=%llu\n", hour,
                          static_cast<unsigned long long>(checksum));
    }
    SceneShot_Shutdown();
    return 0;
}

// R6b: path-driven frame. Same triangle soup + CPU sampler as DrawWorldFrame
// but the camera is fully determined by the path: eye at the waypoint,
// lookAt forward along the segment yaw with a fixed -10deg pitch, fixed
// 60deg-vertical frustum. No turntable: re-running the same path must give
// the same pixels.
void DrawE2EFrame(const WorldShotScene& scene, int width, int height, const float* eye,
                  const float* target, std::vector<uint8>& outPixels, TexFrameStats& stats) {
    TexSample_RenderPath(scene, width, height, eye, target, outPixels, stats);
}

uint64_t PixelsChecksum(const std::vector<uint8>& pixels, uint64_t& sumR, uint64_t& sumG, uint64_t& sumB,
                        uint64_t& nonBlack) {
    sumR = 0;
    sumG = 0;
    sumB = 0;
    nonBlack = 0;
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
    return checksum;
}

// R6b e2e: camera walks a straight A->B path split into W waypoints; at each
// waypoint the streaming pager pages grid sectors around the camera (evicting
// past R+hysteresis), F frames are rendered, and one TGA per waypoint is
// written. Summary line carries the acceptance fields.
int RunE2E(int argc, char** argv) {
    float ax = 1600.0f;
    float ay = -1700.0f;
    float az = 70.0f;
    float bx = 1683.0f;
    float by = -2285.0f;
    float bz = 50.0f;
    const char* pathArg = ArgValue(argc, argv, "--path", nullptr);
    if (pathArg) {
        float pax = 0, pay = 0, paz = 0, pbx = 0, pby = 0, pbz = 0;
        if (std::sscanf(pathArg, "%f , %f , %f : %f , %f , %f", &pax, &pay, &paz, &pbx, &pby,
                         &pbz) != 6) {
            (void)std::printf("e2e-fail bad --path '%s' (want Ax,Ay,Az:Bx,By,Bz)\n", pathArg);
            return 1;
        }
        ax = pax;
        ay = pay;
        az = paz;
        bx = pbx;
        by = pby;
        bz = pbz;
    }
    int waypoints = std::atoi(ArgValue(argc, argv, "--waypoints", "3"));
    int framesPerLeg = std::atoi(ArgValue(argc, argv, "--frames-per-leg", "5"));
    if (waypoints < 1 || waypoints > 64 || framesPerLeg < 1 || framesPerLeg > 120) {
        (void)std::printf(
            "e2e-fail bad args waypoints='%s' frames-per-leg='%s'\n", ArgValue(argc, argv, "--waypoints", "3"),
            ArgValue(argc, argv, "--frames-per-leg", "5")
        );
        return 1;
    }
    // Output prefix: --out wins; else a non-flag token right after --e2e; else "e2e".
    std::string prefix = "e2e";
    const char* outArg = ArgValue(argc, argv, "--out", nullptr);
    if (outArg && outArg[0] != '\0') {
        prefix = outArg;
    } else {
        for (int i = 1; i + 1 < argc; ++i) {
            if (std::strcmp(argv[i], "--e2e") == 0 && argv[i + 1][0] != '-') {
                prefix = argv[i + 1];
                break;
            }
        }
    }
    const int width = 640;
    const int height = 480;
    auto getPlatformDisplay = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(
        eglGetProcAddress("eglGetPlatformDisplayEXT")
    );
    if (!getPlatformDisplay) {
        (void)std::printf("e2e-fail no eglGetPlatformDisplayEXT\n");
        return 1;
    }
#ifndef EGL_PLATFORM_SURFACELESS_MESA
#define EGL_PLATFORM_SURFACELESS_MESA 0x31DD
#endif
    EGLDisplay display = getPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
    if (display == EGL_NO_DISPLAY) {
        (void)std::printf("e2e-fail no surfaceless display 0x%x\n", eglGetError());
        return 1;
    }
    if (!eglInitialize(display, nullptr, nullptr)) {
        (void)std::printf("e2e-fail egl init 0x%x\n", eglGetError());
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
        (void)std::printf("e2e-fail choose config 0x%x\n", eglGetError());
        eglTerminate(display);
        return 1;
    }
    const EGLint pbufferAttrs[] = { EGL_WIDTH, width, EGL_HEIGHT, height, EGL_NONE };
    EGLSurface surface = eglCreatePbufferSurface(display, config, pbufferAttrs);
    if (surface == EGL_NO_SURFACE) {
        (void)std::printf("e2e-fail pbuffer 0x%x\n", eglGetError());
        eglTerminate(display);
        return 1;
    }
    (void)eglBindAPI(EGL_OPENGL_API);
    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, nullptr);
    if (context == EGL_NO_CONTEXT) {
        (void)std::printf("e2e-fail context 0x%x\n", eglGetError());
        eglDestroySurface(display, surface);
        eglTerminate(display);
        return 1;
    }
    if (!eglMakeCurrent(display, surface, surface, context)) {
        (void)std::printf("e2e-fail make current 0x%x\n", eglGetError());
        eglDestroyContext(display, context);
        eglDestroySurface(display, surface);
        eglTerminate(display);
        return 1;
    }
    const char* glVersion = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    (void)std::printf("e2e-gl %s\n", glVersion ? glVersion : "(null)");
    std::string gameDir = ResolveGameDir(argc, argv);
    E2ELoadInfo load{};
    char pagerErr[512] = {};
    if (!StreamPager_Init(gameDir.c_str(), load, pagerErr, sizeof(pagerErr))) {
        (void)std::printf("e2e-fail pager-init %s (game=%s)\n", pagerErr, gameDir.c_str());
        StreamPager_Shutdown();
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroyContext(display, context);
        eglDestroySurface(display, surface);
        eglTerminate(display);
        return 1;
    }
    (void)std::printf(
        "e2e-load iplTotal=%d kept=%d ide=%d ideFiles=%d iplFiles=%d cell=300 R=300 H=100 cap=80\n",
        load.iplTotal, load.iplKept, load.ideModels, load.ideFiles, load.iplFiles
    );
    (void)std::printf(
        "e2e-path A=%.2f,%.2f,%.2f B=%.2f,%.2f,%.2f W=%d F=%d\n", ax, ay, az, bx, by, bz, waypoints,
        framesPerLeg
    );
    std::vector<uint64_t> checksums;
    checksums.reserve(static_cast<size_t>(waypoints));
    TexFrameStats texAgg{};
    int totalFrames = 0;
    bool failed = false;
    for (int w = 0; w < waypoints && !failed; ++w) {
        float t = waypoints <= 1 ? 0.0f : static_cast<float>(w) / static_cast<float>(waypoints - 1);
        float eye[3] = { ax + (bx - ax) * t, ay + (by - ay) * t, az + (bz - az) * t };
        // Course yaw from the segment through this waypoint (fixed formula).
        float dx = 0.0f;
        float dy = 0.0f;
        if (waypoints <= 1) {
            dx = bx - ax;
            dy = by - ay;
        } else if (w + 1 < waypoints) {
            float nt = static_cast<float>(w + 1) / static_cast<float>(waypoints - 1);
            dx = (ax + (bx - ax) * nt) - eye[0];
            dy = (ay + (by - ay) * nt) - eye[1];
        } else {
            float pt = static_cast<float>(w - 1) / static_cast<float>(waypoints - 1);
            dx = eye[0] - (ax + (bx - ax) * pt);
            dy = eye[1] - (ay + (by - ay) * pt);
        }
        if (dx == 0.0f && dy == 0.0f) {
            dx = 1.0f;
        }
        float yaw = std::atan2(dy, dx);
        constexpr float kPitch = -10.0f * 3.14159265f / 180.0f;
        float cp = std::cos(kPitch);
        float fx = std::cos(yaw) * cp;
        float fy = std::sin(yaw) * cp;
        float fz = std::sin(kPitch);
        float target[3] = { eye[0] + fx * 200.0f, eye[1] + fy * 200.0f, eye[2] + fz * 200.0f };
        WorldShotScene scene{};
        E2EPagerFrame pf{};
        if (!StreamPager_Update(eye[0], eye[1], eye[2], scene, pf, pagerErr, sizeof(pagerErr))) {
            (void)std::printf("e2e-fail pager-update wp=%d %s\n", w, pagerErr);
            failed = true;
            break;
        }
        (void)std::printf(
            "e2e-pager wp=%d cam=%.2f,%.2f,%.2f yaw=%.3f active=%d loaded=%d evicted=%d "
            "instances=%d models=%d cached=%d tris=%d fallback=%d\n",
            w, eye[0], eye[1], eye[2], yaw, pf.activeCells, pf.loadedCells, pf.evictedCells,
            pf.instances, pf.modelsUnique, pf.cacheModels, pf.tris, pf.fallback
        );
        for (int e = 0; e < pf.evictedShown; ++e) {
            (void)std::printf(
                "e2e-evict wp=%d sector=(%d,%d) dist=%d\n", w, pf.evictedCX[e], pf.evictedCY[e],
                pf.evictedDist[e]
            );
        }
        for (int f = 0; f < framesPerLeg; ++f) {
            SDL_Event event = {};
            while (SDL_PollEvent(&event)) {
            }
        }
        totalFrames += framesPerLeg;
        std::vector<uint8> pixels;
        TexFrameStats texStats{};
        DrawE2EFrame(scene, width, height, eye, target, pixels, texStats);
        texAgg.tris += texStats.tris;
        texAgg.sampledTri += texStats.sampledTri;
        texAgg.fallbackTri += texStats.fallbackTri;
        texAgg.flatTri += texStats.flatTri;
        texAgg.texelFetch += texStats.texelFetch;
        texAgg.texPixels += texStats.texPixels;
        texAgg.fallbackPixels += texStats.fallbackPixels;
        texAgg.flatPixels += texStats.flatPixels;
        if (texStats.haveUV) {
            if (!texAgg.haveUV) {
                texAgg.uvMin[0] = texStats.uvMin[0];
                texAgg.uvMax[0] = texStats.uvMax[0];
                texAgg.uvMin[1] = texStats.uvMin[1];
                texAgg.uvMax[1] = texStats.uvMax[1];
                texAgg.haveUV = true;
            } else {
                if (texStats.uvMin[0] < texAgg.uvMin[0]) {
                    texAgg.uvMin[0] = texStats.uvMin[0];
                }
                if (texStats.uvMax[0] > texAgg.uvMax[0]) {
                    texAgg.uvMax[0] = texStats.uvMax[0];
                }
                if (texStats.uvMin[1] < texAgg.uvMin[1]) {
                    texAgg.uvMin[1] = texStats.uvMin[1];
                }
                if (texStats.uvMax[1] > texAgg.uvMax[1]) {
                    texAgg.uvMax[1] = texStats.uvMax[1];
                }
            }
        }
        if (!texAgg.haveFirst && texStats.haveFirst) {
            texAgg.haveFirst = true;
            (void)std::snprintf(texAgg.firstTex, sizeof(texAgg.firstTex), "%s",
                                texStats.firstTex);
            for (int k = 0; k < 4; ++k) {
                texAgg.firstTexel[k] = texStats.firstTexel[k];
            }
            for (int k = 0; k < 3; ++k) {
                texAgg.firstPixel[k] = texStats.firstPixel[k];
            }
        }
        uint64_t sumR = 0;
        uint64_t sumG = 0;
        uint64_t sumB = 0;
        uint64_t nonBlack = 0;
        uint64_t checksum = PixelsChecksum(pixels, sumR, sumG, sumB, nonBlack);
        uint64_t total = static_cast<uint64_t>(width) * static_cast<uint64_t>(height);
        if (nonBlack == 0) {
            (void)std::printf("e2e-fail black frame wp=%d\n", w);
            failed = true;
            break;
        }
        char outPath[1024];
        (void)std::snprintf(outPath, sizeof(outPath), "%s_W%d.tga", prefix.c_str(), w);
        if (!WriteTga24(outPath, width, height, pixels)) {
            (void)std::printf("e2e-fail write '%s'\n", outPath);
            failed = true;
            break;
        }
        checksums.push_back(checksum);
        (void)std::printf(
            "e2e-shot wp=%d out=%s instances=%d tris=%d nonblack=%llu/%llu avg=%llu,%llu,%llu "
            "checksum=%llu\n",
            w, outPath, pf.instances, pf.tris, static_cast<unsigned long long>(nonBlack),
            static_cast<unsigned long long>(total), static_cast<unsigned long long>(sumR / total),
            static_cast<unsigned long long>(sumG / total),
            static_cast<unsigned long long>(sumB / total), static_cast<unsigned long long>(checksum)
        );
    }
    int sectorsLoaded = 0;
    int sectorsEvicted = 0;
    int modelsPeak = 0;
    int trisPeak = 0;
    StreamPager_Counters(sectorsLoaded, sectorsEvicted, modelsPeak, trisPeak);
    struct rusage ru = {};
    long rssMb = 0;
    if (getrusage(RUSAGE_SELF, &ru) == 0) {
        rssMb = ru.ru_maxrss / 1024; // Linux ru_maxrss is KiB
    }
    (void)std::printf("e2e-mem rssPeakMb=%ld\n", rssMb);
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(display, context);
    eglDestroySurface(display, surface);
    eglTerminate(display);
    if (failed) {
        StreamPager_Shutdown();
        return 1;
    }
    std::string cs;
    for (size_t i = 0; i < checksums.size(); ++i) {
        char cell[32];
        (void)std::snprintf(cell, sizeof(cell), "%s%llu", i ? "," : "",
                            static_cast<unsigned long long>(checksums[i]));
        cs += cell;
    }
    OS_DebugOut("mad-sa-linux e2e living world");
    (void)std::printf(
        "texe2e-ok tris=%d sampledTri=%d texelFetch=%ld greyFallback=%d flatTri=%d texPixels=%ld "
        "uv=[%.3f,%.3f]x[%.3f,%.3f] firstTex=%s texel=%d,%d,%d,%d pixel=%d,%d,%d render=cpu\n",
        texAgg.tris, texAgg.sampledTri, texAgg.texelFetch, texAgg.fallbackTri, texAgg.flatTri,
        texAgg.texPixels, texAgg.haveUV ? texAgg.uvMin[0] : 0.0f,
        texAgg.haveUV ? texAgg.uvMax[0] : 0.0f, texAgg.haveUV ? texAgg.uvMin[1] : 0.0f,
        texAgg.haveUV ? texAgg.uvMax[1] : 0.0f, texAgg.haveFirst ? texAgg.firstTex : "-",
        texAgg.firstTexel[0], texAgg.firstTexel[1], texAgg.firstTexel[2], texAgg.firstTexel[3],
        texAgg.firstPixel[0], texAgg.firstPixel[1], texAgg.firstPixel[2]
    );
    (void)std::printf(
        "world-ok waypoints=%d frames=%d sectorsLoaded=%d sectorsEvicted=%d modelsPeak=%d "
        "trisPeak=%d checksums=%s\n",
        waypoints, totalFrames, sectorsLoaded, sectorsEvicted, modelsPeak, trisPeak, cs.c_str()
    );
    StreamPager_Shutdown();
    return 0;
}
// R6e: main-menu 2D frame. Strings come from the GXT MAIN table, letter
// pixels come from the fonts.txd font2 texels (MenuShot CPU blit, no GL).
int RunShotMenu(int argc, char** argv) {
    const char* outPath = ArgValue(argc, argv, "--shot-menu", "menu.tga");
    const char* lang = ArgValue(argc, argv, "--lang", "english");
    if (!outPath || outPath[0] == '\0' || !lang || lang[0] == '\0') {
        (void)std::printf("menu-fail bad args shot-menu='%s' lang='%s'\n", outPath, lang);
        return 1;
    }
    std::string gameDir = ResolveGameDir(argc, argv);
    std::vector<uint8> pixels;
    MenuShotStats mst{};
    char menuErr[512] = {};
    if (!MenuShot_Render(gameDir.c_str(), lang, pixels, mst, menuErr, sizeof(menuErr))) {
        (void)std::printf("menu-fail %s (game=%s lang=%s)\n", menuErr, gameDir.c_str(), lang);
        MenuShot_Shutdown();
        return 1;
    }
    (void)std::printf("menutext-load lang=%s file=%s keys=%d\n", mst.lang, mst.gxtFile, mst.gxtKeys);
    for (int i = 0; i < 4; ++i) {
        (void)std::printf("menuitem key=%s text=\"%s\"\n", mst.itemKey[i], mst.itemText[i]);
    }
    (void)std::printf(
        "fonttex name=%s %dx%d solidTexels=%ld\n", mst.fontName, mst.fontW, mst.fontH,
        mst.solidTexels
    );
    uint64_t sumR = 0;
    uint64_t sumG = 0;
    uint64_t sumB = 0;
    uint64_t nonBlack = 0;
    uint64_t checksum = PixelsChecksum(pixels, sumR, sumG, sumB, nonBlack);
    uint64_t total = 640ULL * 480ULL;
    if (pixels.size() != total * 4 || nonBlack == 0) {
        (void)std::printf("menu-fail bad frame pixels=%d nonblack=%llu\n",
                           static_cast<int>(pixels.size()),
                           static_cast<unsigned long long>(nonBlack));
        MenuShot_Shutdown();
        return 1;
    }
    if (!WriteTga24(outPath, 640, 480, pixels)) {
        (void)std::printf("menu-fail write '%s'\n", outPath);
        MenuShot_Shutdown();
        return 1;
    }
    std::string miss = "";
    for (int i = 0; i < mst.missingCount; ++i) {
        char cell[16];
        (void)std::snprintf(cell, sizeof(cell), "%s0x%02X", i ? "," : "",
                             mst.missing[i]);
        miss += cell;
    }
    OS_DebugOut("mad-sa-linux menu shot");
    (void)std::printf(
        "menu-ok lang=%s strings=%d glyphs=%d missingGlyphs=%d missing=[%s] checksum=%llu\n",
        mst.lang, mst.strings, mst.glyphs, mst.missingCount, miss.c_str(),
        static_cast<unsigned long long>(checksum)
    );
    (void)std::printf(
        "menushot-ok out=%s nonblack=%llu/%llu avg=%llu,%llu,%llu checksum=%llu\n", outPath,
        static_cast<unsigned long long>(nonBlack), static_cast<unsigned long long>(total),
        static_cast<unsigned long long>(sumR / total),
        static_cast<unsigned long long>(sumG / total),
        static_cast<unsigned long long>(sumB / total), static_cast<unsigned long long>(checksum)
    );
    MenuShot_Shutdown();
    return 0;
}

// R6f: closed-loop menu navigation. Every seq command travels as a real
// SDL_EVENT_KEY_DOWN through SDL_PushEvent (MenuNav.cpp MenuNav_PushCommand)
// and is applied only inside the SDL_PollEvent drain (MenuNav_PumpEvents:
// down=(s+1)%3, up=(s+2)%3, enter pins chosen); frames are full MenuShot
// re-renders at the live highlight. H0 is the pre-input frame (== static
// --shot-menu when sel=0). Log carries one checksum per frame (H0 + per cmd).
int RunMenuNav(int argc, char** argv) {
    const char* seq = ArgValue(argc, argv, "--menu-nav", nullptr);
    const char* outPath = ArgValue(argc, argv, "--out", "nav.tga");
    const char* lang = ArgValue(argc, argv, "--lang", "english");
    if (!seq || seq[0] == '\0' || !outPath || outPath[0] == '\0' || !lang || lang[0] == '\0') {
        (void)std::printf("nav-fail bad args menu-nav='%s' out='%s' lang='%s'\n",
                           seq ? seq : "(null)", outPath ? outPath : "(null)",
                           lang ? lang : "(null)");
        return 1;
    }
    std::string gameDir = ResolveGameDir(argc, argv);
    std::vector<uint8> pixels;
    MenuNavResult nav{};
    char navErr[512] = {};
    if (!MenuNav_Run(gameDir.c_str(), lang, seq, pixels, nav, navErr, sizeof(navErr))) {
        (void)std::printf("nav-fail %s (game=%s seq=\"%s\")\n", navErr, gameDir.c_str(), seq);
        MenuShot_Shutdown();
        return 1;
    }
    (void)std::printf("navtext-load lang=%s file=%s keys=%d\n", nav.lastStats.lang,
                       nav.lastStats.gxtFile, nav.lastStats.gxtKeys);
    for (int i = 0; i < 4; ++i) {
        (void)std::printf("navitem key=%s text=\"%s\"\n", nav.lastStats.itemKey[i],
                           nav.lastStats.itemText[i]);
    }
    (void)std::printf("navfont name=%s %dx%d solidTexels=%ld glyphs=%d\n", nav.lastStats.fontName,
                       nav.lastStats.fontW, nav.lastStats.fontH, nav.lastStats.solidTexels,
                       nav.lastStats.glyphs);
    uint64_t sumR = 0;
    uint64_t sumG = 0;
    uint64_t sumB = 0;
    uint64_t nonBlack = 0;
    uint64_t finalChecksum = PixelsChecksum(pixels, sumR, sumG, sumB, nonBlack);
    uint64_t total = 640ULL * 480ULL;
    if (pixels.size() != total * 4 || nonBlack == 0) {
        (void)std::printf("nav-fail bad frame pixels=%d nonblack=%llu\n",
                           static_cast<int>(pixels.size()),
                           static_cast<unsigned long long>(nonBlack));
        MenuShot_Shutdown();
        return 1;
    }
    if (!WriteTga24(outPath, 640, 480, pixels)) {
        (void)std::printf("nav-fail write '%s'\n", outPath);
        MenuShot_Shutdown();
        return 1;
    }
    std::string cs;
    for (std::size_t i = 0; i < nav.checksums.size(); ++i) {
        char cell[32];
        (void)std::snprintf(cell, sizeof(cell), "%s%llu", i ? "," : "",
                             static_cast<unsigned long long>(nav.checksums[i]));
        cs += cell;
    }
    int steps = nav.checksums.empty() ? 0 : static_cast<int>(nav.checksums.size()) - 1;
    OS_DebugOut("mad-sa-linux menu nav");
    (void)std::printf("nav-choose index=%d key=%s text=\"%s\"\n", nav.chosen,
                       nav.chosen >= 0 ? nav.chosenKey : "-",
                       nav.chosen >= 0 ? nav.chosenText : "-");
    (void)std::printf("nav-ok seq=\"%s\" steps=%d selected=%d chosen=%d checksums=%s\n", seq, steps,
                       nav.selected, nav.chosen, cs.c_str());
    (void)std::printf(
        "navshot-ok out=%s nonblack=%llu/%llu avg=%llu,%llu,%llu checksum=%llu\n", outPath,
        static_cast<unsigned long long>(nonBlack), static_cast<unsigned long long>(total),
        static_cast<unsigned long long>(sumR / total),
        static_cast<unsigned long long>(sumG / total),
        static_cast<unsigned long long>(sumB / total),
        static_cast<unsigned long long>(finalChecksum));
    MenuShot_Shutdown();
    return 0;
}

// Round 11 (R6i): skinned character in bind pose. Loads one skinned DFF
// (default --model cj; cj.dff does not ship, so the documented fallback
// picks the first skinned DFF in gta3.img archive order and names it in
// the log), runs CPU bind-pose skinning (anim=bind, no HAnim), and renders
// through the same CPU orbit rasterizer. No synthesis: every vertex, bone
// index and weight comes from DFF bytes; the round gates wsum~=1.0.
int RunShotPed(int argc, char** argv) {
    const char* outPath = ArgValue(argc, argv, "--shot-ped", "ped.tga");
    const char* model = ArgValue(argc, argv, "--model", "cj");
    if (!outPath || outPath[0] == '\0' || !model || model[0] == '\0') {
        (void)std::printf("ped-fail bad args shot-ped='%s' model='%s'\n", outPath ? outPath : "(null)",
                           model ? model : "(null)");
        return 1;
    }
    const int width = 640;
    const int height = 480;
    auto getPlatformDisplay = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(
        eglGetProcAddress("eglGetPlatformDisplayEXT")
    );
    if (!getPlatformDisplay) {
        (void)std::printf("ped-fail no eglGetPlatformDisplayEXT\n");
        return 1;
    }
#ifndef EGL_PLATFORM_SURFACELESS_MESA
#define EGL_PLATFORM_SURFACELESS_MESA 0x31DD
#endif
    EGLDisplay display = getPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
    if (display == EGL_NO_DISPLAY) {
        (void)std::printf("ped-fail no surfaceless display 0x%x\n", eglGetError());
        return 1;
    }
    if (!eglInitialize(display, nullptr, nullptr)) {
        (void)std::printf("ped-fail egl init 0x%x\n", eglGetError());
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
        (void)std::printf("ped-fail choose config 0x%x\n", eglGetError());
        eglTerminate(display);
        return 1;
    }
    const EGLint pbufferAttrs[] = { EGL_WIDTH, width, EGL_HEIGHT, height, EGL_NONE };
    EGLSurface surface = eglCreatePbufferSurface(display, config, pbufferAttrs);
    if (surface == EGL_NO_SURFACE) {
        (void)std::printf("ped-fail pbuffer 0x%x\n", eglGetError());
        eglTerminate(display);
        return 1;
    }
    (void)eglBindAPI(EGL_OPENGL_API);
    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, nullptr);
    if (context == EGL_NO_CONTEXT) {
        (void)std::printf("ped-fail context 0x%x\n", eglGetError());
        eglDestroySurface(display, surface);
        eglTerminate(display);
        return 1;
    }
    if (!eglMakeCurrent(display, surface, surface, context)) {
        (void)std::printf("ped-fail make current 0x%x\n", eglGetError());
        eglDestroyContext(display, context);
        eglDestroySurface(display, surface);
        eglTerminate(display);
        return 1;
    }
    const char* glVersion = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    (void)std::printf("ped-gl %s\n", glVersion ? glVersion : "(null)");
    std::string gameDir = ResolveGameDir(argc, argv);
    WorldShotScene scene{};
    SkinPedStats pst{};
    char pedErr[512] = {};
    if (!SkinPed_Init(gameDir.c_str(), model, scene, pst, pedErr, sizeof(pedErr))) {
        (void)std::printf("ped-fail load %s (game=%s model=%s)\n", pedErr, gameDir.c_str(), model);
        SkinPed_Shutdown();
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroyContext(display, context);
        eglDestroySurface(display, surface);
        eglTerminate(display);
        return 1;
    }
    (void)std::printf(
        "ped-load model=%s src=%s txd=%s textures=%d geoms=%d frames=%d "
        "bbox=[%.2f,%.2f,%.2f]-[%.2f,%.2f,%.2f]\n",
        pst.model, pst.src, pst.txd, pst.textures, pst.geoms, pst.frames,
        scene.bboxMin[0], scene.bboxMin[1], scene.bboxMin[2],
        scene.bboxMax[0], scene.bboxMax[1], scene.bboxMax[2]
    );
    if (pst.tried > 0) {
        (void)std::printf("ped-fallback requested=%s picked=%s tried=%d cap=128sec\n", pst.requested,
                           pst.src, pst.tried);
    }
    (void)std::printf("ped-skin bones=%d attached=%d/%d wsum=%.6f binddev=%.8f anim=bind\n",
                       pst.bones, pst.attached, pst.bones, pst.wsum, pst.binddev);
    {
        SkinPedVert v{};
        if (SkinPed_SampleVert(v)) {
            (void)std::printf(
                "ped-vert n=0 stored=(%.4f,%.4f,%.4f) skinned=(%.4f,%.4f,%.4f) "
                "bones=[%d,%d,%d,%d] weights=[%.4f,%.4f,%.4f,%.4f]\n",
                v.stored[0], v.stored[1], v.stored[2], v.skinned[0], v.skinned[1], v.skinned[2],
                v.bones[0], v.bones[1], v.bones[2], v.bones[3], v.weights[0], v.weights[1],
                v.weights[2], v.weights[3]
            );
        }
    }
    bool gateVerts = pst.verts > 1000;
    bool gateTris = pst.tris > 1000;
    bool gateBones = pst.bones >= 10;
    bool gateWsum = std::fabs(pst.wsum - 1.0) < 0.01;
    if (!(gateVerts && gateTris && gateBones && gateWsum)) {
        (void)std::printf(
            "ped-fail gate verts=%d(>1000) tris=%d(>1000) bones=%d(>=10) wsum=%.6f(~1.0)\n",
            pst.verts, pst.tris, pst.bones, pst.wsum
        );
        SkinPed_Shutdown();
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroyContext(display, context);
        eglDestroySurface(display, surface);
        eglTerminate(display);
        return 1;
    }
    std::vector<uint8> pixels;
    TexFrameStats texStats{};
    DrawWorldFrame(scene, width, height, 60.0f, nullptr, pixels, texStats);
    uint64_t sumR = 0;
    uint64_t sumG = 0;
    uint64_t sumB = 0;
    uint64_t nonBlack = 0;
    uint64_t checksum = PixelsChecksum(pixels, sumR, sumG, sumB, nonBlack);
    uint64_t total = static_cast<uint64_t>(width) * static_cast<uint64_t>(height);
    if (nonBlack == 0) {
        (void)std::printf("ped-fail black frame\n");
        SkinPed_Shutdown();
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroyContext(display, context);
        eglDestroySurface(display, surface);
        eglTerminate(display);
        return 1;
    }
    if (!WriteTga24(outPath, width, height, pixels)) {
        (void)std::printf("ped-fail write '%s'\n", outPath);
        SkinPed_Shutdown();
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
    OS_DebugOut("mad-sa-linux ped shot");
    (void)std::printf(
        "texped-ok tris=%d sampledTri=%d texelFetch=%ld greyFallback=%d flatTri=%d texPixels=%ld "
        "uv=[%.3f,%.3f]x[%.3f,%.3f] firstTex=%s texel=%d,%d,%d,%d pixel=%d,%d,%d render=cpu\n",
        texStats.tris, texStats.sampledTri, texStats.texelFetch, texStats.fallbackTri, texStats.flatTri,
        texStats.texPixels, texStats.haveUV ? texStats.uvMin[0] : 0.0f,
        texStats.haveUV ? texStats.uvMax[0] : 0.0f, texStats.haveUV ? texStats.uvMin[1] : 0.0f,
        texStats.haveUV ? texStats.uvMax[1] : 0.0f, texStats.haveFirst ? texStats.firstTex : "-",
        texStats.firstTexel[0], texStats.firstTexel[1], texStats.firstTexel[2],
        texStats.firstTexel[3], texStats.firstPixel[0], texStats.firstPixel[1],
        texStats.firstPixel[2]
    );
    (void)std::printf(
        "ped-ok model=%s verts=%d tris=%d bones=%d weights=W4sum=%.6f checksum=%llu anim=bind "
        "src=%s\n",
        pst.model, pst.verts, pst.tris, pst.bones, pst.wsum,
        static_cast<unsigned long long>(checksum), pst.src
    );
    (void)std::printf(
        "pedshot-ok out=%s nonblack=%llu/%llu avg=%llu,%llu,%llu checksum=%llu\n", outPath,
        static_cast<unsigned long long>(nonBlack), static_cast<unsigned long long>(total),
        static_cast<unsigned long long>(sumR / total),
        static_cast<unsigned long long>(sumG / total),
        static_cast<unsigned long long>(sumB / total), static_cast<unsigned long long>(checksum)
    );
    SkinPed_Shutdown();
    return 0;
}

// Round 12 (R6j): ped standing via one IFP keyframe. Loads the same DFF as
// --shot-ped, samples a single keyframe per bone from anim/ped.ifp at
// fractional time T (default 0.5, no lerp/slerp between frames this round),
// retargets by bone tag/name (unmapped bones keep bind), skins with the
// animated matrices, and renders through the same CPU orbit rasterizer.
// Every quat/trans comes from IFP bytes; procedural uprights are forbidden.
int RunShotAnim(int argc, char** argv) {
    const char* outPath = ArgValue(argc, argv, "--shot-anim", "anim.tga");
    const char* model = ArgValue(argc, argv, "--model", "andre");
    const char* anim = ArgValue(argc, argv, "--anim", "IDLE_stance");
    const char* timeArg = ArgValue(argc, argv, "--time", "0.5");
    double timeFrac = std::atof(timeArg ? timeArg : "0.5");
    if (!outPath || outPath[0] == '\0' || !model || model[0] == '\0' || !anim ||
        anim[0] == '\0' || !(timeFrac >= 0.0 && timeFrac <= 1.0)) {
        (void)std::printf("anim-fail bad args shot-anim='%s' model='%s' anim='%s' time='%s'\n",
                           outPath ? outPath : "(null)", model ? model : "(null)",
                           anim ? anim : "(null)", timeArg ? timeArg : "(null)");
        return 1;
    }
    const int width = 640;
    const int height = 480;
    auto getPlatformDisplay = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(
        eglGetProcAddress("eglGetPlatformDisplayEXT")
    );
    if (!getPlatformDisplay) {
        (void)std::printf("anim-fail no eglGetPlatformDisplayEXT\n");
        return 1;
    }
#ifndef EGL_PLATFORM_SURFACELESS_MESA
#define EGL_PLATFORM_SURFACELESS_MESA 0x31DD
#endif
    EGLDisplay display = getPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
    if (display == EGL_NO_DISPLAY) {
        (void)std::printf("anim-fail no surfaceless display 0x%x\n", eglGetError());
        return 1;
    }
    if (!eglInitialize(display, nullptr, nullptr)) {
        (void)std::printf("anim-fail egl init 0x%x\n", eglGetError());
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
        (void)std::printf("anim-fail choose config 0x%x\n", eglGetError());
        eglTerminate(display);
        return 1;
    }
    const EGLint pbufferAttrs[] = { EGL_WIDTH, width, EGL_HEIGHT, height, EGL_NONE };
    EGLSurface surface = eglCreatePbufferSurface(display, config, pbufferAttrs);
    if (surface == EGL_NO_SURFACE) {
        (void)std::printf("anim-fail pbuffer 0x%x\n", eglGetError());
        eglTerminate(display);
        return 1;
    }
    (void)eglBindAPI(EGL_OPENGL_API);
    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, nullptr);
    if (context == EGL_NO_CONTEXT) {
        (void)std::printf("anim-fail context 0x%x\n", eglGetError());
        eglDestroySurface(display, surface);
        eglTerminate(display);
        return 1;
    }
    if (!eglMakeCurrent(display, surface, surface, context)) {
        (void)std::printf("anim-fail make current 0x%x\n", eglGetError());
        eglDestroyContext(display, context);
        eglDestroySurface(display, surface);
        eglTerminate(display);
        return 1;
    }
    const char* glVersion = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    (void)std::printf("anim-gl %s\n", glVersion ? glVersion : "(null)");
    std::string gameDir = ResolveGameDir(argc, argv);
    WorldShotScene scene{};
    IfpAnimStats ast{};
    char animErr[512] = {};
    if (!IfpAnim_Init(gameDir.c_str(), model, anim, timeFrac, scene, ast, animErr, sizeof(animErr))) {
        (void)std::printf("anim-fail load %s (game=%s model=%s anim=%s time=%s)\n", animErr,
                           gameDir.c_str(), model, anim, timeArg);
        IfpAnim_Shutdown();
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroyContext(display, context);
        eglDestroySurface(display, surface);
        eglTerminate(display);
        return 1;
    }
    (void)std::printf(
        "anim-load model=%s src=%s txd=%s textures=%d geoms=%d frames=%d bank=%s bankSrc=%s "
        "anim=%s seqs=%d bankAnims=%d total=%.4f time=%.4f timeAbs=%.4f\n",
        ast.model, ast.src, ast.txd, ast.textures, ast.geoms, ast.frames, ast.bank, ast.bankSrc,
        ast.anim, ast.seqs, ast.animsInBank, ast.animTotal, ast.time, ast.timeAbs
    );
    if (ast.tried > 0) {
        (void)std::printf("anim-fallback requested=%s picked=%s tried=%d cap=128sec\n", ast.requested,
                           ast.src, ast.tried);
    }
    (void)std::printf(
        "anim-skin bones=%d mapped=%d unmapped=%d wsum=%.6f rootDelta=%.6f\n", ast.bones,
        ast.mapped, ast.unmapped, ast.wsum, ast.rootDelta
    );
    (void)std::printf(
        "anim-bone name=\"%s\" tag=%d q=(%.4f,%.4f,%.4f,%.4f) t=(%.4f,%.4f,%.4f) hasTrans=%d "
        "frame=%d/%d\n",
        ast.boneName, ast.boneTag, ast.boneQ[0], ast.boneQ[1], ast.boneQ[2], ast.boneQ[3],
        ast.boneT[0], ast.boneT[1], ast.boneT[2], ast.boneHasTrans, ast.boneFrame, ast.boneFrames
    );
    (void)std::printf(
        "anim-aabb aabbBind=[%.2f,%.2f,%.2f]-[%.2f,%.2f,%.2f] "
        "aabbAnim=[%.2f,%.2f,%.2f]-[%.2f,%.2f,%.2f] abasis=bind:skin,anim:world\n",
        ast.bindMin[0], ast.bindMin[1], ast.bindMin[2], ast.bindMax[0], ast.bindMax[1],
        ast.bindMax[2], ast.animMin[0], ast.animMin[1], ast.animMin[2], ast.animMax[0],
        ast.animMax[1], ast.animMax[2]
    );
    bool gateBones = ast.bones == 32;
    bool gateMapped = ast.mapped >= 24;
    bool gateVerts = ast.verts > 1000;
    bool gateTris = ast.tris > 1000;
    bool gateWsum = std::fabs(ast.wsum - 1.0) < 0.01;
    float bindHeightZ = ast.bindMax[2] - ast.bindMin[2];
    float animHeightZ = ast.animMax[2] - ast.animMin[2];
    float bindSpanX = ast.bindMax[0] - ast.bindMin[0];
    float animSpanX = ast.animMax[0] - ast.animMin[0];
    bool gateStandZ = animHeightZ >= 1.5f;
    bool gateStandX = animSpanX <= 1.2f;
    bool gateRoot = ast.rootDelta > 1e-6f;
    if (!(gateBones && gateMapped && gateVerts && gateTris && gateWsum && gateStandZ && gateStandX &&
          gateRoot)) {
        (void)std::printf(
            "anim-fail gate bones=%d(==32) mapped=%d(>=24) verts=%d(>1000) tris=%d(>1000) "
            "wsum=%.6f(~1.0) animHeightZ=%.2f(>=1.50, bind=%.2f) animSpanX=%.2f(<=1.20, "
            "bind=%.2f) rootDelta=%.6f(>0)\n",
            ast.bones, ast.mapped, ast.verts, ast.tris, ast.wsum, animHeightZ, bindHeightZ,
            animSpanX, bindSpanX, ast.rootDelta
        );
        IfpAnim_Shutdown();
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroyContext(display, context);
        eglDestroySurface(display, surface);
        eglTerminate(display);
        return 1;
    }
    std::vector<uint8> pixels;
    TexFrameStats texStats{};
    DrawWorldFrame(scene, width, height, 60.0f, nullptr, pixels, texStats);
    uint64_t sumR = 0;
    uint64_t sumG = 0;
    uint64_t sumB = 0;
    uint64_t nonBlack = 0;
    uint64_t checksum = PixelsChecksum(pixels, sumR, sumG, sumB, nonBlack);
    uint64_t total = static_cast<uint64_t>(width) * static_cast<uint64_t>(height);
    if (nonBlack == 0) {
        (void)std::printf("anim-fail black frame\n");
        IfpAnim_Shutdown();
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroyContext(display, context);
        eglDestroySurface(display, surface);
        eglTerminate(display);
        return 1;
    }
    if (checksum == 8661044579928738921ULL) {
        (void)std::printf("anim-fail checksum equals bind pose (no movement)\n");
        IfpAnim_Shutdown();
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroyContext(display, context);
        eglDestroySurface(display, surface);
        eglTerminate(display);
        return 1;
    }
    if (!WriteTga24(outPath, width, height, pixels)) {
        (void)std::printf("anim-fail write '%s'\n", outPath);
        IfpAnim_Shutdown();
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
    OS_DebugOut("mad-sa-linux anim shot");
    (void)std::printf(
        "texanim-ok tris=%d sampledTri=%d texelFetch=%ld greyFallback=%d flatTri=%d texPixels=%ld "
        "uv=[%.3f,%.3f]x[%.3f,%.3f] firstTex=%s texel=%d,%d,%d,%d pixel=%d,%d,%d render=cpu\n",
        texStats.tris, texStats.sampledTri, texStats.texelFetch, texStats.fallbackTri, texStats.flatTri,
        texStats.texPixels, texStats.haveUV ? texStats.uvMin[0] : 0.0f,
        texStats.haveUV ? texStats.uvMax[0] : 0.0f, texStats.haveUV ? texStats.uvMin[1] : 0.0f,
        texStats.haveUV ? texStats.uvMax[1] : 0.0f, texStats.haveFirst ? texStats.firstTex : "-",
        texStats.firstTexel[0], texStats.firstTexel[1], texStats.firstTexel[2],
        texStats.firstTexel[3], texStats.firstPixel[0], texStats.firstPixel[1],
        texStats.firstPixel[2]
    );
    (void)std::printf(
        "anim-ok model=%s anim=%s time=%.4f bones=%d mapped=%d unmapped=%d posedVerts=%d "
        "tris=%d checksum=%llu rootDelta=%.6f wsum=%.6f "
        "aabbBind=[%.2f,%.2f,%.2f]-[%.2f,%.2f,%.2f] "
        "aabbAnim=[%.2f,%.2f,%.2f]-[%.2f,%.2f,%.2f] bank=%s src=%s\n",
        ast.model, ast.anim, ast.time, ast.bones, ast.mapped, ast.unmapped, ast.verts, ast.tris,
        static_cast<unsigned long long>(checksum), ast.rootDelta, ast.wsum, ast.bindMin[0],
        ast.bindMin[1], ast.bindMin[2], ast.bindMax[0], ast.bindMax[1], ast.bindMax[2],
        ast.animMin[0], ast.animMin[1], ast.animMin[2], ast.animMax[0], ast.animMax[1],
        ast.animMax[2], ast.bankSrc, ast.src
    );
    (void)std::printf(
        "animshot-ok out=%s nonblack=%llu/%llu avg=%llu,%llu,%llu checksum=%llu\n", outPath,
        static_cast<unsigned long long>(nonBlack), static_cast<unsigned long long>(total),
        static_cast<unsigned long long>(sumR / total),
        static_cast<unsigned long long>(sumG / total),
        static_cast<unsigned long long>(sumB / total), static_cast<unsigned long long>(checksum)
    );
    IfpAnim_Shutdown();
    return 0;
}

// Round 13 (R6k): walk-cycle interpolation sequence. Samples K evenly
// spaced times (inclusive endpoints T_i=i/(K-1), default K=6) of a walk
// animation (default WALK_civi from anim/ped.ifp) with lerp (trans) + slerp
// (quat) between neighbouring IFP keys, skins+renders every frame through
// the same CPU orbit rasterizer, writes the LAST frame TGA, and reports
// animseq-ok with per-frame checksums, rootTravel (world distance of the
// tag-0 bone between frame 0 and K-1) and loopGap (mean joint-space pose
// distance between frame K-1 and frame 0 from IFP bytes only). All SRT come
// from IFP bytes; procedural poses are forbidden.
int RunAnimSeq(int argc, char** argv) {
    const char* outPath = ArgValue(argc, argv, "--anim-seq", "animseq.tga");
    const char* model = ArgValue(argc, argv, "--model", "andre");
    const char* animReq = ArgValue(argc, argv, "--anim", "WALK_civi");
    const char* framesArg = ArgValue(argc, argv, "--frames", "6");
    int frames = std::atoi(framesArg ? framesArg : "6");
    if (!outPath || outPath[0] == '\0' || !model || model[0] == '\0' || !animReq ||
        animReq[0] == '\0' || frames < 2 || frames > 64) {
        (void)std::printf("animseq-fail bad args anim-seq='%s' model='%s' anim='%s' frames='%s'\n",
                           outPath ? outPath : "(null)", model ? model : "(null)",
                           animReq ? animReq : "(null)", framesArg ? framesArg : "(null)");
        return 1;
    }
    const int width = 640;
    const int height = 480;
    auto getPlatformDisplay = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(
        eglGetProcAddress("eglGetPlatformDisplayEXT")
    );
    if (!getPlatformDisplay) {
        (void)std::printf("animseq-fail no eglGetPlatformDisplayEXT\n");
        return 1;
    }
#ifndef EGL_PLATFORM_SURFACELESS_MESA
#define EGL_PLATFORM_SURFACELESS_MESA 0x31DD
#endif
    EGLDisplay display = getPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
    if (display == EGL_NO_DISPLAY) {
        (void)std::printf("animseq-fail no surfaceless display 0x%x\n", eglGetError());
        return 1;
    }
    if (!eglInitialize(display, nullptr, nullptr)) {
        (void)std::printf("animseq-fail egl init 0x%x\n", eglGetError());
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
        (void)std::printf("animseq-fail choose config 0x%x\n", eglGetError());
        eglTerminate(display);
        return 1;
    }
    const EGLint pbufferAttrs[] = { EGL_WIDTH, width, EGL_HEIGHT, height, EGL_NONE };
    EGLSurface surface = eglCreatePbufferSurface(display, config, pbufferAttrs);
    if (surface == EGL_NO_SURFACE) {
        (void)std::printf("animseq-fail pbuffer 0x%x\n", eglGetError());
        eglTerminate(display);
        return 1;
    }
    (void)eglBindAPI(EGL_OPENGL_API);
    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, nullptr);
    if (context == EGL_NO_CONTEXT) {
        (void)std::printf("animseq-fail context 0x%x\n", eglGetError());
        eglDestroySurface(display, surface);
        eglTerminate(display);
        return 1;
    }
    if (!eglMakeCurrent(display, surface, surface, context)) {
        (void)std::printf("animseq-fail make current 0x%x\n", eglGetError());
        eglDestroyContext(display, context);
        eglDestroySurface(display, surface);
        eglTerminate(display);
        return 1;
    }
    const char* glVersion = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    (void)std::printf("animseq-gl %s\n", glVersion ? glVersion : "(null)");
    std::string gameDir = ResolveGameDir(argc, argv);
    std::vector<IfpAnimSeqFrame> seq;
    char seqErr[512] = {};
    std::string animUse = animReq;
    bool didFallback = false;
    std::string fallbackPick;
    if (!IfpAnim_Seq(gameDir.c_str(), model, animUse.c_str(), frames, seq, seqErr, sizeof(seqErr))) {
        // Walk-cycle fallback: if the requested animation is missing from
        // the bank, pick the nearest walk cycle (prefer WALK_civi, else the
        // first stored name containing "walk") and retry once.
        std::string low = animUse;
        for (char& c : low) {
            if (c >= 'A' && c <= 'Z') {
                c = static_cast<char>(c + 32);
            }
        }
        (void)low;
        std::vector<std::string> names;
        char bankSrc[160] = {};
        char listErr[256] = {};
        bool listed = IfpAnim_List(gameDir.c_str(), names, bankSrc, sizeof(bankSrc), listErr,
                                   sizeof(listErr));
        std::string candidate;
        if (listed) {
            for (const auto& n : names) {
                std::string l = n;
                for (char& c : l) {
                    if (c >= 'A' && c <= 'Z') {
                        c = static_cast<char>(c + 32);
                    }
                }
                if (l == "walk_civi") {
                    candidate = n;
                    break;
                }
            }
            if (candidate.empty()) {
                for (const auto& n : names) {
                    std::string l = n;
                    for (char& c : l) {
                        if (c >= 'A' && c <= 'Z') {
                            c = static_cast<char>(c + 32);
                        }
                    }
                    if (l.find("walk") != std::string::npos) {
                        candidate = n;
                        break;
                    }
                }
            }
        }
        if (!candidate.empty() && candidate != animUse) {
            fallbackPick = candidate;
            animUse = candidate;
            didFallback = true;
            seq.clear();
            if (!IfpAnim_Seq(gameDir.c_str(), model, animUse.c_str(), frames, seq, seqErr,
                             sizeof(seqErr))) {
                (void)std::printf("animseq-fail load %s (game=%s model=%s anim=%s frames=%d)\n",
                                   seqErr, gameDir.c_str(), model, animUse.c_str(), frames);
                IfpAnim_Shutdown();
                eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
                eglDestroyContext(display, context);
                eglDestroySurface(display, surface);
                eglTerminate(display);
                return 1;
            }
        } else {
            (void)std::printf("animseq-fail load %s (game=%s model=%s anim=%s frames=%d)\n", seqErr,
                               gameDir.c_str(), model, animUse.c_str(), frames);
            IfpAnim_Shutdown();
            eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            eglDestroyContext(display, context);
            eglDestroySurface(display, surface);
            eglTerminate(display);
            return 1;
        }
    }
    if (static_cast<int>(seq.size()) != frames) {
        (void)std::printf("animseq-fail short seq got=%d want=%d\n", static_cast<int>(seq.size()),
                           frames);
        IfpAnim_Shutdown();
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroyContext(display, context);
        eglDestroySurface(display, surface);
        eglTerminate(display);
        return 1;
    }
    const IfpAnimStats& s0 = seq.front().stats;
    (void)std::printf(
        "animseq-load model=%s src=%s txd=%s textures=%d geoms=%d frames=%d bank=%s bankSrc=%s "
        "anim=%s seqs=%d bankAnims=%d total=%.4f kframes=%d interp=lerp+slerp\n",
        s0.model, s0.src, s0.txd, s0.textures, s0.geoms, s0.frames, s0.bank, s0.bankSrc, s0.anim,
        s0.seqs, s0.animsInBank, s0.animTotal, frames
    );
    if (didFallback) {
        (void)std::printf("animseq-fallback requested=%s picked=%s\n", animReq, fallbackPick.c_str());
    }
    if (s0.tried > 0) {
        (void)std::printf("animseq-model-fallback requested=%s picked=%s tried=%d cap=128sec\n",
                           s0.requested, s0.src, s0.tried);
    }
    std::vector<uint64_t> checksums;
    checksums.reserve(static_cast<size_t>(frames));
    std::vector<uint8_t> lastPixels;
    TexFrameStats lastTex{};
    for (int i = 0; i < frames; ++i) {
        const IfpAnimSeqFrame& fr = seq[static_cast<size_t>(i)];
        std::vector<uint8_t> pixels;
        TexFrameStats texStats{};
        DrawWorldFrame(fr.scene, width, height, 60.0f, nullptr, pixels, texStats);
        uint64_t sumR = 0;
        uint64_t sumG = 0;
        uint64_t sumB = 0;
        uint64_t nonBlack = 0;
        uint64_t checksum = PixelsChecksum(pixels, sumR, sumG, sumB, nonBlack);
        uint64_t total = static_cast<uint64_t>(width) * static_cast<uint64_t>(height);
        if (nonBlack == 0) {
            (void)std::printf("animseq-fail black frame i=%d\n", i);
            IfpAnim_Shutdown();
            eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            eglDestroyContext(display, context);
            eglDestroySurface(display, surface);
            eglTerminate(display);
            return 1;
        }
        checksums.push_back(checksum);
        (void)std::printf(
            "animseq-frame i=%d time=%.4f timeAbs=%.4f checksum=%llu root=(%.4f,%.4f,%.4f) "
            "mapped=%d unmapped=%d wsum=%.6f nonblack=%llu/%llu\n",
            i, fr.stats.time, fr.stats.timeAbs, static_cast<unsigned long long>(checksum),
            fr.stats.rootWorld[0], fr.stats.rootWorld[1], fr.stats.rootWorld[2], fr.stats.mapped,
            fr.stats.unmapped, fr.stats.wsum, static_cast<unsigned long long>(nonBlack),
            static_cast<unsigned long long>(total)
        );
        if (i == frames - 1) {
            lastPixels = std::move(pixels);
            lastTex = texStats;
        }
    }
    // Root travel D: world distance of the tag-0 bone, frame 0 -> frame K-1.
    double dx =
        static_cast<double>(seq.back().stats.rootWorld[0]) - seq.front().stats.rootWorld[0];
    double dy =
        static_cast<double>(seq.back().stats.rootWorld[1]) - seq.front().stats.rootWorld[1];
    double dz =
        static_cast<double>(seq.back().stats.rootWorld[2]) - seq.front().stats.rootWorld[2];
    double rootTravel = std::sqrt(dx * dx + dy * dy + dz * dz);
    // Loop gap G: mean joint-space pose distance (IFP bytes only).
    float loopGap = 0.0f;
    int gapMapped = 0;
    {
        char gapErr[256] = {};
        if (!IfpAnim_LoopGap(gameDir.c_str(), seq.front().stats.anim, &loopGap, &gapMapped, gapErr,
                             sizeof(gapErr))) {
            (void)std::printf("animseq-fail loopgap %s\n", gapErr);
            IfpAnim_Shutdown();
            eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            eglDestroyContext(display, context);
            eglDestroySurface(display, surface);
            eglTerminate(display);
            return 1;
        }
    }
    // Keyaudit: first frame with a fractional bracket proves real lerp+slerp
    // between two neighbouring IFP keys (all values are IFP bytes).
    const IfpAnimStats* audit = &seq.front().stats;
    for (int i = 0; i < frames; ++i) {
        const IfpAnimStats& st = seq[static_cast<size_t>(i)].stats;
        if (st.interp == 1 && st.keyK0 >= 0 && st.keyK1 == st.keyK0 + 1 && st.keyAlpha > 0.0001f &&
            st.keyAlpha < 0.9999f) {
            audit = &st;
            break;
        }
    }
    (void)std::printf(
        "keyaudit bone=\"%s\" tag=%d k0=%d k1=%d t0=%.4f t1=%.4f alpha=%.4f timeAbs=%.4f "
        "q0=(%.4f,%.4f,%.4f,%.4f) q1=(%.4f,%.4f,%.4f,%.4f) qi=(%.4f,%.4f,%.4f,%.4f) "
        "p0=(%.4f,%.4f,%.4f) p1=(%.4f,%.4f,%.4f) pi=(%.4f,%.4f,%.4f) hasT=%d interp=lerp+slerp\n",
        audit->keyBone, audit->keyTag, audit->keyK0, audit->keyK1, audit->keyT0, audit->keyT1,
        audit->keyAlpha, audit->keyTimeAbs, audit->keyQ0[0], audit->keyQ0[1], audit->keyQ0[2],
        audit->keyQ0[3], audit->keyQ1[0], audit->keyQ1[1], audit->keyQ1[2], audit->keyQ1[3],
        audit->keyQI[0], audit->keyQI[1], audit->keyQI[2], audit->keyQI[3], audit->keyP0[0],
        audit->keyP0[1], audit->keyP0[2], audit->keyP1[0], audit->keyP1[1], audit->keyP1[2],
        audit->keyPI[0], audit->keyPI[1], audit->keyPI[2], audit->keyHasT
    );
    // Gates (honest, no tuning): distinct frames, real motion, closed loop.
    bool gateDistinct = true;
    for (int i = 0; i < frames && gateDistinct; ++i) {
        for (int j = i + 1; j < frames; ++j) {
            if (checksums[static_cast<size_t>(i)] == checksums[static_cast<size_t>(j)]) {
                gateDistinct = false;
                break;
            }
        }
    }
    bool gateKnown = true;
    for (int i = 0; i < frames; ++i) {
        if (checksums[static_cast<size_t>(i)] == 8661044579928738921ULL ||
            checksums[static_cast<size_t>(i)] == 4444196192875791124ULL) {
            gateKnown = false;
            break;
        }
    }
    bool gateTravel = rootTravel > 0.05;
    bool gateLoop = loopGap < rootTravel;
    bool gateMapped = s0.mapped >= 24;
    bool gateWsum = std::fabs(s0.wsum - 1.0) < 0.01;
    if (!(gateDistinct && gateKnown && gateTravel && gateLoop && gateMapped && gateWsum)) {
        (void)std::printf(
            "animseq-fail gate distinct=%d known=%d travel=%.6f(>0.05) gap=%.6f(<travel) "
            "mapped=%d(>=24) wsum=%.6f(~1.0)\n",
            gateDistinct ? 1 : 0, gateKnown ? 1 : 0, rootTravel, loopGap, s0.mapped, s0.wsum
        );
        IfpAnim_Shutdown();
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroyContext(display, context);
        eglDestroySurface(display, surface);
        eglTerminate(display);
        return 1;
    }
    if (!WriteTga24(outPath, width, height, lastPixels)) {
        (void)std::printf("animseq-fail write '%s'\n", outPath);
        IfpAnim_Shutdown();
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
    OS_DebugOut("mad-sa-linux anim seq");
    (void)std::printf(
        "texanimseq-ok tris=%d sampledTri=%d texelFetch=%ld greyFallback=%d flatTri=%d "
        "texPixels=%ld firstTex=%s render=cpu\n",
        lastTex.tris, lastTex.sampledTri, lastTex.texelFetch, lastTex.fallbackTri, lastTex.flatTri,
        lastTex.texPixels, lastTex.haveFirst ? lastTex.firstTex : "-"
    );
    std::string cs;
    for (int i = 0; i < frames; ++i) {
        char cell[32];
        (void)std::snprintf(cell, sizeof(cell), "%s%llu", i ? "," : "",
                             static_cast<unsigned long long>(checksums[static_cast<size_t>(i)]));
        cs += cell;
    }
    (void)std::printf("animseq-ok model=%s anim=%s frames=%d checksums=%s rootTravel=%.6f loopGap=%.6f "
                       "interp=lerp+slerp\n",
                       s0.model, s0.anim, frames, cs.c_str(), rootTravel, loopGap);
    (void)std::printf("animseqshot-ok out=%s frames=%d rootTravel=%.6f loopGap=%.6f\n", outPath, frames,
                       rootTravel, loopGap);
    IfpAnim_Shutdown();
    return 0;
}

// Round 14 (R6l): car wheels steer + spin. Loads one car DFF (default
// --model landstal) with its per-model TXD, finds the wheel dummy frames by
// their stored hierarchy names, applies --steer (front-pair yaw, degrees)
// and --spin (all-wheel roll, degrees) as extra local matrices over the DFF
// transforms (CarPose: the retail RpAtomicClone instancing of the stored
// `wheel` mesh onto every dummy, front steer like m_fSteerAngle, roll like
// m_wheelRotation), and renders through the same CPU orbit rasterizer. Every
// vertex comes from DFF bytes, every angle from argv; the body audit frame
// must stay bit-identical (whole-body rotation instead of wheels fails).
int RunShotCar(int argc, char** argv) {
    const char* outPath = ArgValue(argc, argv, "--shot-car", "car.tga");
    const char* model = ArgValue(argc, argv, "--model", "landstal");
    const char* steerArg = ArgValue(argc, argv, "--steer", "0");
    const char* spinArg = ArgValue(argc, argv, "--spin", "0");
    double steerDeg = std::atof(steerArg ? steerArg : "0");
    double spinDeg = std::atof(spinArg ? spinArg : "0");
    if (!outPath || outPath[0] == '\0' || !model || model[0] == '\0' ||
        !std::isfinite(steerDeg) || !std::isfinite(spinDeg)) {
        (void)std::printf("car-fail bad args shot-car='%s' model='%s' steer='%s' spin='%s'\n",
                           outPath ? outPath : "(null)", model ? model : "(null)",
                           steerArg ? steerArg : "(null)", spinArg ? spinArg : "(null)");
        return 1;
    }
    const int width = 640;
    const int height = 480;
    auto getPlatformDisplay = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(
        eglGetProcAddress("eglGetPlatformDisplayEXT")
    );
    if (!getPlatformDisplay) {
        (void)std::printf("car-fail no eglGetPlatformDisplayEXT\n");
        return 1;
    }
#ifndef EGL_PLATFORM_SURFACELESS_MESA
#define EGL_PLATFORM_SURFACELESS_MESA 0x31DD
#endif
    EGLDisplay display = getPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
    if (display == EGL_NO_DISPLAY) {
        (void)std::printf("car-fail no surfaceless display 0x%x\n", eglGetError());
        return 1;
    }
    if (!eglInitialize(display, nullptr, nullptr)) {
        (void)std::printf("car-fail egl init 0x%x\n", eglGetError());
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
        (void)std::printf("car-fail choose config 0x%x\n", eglGetError());
        eglTerminate(display);
        return 1;
    }
    const EGLint pbufferAttrs[] = { EGL_WIDTH, width, EGL_HEIGHT, height, EGL_NONE };
    EGLSurface surface = eglCreatePbufferSurface(display, config, pbufferAttrs);
    if (surface == EGL_NO_SURFACE) {
        (void)std::printf("car-fail pbuffer 0x%x\n", eglGetError());
        eglTerminate(display);
        return 1;
    }
    (void)eglBindAPI(EGL_OPENGL_API);
    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, nullptr);
    if (context == EGL_NO_CONTEXT) {
        (void)std::printf("car-fail context 0x%x\n", eglGetError());
        eglDestroySurface(display, surface);
        eglTerminate(display);
        return 1;
    }
    if (!eglMakeCurrent(display, surface, surface, context)) {
        (void)std::printf("car-fail make current 0x%x\n", eglGetError());
        eglDestroyContext(display, context);
        eglDestroySurface(display, surface);
        eglTerminate(display);
        return 1;
    }
    const char* glVersion = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    (void)std::printf("car-gl %s\n", glVersion ? glVersion : "(null)");
    std::string gameDir = ResolveGameDir(argc, argv);
    WorldShotScene scene{};
    CarPoseStats cst{};
    CarPoseAudit audit{};
    char carErr[512] = {};
    if (!CarPose_Init(gameDir.c_str(), model, steerDeg, spinDeg, scene, cst, audit, carErr,
                       sizeof(carErr))) {
        (void)std::printf("car-fail load %s (game=%s model=%s steer=%s spin=%s)\n", carErr,
                           gameDir.c_str(), model, steerArg, spinArg);
        CarPose_Shutdown();
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroyContext(display, context);
        eglDestroySurface(display, surface);
        eglTerminate(display);
        return 1;
    }
    (void)std::printf(
        "car-load model=%s src=%s txd=%s textures=%d geoms=%d frames=%d "
        "bbox=[%.2f,%.2f,%.2f]-[%.2f,%.2f,%.2f]\n",
        cst.model, cst.src, cst.txd, cst.textures, cst.geoms, cst.frames,
        scene.bboxMin[0], scene.bboxMin[1], scene.bboxMin[2],
        scene.bboxMax[0], scene.bboxMax[1], scene.bboxMax[2]
    );
    {
        std::string wl;
        for (int i = 0; i < cst.wheels && i < 4; ++i) {
            char cell[40];
            (void)std::snprintf(cell, sizeof(cell), "%s%s", i ? "," : "",
                                 cst.wheelNames[i]);
            wl += cell;
        }
        (void)std::printf("wheels=%s\n", wl.c_str());
    }
    (void)std::printf("car-pose steer=%.3f spin=%.3f fronts=%d wheels=%d bodyTris=%d wheelTris=%d\n",
                       cst.steerDeg, cst.spinDeg, cst.fronts, cst.wheels, cst.bodyTris,
                       cst.wheelTris);
    if (!cst.chassisSame) {
        (void)std::printf("car-fail body moved during wheel pose (audit frame changed)\n");
        CarPose_Shutdown();
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroyContext(display, context);
        eglDestroySurface(display, surface);
        eglTerminate(display);
        return 1;
    }
    std::vector<uint8> pixels;
    TexFrameStats texStats{};
    DrawWorldFrame(scene, width, height, 60.0f, nullptr, pixels, texStats);
    uint64_t sumR = 0;
    uint64_t sumG = 0;
    uint64_t sumB = 0;
    uint64_t nonBlack = 0;
    uint64_t checksum = PixelsChecksum(pixels, sumR, sumG, sumB, nonBlack);
    uint64_t total = static_cast<uint64_t>(width) * static_cast<uint64_t>(height);
    if (nonBlack == 0) {
        (void)std::printf("car-fail black frame\n");
        CarPose_Shutdown();
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroyContext(display, context);
        eglDestroySurface(display, surface);
        eglTerminate(display);
        return 1;
    }
    if (!WriteTga24(outPath, width, height, pixels)) {
        (void)std::printf("car-fail write '%s'\n", outPath);
        CarPose_Shutdown();
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
    OS_DebugOut("mad-sa-linux car shot");
    (void)std::printf(
        "texcar-ok tris=%d sampledTri=%d texelFetch=%ld greyFallback=%d flatTri=%d texPixels=%ld "
        "uv=[%.3f,%.3f]x[%.3f,%.3f] firstTex=%s texel=%d,%d,%d,%d pixel=%d,%d,%d render=cpu\n",
        texStats.tris, texStats.sampledTri, texStats.texelFetch, texStats.fallbackTri, texStats.flatTri,
        texStats.texPixels, texStats.haveUV ? texStats.uvMin[0] : 0.0f,
        texStats.haveUV ? texStats.uvMax[0] : 0.0f, texStats.haveUV ? texStats.uvMin[1] : 0.0f,
        texStats.haveUV ? texStats.uvMax[1] : 0.0f, texStats.haveFirst ? texStats.firstTex : "-",
        texStats.firstTexel[0], texStats.firstTexel[1], texStats.firstTexel[2],
        texStats.firstTexel[3], texStats.firstPixel[0], texStats.firstPixel[1],
        texStats.firstPixel[2]
    );
    {
        char b[12 * 24 + 1] = {};
        char a[12 * 24 + 1] = {};
        size_t bo = 0, ao = 0;
        for (int i = 0; i < 12; ++i) {
            bo += static_cast<size_t>(std::snprintf(b + bo, sizeof(b) - bo, "%s%.5f",
                                                     i ? "," : "", audit.before[i]));
            ao += static_cast<size_t>(std::snprintf(a + ao, sizeof(a) - ao, "%s%.5f",
                                                     i ? "," : "", audit.after[i]));
        }
        (void)std::printf("wheelAudit=%s:before=%s:after=%s:body=%s:same=%d\n", audit.wheel, b,
                           a, audit.body, audit.bodySame);
        (void)std::printf("car-ok model=%s wheels=%d steer=%.3f spin=%.3f checksum=%llu "
                           "wheelAudit=%s:before=%s:after=%s:body=%s:same=%d\n",
                           cst.model, cst.wheels, cst.steerDeg, cst.spinDeg,
                           static_cast<unsigned long long>(checksum), audit.wheel, b, a,
                           audit.body, audit.bodySame);
    }
    (void)std::printf(
        "carshot-ok out=%s nonblack=%llu/%llu avg=%llu,%llu,%llu checksum=%llu\n", outPath,
        static_cast<unsigned long long>(nonBlack), static_cast<unsigned long long>(total),
        static_cast<unsigned long long>(sumR / total),
        static_cast<unsigned long long>(sumG / total),
        static_cast<unsigned long long>(sumB / total), static_cast<unsigned long long>(checksum)
    );
    CarPose_Shutdown();
    return 0;
}

// Round 16 (R6n): kinematic drive. Car (CarPose DFF/TXD, steer+spin) rides a
// caller-supplied XY polyline; Z comes only from the COL raycast
// (carZ = groundH + clearance), yaw from the segment, front steer from
// curvature, spin from distance/wheelR, chase-cam behind-above, world from
// the existing StreamPager, pixels from the existing CPU rasterizer.
// Kinematics only: no handling.cfg physics in this slice (honest cut).
int RunDrive(int argc, char** argv) {
    const char* pathArg = ArgValue(argc, argv, "--path", nullptr);
    const char* model = ArgValue(argc, argv, "--model", "landstal");
    const char* outArg = ArgValue(argc, argv, "--out", "drive");
    if (!model || model[0] == '\0' || !outArg || outArg[0] == '\0') {
        (void)std::printf("drive-fail bad args model='%s' out='%s'\n", model ? model : "(null)",
                           outArg ? outArg : "(null)");
        return 1;
    }
    // Default drive path: downtown freeway -> crossroads deck -> airport
    // terminal, all on bound COL road decks (see R6g probes). Bend ~68 deg,
    // ground span ~14 m (no constant height).
    std::string pathStr = pathArg ? pathArg : "1608.20,-1721.80:1755.60,-1812.30:1683.22,-2242.96";
    std::vector<std::pair<double, double>> ctrl;
    {
        size_t pos = 0;
        while (pos <= pathStr.size()) {
            size_t end = pathStr.find(':', pos);
            std::string tok =
                pathStr.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
            pos = end == std::string::npos ? pathStr.size() + 1 : end + 1;
            // Trim spaces.
            size_t b = tok.find_first_not_of(" \t");
            size_t e = tok.find_last_not_of(" \t");
            if (b == std::string::npos) {
                (void)std::printf("drive-fail bad --path '%s' (empty point)\n", pathStr.c_str());
                return 1;
            }
            tok = tok.substr(b, e - b + 1);
            double x = 0.0, y = 0.0;
            // XY only in this round (Z comes from the COL raycast).
            if (std::sscanf(tok.c_str(), "%lf , %lf", &x, &y) != 2 || !std::isfinite(x) ||
                !std::isfinite(y)) {
                (void)std::printf("drive-fail bad --path '%s' (want Ax,Ay:Bx,By:...)\n",
                                   pathStr.c_str());
                return 1;
            }
            ctrl.emplace_back(x, y);
            if (pos > pathStr.size()) {
                break;
            }
        }
    }
    if (ctrl.size() < 2) {
        (void)std::printf("drive-fail need >= 2 path points (got %d)\n",
                           static_cast<int>(ctrl.size()));
        return 1;
    }
    const char* wpArg = ArgValue(argc, argv, "--waypoints", nullptr);
    int waypoints = wpArg ? std::atoi(wpArg) : static_cast<int>(ctrl.size());
    int framesPerLeg = std::atoi(ArgValue(argc, argv, "--frames-per-leg", "3"));
    if (waypoints < 3 || waypoints > 64 || framesPerLeg < 1 || framesPerLeg > 120) {
        (void)std::printf("drive-fail bad args waypoints='%s' frames-per-leg='%s' (want W 3..64, F 1..120)\n",
                           ArgValue(argc, argv, "--waypoints", "3"),
                           ArgValue(argc, argv, "--frames-per-leg", "3"));
        return 1;
    }
    std::string prefix = outArg;
    // "--out dir/" (trailing slash) means <dir>/drive_W?.tga; otherwise the
    // value is a file prefix exactly like the --e2e mode.
    if (!prefix.empty() && prefix.back() == '/') {
        prefix += "drive";
    }
    const int width = 640;
    const int height = 480;
    auto getPlatformDisplay = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(
        eglGetProcAddress("eglGetPlatformDisplayEXT")
    );
    if (!getPlatformDisplay) {
        (void)std::printf("drive-fail no eglGetPlatformDisplayEXT\n");
        return 1;
    }
#ifndef EGL_PLATFORM_SURFACELESS_MESA
#define EGL_PLATFORM_SURFACELESS_MESA 0x31DD
#endif
    EGLDisplay display = getPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
    if (display == EGL_NO_DISPLAY) {
        (void)std::printf("drive-fail no surfaceless display 0x%x\n", eglGetError());
        return 1;
    }
    if (!eglInitialize(display, nullptr, nullptr)) {
        (void)std::printf("drive-fail egl init 0x%x\n", eglGetError());
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
        (void)std::printf("drive-fail choose config 0x%x\n", eglGetError());
        eglTerminate(display);
        return 1;
    }
    const EGLint pbufferAttrs[] = { EGL_WIDTH, width, EGL_HEIGHT, height, EGL_NONE };
    EGLSurface surface = eglCreatePbufferSurface(display, config, pbufferAttrs);
    if (surface == EGL_NO_SURFACE) {
        (void)std::printf("drive-fail pbuffer 0x%x\n", eglGetError());
        eglTerminate(display);
        return 1;
    }
    (void)eglBindAPI(EGL_OPENGL_API);
    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, nullptr);
    if (context == EGL_NO_CONTEXT) {
        (void)std::printf("drive-fail context 0x%x\n", eglGetError());
        eglDestroySurface(display, surface);
        eglTerminate(display);
        return 1;
    }
    if (!eglMakeCurrent(display, surface, surface, context)) {
        (void)std::printf("drive-fail make current 0x%x\n", eglGetError());
        eglDestroyContext(display, context);
        eglDestroySurface(display, surface);
        eglTerminate(display);
        return 1;
    }
    const char* glVersion = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    (void)std::printf("drive-gl %s\n", glVersion ? glVersion : "(null)");
    std::string gameDir = ResolveGameDir(argc, argv);
    // 1. DFF kinematics constants (wheelR/wheelbase/clearance from DFF bytes).
    DriveMeasure meas{};
    {
        char mErr[512] = {};
        if (!DriveSim_Measure(gameDir.c_str(), model, meas, mErr, sizeof(mErr))) {
            (void)std::printf("drive-fail measure %s (game=%s model=%s)\n", mErr,
                               gameDir.c_str(), model);
            eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            eglDestroyContext(display, context);
            eglDestroySurface(display, surface);
            eglTerminate(display);
            return 1;
        }
    }
    (void)std::printf(
        "drive-load model=%s src=%s wheels=%d wheelR=%.6f wheelbase=%.6f clearance=%.6f "
        "frontY=%.6f rearY=%.6f\n",
        meas.model, meas.src, meas.wheels, meas.wheelR, meas.wheelbase, meas.clearance,
        meas.frontY, meas.rearY);
    // 2. World (pager + COL) around the drive corridor.
    E2ELoadInfo load{};
    {
        char wErr[512] = {};
        if (!DriveSim_InitWorld(gameDir.c_str(), load, wErr, sizeof(wErr))) {
            (void)std::printf("drive-fail world-init %s (game=%s)\n", wErr, gameDir.c_str());
            eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            eglDestroyContext(display, context);
            eglDestroySurface(display, surface);
            eglTerminate(display);
            return 1;
        }
    }
    (void)std::printf("drive-world iplTotal=%d kept=%d ide=%d ideFiles=%d iplFiles=%d cell=300 "
                       "R=300 H=100 cap=80\n",
                       load.iplTotal, load.iplKept, load.ideModels, load.ideFiles, load.iplFiles);
    // Fixed chase + steering laws (logged verbatim for the report).
    constexpr double kCamD = 8.0;
    constexpr double kCamH = 4.0;
    (void)std::printf("drive-formula steerFormula=atan(wheelbase*dyaw/ds) camD=%.1f camH=%.1f "
                       "clearance=%.6f wheelR=%.6f\n",
                       kCamD, kCamH, meas.clearance, meas.wheelR);
    double maxTurn = DriveSim_MaxTurnDeg(ctrl);
    {
        std::string ps;
        for (size_t i = 0; i < ctrl.size(); ++i) {
            char cell[64];
            (void)std::snprintf(cell, sizeof(cell), "%s%.2f,%.2f", i ? ":" : "", ctrl[i].first,
                                 ctrl[i].second);
            ps += cell;
        }
        (void)std::printf("drive-path controls=%d waypoints=%d framesPerLeg=%d path=%s "
                           "maxTurn=%.1f\n",
                           static_cast<int>(ctrl.size()), waypoints, framesPerLeg, ps.c_str(),
                           maxTurn);
    }
    if (maxTurn < 20.0) {
        (void)std::printf("drive-fail no turn >= 20 deg (maxTurn=%.1f)\n", maxTurn);
        DriveSim_ShutdownWorld();
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroyContext(display, context);
        eglDestroySurface(display, surface);
        eglTerminate(display);
        return 1;
    }
    // 3. Kinematic sample (yaw/steer/spin/dist from path + DFF constants).
    std::vector<DriveWaypoint> wps;
    {
        char sErr[512] = {};
        if (!DriveSim_Sample(ctrl, waypoints, meas.wheelbase, meas.wheelR, wps, sErr,
                              sizeof(sErr))) {
            (void)std::printf("drive-fail sample %s\n", sErr);
            DriveSim_ShutdownWorld();
            eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            eglDestroyContext(display, context);
            eglDestroySurface(display, surface);
            eglTerminate(display);
            return 1;
        }
    }
    // 4. Ground every waypoint (COL raycast only, no constant heights).
    bool groundClear = true;
    double zMin = 0.0, zMax = 0.0;
    for (int i = 0; i < waypoints; ++i) {
        DriveWaypoint& w = wps[static_cast<size_t>(i)];
        double h = -50.0;
        char gm[32] = {};
        char gp[8] = {};
        bool hit = DriveSim_Ground(w.x, w.y, h, gm, sizeof(gm), gp, sizeof(gp));
        w.groundH = h;
        (void)std::snprintf(w.groundModel, sizeof(w.groundModel), "%s", gm[0] ? gm : "-");
        (void)std::snprintf(w.groundPrim, sizeof(w.groundPrim), "%s", gp[0] ? gp : "none");
        if (!hit || !std::isfinite(h) || h < -50.0 || h > 500.0) {
            groundClear = false;
        }
        w.carZ = h + meas.clearance;
        if (i == 0) {
            zMin = zMax = w.carZ;
        } else {
            if (w.carZ < zMin) {
                zMin = w.carZ;
            }
            if (w.carZ > zMax) {
                zMax = w.carZ;
            }
        }
        (void)std::printf("drive-wp i=%d x=%.2f y=%.2f ground=%.2f model=%s prim=%s carZ=%.2f "
                           "yaw=%.3f steer=%.3f spinDeg=%.3f spinRad=%.6f dist=%.2f\n",
                           i, w.x, w.y, w.groundH, w.groundModel, w.groundPrim, w.carZ, w.yawPath,
                           w.steerDeg, w.spinDeg, w.spinRad, w.dist);
    }
    (void)std::printf("drive-heights min=%.2f max=%.2f span=%.2f\n", zMin, zMax, zMax - zMin);
    if (!groundClear) {
        (void)std::printf("drive-fail ground miss (need COL hit on every waypoint)\n");
        DriveSim_ShutdownWorld();
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroyContext(display, context);
        eglDestroySurface(display, surface);
        eglTerminate(display);
        return 1;
    }
    // 5. Ride the waypoints: page + pose + merge + chase render, one TGA each.
    std::vector<uint64_t> checksums;
    checksums.reserve(static_cast<size_t>(waypoints));
    TexFrameStats texAgg{};
    int totalFrames = 0;
    bool failed = false;
    for (int i = 0; i < waypoints && !failed; ++i) {
        const DriveWaypoint& w = wps[static_cast<size_t>(i)];
        WorldShotScene worldScene{};
        E2EPagerFrame pf{};
        char pErr[512] = {};
        if (!DriveSim_Page(w.x, w.y, w.carZ, worldScene, pf, pErr, sizeof(pErr))) {
            (void)std::printf("drive-fail pager wp=%d %s\n", i, pErr);
            failed = true;
            break;
        }
        (void)std::printf("drive-pager wp=%d cam=%.2f,%.2f,%.2f yaw=%.3f active=%d loaded=%d "
                           "evicted=%d instances=%d models=%d cached=%d tris=%d fallback=%d\n",
                           i, w.x, w.y, w.carZ, w.yawPath, pf.activeCells, pf.loadedCells,
                           pf.evictedCells, pf.instances, pf.modelsUnique, pf.cacheModels, pf.tris,
                           pf.fallback);
        for (int e = 0; e < pf.evictedShown; ++e) {
            (void)std::printf("drive-evict wp=%d sector=(%d,%d) dist=%d\n", i, pf.evictedCX[e],
                               pf.evictedCY[e], pf.evictedDist[e]);
        }
        WorldShotScene carScene{};
        {
            char cErr[512] = {};
            if (!DriveSim_Car(gameDir.c_str(), model, w.steerDeg, w.spinDeg, carScene, cErr,
                              sizeof(cErr))) {
                (void)std::printf("drive-fail car wp=%d %s\n", i, cErr);
                failed = true;
                break;
            }
        }
        WorldShotScene frame{};
        DriveSim_Merge(worldScene, carScene, w.x, w.y, w.carZ, w.yawBody, frame);
        float eye[3], target[3];
        DriveSim_Chase(w.x, w.y, w.carZ, w.yawPath, kCamD, kCamH, eye, target);
        (void)std::printf("drive-cam wp=%d eye=%.2f,%.2f,%.2f target=%.2f,%.2f,%.2f\n", i, eye[0],
                           eye[1], eye[2], target[0], target[1], target[2]);
        for (int f = 0; f < framesPerLeg; ++f) {
            SDL_Event event = {};
            while (SDL_PollEvent(&event)) {
            }
        }
        totalFrames += framesPerLeg;
        std::vector<uint8> pixels;
        TexFrameStats texStats{};
        TexSample_RenderPath(frame, width, height, eye, target, pixels, texStats);
        texAgg.tris += texStats.tris;
        texAgg.sampledTri += texStats.sampledTri;
        texAgg.fallbackTri += texStats.fallbackTri;
        texAgg.flatTri += texStats.flatTri;
        texAgg.texelFetch += texStats.texelFetch;
        texAgg.texPixels += texStats.texPixels;
        texAgg.fallbackPixels += texStats.fallbackPixels;
        texAgg.flatPixels += texStats.flatPixels;
        uint64_t sumR = 0, sumG = 0, sumB = 0, nonBlack = 0;
        uint64_t checksum = PixelsChecksum(pixels, sumR, sumG, sumB, nonBlack);
        uint64_t total = static_cast<uint64_t>(width) * static_cast<uint64_t>(height);
        if (nonBlack == 0) {
            (void)std::printf("drive-fail black frame wp=%d\n", i);
            failed = true;
            break;
        }
        char outPath[1024];
        (void)std::snprintf(outPath, sizeof(outPath), "%s_W%d.tga", prefix.c_str(), i);
        if (!WriteTga24(outPath, width, height, pixels)) {
            (void)std::printf("drive-fail write '%s'\n", outPath);
            failed = true;
            break;
        }
        checksums.push_back(checksum);
        (void)std::printf("drive-shot wp=%d out=%s carTris=%d worldTris=%d nonblack=%llu/%llu "
                           "checksum=%llu\n",
                           i, outPath, carScene.stats.triangles, pf.tris,
                           static_cast<unsigned long long>(nonBlack),
                           static_cast<unsigned long long>(total),
                           static_cast<unsigned long long>(checksum));
    }
    int sectorsLoaded = 0, sectorsEvicted = 0, modelsPeak = 0, trisPeak = 0;
    StreamPager_Counters(sectorsLoaded, sectorsEvicted, modelsPeak, trisPeak);
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(display, context);
    eglDestroySurface(display, surface);
    eglTerminate(display);
    if (failed) {
        DriveSim_ShutdownWorld();
        return 1;
    }
    // 6. Honest kinematics gates (no separate pictures).
    double distTotal = wps.back().dist;
    double spinTotal = wps.back().spinRad;
    double expectSpin = (meas.wheelR > 0.0) ? distTotal / meas.wheelR : 0.0;
    double spinErr = (expectSpin > 1e-9) ? std::fabs(spinTotal - expectSpin) / expectSpin : 1.0;
    bool gateSpin = spinErr < 0.01;
    bool gateDistinct = true;
    for (size_t i = 0; i < checksums.size() && gateDistinct; ++i) {
        for (size_t j = i + 1; j < checksums.size(); ++j) {
            if (checksums[i] == checksums[j]) {
                gateDistinct = false;
                break;
            }
        }
    }
    double span = zMax - zMin;
    if (!(gateSpin && gateDistinct && groundClear)) {
        (void)std::printf("drive-fail gate spinErr=%.6f(<0.01) distinct=%d groundClear=%d "
                           "spin=%.6f expect=%.6f\n",
                           spinErr, gateDistinct ? 1 : 0, groundClear ? 1 : 0, spinTotal,
                           expectSpin);
        DriveSim_ShutdownWorld();
        return 1;
    }
    std::string cs;
    for (size_t i = 0; i < checksums.size(); ++i) {
        char cell[32];
        (void)std::snprintf(cell, sizeof(cell), "%s%llu", i ? "," : "",
                             static_cast<unsigned long long>(checksums[i]));
        cs += cell;
    }
    OS_DebugOut("mad-sa-linux drive");
    (void)std::printf("drive-ok waypoints=%d frames=%d model=%s distTotal=%.3f wheelSpinTotal=%.6f "
                       "groundClear=OK checksums=%s\n",
                       waypoints, totalFrames, meas.model, distTotal, spinTotal, cs.c_str());
    (void)std::printf("drive-verify distTotal=%.3f wheelR=%.6f expectSpin=%.6f spinTotal=%.6f "
                       "relErr=%.6f span=%.2f maxTurn=%.1f\n",
                       distTotal, meas.wheelR, expectSpin, spinTotal, spinErr, span, maxTurn);
    DriveSim_ShutdownWorld();
    return 0;
}

// Round 12 helper: enumerate the ped IFP bank (names as stored).
int RunListAnims(int argc, char** argv) {
    std::string gameDir = ResolveGameDir(argc, argv);
    std::vector<std::string> names;
    char bankSrc[160] = {};
    char listErr[512] = {};
    if (!IfpAnim_List(gameDir.c_str(), names, bankSrc, sizeof(bankSrc), listErr, sizeof(listErr))) {
        (void)std::printf("animlist-fail %s (game=%s)\n", listErr, gameDir.c_str());
        return 1;
    }
    (void)std::printf("anim-list bank=ped src=%s count=%d\n", bankSrc, static_cast<int>(names.size()));
    for (const auto& n : names) {
        (void)std::printf("anim-name %s\n", n.c_str());
    }
    (void)std::printf("animlist-ok count=%d\n", static_cast<int>(names.size()));
    return 0;
}

// Round 10 (R6h): real radio sound. Decodes the first S seconds (default 5)
// of the station's first track (StrmPaks.dat + TrakLkup.dat cut table,
// XOR de-obfuscation, Vorbis decode — see RadioDecode) to stereo PCM16,
// uploads it to one OpenAL buffer on the null sink (same ALSOFT_DRIVERS
// trick as R4/R6d) with size/frequency/bits/channels verification, and
// reports stream-verified metrics. No synthesis anywhere on this path.
int RunSmokeRadio(int argc, char** argv) {
    const char* stationArg = ArgValue(argc, argv, "--station", "RE");
    int seconds = std::atoi(ArgValue(argc, argv, "--seconds", "5"));
    if (!stationArg || stationArg[0] == '\0' || seconds <= 0) {
        (void)std::printf("radio-fail bad args station='%s' seconds='%s'\n",
                           stationArg ? stationArg : "(null)",
                           ArgValue(argc, argv, "--seconds", "5"));
        return 1;
    }
    std::string gameDir = ResolveGameDir(argc, argv);
    OS_SetFilePathOffset(gameDir.c_str());

    RadioDecodeResult decoded;
    if (!RadioDecode_Station(stationArg, seconds, decoded)) {
        (void)std::printf("radio-fail station=%s reason=%s\n", stationArg,
                           decoded.failReason.c_str());
        return 1;
    }

    bool forcedNull = false;
    if (!std::getenv("ALSOFT_DRIVERS")) {
        (void)setenv("ALSOFT_DRIVERS", "null", 1);
        forcedNull = true;
    }
    const char* backend = forcedNull ? "null" : "default";
    ALCdevice* device = alcOpenDevice(nullptr);
    if (!device) {
        (void)std::printf("radio-fail open device\n");
        return 1;
    }
    ALCcontext* context = alcCreateContext(device, nullptr);
    if (!context || alcMakeContextCurrent(context) == ALC_FALSE) {
        (void)std::printf("radio-fail create context\n");
        if (context) {
            alcDestroyContext(context);
        }
        alcCloseDevice(device);
        return 1;
    }
    ALuint buffer = 0;
    alGenBuffers(1, &buffer);
    if (alGetError() != AL_NO_ERROR || buffer == 0) {
        (void)std::printf("radio-fail gen buffer\n");
        alcMakeContextCurrent(nullptr);
        alcDestroyContext(context);
        alcCloseDevice(device);
        return 1;
    }
    // Game parity: CAEVorbisDecoder always sinks stereo PCM16 (mono is
    // duplicated in FillBuffer); RadioDecode produces stereo accordingly.
    alBufferData(buffer, AL_FORMAT_STEREO16, decoded.pcm.data(),
                 static_cast<ALsizei>(decoded.decodedBytes),
                 static_cast<ALsizei>(decoded.rateHz));
    if (alGetError() != AL_NO_ERROR) {
        (void)std::printf("radio-fail buffer data\n");
        alDeleteBuffers(1, &buffer);
        alcMakeContextCurrent(nullptr);
        alcDestroyContext(context);
        alcCloseDevice(device);
        return 1;
    }
    // Verify the upload stuck: size/frequency/bits/channels round-trip.
    ALint gotSize = 0, gotFreq = 0, gotBits = 0, gotCh = 0;
    alGetBufferi(buffer, AL_SIZE, &gotSize);
    alGetBufferi(buffer, AL_FREQUENCY, &gotFreq);
    alGetBufferi(buffer, AL_BITS, &gotBits);
    alGetBufferi(buffer, AL_CHANNELS, &gotCh);
    if (alGetError() != AL_NO_ERROR ||
        gotSize != (ALint)decoded.decodedBytes ||
        gotFreq != (ALint)decoded.rateHz || gotBits != 16 || gotCh != 2) {
        (void)std::printf("radio-fail buffer verify\n");
        alDeleteBuffers(1, &buffer);
        alcMakeContextCurrent(nullptr);
        alcDestroyContext(context);
        alcCloseDevice(device);
        return 1;
    }
    // Honest non-silence gate: peak must clearly exceed the floor.
    double ratio = decoded.rms > 0.0
                       ? static_cast<double>(decoded.peak) / decoded.rms
                       : 0.0;
    if (!(decoded.decodedBytes > 50000 && decoded.rms > 0.0 &&
          ratio > 1.5)) {
        (void)std::printf("radio-fail silence gate bytes=%llu rms=%.1f peak=%d\n",
                           static_cast<unsigned long long>(decoded.decodedBytes),
                           decoded.rms, decoded.peak);
        alDeleteBuffers(1, &buffer);
        alcMakeContextCurrent(nullptr);
        alcDestroyContext(context);
        alcCloseDevice(device);
        return 1;
    }
    // De-obfuscation/codec layout: exact cut position, codec, rate, beats,
    // and the on-disk vs decrypted head bytes (hex proof in the log).
    (void)std::printf(
        "radio-detail station=%s pack=%d tracks=%d track=%u audioOffset=%u "
        "audioSize=%u codec=vorbis-xor16 rate=%u srcCh=%d outCh=2 "
        "beats=%u firstBeat={time=%u key=%u} raw=%s dec=%s\n",
        decoded.station.c_str(), decoded.packId, decoded.trackCount,
        decoded.trackId, decoded.audioOffset, decoded.audioSize,
        decoded.rateHz, decoded.srcChannels, decoded.beatCount,
        decoded.firstBeatTime, decoded.firstBeatKey,
        decoded.headRawHex.c_str(), decoded.headDecHex.c_str());
    (void)std::printf(
        "radio-ok station=%s seconds=%d decodedBytes=%llu rms=%.1f peak=%d "
        "bufChecksum=%llu backend=%s\n",
        decoded.station.c_str(), decoded.seconds,
        static_cast<unsigned long long>(decoded.decodedBytes), decoded.rms,
        decoded.peak, static_cast<unsigned long long>(decoded.bufChecksum),
        backend);
    alDeleteBuffers(1, &buffer);
    alcMakeContextCurrent(nullptr);
    alcDestroyContext(context);
    alcCloseDevice(device);
    OS_DebugOut("mad-sa-linux real radio audio");
    return 0;
}

// R6g: ground under feet. Probes N fixed world points (IPL-dense downtown
// LA <-> airport corridor) with a vertical raycast against bound COL
// models. Heights come only from COL bytes; a miss reports h=-50.00 with
// prim=none (ray bottom, never NaN). Checksum is FNV-1a over the heights
// quantized to 1e-3, so two clean runs must agree bit for bit.
int RunCollProbe(int argc, char** argv) {
    // Fixed probe points: exact XY of bound text-IPL instances (model names
    // verified against the COL set before the round; see R6g evidence).
    static const double kProbes[][2] = {
        { 1755.60, -1812.30 }, // Roads03_LAn deck (instance origin sits in the
                               // crossroads gap; 30m south along the same deck)
        { 1608.20, -1721.80 }, // GSFreeway7_LAn (downtown freeway)
        { 1544.84, -1516.85 }, // fighotblok1_LAn (downtown block)
        { 1567.60, -1248.70 }, // LAskyscrap1_LAn (downtown tower)
        { 1451.99, -1067.40 }, // towerlan2 (north downtown tower)
        { 1044.91, -2023.39 }, // LAroadsbrk_05_LAs (coast road)
        { 1645.38, -2292.76 }, // lasairprt4 (airport apron)
        { 1474.41, -2286.80 }, // lasairprt5 (airport apron)
        { 1683.22, -2242.96 }, // lasairprterm1_LAS (terminal)
        { 1683.22, -2328.43 }, // lasairprterm2_LAS (terminal)
        { 1036.52, -2204.44 }, // LAroads_05_LAs (west airport road)
        { 2056.88, -2187.35 }, // LAroads_20ghi_LAs (east airport road)
    };
    static const int kProbeTotal = 12;
    int count = std::atoi(ArgValue(argc, argv, "--count", "12"));
    if (count < 1 || count > kProbeTotal) {
        (void)std::printf("coll-fail bad --count '%s' (want 1..%d)\n",
                           ArgValue(argc, argv, "--count", "12"), kProbeTotal);
        return 1;
    }
    std::string gameDir = ResolveGameDir(argc, argv);
    ColLoadStats stats{};
    char err[512] = {};
    if (!ColLoad_Init(gameDir.c_str(), stats, err, sizeof(err))) {
        (void)std::printf("coll-fail load %s (game=%s)\n", err, gameDir.c_str());
        ColLoad_Shutdown();
        return 1;
    }
    (void)std::printf("coll-load files=%d models=%d spheres=%ld boxes=%ld verts=%ld tris=%ld "
                       "instances=%d\n",
                       stats.files, stats.models, stats.spheres, stats.boxes, stats.verts,
                       stats.tris, stats.instances);
    int hits = 0;
    int nan = 0;
    uint64_t checksum = 1469598103934665603ULL;
    for (int i = 0; i < count; ++i) {
        ColProbeHit hit{};
        ColLoad_Probe(kProbes[i][0], kProbes[i][1], hit);
        bool finite = std::isfinite(hit.h);
        bool inRange = finite && hit.h >= -50.0 && hit.h <= 500.0;
        if (!finite) {
            ++nan;
        }
        bool isHit = std::strcmp(hit.prim, "none") != 0;
        if (isHit && inRange) {
            ++hits;
        }
        // FNV-1a over the height quantized to 1e-3 (int64 LE bytes).
        int64_t q = finite ? static_cast<int64_t>(std::llround(hit.h * 1000.0)) : 0;
        if (!finite) {
            q = 0;
        }
        for (int b = 0; b < 8; ++b) {
            checksum ^= static_cast<uint64_t>((q >> (b * 8)) & 0xFF);
            checksum *= 1099511628211ULL;
        }
        (void)std::printf("coll-hit x=%.2f y=%.2f h=%.2f model=%s prim=%s near=%s@%.2f\n",
                           hit.x, hit.y, finite ? hit.h : -50.0, hit.model, hit.prim, hit.near,
                           hit.nearDist);
        if (!inRange) {
            (void)std::printf("coll-fail height out of range probe=%d h=%.2f\n", i, hit.h);
            ColLoad_Shutdown();
            return 1;
        }
    }
    OS_DebugOut("mad-sa-linux collision probe");
    (void)std::printf("coll-ok probes=%d hits=%d nan=%d checksum=%llu\n", count, hits, nan,
                       static_cast<unsigned long long>(checksum));
    ColLoad_Shutdown();
    if (nan != 0) {
        return 1;
    }
    if (hits * 3 < count * 2) {
        (void)std::printf("coll-fail sparse hits=%d probes=%d (need >= 2/3)\n", hits, count);
        return 1;
    }
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
    if (HasArg(argc, argv, "--smoke-audio-real")) {
        return RunSmokeAudioReal(argc, argv);
    }
    if (HasArg(argc, argv, "--smoke-radio")) {
        return RunSmokeRadio(argc, argv);
    }
    if (HasArg(argc, argv, "--smoke-audio")) {
        return RunSmokeAudio();
    }
    if (HasArg(argc, argv, "--headless")) {
        return RunHeadless(argc, argv);
    }
    if (HasArg(argc, argv, "--menu-nav")) {
        return RunMenuNav(argc, argv);
    }
    if (HasArg(argc, argv, "--coll-probe")) {
        return RunCollProbe(argc, argv);
    }
    if (HasArg(argc, argv, "--e2e")) {
        return RunE2E(argc, argv);
    }
    if (HasArg(argc, argv, "--shot-menu")) {
        return RunShotMenu(argc, argv);
    }
    if (HasArg(argc, argv, "--shot-ped")) {
        return RunShotPed(argc, argv);
    }
    if (HasArg(argc, argv, "--shot-anim")) {
        return RunShotAnim(argc, argv);
    }
    if (HasArg(argc, argv, "--anim-seq")) {
        return RunAnimSeq(argc, argv);
    }
    if (HasArg(argc, argv, "--shot-car")) {
        return RunShotCar(argc, argv);
    }
    if (HasArg(argc, argv, "--drive")) {
        return RunDrive(argc, argv);
    }
    if (HasArg(argc, argv, "--list-anims")) {
        return RunListAnims(argc, argv);
    }
    if (HasArg(argc, argv, "--shot-scene")) {
        return RunShotScene(argc, argv);
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
