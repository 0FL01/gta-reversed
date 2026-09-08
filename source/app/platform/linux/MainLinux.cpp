// mad-sa Linux native entry point (R1 skeleton, R2 SDL3 video).
// Standalone `main()` for the `mad-sa-linux` ELF track. It must not depend on
// the Windows DLL/hook model (`dllmain`/`InjectHooks`) nor on Win libraries.
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <SDL3/SDL.h>

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
    (void)std::printf("usage: %s --smoke | --smoke-video\n", prog ? prog : "mad-sa-linux");
}

bool HasArg(int argc, char** argv, const char* want) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], want) == 0) {
            return true;
        }
    }
    return false;
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
    if (HasArg(argc, argv, "--help") || HasArg(argc, argv, "-h")) {
        PrintUsage(argc > 0 ? argv[0] : nullptr);
        return 0;
    }
    PrintUsage(argc > 0 ? argv[0] : nullptr);
    return 1;
}
