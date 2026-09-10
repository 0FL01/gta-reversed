// Intercept only the real runtime swap: app-owned GL_BACK, before any overlay.
// No fallback renderer, desktop capture, or substituted script/scene services.
#define GL_GLEXT_PROTOTYPES
#include <SDL3/SDL.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include "app/platform/linux/RealtimeScriptHost.h"
#include "app/platform/linux/RealtimeHud.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace boot_probe {
unsigned frames = 0;
bool passed = true;

static bool Swap(SDL_Window* window, const RealtimeHudState& hud, const RealtimeScriptHost& host,
                 const RealtimeGameplay& game, const WorldShotScene& scene,
                 const RealtimeGameplayWorld& collision) {
    ++frames;
    int width = 0, height = 0;
    GLint framebuffer = -1, packBuffer = -1, readBuffer = -1;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &framebuffer);
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &packBuffer);
    glGetIntegerv(GL_READ_BUFFER, &readBuffer);
    bool ok = SDL_GetWindowSizeInPixels(window, &width, &height) && width > 0 && height > 0 && framebuffer == 0;
    std::vector<unsigned char> rgb;
    if (ok) {
        rgb.resize(static_cast<std::size_t>(width) * height * 3);
        glPushAttrib(GL_PIXEL_MODE_BIT);
        glPushClientAttrib(GL_CLIENT_PIXEL_STORE_BIT);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        glReadBuffer(GL_BACK);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        for (auto name : {GL_PACK_ROW_LENGTH, GL_PACK_SKIP_ROWS, GL_PACK_SKIP_PIXELS,
                         GL_PACK_IMAGE_HEIGHT, GL_PACK_SKIP_IMAGES, GL_PACK_SWAP_BYTES, GL_PACK_LSB_FIRST}) {
            glPixelStorei(name, 0);
        }
        for (auto name : {GL_RED_SCALE, GL_GREEN_SCALE, GL_BLUE_SCALE, GL_ALPHA_SCALE}) {
            glPixelTransferf(name, 1);
        }
        for (auto name : {GL_RED_BIAS, GL_GREEN_BIAS, GL_BLUE_BIAS, GL_ALPHA_BIAS}) {
            glPixelTransferf(name, 0);
        }
        glPixelTransferi(GL_MAP_COLOR, GL_FALSE);
        glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, rgb.data());
        glPopClientAttrib();
        glPopAttrib();
        glBindBuffer(GL_PIXEL_PACK_BUFFER, packBuffer);
        glReadBuffer(readBuffer);
        ok &= glGetError() == GL_NO_ERROR;
    }
    const bool black = !rgb.empty() && std::all_of(rgb.begin(), rgb.end(), [](auto v) { return v == 0; });
    const auto& state = game.State();
    const auto& vm = host.State();
    const auto& publication = host.Publication();
    const auto near = [](float a, float b) { return std::abs(a - b) < 0.0005f; };
    const bool paired = host.World() == &collision && publication.Scene &&
        scene.stats.triangles == publication.Scene->stats.triangles && !scene.meshes.empty() &&
        scene.meshes.front().pos == publication.Scene->meshes.front().pos && collision.TriangleCount() > 0;
    const bool actor = state.Ready && state.MissionCreated && state.PlayerOnFootTask && !state.CarPresent &&
        !state.Ticks && game.Actors().stats.triangles == 2 &&
        near(state.Ped.X, 2488.562255859375f) && near(state.Ped.Y, -1666.864501953125f) &&
        near(state.Ped.Z, 12.8757f) && near(state.PedRoot.Z, 13.8757f) &&
        near(publication.Center.X, state.Ped.X) && near(publication.Center.Y, state.Ped.Y) &&
        near(publication.Center.Z, 13.3757f) && near(state.PedCurrentRotation, 262 * 3.14159265358979323846f / 180) &&
        near(hud.playerX, state.Ped.X) && near(hud.playerY, state.Ped.Y) &&
        host.Events().size() == 7 && host.ResolvePed({1}) == &game && host.ResolveGroup({65536}) &&
        game.Camera().ScriptDirectlyBehind;
    const bool startup = vm.Commands == 53 && vm.IP == 56369 && host.WorldRevision() == 2 &&
        host.Session().Threads().size() == 2 && host.Session().Threads()[1].Commands == 0 &&
        host.Session().Threads()[1].IP == 200000 && vm.TimeMs == 0 &&
        vm.Clock.Hours == 8 && vm.Clock.Minutes == 0 && hud.hour == 8 && hud.minute == 0 &&
        vm.Fade.Direction == 0 && vm.Fade.DurationSeconds == 0 && vm.Fade.Alpha == 255;
    ok &= frames == 1 && black && paired && actor && startup;
    if (const auto* path = std::getenv("MAD_SA_BOOT_CAPTURE"); path && !rgb.empty()) {
        // Optional diagnostic of this very same back buffer, never asset output.
        FILE* file = std::fopen(path, "wb");
        ok &= file != nullptr;
        if (file) {
            ok &= std::fprintf(file, "P6\n%d %d\n255\n", width, height) > 0;
            for (int y = height - 1; y >= 0; --y) {
                ok &= std::fwrite(rgb.data() + static_cast<std::size_t>(y) * width * 3, 3, width, file) == static_cast<std::size_t>(width);
            }
            ok &= std::fclose(file) == 0;
        }
    }
    std::printf("boot-capture %s frame=%u black=%d clock=%02d:%02d paired=%d actor=%d startup=%d drawable=%dx%d\n",
        ok ? "PASS" : "FAIL", frames, black, hud.hour, hud.minute, paired, actor, startup, width, height);
    std::fflush(stdout);
    passed &= ok;
    return SDL_GL_SwapWindow(window) && ok;
}
} // namespace boot_probe

#define SDL_GL_SwapWindow(window) boot_probe::Swap((window), hudState, scriptHost, gameplay, world.active->cpu->Scene, world.active->Collision())
#include "Realtime.cpp"
#undef SDL_GL_SwapWindow

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const char* gameDir = nullptr;
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--game-dir") == 0) gameDir = argv[i + 1];
    }
    if (!gameDir) return 2;
    const int result = Realtime_Run(argc, argv, gameDir);
    const bool ok = result == 1 && boot_probe::passed && boot_probe::frames == 1;
    std::printf("boot-runtime %s exit=%d swaps=%u fullboot=0\n", ok ? "PASS" : "FAIL", result, boot_probe::frames);
    return ok ? result : 2; // runner must validate the exact terminal log, not accept arbitrary exit 1
}
