// TEST ONLY: observe the actual LiveWorld commit call without substituting its
// registry, worker, GPU uploader, reconciliation result, or cleanup authority.
#include "app/platform/linux/RealtimeScriptHost.h"
namespace publication_probe {
std::uint64_t Before(std::uint64_t, const std::shared_ptr<const NativeCollisionSnapshot>&);
}
#define ReconcileCarGeneratorsBeforeWorldCommit(generation, snapshot) \
    ReconcileCarGeneratorsBeforeWorldCommit(publication_probe::Before((generation), (snapshot)), (snapshot))
#include "Realtime.cpp"
#undef ReconcileCarGeneratorsBeforeWorldCommit

namespace publication_probe {
LiveWorld* observed = nullptr;
ResidentWorld* oldActive = nullptr;
std::uint64_t oldGeneration = 0, oldRevision = 0;
unsigned calls = 0;

static void Require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "publication-probe FAIL %s SDL=%s GL=%x\n", message, SDL_GetError(), glGetError());
        std::exit(2);
    }
}

std::uint64_t Before(std::uint64_t generation, const std::shared_ptr<const NativeCollisionSnapshot>& snapshot) {
    Require(observed && observed->pending && observed->pending->gpu.complete, "reconcile only AFTER complete GPU upload");
    Require(observed->active.get() == oldActive && observed->active->cpu->Generation == oldGeneration &&
        observed->scriptHost->CarGeneratorResidency().Generation() == oldGeneration &&
        observed->scriptHost->CarGenerators().Revision() == oldRevision, "reconcile BEFORE matched world/registry publication");
    Require(generation == observed->pending->cpu->Generation && snapshot == observed->pending->cpu->SourceCollision &&
        &observed->pending->Collision() == &observed->pending->cpu->QueryWorld(), "exact selected pending packet");
    ++calls;
    std::printf("publication-before call=%u old=%llu next=%llu gpuComplete=1 activeOld=1 registryOld=1 exactSnapshot=1\n",
        calls, static_cast<unsigned long long>(oldGeneration), static_cast<unsigned long long>(generation));
    return generation;
}

static void Remember(LiveWorld& world) {
    observed = &world;
    oldActive = world.active.get();
    oldGeneration = world.active->cpu->Generation;
    oldRevision = world.scriptHost->CarGenerators().Revision();
}
} // namespace publication_probe

int main(int argc, char** argv) {
    using namespace publication_probe;
    Require(argc == 2, "owned game directory required");
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    Window window;
    Require(SDL_Init(SDL_INIT_VIDEO), "SDL video");
    Require(SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2) && SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1) &&
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1) && SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24), "GL attributes");
    window.window = SDL_CreateWindow("mad-sa | car-generator publication probe", 640, 448, SDL_WINDOW_OPENGL);
    Require(window.window && (window.context = SDL_GL_CreateContext(window.window)) &&
        SDL_GL_MakeCurrent(window.window, window.context), "own GL window");
    Require(std::strcmp(SDL_GetCurrentVideoDriver(), "wayland") == 0, "native Wayland");
    std::printf("publication-video driver=%s renderer=%s\n", SDL_GetCurrentVideoDriver(), glGetString(GL_RENDERER));
    Pager pager;
    E2ELoadInfo load{};
    char message[512]{};
    std::string error;
    Require(StreamPager_Init(argv[1], load, message, sizeof(message), {true, 900, 4096}), message);
    RealtimeGameplay gameplay;
    RealtimeScriptHost host(gameplay);
    Require(host.SeedSourceRngAfterRwInit(error), error.c_str());
    const auto context = NativeCollisionContext::LoadBeforeWorker(argv[1], 900, error);
    Require(context && host.InitializeBeforeWorker(argv[1], error, context), error.c_str());
    Require(host.RunPass(256).Status == NativeScriptStatus::Waiting && host.State().Commands == 53, "actual main53");
    Require(host.PrepareInitialGarageWorldBeforeWorker(error) && host.AdvanceTime(0, error), error.c_str());
    LiveWorld world;
    world.scriptHost = &host;
    world.collisionContext = context;
    Require(world.Initialize(host), "actual initial GPU upload");
    Require(world.active->cpu->Generation == 3 && &world.active->Collision() == host.World() &&
        world.active->cpu->BorrowedCollision == host.Publication().Collision &&
        world.active->cpu->Collision.TriangleCount() == 0 && host.CarGeneratorResidency().Active().size() == 22 &&
        host.CarGenerators().Census().Registered == 88, "initial generation3 exact borrowed BVH and 22/88 residency");
    host.SealStartup();
    world.Start(true);
    Remember(world);
    // Request a genuine worker rebuild at the same location: source membership
    // is unchanged, so no unimplemented departure cleanup is needed for Ready.
    const auto center = world.active->cpu->Position;
    world.worker->Request(center, true);
    const auto deadline = realtime_streaming::Milliseconds() + 120000;
    while (!world.pending && realtime_streaming::Milliseconds() < deadline) {
        if (auto cpu = world.worker->TakeReady()) {
            world.pending = std::make_unique<ResidentWorld>();
            world.pending->cpu = std::move(cpu);
        }
        SDL_PumpEvents();
        SDL_Delay(1);
    }
    Require(world.pending && world.pending->cpu->Generation == 4 && world.pending->cpu->Error.empty() &&
        !world.pending->cpu->BorrowedCollision && !world.pending->gpu.complete && calls == 0,
        "real Worker continues3 to4; CPU readiness does not reconcile");
    const auto nextCollision = world.pending->cpu->SourceCollision;
    const auto* nextQuery = &world.pending->Collision();
    const auto* nextScene = &world.pending->cpu->Scene;
    unsigned incomplete = 0;
    bool published = false;
    while (!published && realtime_streaming::Milliseconds() < deadline) {
        Require(world.Advance(center, published), "Ready advance");
        if (!published) {
            ++incomplete;
            Require(calls == 0 && world.active.get() == oldActive && host.CarGeneratorResidency().Generation() == 3 &&
                host.CarGenerators().Revision() == oldRevision, "partial GPU retains old registry/world");
        }
        SDL_PumpEvents();
    }
    Require(published && calls == 1 && incomplete > 0 && world.active->cpu->Generation == 4 && world.active->gpu.complete &&
        world.active->cpu->SourceCollision == nextCollision && &world.active->Collision() == nextQuery &&
        &world.active->cpu->Scene == nextScene && host.CarGeneratorResidency().Snapshot() == nextCollision &&
        host.CarGeneratorResidency().Generation() == 4 && host.CarGenerators().Census().Registered == 88,
        "Ready immediately publishes exact generation4 soup/BVH/GPU/residency");
    std::printf("publication-ready PASS generation=4 partialUploads=%u sources=%zu definitions=88 exactQuery=1 exactScene=1\n",
        incomplete, host.CarGeneratorResidency().Active().size());
    while (world.retiring && realtime_streaming::Milliseconds() < deadline) Require(world.Advance(center, published), "incremental retirement");
    Require(!world.retiring, "old GPU retired");
    Remember(world);
    const auto retainedSnapshot = host.CarGeneratorResidency().Snapshot();
    const auto retainedSources = std::vector<NativeCarGeneratorResidentSource>(host.CarGeneratorResidency().Active().begin(),
        host.CarGeneratorResidency().Active().end());
    const realtime_streaming::Center departure{-2000, 2000, 50};
    bool advanced = true;
    while (advanced && realtime_streaming::Milliseconds() < deadline) {
        advanced = world.Advance(departure, published);
        Require(!published && world.active.get() == oldActive && host.CarGeneratorResidency().Snapshot() == retainedSnapshot &&
            host.CarGenerators().Revision() == oldRevision, "departure never prematurely publishes/cleans up");
        SDL_PumpEvents();
        SDL_Delay(1);
    }
    Require(!advanced && calls == 2 && world.pending && world.pending->gpu.complete && world.pending->cpu->Generation == 5,
        "actual departing Worker5 returns explicit terminal after GPU upload");
    const auto pending = host.ReconcileCarGeneratorsBeforeWorldCommit(5, world.pending->cpu->SourceCollision);
    Require(pending.Status == NativeCarGeneratorResidencyStatus::PendingCleanup && !pending.Removals.empty() &&
        host.CarGeneratorResidency().Generation() == 4 && host.CarGeneratorResidency().Snapshot() == retainedSnapshot &&
        std::ranges::equal(host.CarGeneratorResidency().Active(), retainedSources) && host.CarGenerators().Revision() == oldRevision &&
        host.CarGenerators().Census().Registered == 88 && host.Vehicles().Census().CreatedEvents == 0 &&
        host.InspectSourceRng().Value->DrawCount == 0, "pending retry retains every registry/source; no false fulfillment");
    Require(!world.Advance(departure, published) && !published && world.active.get() == oldActive && calls == 3,
        "repeated pending terminal retains old active world");
    // TEST ONLY invalid generation on the genuine uploaded worker packet.
    // Exercise the coordinator's Error branch; never invent a cleanup result.
    world.pending->cpu->Generation = 4;
    Require(!world.Advance(departure, published) && !published && world.active.get() == oldActive && calls == 4 &&
        host.CarGenerators().Revision() == oldRevision && host.CarGeneratorResidency().Snapshot() == retainedSnapshot,
        "Error also retains old active world/registry");
    Require(glGetError() == GL_NO_ERROR, "real upload GL status");
    std::printf("publication-probe PASS worker=4 ready-after-gpu-before-swap=1 pending=5 removals=%zu retained=4 error-retained=1 false-cleanup=0 fullboot=0\n",
        pending.Removals.size());
    observed = nullptr;
    return 0;
}
