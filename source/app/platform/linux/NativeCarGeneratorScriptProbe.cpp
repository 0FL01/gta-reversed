// Actual host/SCM execution with the production GL HUD and runtime adapter.
#ifndef GL_GLEXT_PROTOTYPES
#define GL_GLEXT_PROTOTYPES 1
#endif
#include "app/platform/linux/Realtime.cpp"
#include "app/platform/linux/NativeCarGeneratorRuntime.h"
#include <EGL/eglext.h>
#include <stdexcept>

namespace {
void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
struct ProbeGl {
    EGLDisplay Display = EGL_NO_DISPLAY;
    EGLSurface Surface = EGL_NO_SURFACE;
    EGLContext Context = EGL_NO_CONTEXT;
    ProbeGl() {
        const auto getDisplay = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
        Require(bool(getDisplay), "surfaceless EGL entry");
        Display = getDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
        Require(Display != EGL_NO_DISPLAY && eglInitialize(Display, nullptr, nullptr) && eglBindAPI(EGL_OPENGL_API), "EGL init");
        const EGLint attributes[]{EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_BIT,
            EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_DEPTH_SIZE,24,EGL_NONE};
        EGLConfig config{}; EGLint count{};
        Require(eglChooseConfig(Display, attributes, &config, 1, &count) && count, "EGL config");
        const EGLint dimensions[]{EGL_WIDTH,640,EGL_HEIGHT,448,EGL_NONE};
        Surface = eglCreatePbufferSurface(Display, config, dimensions);
        Context = eglCreateContext(Display, config, EGL_NO_CONTEXT, nullptr);
        Require(Surface != EGL_NO_SURFACE && Context != EGL_NO_CONTEXT &&
            eglMakeCurrent(Display, Surface, Surface, Context), "EGL context");
        std::printf("real-gl renderer=%s\n", glGetString(GL_RENDERER));
    }
    ~ProbeGl() {
        eglMakeCurrent(Display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroyContext(Display, Context); eglDestroySurface(Display, Surface); eglTerminate(Display);
    }
};

void Journal(const RealtimeScriptHost& host) {
    for (const auto& event : host.CarGenerators().Events()) {
        const auto& r = event.Create;
        std::printf("generator-event sequence=%llu opcode=%04X session=%llu instruction=%llu ip=%u ref=%d "
            "xyz=%.9g,%.9g,%.9g degrees=%.9g model=%d colors=%d,%d force=%d alarm=%d lock=%d delays=%d,%d count=%d status=%d\n",
            (unsigned long long)event.Sequence, event.Kind == NativeCarGeneratorEventKind::Create014B ? 0x014B : 0x014C,
            (unsigned long long)event.Id.Session, (unsigned long long)event.Id.Instruction, event.Id.IP, event.Generator.Value,
            r.Position.X,r.Position.Y,r.Position.Z,r.AngleDegrees,r.ModelId,r.PrimaryColor,r.SecondaryColor,
            r.ForceSpawn,r.AlarmChance,r.DoorLockChance,r.MinDelay,r.MaxDelay,event.Count,int(event.Result.Status));
    }
}
} // namespace

int main(int argc, char** argv) try {
    Require(argc == 2 || (argc == 3 && (std::string_view(argv[2]) == "--cpu" || std::string_view(argv[2]) == "--random")),
        "game directory [--cpu|--random] required");
    const bool cpu = argc == 3 && std::string_view(argv[2]) == "--cpu";
    const bool random = argc == 3 && std::string_view(argv[2]) == "--random";
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::string error; char message[512]{};
    RealtimeGameplay gameplay;
    RealtimeScriptHost host(gameplay);
    E2ELoadInfo info;
    Require(StreamPager_Init(argv[1], info, message, sizeof(message), {true,300,1200}), message);
    Require(host.SeedSourceRngAfterRwInit(error), error);
    const auto seed = host.InspectSourceRng();
    Require(seed.Status == NativeSourceRngStatus::Ready && seed.Value && !seed.Value->DrawCount, "single post-RW platform seed");
    auto context = NativeCollisionContext::LoadBeforeWorker(argv[1], 900, error);
    Require(bool(context) && host.InitializeBeforeWorker(argv[1], error, context), error);
    Require(host.CarGenerators().Census().Registered == 0 && host.CarGenerators().Census().DeferredAssetRecords == 1045 &&
        host.CarGeneratorResidency().Catalog().size() == 190, "full deferred binary catalog before source world publication");
    RealtimeHud hud;
    if (!cpu) Require(hud.Load(argv[1], message, sizeof(message)), message);
    Require(host.RunPass(1000).Status == NativeScriptStatus::Waiting && host.State().Commands == 53, "actual main53 first yield");
    Require(host.CarGeneratorResidency().Active().size() == 22 && host.CarGenerators().Census().Registered == 88 &&
        host.CarGeneratorResidency().Snapshot() == host.Publication().SourceCollision && host.WorldRevision() == 2,
        "initial Ready matched source-COL: 22 generator sources/88 definitions");
    const auto original = host.Publication();
    Require(host.PrepareInitialGarageWorldBeforeWorker(error), error);
    const auto paired = host.Publication();
    Require(host.WorldRevision() == 3 && paired.Scene == original.Scene && paired.SourceCollision != original.SourceCollision &&
        paired.Overrides == paired.SourceCollision->Overrides && paired.Overrides->Entries().size() == 14 &&
        std::ranges::count_if(paired.Overrides->Entries(), [](const auto& p) { return !p.CollisionEnabled; }) == 13 &&
        host.CarGeneratorResidency().Snapshot() == paired.SourceCollision && host.CarGeneratorResidency().Generation() == 3 &&
        host.CarGenerators().Census().Registered == 88, "paired garage preparation retains scene/lifetimes and generator source set");
    Require(host.AdvanceTime(0, error), error);
    std::printf("first-ready main=53 worldRevision=3 sources=%zu definitions=%zu catalog=%zu renderInstances=%d overrides=14 disabled=13\n",
        host.CarGeneratorResidency().Active().size(), host.CarGenerators().Census().Registered,
        host.CarGeneratorResidency().Catalog().size(), paired.Frame.instances);
    if (cpu) {
        host.SealStartup();
        const auto terminal = host.RunPass(5000);
        Require(terminal.Status == NativeScriptStatus::Unsupported && terminal.IP == 205876 && terminal.Opcode == 0x0570 &&
            host.Session().Threads()[1].Commands == 539 && host.CarGenerators().Events().empty(), "CPU-negative539 no GPU HUD readiness");
        std::printf("NativeCarGeneratorScriptProbe CPU PASS commands=539 terminal=0570@205876 generators=88\n");
        StreamPager_Shutdown(); return 0;
    }
    {
        ProbeGl gl;
        Require(hud.Upload(message, sizeof(message)) && hud.IsRadarSpriteUploaded(33), message);
        host.SetRadarSpriteReady([&](int sprite) { return hud.IsRadarSpriteUploaded(sprite); });
        GpuScene gpu;
        Require(gpu.UploadTextures(*paired.Scene), "actual initial scene GL upload");
        const auto& view = gameplay.Camera();
        Camera camera{view.Position.X,view.Position.Y,view.Position.Z,view.Yaw,view.Pitch};
        glClearColor(.1f,.12f,.15f,1); glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
        camera.Apply(640,448,1600); gpu.Draw(*paired.Scene);
        const auto captured = RealtimeHud::CapturePriceView(.1f,1600,60);
        Require(glGetError() == GL_NO_ERROR, "actual initial scene GL publication");
        realtime_streaming::CpuWorld world;
        world.Position = {paired.Center.X,paired.Center.Y,paired.Center.Z}; world.Generation = host.WorldRevision();
        world.Scene = *paired.Scene; world.Frame = paired.Frame;
        Require(world.Collision.Rebuild(*paired.SourceCollision, error), error);
        world.SourceCollision = paired.SourceCollision; world.Overrides = paired.Overrides;
        host.SealStartup(); // no further RW/main parser calls
        NativeScriptResult terminal;
        std::size_t creates = 0, switches = 0;
        for (unsigned i = 0; i < 5000; ++i) {
            terminal = host.RunPass(1);
            const auto& thread = host.Session().Threads()[1];
            if (terminal.Status != NativeScriptStatus::BudgetYield) break;
            if (thread.LastOpcode == 0x014B || thread.LastOpcode == 0x014C) {
                const auto& event = host.CarGenerators().Events().back();
                const auto* entry = host.CarGenerators().Resolve(event.Generator);
                Require(entry && entry->Used && entry->IplId == 0 && entry->Vehicle.Value == -1, "real typed SCM generator allocation without vehicle");
                if (thread.LastOpcode == 0x014B) {
                    ++creates;
                    Require(thread.LastOutputWrite.IP == thread.LastInstructionIP && thread.LastOutputWrite.Value == event.Generator.Value,
                        "actual VM destination stores the owned slot reference");
                    std::printf("vm014B commands=%llu ip=%u next=%u output=%u ref=%d compressed=%d,%d,%d angle=%d count=%u timer=%u\n",
                        (unsigned long long)thread.Commands,thread.LastInstructionIP,thread.IP,thread.LastOutputWrite.Variable,
                        event.Generator.Value,entry->CompressedPosition[0],entry->CompressedPosition[1],entry->CompressedPosition[2],
                        entry->Angle,entry->GenerateCount,entry->NextGenerationTime);
                } else ++switches;
            }
        }
        Journal(host);
        std::printf("strict-terminal status=%d commands=%llu ip=%u opcode=%04X creates=%zu switches=%zu message=%s\n",
            int(terminal.Status),(unsigned long long)host.Session().Threads()[1].Commands,terminal.IP,terminal.Opcode,
            creates,switches,terminal.Message.c_str());
        Require(terminal.Status == NativeScriptStatus::Unsupported && creates && switches &&
            host.Session().Threads()[1].Commands > 679 && terminal.Opcode != 0x014B && terminal.Opcode != 0x014C,
            "actual mission crosses679/create/switch and stops at next strict unsupported");
        Require(host.Session().Threads()[1].Commands == 1207 && terminal.Opcode == 0x04CE && terminal.IP == 212086 &&
            creates == 10 && switches == 10, "measured legal mission0 strict frontier");
        const auto first = host.CarGenerators().Events().front();
        Require(first.Create.ModelId == 476 && first.Id.IP == 207007 && first.Generator.Value == 88 &&
            first.Create.AngleDegrees == 180 && first.Create.MinDelay == 0 && first.Create.MaxDelay == 10000,
            "first real source014B payload and source-resident allocation order");
        // Labelled host service replay/collision checks, after the actual VM stop.
        NativeScriptCarGeneratorRequest replay{first.Create.Id,first.Create.Position,first.Create.AngleDegrees,
            first.Create.ModelId,first.Create.PrimaryColor,first.Create.SecondaryColor,first.Create.ForceSpawn,
            first.Create.AlarmChance,first.Create.DoorLockChance,first.Create.MinDelay,first.Create.MaxDelay};
        const auto revision = host.CarGenerators().Revision();
        Require(host.CreateCarGenerator(replay).Reference.Value == 88 && host.CarGenerators().Revision() == revision, "create replay stable");
        ++replay.MaxDelay;
        Require(host.CreateCarGenerator(replay).Result.Status == NativeScriptServiceStatus::Error &&
            host.SetCameraBehindPlayer({replay.Id}).Status == NativeScriptServiceStatus::Error &&
            host.CreatePickup({replay.Id,0,0,{}}).Result.Status == NativeScriptServiceStatus::Error &&
            host.SetEntryExitFlag({replay.Id}).Status == NativeScriptServiceStatus::Error &&
            host.DeactivateGarage({replay.Id}).Status == NativeScriptServiceStatus::Error,
            "generator identity globally excludes changed arguments and player/entity/ENEX/garage commands");
        for (const auto opcode : {0x04E4,0x0053,0x09B4,0x02B9}) {
            const auto event = std::ranges::find(host.Events(), opcode, &RealtimeScriptHostEvent::Opcode);
            Require(event != host.Events().end(), "real world/player/ENEX/garage request exists");
            replay.Id = event->Id;
            Require(host.CreateCarGenerator(replay).Result.Status == NativeScriptServiceStatus::Error,
                "prior world/player/ENEX/garage identity excludes generator create");
        }
        replay.Id = {99002,1,1};
        Require(host.SetBlipDisplay({replay.Id,host.Entities().Blips()[0].Reference,2}).Status == NativeScriptServiceStatus::Ready &&
            host.CreateCarGenerator(replay).Result.Status == NativeScriptServiceStatus::Error,
            "prior entity identity excludes generator create");
        const NativeScriptRequestId invalidSwitch{99003,1,1};
        Require(host.SwitchCarGenerator({invalidSwitch,{-1},0}).Status == NativeScriptServiceStatus::Error &&
            host.SetCameraBehindPlayer({invalidSwitch}).Status == NativeScriptServiceStatus::Error,
            "failed typed generator request also retains global identity");
        Require(host.SwitchCarGenerator({{99001,1,1},{0},0}).Status == NativeScriptServiceStatus::Ready &&
            host.CarGenerators().Resolve({0})->GenerateCount == 0, "source slot zero is valid in typed host service");
        Require(host.AdvanceTime(1000, error), error);
        NativeCarGeneratorRuntime runtime(host.CarGenerators(), gameplay, host.Vehicles(), host.State());
        NativeScriptServiceResult result;
        for (std::uint64_t frame = 1; frame <= 4; ++frame) {
            auto vehicles = host.PublishVehicles(frame, error); Require(bool(vehicles), error);
            result = runtime.Tick(world, {.Frame=frame,.Camera=captured}, vehicles);
            if (result.Status != NativeScriptServiceStatus::Ready) break;
        }
        Require(!runtime.Frame().Process.Actions.empty(), "actual adapter consumes host-owned definitions");
        std::printf("runtime-startup actualPlayer=1 quarters=4 definitions=%zu visitedLast=%zu demands=%zu status=%d\n",
            host.CarGenerators().Census().Registered,runtime.Frame().Process.Visited,runtime.Frame().Demands.size(),int(result.Status));
        Require(result.Status == NativeScriptServiceStatus::Ready && runtime.Frame().Demands.empty(),
            "untouched startup position reaches only source distance/disabled gates");
        // LABELLED proximity fixture: reposition the actual source-created
        // controller within this already published COL world. Definitions stay
        // authored; no fallback model, RNG, blockage or spawn result is supplied.
        bool positioned = false;
        for (std::size_t slot = 1; slot < 88; ++slot) {
            const auto* generator = host.CarGenerators().Resolve({std::int32_t(slot)});
            if (!generator || !generator->GenerateCount || (random ? generator->ModelId != -1 : generator->ModelId <= 0)) continue;
            const auto p = generator->Position();
            float ground = 0;
            if (!world.Collision.Ground(p.X + 150, p.Y, p.Z + 20, p.Z - 40, ground)) continue;
            if (!gameplay.SpawnScriptPlayer(world.Collision, {p.X + 150,p.Y,ground + .5f}, error)) continue;
            Require(gameplay.SetScriptCameraBehind(world.Collision, error), error);
            std::printf("runtime-proximity fixture=controller-reposition generator=%zu model=%d source=%s xyz=%.3f,%.3f,%.3f\n",
                slot,generator->ModelId,generator->Provenance.Source.c_str(),p.X,p.Y,p.Z);
            positioned = true; break;
        }
        Require(positioned, "owned resident generator has nearby actual source ground for labelled proximity fixture");
        const auto& nearbyCamera = gameplay.Camera();
        camera = {nearbyCamera.Position.X,nearbyCamera.Position.Y,nearbyCamera.Position.Z,nearbyCamera.Yaw,nearbyCamera.Pitch};
        camera.Apply(640,448,1600);
        const auto nearbyCapture = RealtimeHud::CapturePriceView(.1f,1600,60);
        NativeCarGeneratorRuntime nearby(host.CarGenerators(), gameplay, host.Vehicles(), host.State());
        for (std::uint64_t frame = 5; frame <= 8; ++frame) {
            auto vehicles = host.PublishVehicles(frame, error); Require(bool(vehicles), error);
            result = nearby.Tick(world, {.Frame=frame,.Camera=nearbyCapture}, vehicles);
            if (result.Status != NativeScriptServiceStatus::Ready) break;
        }
        for (const auto& demand : nearby.Frame().Demands) {
            Require(demand.Collision == paired.SourceCollision && demand.Vehicles->Owner() == host.Vehicles().Owner() &&
                nearby.ResolveDemand(demand.Id) == &demand, "runtime retains typed request with real world/pool authority");
            std::printf("runtime-demand ref=%d model=%d requirement=%d status=%d world=%llu source=%s\n",
                demand.Action.Generator.Value,demand.Action.Request.ModelId,int(demand.Action.Requirement),int(demand.Action.Result.Status),
                (unsigned long long)demand.WorldGeneration,demand.GeneratorState.Provenance.Source.c_str());
        }
        Require(!nearby.Frame().Demands.empty() && (result.Status == NativeScriptServiceStatus::Pending ||
            result.Status == NativeScriptServiceStatus::Unsupported), "actual initial source generators reach retained consumer boundary");
        Require(std::ranges::any_of(nearby.Frame().Demands, [&](const auto& demand) {
            return demand.Action.Requirement == (random ? NativeCarGeneratorRequirement::RandomPopulationSelection :
                NativeCarGeneratorRequirement::CollisionBlockage);
        }), "authored fixed/random definition retains its exact typed missing consumer");
        const auto counter = host.CarGenerators().ProcessCounter();
        const auto demandId = nearby.Frame().Demands.front().Id;
        const auto* retained = nearby.ResolveDemand(demandId);
        auto vehicles = host.PublishVehicles(10, error); Require(bool(vehicles), error);
        Require(nearby.Tick(world, {.Frame=10,.Camera=nearbyCapture}, vehicles).Status == result.Status &&
            nearby.ResolveDemand(demandId) == retained && host.CarGenerators().ProcessCounter() == counter,
            "pending/unsupported request retained without repeated Process");
        Require(host.Vehicles().Census().Alive == 0 && host.InspectSourceRng().Value == seed.Value &&
            !RealtimeScriptHost::OriginalRandomDrawOrderParity, "no invented spawn or RNG draws/order parity");
        auto pendingCollision = std::make_shared<NativeCollisionSnapshot>();
        Require(context->Snapshot(0, 0, *pendingCollision, error, paired.Overrides), error);
        const auto beforeResidency = host.CarGenerators().Revision();
        const auto provision = host.ReconcileCarGeneratorsBeforeWorldCommit(4, pendingCollision);
        std::printf("host-provision status=%d detail=%s removals=%zu records=%zu generation=%llu registryBefore=%llu registryAfter=%llu\n",
            int(provision.Status),provision.Detail.c_str(),provision.Removals.size(),provision.Records,
            (unsigned long long)host.CarGeneratorResidency().Generation(),(unsigned long long)beforeResidency,
            (unsigned long long)host.CarGenerators().Revision());
        Require(provision.Status != NativeCarGeneratorResidencyStatus::Ready && !provision.Removals.empty() &&
            host.CarGenerators().Revision() == beforeResidency && host.CarGeneratorResidency().Generation() == 3 &&
            host.CarGeneratorResidency().Snapshot() == paired.SourceCollision && host.Publication().Scene == paired.Scene &&
            host.Publication().SourceCollision == paired.SourceCollision && host.WorldRevision() == 3,
            "real source provisioning boundary retains active world and registry");
        auto departingCollision = std::make_shared<NativeCollisionSnapshot>();
        // Labelled narrower source-COL window, built by the actual asset API;
        // this pending packet never replaces the active 900-unit world.
        Require(context->Assets.Snapshot(context->Population, paired.Center.X, paired.Center.Y, 100,
            *departingCollision, error, 0, paired.Overrides), error);
        const auto departure = host.ReconcileCarGeneratorsBeforeWorldCommit(5, departingCollision);
        Require(departure.Status == NativeCarGeneratorResidencyStatus::PendingCleanup && !departure.Removals.empty() &&
            host.CarGenerators().Revision() == beforeResidency && host.CarGeneratorResidency().Generation() == 3 &&
            host.CarGeneratorResidency().Snapshot() == paired.SourceCollision && host.Publication().SourceCollision == paired.SourceCollision,
            "public pre-world-commit departure remains pending on actual model/vehicle ownership cleanup");
        Require(host.ReconcileCarGeneratorsBeforeWorldCommit(5, departingCollision).Status == NativeCarGeneratorResidencyStatus::PendingCleanup,
            "exact immutable precommit retry remains pending without fake cleanup");
        std::printf("host-provision pendingGeneration=5 retainedGeneration=3 cleanupObligations=%zu acknowledged=0\n",departure.Removals.size());
        std::printf("NativeCarGeneratorScriptProbe GL PASS commands=%llu creates=%zu switches=%zu terminal=%04X@%u "
            "definitions=%zu demands=%zu rngSeed=%u rngDraws=%llu originalDrawOrderParity=0 vehicles=0 proximity=%s\n",
            (unsigned long long)host.Session().Threads()[1].Commands,creates,switches,terminal.Opcode,terminal.IP,
            host.CarGenerators().Census().Registered,nearby.Frame().Demands.size(),seed.Value->Seed,
            (unsigned long long)host.InspectSourceRng().Value->DrawCount,random ? "labelled-random" : "labelled-fixed");
        host.SetRadarSpriteReady({}); hud.ReleaseGpu();
    }
    StreamPager_Shutdown(); return 0;
} catch (const std::exception& exception) {
    std::fprintf(stderr,"NativeCarGeneratorScriptProbe FAIL %s\n",exception.what()); return 2;
}
