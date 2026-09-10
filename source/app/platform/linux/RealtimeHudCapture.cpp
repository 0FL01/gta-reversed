// Test-only interception of production presentation. No alternate renderer.
// Build/run with RealtimeHudCapture.py; captures only this app's GL_BACK.
#define GL_GLEXT_PROTOTYPES
#include <SDL3/SDL.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <array>
#include <cstdio>
#include <vector>
#include "app/platform/linux/RealtimeGameplay.h"
#include "app/platform/linux/RealtimeHud.h"
#include "app/platform/linux/RealtimeEnvironment.h"

namespace hud_capture {
struct ReadState {
    std::array<GLint, 11> integers{};
    std::array<GLfloat, 8> transfer{};
    ReadState() {
        constexpr GLenum names[]{GL_READ_BUFFER, GL_PIXEL_PACK_BUFFER_BINDING,
            GL_PACK_ALIGNMENT, GL_PACK_ROW_LENGTH, GL_PACK_SKIP_ROWS, GL_PACK_SKIP_PIXELS,
            GL_PACK_SWAP_BYTES, GL_PACK_LSB_FIRST, GL_PACK_IMAGE_HEIGHT, GL_PACK_SKIP_IMAGES,
            GL_MAP_COLOR};
        constexpr GLenum floats[]{GL_RED_SCALE, GL_GREEN_SCALE, GL_BLUE_SCALE, GL_ALPHA_SCALE,
            GL_RED_BIAS, GL_GREEN_BIAS, GL_BLUE_BIAS, GL_ALPHA_BIAS};
        for (unsigned i = 0; i < integers.size(); ++i) {
            glGetIntegerv(names[i], &integers[i]);
        }
        for (unsigned i = 0; i < transfer.size(); ++i) {
            glGetFloatv(floats[i], &transfer[i]);
        }
    }
};

static bool Swap(SDL_Window* window, const RealtimeHudView& view,
                 const RealtimeHudState& hud, const RealtimeGameplayState& state, const RealtimeWaterState& water) {
    static unsigned frame = 0;
    static Uint64 start = SDL_GetTicksNS();
    static unsigned timed = 0;
    constexpr double times[]{5.0, 12.0};
    const double elapsed = static_cast<double>(SDL_GetTicksNS() - start) / 1e9;
    bool capture = ++frame == 1 || frame == 300 || frame == 700;
    if (timed < std::size(times) && elapsed >= times[timed]) {
        ++timed;
        capture = true;
    }
    bool ok = true;
    if (capture) {
        int width = 0, height = 0;
        GLint framebuffer = -1;
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &framebuffer);
        ok = SDL_GetWindowSizeInPixels(window, &width, &height) && width > 0 && height > 0 && framebuffer == 0;
        if (ok) {
            std::vector<unsigned char> rgb(static_cast<size_t>(width) * height * 3);
            const ReadState before;
            glPushAttrib(GL_PIXEL_MODE_BIT);
            glPushClientAttrib(GL_CLIENT_PIXEL_STORE_BIT);
            glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
            glReadBuffer(GL_BACK);
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glPixelStorei(GL_PACK_ROW_LENGTH, 0);
            glPixelStorei(GL_PACK_SKIP_ROWS, 0);
            glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
            glPixelStorei(GL_PACK_SWAP_BYTES, GL_FALSE);
            glPixelStorei(GL_PACK_LSB_FIRST, GL_FALSE);
            glPixelStorei(GL_PACK_IMAGE_HEIGHT, 0);
            glPixelStorei(GL_PACK_SKIP_IMAGES, 0);
            for (auto name : {GL_RED_SCALE, GL_GREEN_SCALE, GL_BLUE_SCALE, GL_ALPHA_SCALE}) {
                glPixelTransferf(name, 1.0f);
            }
            for (auto name : {GL_RED_BIAS, GL_GREEN_BIAS, GL_BLUE_BIAS, GL_ALPHA_BIAS}) {
                glPixelTransferf(name, 0.0f);
            }
            glPixelTransferi(GL_MAP_COLOR, GL_FALSE);
            glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, rgb.data());
            glPopClientAttrib();
            glPopAttrib();
            glBindBuffer(GL_PIXEL_PACK_BUFFER, before.integers[1]);
            glReadBuffer(before.integers[0]);
            const ReadState after;
            const auto error = glGetError();
            ok = error == GL_NO_ERROR && before.integers == after.integers && before.transfer == after.transfer;
            char path[256];
            std::snprintf(path, sizeof(path), "artifacts/graphics/realtime-%s-integrated-%04u.ppm",
                view.radar ? "hud" : "water", frame);
            if (ok) {
                FILE* file = std::fopen(path, "wb");
                ok = file != nullptr;
                if (file) {
                    ok = std::fprintf(file, "P6\n%d %d\n255\n", width, height) > 0;
                    for (int y = height - 1; y >= 0; --y) {
                        ok &= std::fwrite(rgb.data() + static_cast<size_t>(y) * width * 3, 3, width, file) == static_cast<size_t>(width);
                    }
                    ok &= std::fclose(file) == 0;
                }
            }
            std::printf("hud-capture %s frame=%u elapsed=%.3f simulated=%.3f drawable=%dx%d "
                "cameraYaw=%.6f player=%.3f,%.3f heading=%.6f clock=%02d:%02d radar=%d "
                "mode=%s jumps=%llu landings=%llu entries=%llu exits=%llu GL=0x%x restored=%d path=%s\n",
                ok ? "PASS" : "FAIL", frame, elapsed, state.SimulatedSeconds, width, height,
                view.cameraYaw, hud.playerX, hud.playerY, hud.playerYaw, hud.hour, hud.minute, view.radar,
                state.InVehicle ? "car" : "foot", static_cast<unsigned long long>(state.Jumps),
                static_cast<unsigned long long>(state.Landings), static_cast<unsigned long long>(state.Entries),
                static_cast<unsigned long long>(state.Exits), error,
                before.integers == after.integers && before.transfer == after.transfer, path);
            std::fflush(stdout);
            std::printf("water-capture clockMs=%u wavyness=%.6f\n", water.gameMs, water.wavyness);
        } else {
            std::printf("hud-capture FAIL invalid drawable/default framebuffer=%d\n", framebuffer);
        }
    }
    // MangoHud draws inside the real swap; the preceding readback excludes it.
    const bool swapped = SDL_GL_SwapWindow(window);
    return swapped && ok;
}
} // namespace hud_capture

// SDL headers are already included. Only the production call site is replaced;
// HUD inputs are the exact locals submitted to Draw, with no replay injection.
#define SDL_GL_SwapWindow(window) hud_capture::Swap((window), hudView, hudState, gameplay.State(), environment.GetWaterState())
#include "Realtime.cpp"
#undef SDL_GL_SwapWindow
