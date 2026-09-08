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

#include <sys/resource.h>

namespace {
void PrintUsage(const char* prog) {
    (void)std::printf(
        "usage: %s --smoke | --smoke-video | --smoke-audio | --smoke-audio-real [--bank NAME] [--samples K] | --headless [--ticks N] | --shot <out.tga> [--frames N] | --shot-scene <out.tga> [--frames N] [--cam x,y,z] | --e2e [--path Ax,Ay,Az:Bx,By,Bz] [--waypoints W] [--frames-per-leg F] [--out prefix]\n",
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
    std::vector<uint8> pixels;
    TexFrameStats texStats{};
    DrawWorldFrame(scene, width, height, 60.0f, camOverride, pixels, texStats);
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
    if (HasArg(argc, argv, "--smoke-audio")) {
        return RunSmokeAudio();
    }
    if (HasArg(argc, argv, "--headless")) {
        return RunHeadless(argc, argv);
    }
    if (HasArg(argc, argv, "--e2e")) {
        return RunE2E(argc, argv);
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
