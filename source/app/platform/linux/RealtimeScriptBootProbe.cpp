// Intercept only the real runtime swap: app-owned GL_BACK, before any overlay.
// No fallback renderer, desktop capture, or substituted script/scene services.
#define GL_GLEXT_PROTOTYPES
#include <SDL3/SDL.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include "app/platform/linux/RealtimeScriptHost.h"
#include "app/platform/linux/RealtimeHud.h"
#include "app/platform/linux/NativeGaragesRuntime.h"
#include "app/platform/linux/NativeCarGeneratorRuntime.h"
#include "app/platform/linux/RealtimeStreaming.h"
#include "app/platform/linux/IfpAnim.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace boot_probe {
unsigned frames = 0;
bool passed = true;
double previousSimulation = 0;
float previousVerticalSpeed = 0, previousPedZ = 12.8757f;

static bool Swap(SDL_Window* window, const RealtimeHudState& hud, const RealtimeScriptHost& host,
                 const RealtimeGameplay& game, const realtime_streaming::CpuWorld& cpu,
                 const RealtimeGameplayWorld& collision, const NativeGaragesRuntime& runtime,
                 const NativeCarGeneratorRuntime* generators) {
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
    const auto& scene = cpu.Scene;
    const auto& garages = host.Garages();
    const auto& garageFrame = runtime.Frame();
    const auto& view = garageFrame.View;
    const auto& camera = game.Camera();
    const auto near = [](float a, float b) { return std::abs(a - b) < 0.0005f; };
    const auto sameVector = [&](const RealtimeVec3& a, const RealtimeVec3& b) {
        return a.X == b.X && a.Y == b.Y && a.Z == b.Z;
    };
    bool paired = host.World() == &collision && publication.Collision.get() == &collision && publication.Scene &&
        cpu.Generation == 3 && host.WorldRevision() == 3 && cpu.Error.empty() &&
        cpu.BorrowedCollision == publication.Collision && &cpu.QueryWorld() == &collision &&
        cpu.Collision.TriangleCount() == 0 && cpu.Collision.SphereCount() == 0 && cpu.Collision.BoxCount() == 0 &&
        cpu.SourceCollision && cpu.SourceCollision == publication.SourceCollision &&
        cpu.Overrides && cpu.Overrides == publication.Overrides && cpu.Overrides == host.InitialPlacementOverrides() &&
        cpu.SourceCollision->Overrides == cpu.Overrides &&
        scene.stats.triangles == publication.Scene->stats.triangles && !scene.meshes.empty() &&
        scene.meshes.size() == publication.Scene->meshes.size() && collision.TriangleCount() > 0;
    if (paired) {
        for (std::size_t i = 0; i < scene.meshes.size(); ++i) {
            paired &= scene.meshes[i].pos == publication.Scene->meshes[i].pos;
        }
    }
    std::size_t disabled = 0;
    if (cpu.Overrides) for (const auto& entry : cpu.Overrides->Entries()) if (!entry.CollisionEnabled) {
        ++disabled;
        if (cpu.SourceCollision) paired &= std::none_of(cpu.SourceCollision->Instances.begin(), cpu.SourceCollision->Instances.end(),
            [&](const auto& instance) { return entry.Identity.Matches(instance.Placement); });
    }
    paired &= disabled == 13 && cpu.Overrides && cpu.Overrides->Entries().size() == 14;
    const bool actor = state.Ready && state.MissionCreated && state.PlayerOnFootTask && !state.CarPresent &&
        !state.InVehicle && game.Actors().stats.triangles == 2 && std::strcmp(game.PlayerModelStats().model, "player") == 0 &&
        near(state.Ped.X, 2488.562255859375f) && near(state.Ped.Y, -1666.864501953125f) &&
        near(state.PedRoot.X, state.Ped.X) && near(state.PedRoot.Y, state.Ped.Y) && near(state.PedRoot.Z, state.Ped.Z + 1) &&
        near(publication.Center.X, state.Ped.X) && near(publication.Center.Y, state.Ped.Y) &&
        near(publication.Center.Z, 13.3757f) && near(state.PedCurrentRotation, 262 * 3.14159265358979323846f / 180) &&
        near(hud.playerX, state.Ped.X) && near(hud.playerY, state.Ped.Y) &&
        host.ResolvePed({1}) == &game && host.ResolveGroup({65536}) &&
        game.Camera().ScriptDirectlyBehind;
    const auto threads = host.Session().Threads();
    const auto missionCommands = threads.size() == 2 ? threads[1].Commands : ~std::uint64_t{};
    const auto missionIP = threads.size() == 2 ? threads[1].IP : 0;
    // Measured in cargens-boot-measured.log with the actual production quota.
    constexpr std::array<std::uint32_t, 5> expectedIPs{200000, 202662, 205508, 207969, 210403};
    const bool scheduler = frames <= 5 && threads.size() == 2 && vm.Commands == 53 && vm.IP == 56369 &&
        threads[0].Commands == 53 && threads[0].IP == 56369 && threads[0].Waiting && threads[0].Active &&
        threads[0].LastOpcode == 0x0001 && threads[1].Generation == 1 && threads[1].Active && !threads[1].Waiting &&
        threads[1].MissionIndex == 0 && missionCommands == (frames - 1) * 256 &&
        missionIP == expectedIPs[frames - 1];
    const bool presentation =
        vm.Clock.Hours == 8 && vm.Clock.Minutes == 0 && hud.hour == 8 && hud.minute == 0 &&
        vm.Fade.Direction == 0 && vm.Fade.DurationSeconds == 0 && vm.Fade.Alpha == 255;
    const bool startup = frames != 1 || (scheduler && !state.Ticks && vm.TimeMs == 0 &&
        near(state.Ped.Z, 12.8757f) && near(state.PedRoot.Z, 13.8757f) && host.Events().size() == 7 &&
        sameVector(camera.Position, host.Events()[5].Camera.Position) && camera.Yaw == host.Events()[5].Camera.Yaw);
    const auto& baseline = garageFrame.BaselineCamera;
    const bool cameraUnchanged = sameVector(camera.Position, baseline.Position) && sameVector(camera.Target, baseline.Target) &&
        camera.Yaw == baseline.Yaw && camera.Pitch == baseline.Pitch && camera.ScriptDirectlyBehind == baseline.ScriptDirectlyBehind &&
        camera.ScriptPedOrientation == baseline.ScriptPedOrientation && garageFrame.Camera.Apply && !garageFrame.Camera.Outside &&
        !garageFrame.Camera.Garage && !garageFrame.Camera.Previous && !garageFrame.Camera.AvoidFirstPerson;
    const float angle = state.PedHeading - 3.14159265358979323846f * .5f;
    const bool sourceBody = view.Player.Collision && view.Player.Collision == garages.Ped1Collision() &&
        view.Player.ModelId == 0 && !view.Vehicle && !view.Replay && !view.Coop &&
        view.Player.Matrix.Position == NativeCollisionVector{state.PedRoot.X, state.PedRoot.Y, state.PedRoot.Z} &&
        view.Player.Matrix.Basis == std::array<NativeCollisionVector, 3>{{{std::cos(angle), std::sin(angle), 0},
            {-std::sin(angle), std::cos(angle), 0}, {0, 0, 1}}} &&
        view.Camera == NativeCollisionVector{camera.Position.X, camera.Position.Y, camera.Position.Z} &&
        view.PublishedOverrides == cpu.Overrides;
    const auto clearFlags = std::count_if(garages.Entries().begin(), garages.Entries().end(), [](const auto& g) { return !(g.Flags & 0x40); });
    bool garageReady = garageFrame.Revision == frames && view.Frame == frames - 1 && garageFrame.Generation == cpu.Generation &&
        garageFrame.Barrier == NativeGaragesRuntimeBarrier::None && garageFrame.Requirement == NativeGarageRequirement::None &&
        !garageFrame.Garage && !garageFrame.UnsupportedUpdates && garages.Frame().Updates.size() == 50 &&
        clearFlags == 13 && garages.Revision() == (frames == 1 ? 0 : frames == 2 ? 12 : 13);
    for (const auto& update : garages.Frame().Updates) garageReady &= update.Status == NativeScriptServiceStatus::Ready &&
        update.Requirement == NativeGarageRequirement::None;
    for (const auto& door : garages.Doors()) if (door.Garage) {
        const auto* entry = garages.Resolve(*door.Garage);
        garageReady &= entry && door.CollisionEnabled == bool(entry->Flags & 0x40) &&
            NativeGarages::DoorPublished(*entry, door, cpu.Overrides.get(), entry->Flags);
    }
    // Observe real production physics: later frames may fall/land, never demand the authored startup Z forever.
    float ground = 0;
    NativeCollisionHit groundSource;
    const bool groundFound = collision.Ground(state.Ped.X, state.Ped.Y, 14, -100, ground, &groundSource);
    const bool physics = frames == 1 ? state.SimulatedSeconds == 0 && !state.Grounded :
        state.Ticks == frames - 1 && state.SimulatedSeconds > previousSimulation &&
        groundFound && groundSource.ModelId >= 0 && !groundSource.Library.empty() && near(ground, 12.34375f) &&
        std::isfinite(state.VerticalSpeed) && state.Ped.Z <= 12.8757f && state.Ped.Z >= ground - .0005f &&
        (state.Grounded ? near(state.Ped.Z, ground) && near(state.VerticalSpeed, 0) :
            state.Ped.Z < previousPedZ && near(state.VerticalSpeed,
                previousVerticalSpeed - 9.81f * static_cast<float>(state.SimulatedSeconds - previousSimulation)));
    if (!physics) std::printf("boot-physics diagnostic simulated=%.9f previous=%.9f groundFound=%d ground=%.7f model=%d library=%s vertical=%.7f\n",
        state.SimulatedSeconds, previousSimulation, groundFound, ground, groundSource.ModelId,
        groundSource.Library.c_str(), state.VerticalSpeed);
    previousSimulation = state.SimulatedSeconds;
    previousPedZ = state.Ped.Z;
    previousVerticalSpeed = state.VerticalSpeed;
    const auto& registry = host.CarGenerators();
    const auto& residency = host.CarGeneratorResidency();
    const auto rng = host.InspectSourceRng();
    const auto pool = host.Vehicles().Census();
    const auto creates = std::count_if(registry.Events().begin(), registry.Events().end(), [](const auto& event) {
        return event.Kind == NativeCarGeneratorEventKind::Create014B;
    });
    const auto switches = registry.Events().size() - creates;
    constexpr std::array<std::ptrdiff_t, 5> expectedCreates{0, 0, 0, 9, 10};
    bool generatorReady = generators && residency.Generation() == cpu.Generation && residency.Snapshot() == cpu.SourceCollision &&
        residency.Active().size() == 22 && registry.Census().Registered == 88 + static_cast<std::size_t>(creates) &&
        frames <= expectedCreates.size() && creates == expectedCreates[frames - 1] && switches == static_cast<std::size_t>(creates) &&
        !pool.Alive && !pool.CreatedEvents && rng.Status == NativeSourceRngStatus::Ready && rng.Value &&
        rng.Value->DrawCount == 0 && rng.Value->State == rng.Value->Seed;
    if (generators) {
        const auto& frame = generators->Frame();
        std::size_t quarterUsed = 0;
        for (std::size_t slot = frames % 4; slot < NativeCarGenerators::Capacity; slot += 4) {
            quarterUsed += registry.Entries()[slot].Used;
        }
        generatorReady &= frame.Revision == frames && frame.Frame == frames - 1 && frame.WorldGeneration == cpu.Generation &&
            frame.RegistryRevision == registry.Revision() && frame.Demands.empty() &&
            frame.ActivityRevision == game.Activity().Revision &&
            frame.Process.Result.Status == NativeScriptServiceStatus::Ready && frame.Process.ProcessCounterBefore == (frames - 1) % 4 &&
            frame.Process.ProcessCounterAfter == frames % 4 && frame.Process.Visited == quarterUsed;
        for (const auto& action : frame.Process.Actions) generatorReady &= action.Requirement == NativeCarGeneratorRequirement::None &&
            action.Result.Status == NativeScriptServiceStatus::Ready;
    }
    std::printf("boot-cargens %s frame=%u quarter=%u sources=%zu registered=%zu creates=%td switches=%zu demands=%zu poolCreated=%zu seed=%u draws=%llu borrowed=%d\n",
        generatorReady ? "PASS" : "FAIL", frames, generators ? generators->Frame().Process.ProcessCounterAfter : 255,
        residency.Active().size(), registry.Census().Registered, creates, switches,
        generators ? generators->Frame().Demands.size() : ~std::size_t{}, pool.CreatedEvents,
        rng.Value ? rng.Value->Seed : 0, static_cast<unsigned long long>(rng.Value ? rng.Value->DrawCount : ~std::uint64_t{}),
        &cpu.QueryWorld() == host.World());
    ok &= generatorReady;
    ok &= black && paired && actor && scheduler && presentation && startup && garageReady && cameraUnchanged && sourceBody && physics;
    if (const auto* path = std::getenv("MAD_SA_BOOT_CAPTURE"); path && !rgb.empty()) {
        // Optional diagnostic of this very same back buffer, never asset output.
        const auto capture = std::string(path) + ".frame-" + std::to_string(frames) + ".ppm";
        FILE* file = std::fopen(capture.c_str(), "wb");
        ok &= file != nullptr;
        if (file) {
            ok &= std::fprintf(file, "P6\n%d %d\n255\n", width, height) > 0;
            for (int y = height - 1; y >= 0; --y) {
                ok &= std::fwrite(rgb.data() + static_cast<std::size_t>(y) * width * 3, 3, width, file) == static_cast<std::size_t>(width);
            }
            ok &= std::fclose(file) == 0;
        }
    }
    std::printf("boot-capture %s frame=%u black=%d clock=%02d:%02d paired=%d actor=%d startup=%d scheduler=%d mission=%llu ip=%u "
        "worldRevision=%llu sourceCOL=%d overrides=%zu disabled=%zu garageReady=%d updates=%zu flagsCleared=%td garageRevision=%llu "
        "cameraUnchanged=%d sourceBody=%d physics=%d ticks=%llu pedZ=%.7f rootZ=%.7f grounded=%d drawable=%dx%d\n",
        ok ? "PASS" : "FAIL", frames, black, hud.hour, hud.minute, paired, actor, startup, scheduler,
        static_cast<unsigned long long>(missionCommands), missionIP, static_cast<unsigned long long>(host.WorldRevision()),
        bool(cpu.SourceCollision), cpu.Overrides ? cpu.Overrides->Entries().size() : 0, disabled, garageReady, garages.Frame().Updates.size(), clearFlags,
        static_cast<unsigned long long>(garages.Revision()), cameraUnchanged, sourceBody, physics,
        static_cast<unsigned long long>(state.Ticks), state.Ped.Z, state.PedRoot.Z, state.Grounded, width, height);
    std::fflush(stdout);
    passed &= ok;
    return SDL_GL_SwapWindow(window) && ok;
}
} // namespace boot_probe

#define SDL_GL_SwapWindow(window) boot_probe::Swap((window), hudState, scriptHost, gameplay, *world.active->cpu, world.active->Collision(), garageRuntime, carGenerators.get())
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
    const bool ok = result == 1 && boot_probe::passed && boot_probe::frames == 5;
    std::printf("boot-runtime %s exit=%d swaps=%u fullboot=0\n", ok ? "PASS" : "FAIL", result, boot_probe::frames);
    return ok ? result : 2; // runner must validate the exact terminal log, not accept arbitrary exit 1
}
