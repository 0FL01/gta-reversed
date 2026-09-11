// Isolated real parser/GL probe. No runtime integration or game writes.
#include "app/platform/linux/Realtime.cpp"
#include <EGL/eglext.h>
#include <atomic>
#include <stdexcept>

namespace {
void Require(bool ok, const std::string& message) {
    if (!ok) { std::fprintf(stderr, "vehicle-queue FAIL %s\n", message.c_str()); std::abort(); }
}
std::thread::id s_Main = std::this_thread::get_id(), s_Parser;
std::atomic<bool> s_Sealed{}, s_FailTxd{};
std::atomic<size_t> s_MainCalls{}, s_WorkerCalls{}, s_ModelReads{};
std::mutex s_GateMutex;
std::condition_variable s_GateWake;
int s_Gate = -1;
bool s_Entered{}, s_Release{}, s_AutoRelease{};
std::string s_StartupOffset;
void Gate(int kind, bool automatic = false) {
    std::lock_guard lock(s_GateMutex);
    s_Gate = kind; s_Entered = s_Release = false; s_AutoRelease = automatic;
}
void Ungate() {
    std::lock_guard lock(s_GateMutex);
    s_Release = true; s_GateWake.notify_all();
}
bool Entered() { std::lock_guard lock(s_GateMutex); return s_Entered; }
uint64_t Hash(const WorldShotScene& scene) {
    uint64_t result = 14695981039346656037ull;
    const auto add = [&](const auto& data) {
        const auto* bytes = reinterpret_cast<const uint8_t*>(data.data());
        for (size_t i = 0; i < data.size() * sizeof(data[0]); ++i) { result ^= bytes[i]; result *= 1099511628211ull; }
    };
    for (const auto& mesh : scene.meshes) {
        add(mesh.pos); add(mesh.nrm); add(mesh.uv); add(mesh.triCol); add(mesh.triImg); add(mesh.dayColors); add(mesh.nightColors);
    }
    for (const auto& image : scene.images) add(image.rgba);
    return result;
}
}

// Called by link wrappers at actual world/model/TexSample/OS entry boundaries.
// The gate delays the same worker; it never creates a second parser thread.
void NativeVehicleAssetQueueProbe_Trace(int kind) {
    if (!s_Sealed) return;
    if (std::this_thread::get_id() == s_Main) { ++s_MainCalls; Require(false, "main parser/IO call after Seal"); }
    Require(eglGetCurrentContext() == EGL_NO_CONTEXT, "parser worker has no GL context");
    ++s_WorkerCalls;
    if (kind == 1) ++s_ModelReads;
    std::unique_lock lock(s_GateMutex);
    if (s_Parser == std::thread::id{}) s_Parser = std::this_thread::get_id();
    Require(s_Parser == std::this_thread::get_id(), "one parser worker identity");
    if (s_Gate != kind) return;
    s_Entered = true;
    if (s_AutoRelease) s_GateWake.wait_for(lock, std::chrono::milliseconds(100), [] { return s_Release; });
    else Require(s_GateWake.wait_for(lock, std::chrono::seconds(60), [] { return s_Release; }), "gate timeout");
    s_Gate = -1;
}
bool NativeVehicleAssetQueueProbe_FailOpen(const char* path) {
    return s_FailTxd && std::string_view(path).ends_with("models/generic/vehicle.txd");
}
void NativeVehicleAssetQueueProbe_Offset(const char* path) {
    if (s_Sealed) Require(s_StartupOffset == path,"worker repeats exact startup path offset");
    else s_StartupOffset = path;
}

class EmptyReadySource final : public NativeVehicleAssetSource {
public:
    EmptyReadySource() : NativeVehicleAssetSource({}, {}) {}
    NativeGeneratedVehicleAssetResult Load(const NativeCarGeneratorModelDefinition&,
        NativeGeneratedVehicleAsset& out) const override {
        out.reset();
        return {NativeGeneratedVehicleAssetStatus::Ready, {}};
    }
};

int main(int argc, char** argv) {
    Require(argc == 2, "usage: NativeVehicleAssetQueueProbe GAME_DIR");
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    Require(SDL_Init(SDL_INIT_EVENTS), "SDL event queue");
    const auto display = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
    Require(display != EGL_NO_DISPLAY && eglInitialize(display, nullptr, nullptr) && eglBindAPI(EGL_OPENGL_API), "EGL init");
    const EGLint attrs[]{EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_BIT,
        EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_DEPTH_SIZE,24,EGL_NONE};
    EGLConfig config{}; EGLint count{};
    Require(eglChooseConfig(display, attrs, &config, 1, &count) && count, "EGL config");
    constexpr int width = 640, height = 360;
    const EGLint size[]{EGL_WIDTH,width,EGL_HEIGHT,height,EGL_NONE};
    const auto surface = eglCreatePbufferSurface(display, config, size);
    const auto glContext = eglCreateContext(display, config, EGL_NO_CONTEXT, nullptr);
    Require(eglMakeCurrent(display, surface, surface, glContext), "EGL current");
    {
        char message[512]{}; std::string error; E2ELoadInfo load;
        Require(StreamPager_Init(argv[1], load, message, sizeof(message), {true,900,4096}), message);
        auto context = NativeCollisionContext::LoadBeforeWorker(argv[1],900,error);
        Require(bool(context), error);
        NativeCarGenerators generators;
        Require(generators.LoadBeforeWorker(argv[1],0,error), error);
        auto catalog = std::shared_ptr<const NativeCollisionAssets>(context, &context->Assets);
        auto source = std::make_shared<const NativeVehicleAssetSource>(argv[1],catalog);
        std::array<uint64_t,2> modelHashes{}, fingerprints{};
        for (size_t i = 0; i < 2; ++i) {
            NativeGeneratedVehicleAsset asset;
            const auto result = source->Load(*generators.FindModel(i ? 476 : 400),asset);
            Require(bool(result), result.Detail);
            modelHashes[i] = Hash(asset->Scene); fingerprints[i] = asset->DffFingerprint;
        }
        auto active = std::make_unique<realtime_streaming::CpuWorld>();
        Camera camera;
        const realtime_streaming::Center center{camera.x,camera.y,camera.z};
        auto overrides = std::make_shared<const NativePlacementOverrides>(std::vector<NativePlacementOverride>{});
        active->Position = center; active->Generation = 3; active->Build(false,context,overrides);
        Require(active->Error.empty(), active->Error);
        auto initialCollision = std::make_shared<NativeCollisionSnapshot>();
        Require(context->Snapshot(center.X,center.Y,*initialCollision,error,overrides),error);
        auto borrowed = std::make_shared<RealtimeGameplayWorld>();
        Require(borrowed->Rebuild(*initialCollision,error),error);
        active->SourceCollision = std::move(initialCollision);
        active->BorrowedCollision = std::move(borrowed);
        const auto* queryIdentity = &active->QueryWorld();
        const auto sourceIdentity = active->SourceCollision;
        const auto worldHash = Hash(active->Scene);
        GpuScene gpu;
        Require(gpu.Upload(active->Scene), "startup staged upload");
        RealtimeEnvironment environment;
        Require(environment.Load(argv[1],message,sizeof(message)) && environment.Upload(message,sizeof(message)),message);
        RealtimeGameplay gameplay;
        Require(gameplay.Initialize(argv[1],error) && gameplay.Spawn(active->QueryWorld(),center.X,center.Y,center.Z,camera.yaw,error),error);
        glEnable(GL_DEPTH_TEST); glEnable(GL_ALPHA_TEST); glAlphaFunc(GL_GREATER,0.5f); glDisable(GL_CULL_FACE);
        glViewport(0,0,width,height);
        const auto pixels = [&](const GpuScene& render, const WorldShotScene& scene, bool immediate) {
            camera.Apply(width,height,1600);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            environment.DrawSky(camera.x,camera.y,camera.z); environment.BeginWorld();
            if (immediate) render.Draw(scene); else render.Render();
            environment.EndWorld();
            std::vector<uint8_t> result(width*height*4);
            glReadPixels(0,0,width,height,GL_RGBA,GL_UNSIGNED_BYTE,result.data());
            Require(glGetError() == GL_NO_ERROR,"world draw GL");
            return result;
        };
        std::array<std::vector<uint8_t>,2> baseline;
        for (size_t i = 0; i < 2; ++i) {
            environment.SetHour(i ? 0 : 12);
            baseline[i] = pixels(gpu,active->Scene,true);
            Require(baseline[i] == pixels(gpu,active->Scene,false),"startup legacy/staged exact day/night pixels");
        }
        const auto exactTextures = [&](const GpuScene& render, const WorldShotScene& scene) {
            for (size_t i = 0; i < scene.images.size(); ++i) {
                const auto& image = scene.images[i]; std::vector<uint8_t> rgba(image.rgba.size());
                glBindTexture(GL_TEXTURE_2D,render.textures[i]); glGetTexImage(GL_TEXTURE_2D,0,GL_RGBA,GL_UNSIGNED_BYTE,rgba.data());
                Require(rgba == image.rgba,"exact texture RGBA");
            }
        };
        exactTextures(gpu,active->Scene);
        const auto textureCount = active->Scene.images.size();
        std::printf("vehicle-queue world-textures=%zu legacy-reference=1460 center=%.3f,%.3f,%.3f\n",textureCount,center.X,center.Y,center.Z);
        generators.SealStartup();
        s_Sealed = true;
        {
            realtime_streaming::Worker defaultWorker(false);
            Require(defaultWorker.RequestVehicle({7,1,0,0,*generators.FindModel(400)}).Status == NativeVehicleAssetAdmission::Disabled,
                "legacy constructor has disabled mailbox");
            defaultWorker.Stop({},{});
        }
        {
            realtime_streaming::Worker emptyReadyWorker(false, {}, {}, 3, std::make_shared<const EmptyReadySource>());
            const auto submission = emptyReadyWorker.RequestVehicle({7,1,0,0,*generators.FindModel(400)});
            Require(submission.Status == NativeVehicleAssetAdmission::Accepted,"empty Ready source admitted");
            const auto deadline = realtime_streaming::Milliseconds()+120000;
            while (emptyReadyWorker.VehiclePhase(submission.Ticket) != NativeVehicleAssetPhase::Ready) {
                Require(realtime_streaming::Milliseconds()<deadline,"empty Ready source timeout");
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            const auto completion = emptyReadyWorker.TakeVehicleReady(submission.Ticket);
            Require(completion && completion->Result.Status == NativeGeneratedVehicleAssetStatus::Error &&
                !completion->Asset && completion->Result.Detail == "vehicle asset source returned Ready without a CPU packet",
                "Ready cannot publish an empty CPU packet");
            emptyReadyWorker.Stop({},{});
        }
        const std::weak_ptr<const NativeVehicleAssetSource> sourceLife = source;
        auto workerOwner = std::make_unique<realtime_streaming::Worker>(true,context,overrides,3,source);
        auto& worker = *workerOwner;
        source.reset(); catalog.reset(); // worker owns startup path/catalog
        Require(!sourceLife.expired(),"worker retains vehicle source owner");
        size_t events = 0, ticks = 0;
        double maxServiceMs = 0;
        const auto pump = [&] {
            const auto start = realtime_streaming::Milliseconds();
            SDL_Event input{}; input.type = SDL_EVENT_USER; Require(SDL_PushEvent(&input),"push event");
            SDL_Event event{}; while (SDL_PollEvent(&event)) if (event.type == SDL_EVENT_USER) ++events;
            const auto before = gameplay.State().Ticks;
            gameplay.Tick(1.0/60,{},active->QueryWorld());
            Require(gameplay.State().Ticks > before && std::isfinite(gameplay.State().Ped.Z),"physics continuity"); ++ticks;
            Require(&active->QueryWorld() == queryIdentity && active->SourceCollision == sourceIdentity && active->Generation == 3,
                "borrowed source/query/generation identity during model work");
            maxServiceMs = std::max(maxServiceMs,realtime_streaming::Milliseconds()-start);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        };
        const auto wait = [&](const auto& predicate) {
            const auto end = realtime_streaming::Milliseconds()+120000;
            while (!predicate()) { Require(realtime_streaming::Milliseconds()<end,"worker timeout"); pump(); }
        };
        NativeVehicleAssetRequest request{7,1,11,101,*generators.FindModel(400)};
        const auto submit = [&] {
            auto result = worker.RequestVehicle(request);
            Require(result.Status == NativeVehicleAssetAdmission::Accepted,"accepted typed request");
            return result.Ticket;
        };
        const auto ready = [&](const auto& ticket) {
            wait([&] { return worker.VehiclePhase(ticket) == NativeVehicleAssetPhase::Ready; });
            auto result = worker.TakeVehicleReady(ticket); Require(bool(result),"take CPU completion"); return result;
        };

        // Waiting cancellation while an actual world parser is blocked.
        Gate(0); worker.Request(center,true); wait(Entered);
        auto ticket = submit();
        Require(worker.VehiclePhase(ticket) == NativeVehicleAssetPhase::Waiting,"waiting model");
        Require(worker.RequestVehicle(request).Status == NativeVehicleAssetAdmission::Existing,"stable repeat");
        auto conflict = request; ++conflict.Definition.Line;
        Require(worker.RequestVehicle(conflict).Status == NativeVehicleAssetAdmission::Conflict,"full definition identity");
        conflict = request; ++conflict.SourceFrame;
        Require(worker.RequestVehicle(conflict).Status == NativeVehicleAssetAdmission::Conflict,"source frame identity");
        Require(worker.CancelVehicle(ticket) && worker.VehiclePhase(ticket) == NativeVehicleAssetPhase::Cancelled,"waiting cancel");
        const auto beforeReads = s_ModelReads.load();
        Ungate();
        std::unique_ptr<realtime_streaming::CpuWorld> pending;
        wait([&] { pending = worker.TakeReady(); return bool(pending); });
        Require(s_ModelReads == beforeReads && !worker.TakeVehicleReady(ticket),"waiting cancellation did not load/publish");
        Require(pending->Error.empty() && pending->Generation == 4 && pending->Overrides == overrides &&
            pending->SourceCollision->Overrides == overrides && Hash(pending->Scene) == worldHash,"world generation 3->4 exact source/render");

        // The world candidate remains busy/waiting for main GPU budget. A real
        // model can still run, and processing cancellation waits for its finish.
        ++request.Request; Gate(1); ticket = submit(); wait(Entered);
        Require(worker.VehiclePhase(ticket) == NativeVehicleAssetPhase::Running,"running model");
        Require(worker.CancelVehicle(ticket) && worker.VehiclePhase(ticket) == NativeVehicleAssetPhase::Cancelling,"running cancel deferred");
        ++request.Request;
        Require(worker.RequestVehicle(request).Status == NativeVehicleAssetAdmission::Busy,"cancelled parser retains bounded slot");
        for (int i = 0; i < 10; ++i) pump();
        Ungate(); wait([&] { return worker.VehiclePhase(ticket) == NativeVehicleAssetPhase::Cancelled; });
        Require(!worker.TakeVehicleReady(ticket),"running result discarded");
        std::array<std::shared_ptr<const NativeVehicleAssetCompletion>,2> retained;
        for (size_t i = 0; i < 2; ++i) {
            request.Definition = *generators.FindModel(i ? 476 : 400); ++request.Request;
            ticket = submit(); retained[i] = ready(ticket);
            const auto& completion = *retained[i]; const auto& asset = completion.Asset;
            Require(bool(completion.Result) && asset && Hash(asset->Scene) == modelHashes[i] &&
                asset->DffFingerprint == fingerprints[i] && !asset->Frames.empty() && asset->Collision &&
                !asset->Collision->SourceChunk.empty() && !asset->ModelTxdSource.empty() && !asset->CommonTxdSource.empty(),"real model DFF/TXD/COL/frame packet");
            Require(asset->Pose == (i ? NativeGeneratedVehiclePacket::PoseContract::FreshParkedAbandoned476 :
                NativeGeneratedVehiclePacket::PoseContract::ParkedBind400),"exact 400/476 S0 pose contract");
            Require(completion.WorkerGeneration == 4 && completion.Ticket.Identity->SourceFrame == 101 &&
                completion.Ticket.Identity->Owner == 7 && completion.Ticket.Identity->Revision == 11 &&
                completion.Ticket.Identity->Definition.ModelId == (i ? 476 : 400),"packet generation and immutable request provenance");
            std::printf("vehicle-queue model=%d frames=%zu sourceFrame=%llu generation=%llu dff=%llu images=%zu CPU-only=1\n",
                asset->Definition.ModelId,asset->Frames.size(),(unsigned long long)completion.Ticket.Identity->SourceFrame,
                (unsigned long long)completion.WorkerGeneration,(unsigned long long)asset->DffFingerprint,asset->Scene.images.size());
        }
        ++request.Request; ticket = submit();
        wait([&] { return worker.VehiclePhase(ticket) == NativeVehicleAssetPhase::Ready; });
        Require(worker.RequestVehicle(request).Status == NativeVehicleAssetAdmission::Existing,"repeat ready has same result");
        auto busy = request; ++busy.Request;
        Require(worker.RequestVehicle(busy).Status == NativeVehicleAssetAdmission::Busy,"ready slot bounded");
        Require(worker.CancelVehicle(ticket),"ready cancellation");
        wait([&] { return worker.VehiclePhase(ticket) == NativeVehicleAssetPhase::Cancelled; });
        Require(!worker.TakeVehicleReady(ticket),"ready cancelled packet not published");
        size_t unsupportedCount = 0;
        for (const auto& definition : generators.ModelDefinitions()) if (definition.ModelId != 400 && definition.ModelId != 476) {
            request.Definition = definition; ++request.Request;
            auto unsupported = ready(submit());
            Require(unsupported->Result.Status == NativeGeneratedVehicleAssetStatus::Unsupported && !unsupported->Asset,"unsupported model explicit");
            ++unsupportedCount;
        }
        Require(unsupportedCount == 210,"every other actual IDE model unsupported");
        request.Definition = *generators.FindModel(400); ++request.Request;
        s_FailTxd = true;
        auto failed = ready(submit());
        Require(failed->Result.Status == NativeGeneratedVehicleAssetStatus::Error && !failed->Asset && !failed->Result.Detail.empty(),"real TXD read failure");
        s_FailTxd = false; ++request.Request;
        Require(bool(ready(submit())->Result),"repeat after error reparses successfully");

        // Main GL upload and legacy comparison while the worker owns all RW.
        GpuScene candidate;
        while (!candidate.complete) { Require(candidate.UploadStep(pending->Scene,realtime_streaming::Milliseconds()+1),"budgeted world upload"); pump(); }
        exactTextures(candidate,pending->Scene);
        for (size_t i = 0; i < 2; ++i) {
            environment.SetHour(i ? 0 : 12);
            Require(pixels(candidate,pending->Scene,false) == baseline[i] && pixels(gpu,active->Scene,true) == baseline[i],
                "after worker model read world pixels exact legacy/staged day/night");
        }
        Require(s_MainCalls == 0,"before/after main wrapper trace zero");
        // Simulate the coordinator's publication/CPU retirement handshake. The
        // caller-owned model packets survive this independent world generation.
        worker.Retire(std::move(active)); active = std::move(pending); worker.Release();
        queryIdentity = &active->QueryWorld();
        // No more pump assertions pinned to startup generation after publication.
        worker.Request(center,true); ++request.Request;
        auto concurrent = submit();
        const auto end = realtime_streaming::Milliseconds()+120000;
        std::shared_ptr<const NativeVehicleAssetCompletion> concurrentReady;
        while (!pending || !concurrentReady) {
            if (!pending) pending = worker.TakeReady();
            if (!concurrentReady) concurrentReady = worker.TakeVehicleReady(concurrent);
            Require(realtime_streaming::Milliseconds()<end,"world/model fairness");
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        Require(pending->Generation == 5 && active->Generation == 4 && bool(concurrentReady->Result),"concurrent world/model no generation reset or starvation");
        // Stop during a parser read. A timed gate permits join on main without a
        // helper thread; pending/active/catalog/GL all remain alive until return.
        ++request.Request; Gate(1,true); ticket = submit();
        const auto waitEntered = [&] {
            const auto deadline = realtime_streaming::Milliseconds()+120000;
            while (!Entered()) {
                Require(realtime_streaming::Milliseconds()<deadline,"shutdown gate entry timeout");
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        };
        waitEntered();
        worker.Stop(std::move(active),std::move(pending));
        Require(worker.RequestVehicle(request).Status == NativeVehicleAssetAdmission::Stopped &&
            worker.VehiclePhase(ticket) == NativeVehicleAssetPhase::Stopped && !worker.TakeVehicleReady(ticket),"stopped rejects and drains result");
        worker.Request(center,true); worker.Stop({},{});
        Require(!worker.TakeReady(),"shutdown drains pending world");
        // Dedicated stopped-ready and stopped-waiting instances still use one
        // parser thread at a time; reset the trace identity after each join.
        for (bool waiting : {false,true}) {
            s_Parser = {};
            auto shutdownSource = std::make_shared<const NativeVehicleAssetSource>(argv[1],
                std::shared_ptr<const NativeCollisionAssets>(context,&context->Assets));
            realtime_streaming::Worker shutdownWorker(false,{}, {},5,shutdownSource);
            if (waiting) {
                Gate(0,true); shutdownWorker.Request(center,true);
                waitEntered();
            }
            ++request.Request;
            const auto shutdownTicket = shutdownWorker.RequestVehicle(request).Ticket;
            if (waiting) Require(shutdownWorker.VehiclePhase(shutdownTicket) == NativeVehicleAssetPhase::Waiting,"shutdown waiting slot");
            else {
                const auto deadline = realtime_streaming::Milliseconds()+120000;
                while (shutdownWorker.VehiclePhase(shutdownTicket) != NativeVehicleAssetPhase::Ready) {
                    Require(realtime_streaming::Milliseconds()<deadline,"shutdown ready timeout");
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }
            const auto reads = s_ModelReads.load();
            shutdownWorker.Stop({},{});
            Require(!shutdownWorker.TakeVehicleReady(shutdownTicket) &&
                shutdownWorker.RequestVehicle(request).Status == NativeVehicleAssetAdmission::Stopped &&
                s_ModelReads == reads,"shutdown waiting/ready drains without extra model read");
        }
        s_Sealed = false;
        workerOwner.reset();
        Require(sourceLife.expired(),"vehicle source released after joined worker destruction");
        StreamPager_Shutdown();
        const std::weak_ptr<const NativeCollisionContext> contextLife = context;
        context.reset();
        Require(contextLife.expired(),"startup catalog context owner released");
        for (size_t i = 0; i < 2; ++i) Require(Hash(retained[i]->Asset->Scene) == modelHashes[i] &&
            retained[i]->Asset->DffFingerprint == fingerprints[i],"retained CPU packet survives worker/pager/catalog shutdown");
        Require(events == ticks && ticks > 10 && s_MainCalls == 0 && s_WorkerCalls > 0,"event/physics and wrapper trace");
        std::printf("vehicle-queue PASS events=%zu ticks=%zu maxServiceMs=%.3f workerCalls=%zu modelReads=%zu unsupported=%zu mainCallsBeforeAfter=0 exactPixels=day,night exactTextures=%zu generations=3,4,5 joined=waiting,running,ready\n",
            events,ticks,maxServiceMs,s_WorkerCalls.load(),s_ModelReads.load(),unsupportedCount,textureCount);
    }
    eglMakeCurrent(display,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);
    eglDestroyContext(display,glContext); eglDestroySurface(display,surface); eglTerminate(display); SDL_Quit();
}
