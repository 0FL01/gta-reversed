// Joint integration probe for the owned controller, save pickup, SCM ring and
// SDL feedback path. The pickup request is a typed test fixture at the actual
// 0053 player root; it is not a claim that the original PSAVE frontend ran.
#define GL_GLEXT_PROTOTYPES 1
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <SDL3/SDL.h>

#include "app/platform/linux/IfpAnim.h"
#include "app/platform/linux/NativePadFeedback.h"
#include "app/platform/linux/RealtimeHud.h"
#include "app/platform/linux/RealtimeScriptHost.h"
#include "app/platform/linux/StreamPager.h"

#include <rw.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using Bytes = std::vector<std::uint8_t>;
int g_Failures;
std::uint64_t g_Now;

void Check(bool condition, const char* message) {
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", message);
    g_Failures += !condition;
}

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

constexpr std::size_t Primary(NativePlayerPrimarySlot slot) {
    return static_cast<std::size_t>(slot);
}

void Put(Bytes& bytes, std::uint32_t value, unsigned count) {
    for (unsigned i = 0; i < count; ++i) bytes.push_back(std::uint8_t(value >> (8 * i)));
}

void Patch(Bytes& bytes, std::size_t offset, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bytes.at(offset + i) = std::uint8_t(value >> (8 * i));
}

void Op(Bytes& bytes, std::uint16_t opcode) { Put(bytes, opcode, 2); }
void I32(Bytes& bytes, std::int32_t value) { bytes.push_back(1); Put(bytes, std::uint32_t(value), 4); }

Bytes Fixture(const Bytes& code) {
    Bytes bytes;
    const auto chunk = [&](std::uint8_t index, const Bytes& payload) {
        const auto next = std::uint32_t(bytes.size() + payload.size() + 8);
        Op(bytes, 2); I32(bytes, std::int32_t(next)); bytes.push_back(index);
        bytes.insert(bytes.end(), payload.begin(), payload.end());
    };
    chunk(115, Bytes(16));
    chunk(0, Bytes(4));
    Bytes info(16); Patch(info, 0, 104 + std::uint32_t(code.size())); chunk(1, info);
    chunk(2, Bytes(8));
    chunk(3, Bytes(4));
    Bytes extra(8); Patch(extra, 0, 16); chunk(4, extra);
    Require(bytes.size() == 104, "generated pickup feedback SCM header size");
    bytes.insert(bytes.end(), code.begin(), code.end());
    return bytes;
}

void LoadFixture(NativeScriptSession& session, NativeScriptServices& services, const Bytes& code) {
    const auto fixture = Fixture(code);
    std::string error;
    Require(session.LoadMainBytes(fixture, fixture.size(), error), error);
    const auto headers = session.Run(services, 6);
    Require(headers.Status == NativeScriptStatus::BudgetYield && headers.Executed == 6 && session.State().IP == 104,
        "generated pickup feedback SCM header traversal");
}

struct EglContext {
    EGLDisplay Display = EGL_NO_DISPLAY;
    EGLSurface Surface = EGL_NO_SURFACE;
    EGLContext Context = EGL_NO_CONTEXT;

    EglContext() {
        const auto getDisplay = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(
            eglGetProcAddress("eglGetPlatformDisplayEXT"));
        Require(getDisplay, "surfaceless EGL entry point");
        Display = getDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
        Require(Display != EGL_NO_DISPLAY && eglInitialize(Display, nullptr, nullptr) && eglBindAPI(EGL_OPENGL_API),
            "surfaceless EGL initialization");
        const EGLint attributes[]{EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
            EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
        EGLConfig config{}; EGLint count{};
        Require(eglChooseConfig(Display, attributes, &config, 1, &count) && count,
            "pickup feedback EGL config");
        const EGLint size[]{EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
        Surface = eglCreatePbufferSurface(Display, config, size);
        Context = eglCreateContext(Display, config, EGL_NO_CONTEXT, nullptr);
        Require(Surface != EGL_NO_SURFACE && Context != EGL_NO_CONTEXT &&
            eglMakeCurrent(Display, Surface, Surface, Context), "pickup feedback EGL context");
    }

    bool Current() const {
        return eglGetCurrentDisplay() == Display && eglGetCurrentSurface(EGL_DRAW) == Surface &&
            eglGetCurrentSurface(EGL_READ) == Surface && eglGetCurrentContext() == Context;
    }

    ~EglContext() {
        if (Display == EGL_NO_DISPLAY) return;
        eglMakeCurrent(Display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (Context != EGL_NO_CONTEXT) eglDestroyContext(Display, Context);
        if (Surface != EGL_NO_SURFACE) eglDestroySurface(Display, Surface);
        eglTerminate(Display);
    }
};

struct RumbleCapture {
    std::vector<std::pair<Uint16, Uint16>> Calls;
};

bool SDLCALL CaptureRumble(void* userdata, Uint16 low, Uint16 high) {
    static_cast<RumbleCapture*>(userdata)->Calls.emplace_back(low, high);
    return true;
}

SDL_JoystickID AttachVirtual(const char* name, RumbleCapture& capture, bool rumble) {
    SDL_VirtualJoystickDesc description;
    SDL_INIT_INTERFACE(&description);
    description.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    description.naxes = SDL_GAMEPAD_AXIS_COUNT;
    description.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    description.name = name;
    description.userdata = &capture;
    description.Rumble = rumble ? CaptureRumble : nullptr;
    const auto id = SDL_AttachVirtualJoystick(&description);
    Require(id != 0, std::string{"attach virtual gamepad: "} + SDL_GetError());
    return id;
}

SDL_Event WaitDeviceEvent(SDL_EventType type, SDL_JoystickID id) {
    const auto deadline = SDL_GetTicksNS() + 200'000'000;
    do {
        SDL_Event event{};
        while (SDL_PollEvent(&event)) {
            if (event.type == type && event.gdevice.which == id) return event;
        }
        SDL_Delay(1);
    } while (SDL_GetTicksNS() < deadline);
    throw std::runtime_error("SDL did not publish the typed virtual-gamepad event");
}

std::uint64_t FakeClock() {
    return g_Now;
}

double SourceLandingDuration(const char* gameDir) {
    std::vector<IfpAnimSeqFrame> sequence;
    char error[512]{};
    auto* const dictionary = rw::TexDictionary::getCurrent();
    const bool loaded = IfpAnim_Seq(gameDir, "player", "FALL_land", 2, sequence, error, sizeof(error));
    IfpAnim_Shutdown();
    rw::TexDictionary::setCurrent(dictionary);
    Require(loaded, error);
    Require(!sequence.empty() && sequence.front().stats.bones == 32 && sequence.front().stats.mapped == 26,
        "FALL_land comes from mapped ped.ifp data");
    return sequence.front().stats.animTotal;
}

struct FeedbackDrain {
    std::vector<NativeScriptPadShakeEvent> Events;
    std::vector<NativePadFeedbackResult> Results;
};

FeedbackDrain ConsumeAndSubmit(NativeScriptEntities& entities, NativePadFeedback& feedback) {
    FeedbackDrain drained;
    while (const auto shake = entities.ConsumePadShake()) {
        drained.Events.push_back(*shake);
        drained.Results.push_back(feedback.Submit(*shake, true));
    }
    return drained;
}

struct JointFixture {
    RealtimeGameplay& Gameplay;
    RealtimeScriptHost& Host;
    NativeScriptEntities& Entities;
    std::uint32_t Frame = 0;
    std::uint32_t GameMs = 0;
    std::uint64_t Instruction = 1;
    std::string Error;

    NativeScriptPickupRef Create(std::uint64_t session) {
        const auto root = Gameplay.State().PedRoot;
        const NativeScriptPosition position{root.X, root.Y, root.Z};
        NativeScriptPickupRequest request{{session, Instruction++, 900000}, 1277, 3, position};
        const auto created = Host.CreatePickup(request);
        Require(created.Result.Status == NativeScriptServiceStatus::Ready && created.Reference.Value != -1,
            created.Result.Message);
        const auto* pickup = Entities.ResolvePickup(created.Reference);
        const auto& model = Entities.PreparedSaveModel();
        Require(pickup && pickup->Model == 1277 && pickup->Type == 3 && pickup->Actor.stats.triangles > 0 &&
            std::string(model.stats.dffName) == "pickupsave.dff" && std::string(model.stats.txdName) == "icons4.txd",
            "typed fixture allocated the actual pickupsave DFF/TXD");
        return created.Reference;
    }

    bool Publish(NativeScriptPickupRef pickup, NativeScriptPropertyInput input = {}) {
        const auto slot = std::uint32_t(pickup.Value) & 0xffff;
        unsigned partition = 0;
        while (!(slot >= 620 * partition / 6 && slot < 620 * (partition + 1) / 6)) ++partition;
        do { ++Frame; } while (Frame % 6 != partition);
        input.FrameCounter = Frame;
        const auto camera = Gameplay.Camera().Position;
        if (!Host.TickPlayerEntities({camera.X, camera.Y, camera.Z}, input, Error)) return false;
        GameMs += 16;
        return Entities.AdvanceTime(GameMs, Error);
    }
};
} // namespace

int main(int argc, char** argv) try {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const char* gameDir = argc > 1 ? argv[1] : "/game";
    Require(SDL_InitSubSystem(SDL_INIT_VIDEO), std::string{"SDL video init: "} + SDL_GetError());
    Require((SDL_WasInit(SDL_INIT_VIDEO) & SDL_INIT_VIDEO) != 0, "probe owns an SDL video reference");

    double landingSeconds = 0;
    double sourceLandingSeconds = 0;
    std::uint64_t spawnRevision = 0, airRevision = 0, landingRevision = 0, groundRevision = 0;
    std::int32_t supportedReference = -1;
    NativeScriptPosition fixturePosition;
    {
        EglContext egl;
        E2ELoadInfo info{}; char streamError[512]{};
        Require(StreamPager_Init(gameDir, info, streamError, sizeof(streamError),
            {.includeStreamed=true, .radius=300, .maxInstances=1200}), streamError);
        {
            RealtimeGameplay gameplay;
            RealtimeScriptHost host(gameplay);
            std::string error;
            Require(host.InitializeBeforeWorker(gameDir, error), error);
            RealtimeHud hud;
            char hudError[512]{};
            Require(hud.Load(gameDir, hudError, sizeof(hudError)), hudError);
            Require(hud.PreparedRadarSprite(33) && !hud.IsRadarSpriteUploaded(33),
                "real radar_race sprite33 is CPU prepared before GL readiness");
            Require(hud.Upload(hudError, sizeof(hudError)) && hud.IsRadarSpriteUploaded(33), hudError);
            host.SetRadarSpriteReady([&](std::int32_t sprite) { return hud.IsRadarSpriteUploaded(sprite); });

            const auto first = host.RunPass(256);
            Require(first.Status == NativeScriptStatus::Waiting && first.Executed == 53 && host.State().IP == 56369,
                first.Message);
            Require(host.PrepareInitialGarageWorldBeforeWorker(error), error);
            Require(host.AdvanceTime(0, error), error);
            host.SealStartup();
            const auto terminal = host.RunPass(10000);
            const auto& mission = host.Session().Threads()[1];
            Check(terminal.Status == NativeScriptStatus::Unsupported && terminal.Executed == 1219 &&
                mission.Commands == 1219 && terminal.Opcode == 0x016C && terminal.IP == 212309 &&
                mission.LastOpcode == 0x02B9 && mission.LastInstructionIP == 212298 && host.WorldRevision() == 3,
                "actual HUD-ready SCM remains strict 1219 commands and 016C@212309");
            Require(host.World() && gameplay.State().Ready &&
                gameplay.Activity().Authority == NativePlayerActivityAuthority::SourceBacked,
                "actual host owns a ready world and controller activity");
            sourceLandingSeconds = SourceLandingDuration(gameDir);
            Check(std::abs(sourceLandingSeconds - 14.0 / 30.0) < 0.0001,
                "installed FALL_land duration is the source 14/30 second clock");

            auto& entities = host.Entities();
            JointFixture joint{gameplay, host, entities, 0, 0, 1, {}};
            const auto initialRoot = gameplay.State().PedRoot;
            fixturePosition = {initialRoot.X, initialRoot.Y, initialRoot.Z};
            spawnRevision = gameplay.Activity().Revision;
            g_Now = 0;

            {
                NativePadFeedback noDevice({.VirtualDevicesOnly = true, .MonotonicMilliseconds = FakeClock});
                const auto initialized = noDevice.Initialize();
                Check(initialized.Status == NativePadFeedbackStatus::NoDevice &&
                    noDevice.GetState().Devices == 0, "virtual-only no-device startup is explicit and normal");
                const auto pickup = joint.Create(9700);
                const auto before = entities.Revision();
                Require(joint.Publish(pickup), joint.Error);
                const auto drained = ConsumeAndSubmit(entities, noDevice);
                Check(!entities.ResolvePickup(pickup) && entities.Revision() == before + 1 &&
                    drained.Events.size() == 1 && drained.Results.size() == 1 &&
                    drained.Results[0].Status == NativePadFeedbackStatus::NoDevice,
                    "no-device feedback does not turn normal collection into failure");
            }
            Check((SDL_WasInit(SDL_INIT_VIDEO) & SDL_INIT_VIDEO) != 0 &&
                !(SDL_WasInit(SDL_INIT_GAMEPAD) & SDL_INIT_GAMEPAD) && egl.Current(),
                "owned no-device backend shutdown preserves SDL video and the current GL context");

            NativePadFeedback feedback({.VirtualDevicesOnly = true, .MonotonicMilliseconds = FakeClock});
            Require(feedback.Initialize().Status == NativePadFeedbackStatus::NoDevice,
                "supported-device fixture begins without a gamepad");
            RumbleCapture supported;
            const auto supportedId = AttachVirtual("mad-sa joint pickup feedback", supported, true);
            const auto supportedAdded = WaitDeviceEvent(SDL_EVENT_GAMEPAD_ADDED, supportedId);
            const auto added = feedback.HandleEvent(supportedAdded);
            Check(added.Status == NativePadFeedbackStatus::Idle && added.Devices == 1 &&
                added.RumbleDevices == 1 && egl.Current() && (SDL_WasInit(SDL_INIT_VIDEO) & SDL_INIT_VIDEO),
                "typed virtual-gamepad hotplug preserves video/context and opens one rumble device");

            const auto pickup = joint.Create(9701);
            supportedReference = pickup.Value;
            const auto world = host.World();
            gameplay.Tick(1.0 / 60.0, {.Jump = gameplay.State().Grounded}, *world);
            const auto airborneCopy = gameplay.Activity();
            airRevision = airborneCopy.Revision;
            const bool airborneTask = std::ranges::any_of(airborneCopy.PrimaryTasks, [](const auto& chain) {
                return std::ranges::find(chain, NativePlayerTaskType::InAir) != chain.end();
            });
            Check(airRevision > spawnRevision && airborneCopy.InAir && !airborneCopy.Landing && airborneTask &&
                NativePlayerCanStartMission(airborneCopy).Reason == NativeMissionStartReason::PedNotInControlOrDriving,
                "actual controller publishes a new denied in-air task snapshot");
            const auto beforeAir = entities.Revision();
            Require(joint.Publish(pickup), joint.Error);
            Check(entities.ResolvePickup(pickup) && entities.Revision() == beforeAir && !entities.ConsumePadShake(),
                "actual in-air/task denial emits no shake and removes no pickup");

            const auto landings = gameplay.State().Landings;
            for (int i = 0; i < 180 && gameplay.State().Landings == landings; ++i) {
                gameplay.Tick(1.0 / 60.0, {}, *world);
            }
            Require(gameplay.State().Landings == landings + 1 && gameplay.Activity().Landing,
                "actual controller reaches its landing task");
            const auto landingCopy = gameplay.Activity();
            landingRevision = landingCopy.Revision;
            Check(landingRevision > airRevision && !landingCopy.InAir && landingCopy.Landing &&
                airborneCopy.InAir && airborneCopy.Revision == airRevision,
                "activity revisions are monotonic and earlier caller copies remain immutable");
            const bool landingTask = std::ranges::any_of(landingCopy.PrimaryTasks, [](const auto& chain) {
                return std::ranges::find(chain, NativePlayerTaskType::Land) != chain.end();
            });
            Check(landingTask && NativePlayerCanStartMission(landingCopy).Reason ==
                NativeMissionStartReason::PedNotInControlOrDriving,
                "actual landing flag and task remain mission-start denied");
            const auto beforeLanding = entities.Revision();
            Require(joint.Publish(pickup), joint.Error);
            Check(entities.ResolvePickup(pickup) && entities.Revision() == beforeLanding && !entities.ConsumePadShake(),
                "actual landing/task denial emits no shake and removes no pickup");

            const auto landingStarted = gameplay.State().SimulatedSeconds;
            int landingFrames = 0;
            while (gameplay.Activity().Landing && landingFrames++ < 120) {
                Require(gameplay.Activity().Revision == landingRevision,
                    "landing activity revision is stable within its owned task");
                gameplay.Tick(1.0 / 60.0, {}, *world);
            }
            landingSeconds = gameplay.State().SimulatedSeconds - landingStarted;
            groundRevision = gameplay.Activity().Revision;
            Check(!gameplay.Activity().Landing && groundRevision > landingRevision &&
                landingSeconds + 1e-6 >= sourceLandingSeconds &&
                landingSeconds <= sourceLandingSeconds + 2.0 / 60.0 + 1e-5,
                "controller landing lasts the source clip plus callback/process quantization");

            const auto beforeCollection = entities.Revision();
            NativeScriptPropertyInput callerFlags;
            callerFlags.Busy = true;
            callerFlags.Coop = true;
            Require(joint.Publish(pickup, callerFlags), joint.Error);
            const auto drained = ConsumeAndSubmit(entities, feedback);
            Require(drained.Events.size() == 1 && drained.Results.size() == 1,
                "one eligible collection reaches the parent feedback consumer once");
            const auto sourceEvent = drained.Events[0];
            const auto feedbackState = feedback.GetState();
            Check(!entities.ResolvePickup(pickup) && entities.Revision() == beforeCollection + 1 &&
                drained.Results[0].Status == NativePadFeedbackStatus::Applied &&
                drained.Results[0].AppliedDevices == 1 && sourceEvent.Pickup.Value == pickup.Value &&
                sourceEvent.TimeMs == 120 && sourceEvent.Frequency == 100 && sourceEvent.Arg2 == 0 &&
                supported.Calls.size() == 1 && supported.Calls[0] == std::pair<Uint16, Uint16>{25700, 25700} &&
                feedbackState.Active && feedbackState.Rumble == NativePadFeedbackRumble{25700, 25700, 120} &&
                feedbackState.RemainingMilliseconds == 120,
                "one eligible pickup makes one actual 25700/25700 virtual callback with a 120ms deadline");
            Check(callerFlags.Busy && callerFlags.Coop && !entities.ResolvePickup(pickup),
                "host eligibility comes from its copied gameplay activity, not caller busy/coop booleans");

            const auto callbackCount = supported.Calls.size();
            Check(feedback.Submit(sourceEvent, true).Status == NativePadFeedbackStatus::NoReplay &&
                supported.Calls.size() == callbackCount, "submitted feedback identity cannot reshake on replay");
            NativeScriptPropertyInput duplicateFlags;
            duplicateFlags.FrameCounter = joint.Frame;
            duplicateFlags.Busy = true;
            duplicateFlags.Coop = true;
            const auto camera = gameplay.Camera().Position;
            Require(host.TickPlayerEntities({camera.X, camera.Y, camera.Z}, duplicateFlags, error) &&
                entities.AdvanceTime(joint.GameMs, error), error);
            Check(!entities.ConsumePadShake() && supported.Calls.size() == callbackCount,
                "duplicate entity publication does not republish or reshake collection");

            const auto beforeQuery = entities.Revision();
            Bytes operations;
            Op(operations, 0x0214); I32(operations, pickup.Value);
            Op(operations, 0x0214); I32(operations, pickup.Value);
            Op(operations, 0x8214); I32(operations, pickup.Value);
            Op(operations, 0x0215); I32(operations, pickup.Value);
            NativeScriptSession operationSession;
            LoadFixture(operationSession, host, operations);
            const auto firstQuery = operationSession.Step(host);
            Check(firstQuery.Status == NativeScriptStatus::Advanced && operationSession.State().Condition &&
                entities.Revision() == beforeQuery + 1,
                "0214 consumes the inactive full-generation ring reference as true once");
            const auto secondQuery = operationSession.Step(host);
            Check(secondQuery.Status == NativeScriptStatus::Advanced && !operationSession.State().Condition &&
                entities.Revision() == beforeQuery + 1, "repeated 0214 observes the consumed ring reference as false");
            const auto negatedQuery = operationSession.Step(host);
            Check(negatedQuery.Status == NativeScriptStatus::Advanced && operationSession.State().Condition &&
                operationSession.State().LastOpcode == 0x8214 && entities.Revision() == beforeQuery + 1,
                "negated repeated 0214 turns the same false observation into true");
            const auto inactiveRemove = operationSession.Step(host);
            Check(inactiveRemove.Status == NativeScriptStatus::Advanced && operationSession.State().Condition &&
                entities.Revision() == beforeQuery + 1,
                "0215 on the current-generation inactive pickup is a source-valid no-op");

            g_Now = 120;
            Require(feedback.Update(false).Status == NativePadFeedbackStatus::Idle,
                "expire supported virtual feedback");
            Require(SDL_DetachVirtualJoystick(supportedId),
                std::string{"detach supported gamepad: "} + SDL_GetError());
            const auto supportedRemoved = WaitDeviceEvent(SDL_EVENT_GAMEPAD_REMOVED, supportedId);
            Check(feedback.HandleEvent(supportedRemoved).Status == NativePadFeedbackStatus::NoDevice && egl.Current(),
                "typed supported hot-unplug preserves the GL context");

            RumbleCapture unsupported;
            const auto unsupportedId = AttachVirtual("mad-sa joint unsupported feedback", unsupported, false);
            const auto unsupportedAdded = WaitDeviceEvent(SDL_EVENT_GAMEPAD_ADDED, unsupportedId);
            const auto unsupportedResult = feedback.HandleEvent(unsupportedAdded);
            Check(unsupportedResult.Status == NativePadFeedbackStatus::Unsupported &&
                unsupportedResult.Devices == 1 && unsupportedResult.RumbleDevices == 0,
                "virtual gamepad without rumble is explicitly unsupported");
            const auto unsupportedPickup = joint.Create(9702);
            Require(joint.Publish(unsupportedPickup), joint.Error);
            const auto unsupportedDrain = ConsumeAndSubmit(entities, feedback);
            Check(!entities.ResolvePickup(unsupportedPickup) && unsupportedDrain.Events.size() == 1 &&
                unsupportedDrain.Results.size() == 1 &&
                unsupportedDrain.Results[0].Status == NativePadFeedbackStatus::Unsupported &&
                unsupported.Calls.empty(),
                "unsupported hardware remains explicit after successful actual collection");
            Require(SDL_DetachVirtualJoystick(unsupportedId),
                std::string{"detach unsupported gamepad: "} + SDL_GetError());
            const auto unsupportedRemoved = WaitDeviceEvent(SDL_EVENT_GAMEPAD_REMOVED, unsupportedId);
            feedback.HandleEvent(unsupportedRemoved);

            Check((SDL_WasInit(SDL_INIT_VIDEO) & SDL_INIT_VIDEO) != 0 && egl.Current(),
                "all typed SDL feedback events preserve the existing video/context owner");
            std::printf("pickup-feedback measurements fixture=typed-test@0053-root position=%.3f,%.3f,%.3f "
                "pickupRef=%d landingMs=%.3f sourceLandingMs=%.3f revisions=%llu/%llu/%llu/%llu\n",
                fixturePosition.X, fixturePosition.Y, fixturePosition.Z, supportedReference,
                landingSeconds * 1000.0, sourceLandingSeconds * 1000.0,
                static_cast<unsigned long long>(spawnRevision), static_cast<unsigned long long>(airRevision),
                static_cast<unsigned long long>(landingRevision), static_cast<unsigned long long>(groundRevision));
            std::printf("native-pickup-feedback failures=%d actualCommands=%llu terminal=%04X@%u hud33=actual-GL "
                "pickup=1277:pickupsave.dff:icons4.txd callback=1 motors=25700/25700 durationMs=120 "
                "ring=0214-true-false-negated-true noDevice=normal unsupported=explicit virtual-only=1 "
                "originalPSAVE=0 persistence=memory-only saveFrontend=unimplemented fullboot=0\n",
                g_Failures, static_cast<unsigned long long>(mission.Commands), terminal.Opcode, terminal.IP);
            hud.ReleaseGpu();
        }
        StreamPager_Shutdown();
        Check((SDL_WasInit(SDL_INIT_VIDEO) & SDL_INIT_VIDEO) != 0 && egl.Current(),
            "gameplay/host shutdown preserves the probe-owned SDL video and GL context");
    }
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    return g_Failures ? 1 : 0;
} catch (const std::exception& error) {
    std::fprintf(stderr, "NativePickupFeedbackProbe FAIL %s\n", error.what());
    return 2;
}
