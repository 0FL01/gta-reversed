// Test the actual realtime staging/swap methods with the sole worker and GL.
#include "Realtime.cpp"

namespace {
unsigned s_Checks = 0;

void Require(bool condition, const char* message) {
    ++s_Checks;
    if (!condition) {
        std::fprintf(stderr, "script-world-publication-fail: %s SDL=%s GL=%x\n",
            message, SDL_GetError(), glGetError());
        std::exit(1);
    }
}
}

int main(int argc, char** argv) {
    Require(argc == 2, "owned game directory is required");
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    Window window;
    Require(SDL_Init(SDL_INIT_VIDEO), "SDL video");
    Require(SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2) &&
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1) &&
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1), "GL attributes");
    window.window = SDL_CreateWindow("source script world publication", 640, 448, SDL_WINDOW_OPENGL);
    Require(window.window && (window.context = SDL_GL_CreateContext(window.window)) &&
        SDL_GL_MakeCurrent(window.window, window.context), "GL context");
    Require(std::strcmp(SDL_GetCurrentVideoDriver(), "wayland") == 0, "Wayland route");

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
    NativeWorldEntityInfo metadata;
    Require(metadata.LoadBeforeWorker(argv[1],context->Population,error),error.c_str());
    const auto catalog=NativeLodCatalog::LoadBeforeWorker(argv[1],context->Population,error);
    Require(bool(catalog),error.c_str());
    const auto groundAuthority=NativeWorldGround::PrepareWithCatalog(*context,metadata,*catalog,{false,1.0f},error,1);
    Require(bool(groundAuthority),error.c_str());
    Require(host.RunPass(256).Status == NativeScriptStatus::Waiting && host.State().Commands == 53,
        "actual startup main53");
    Require(host.PrepareInitialGarageWorldBeforeWorker(error), error.c_str());
    LiveWorld world;
    world.scriptHost = &host;
    world.collisionContext = context;
    Require(world.Initialize(host) && world.active->cpu->Generation == 3,
        "initial exact source CPU/COL/GPU world");
    const auto center = world.active->cpu->Position;
    const auto verifyGroundPair = [&] {
        const auto& cpu=*world.active->cpu;
        const NativeWorldGroundCommit commit{cpu.Generation,1,cpu.Position.X,cpu.Position.Y,
            context->Radius,cpu.SourceCollision,1.0f};
        const auto publication=groundAuthority->Publish(commit,2229.5f,-1342.0f);
        const auto hit=NativeWorldGround::Query(publication,{2229.5f,-1342.0f,23.125f},cpu.Generation,1);
        Require(hit.Status==NativeSourceGroundStatus::Hit && publication.Diagnostics.empty() &&
            std::bit_cast<std::uint32_t>(hit.Point[2])==std::bit_cast<std::uint32_t>(22.99150276f),
            "source building-only query belongs to exact active scene/COL/GPU generation");
        Require(NativeWorldGround::Query(publication,{2229.5f,-1342.0f,23.125f},cpu.Generation+1,1).Reason==
            NativeSourceGroundReason::StaleWorld,"retired/foreign generation cannot satisfy ground");
    };
    verifyGroundPair();
    host.SealStartup();
    world.Start(true);
    bool scriptPending = false;
    unsigned partialPolls = 0;
    host.SetLiveWorldLoader(
        [&](const NativeScriptSceneRequest& request, const NativeScriptServiceTicket&,
            RealtimeScriptWorldPublication& publication) {
            const auto result = world.PrepareScriptWorld(request, scriptPending);
            if (result.Status != NativeScriptAsyncPrepareStatus::Prepared) {
                if (world.pending && !world.pending->gpu.complete) ++partialPolls;
                return result;
            }
            const auto& cpu = *world.pending->cpu;
            publication.Center = request.Position;
            publication.Generation = cpu.Generation;
            publication.Frame = cpu.Frame;
            publication.Overrides = cpu.Overrides;
            publication.Scene = std::make_shared<const WorldShotScene>(cpu.Scene);
            return result;
        },
        [&](const NativeScriptServiceTicket&) { world.CancelScriptWorld(scriptPending); });

    const auto collisionRequest = [&](std::uint64_t sequence) {
        return NativeScriptCollisionRequest{{host.Session().SessionId(), sequence, 0}, center.X, center.Y};
    };
    const auto pollReady = [&](const NativeScriptCollisionRequest& request) {
        const auto deadline = realtime_streaming::Milliseconds() + 120000;
        NativeScriptServiceResult result;
        do {
            result = host.RequestCollision(request);
            if (result.Status != NativeScriptServiceStatus::Pending) break;
            SDL_PumpEvents();
            SDL_Delay(1);
        } while (realtime_streaming::Milliseconds() < deadline);
        Require(result.Status == NativeScriptServiceStatus::Ready, result.Message.c_str());
    };
    const auto retire = [&] {
        const auto deadline = realtime_streaming::Milliseconds() + 120000;
        while (world.retiring && realtime_streaming::Milliseconds() < deadline) {
            bool published = false;
            Require(world.Advance(center, published, false) && !published, "retirement without normal publication");
        }
        Require(!world.retiring, "GPU retirement completed");
    };

    auto* original = world.active.get();
    pollReady(collisionRequest(100000));
    Require(partialPolls && world.pending && world.pending->gpu.complete &&
        host.WorldRevision() == 4 && world.active.get() == original && world.active->cpu->Generation == 3,
        "GPU complete before host Ready; render swap not premature");
    const auto firstCollision = host.Publication().SourceCollision;
    const auto firstQuery = host.Publication().Collision;
    world.pending->cpu->Generation = 5; // Reject a changed staged publication.
    Require(!world.CommitScriptWorld(host, error) && world.active.get() == original,
        "mismatched generation retains old render world");
    world.pending->cpu->Generation = 4;

    // No render/physics tick between services: the second callback must commit
    // the first matching pair before it can replace the staging slot.
    auto second = host.RequestCollision(collisionRequest(100001));
    Require(second.Status == NativeScriptServiceStatus::Pending && world.active->cpu->Generation == 4 &&
        world.active->cpu->SourceCollision == firstCollision &&
        world.active->cpu->BorrowedCollision == firstQuery && &world.active->Collision() == host.World() &&
        world.retiring && !world.pending, "adjacent services share exact committed scene/COL/GPU");
    verifyGroundPair();
    retire();
    pollReady(collisionRequest(100001));
    Require(host.WorldRevision() == 5 && world.CommitScriptWorld(host, error), error.c_str());
    Require(world.active->cpu->Generation == 5 && world.active->cpu->SourceCollision == host.Publication().SourceCollision &&
        &world.active->Collision() == host.World(), "second exact source publication");
    verifyGroundPair();
    Require(!world.CommitScriptWorld(host, error) && world.active->cpu->Generation == 5,
        "duplicate swap rejected");
    retire();

    const auto thirdRequest = collisionRequest(100002);
    const auto deadline = realtime_streaming::Milliseconds() + 120000;
    NativeScriptServiceResult third;
    do {
        third = host.RequestCollision(thirdRequest);
        if (world.pending) break;
        SDL_PumpEvents();
        SDL_Delay(1);
    } while (realtime_streaming::Milliseconds() < deadline);
    Require(third.Status == NativeScriptServiceStatus::Pending && world.pending && !world.pending->gpu.complete,
        "cancel fixture owns a real partial GPU upload");
    const auto ticket = host.WorldTransaction().Ticket;
    Require(host.CancelPendingWorld(thirdRequest.Id, error) && !world.pending && world.retiring &&
        world.active->cpu->Generation == 5 && host.WorldRevision() == 5,
        "cancel retires staged CPU/GPU, retains committed pair");
    Require(host.AcknowledgeWorldCancellation(ticket, error), error.c_str());
    retire();
    Require(glGetError() == GL_NO_ERROR && host.InspectSourceRng().Value->DrawCount == 0,
        "GL and RNG invariants");
    std::printf("script-world-publication-ok checks=%u generations=3,4,5 gpu-before-ready=1 adjacent-services=paired cancel=retired building-ground=owned rng-draws=0\n",
        s_Checks);
}
