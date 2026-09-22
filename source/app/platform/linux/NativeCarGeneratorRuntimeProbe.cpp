#include "app/platform/linux/NativeCarGeneratorRuntime.h"
#include "app/platform/linux/RealtimeScriptHost.h"
#include "app/platform/linux/RealtimeStreaming.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <source_location>
#include <stdexcept>

namespace {
void Require(bool ok, const std::string& error,
    const std::source_location& where = std::source_location::current()) {
    if (!ok) throw std::runtime_error("line " + std::to_string(where.line()) + ": " + error);
}

NativeCarGeneratorCreateRequest FirstConstructor(const char* gameDir) {
    std::ifstream scm(std::string(gameDir) + "/data/script/main.scm", std::ios::binary);
    Require(bool(scm), "legal main.scm open");
    // LABELLED source-byte fixture. No VM dispatch or execution is inferred.
    scm.seekg(201132);
    std::array<std::uint8_t, 43> bytes{};
    Require(bool(scm.read(reinterpret_cast<char*>(bytes.data()), bytes.size())), "014B source extent");
    std::size_t offset = 0;
    const auto read = [&](std::size_t size) {
        Require(offset + size <= bytes.size(), "014B bounded fixture decoder");
        std::uint32_t value = 0;
        for (std::size_t i = 0; i < size; ++i) value |= std::uint32_t(bytes[offset++]) << (i * 8);
        return value;
    };
    Require(read(2) == 0x014b, "actual first generator opcode");
    NativeCarGeneratorCreateRequest request;
    request.Id = {7001, 680, 207007};
    for (auto* field : {&request.Position.X, &request.Position.Y, &request.Position.Z, &request.AngleDegrees}) {
        Require(read(1) == 6, "014B float operand");
        *field = std::bit_cast<float>(read(4));
    }
    for (auto* field : {&request.ModelId, &request.PrimaryColor, &request.SecondaryColor, &request.ForceSpawn,
        &request.AlarmChance, &request.DoorLockChance, &request.MinDelay, &request.MaxDelay}) {
        const auto tag = read(1);
        Require(tag == 4 || tag == 5, "014B integer operand");
        *field = tag == 4 ? std::int8_t(read(1)) : std::int16_t(read(2));
    }
    Require(read(1) == 2 && read(2) == 7824 && offset == bytes.size(), "014B actual global output/extent");
    Require(request.ModelId == 476 && request.Position.X == 325.1199951171875f &&
        request.Position.Y == 2537.10009765625f && request.AngleDegrees == 180 && request.MaxDelay == 10000,
        "actual legal first generator fixture payload");
    return request;
}

// Independently constructed test projection of the real controller camera.
// Production passes the installed GL matrix capture instead.
RealtimeHudPriceView CameraFixture(const RealtimeGameplayCamera& camera) {
    const auto normalize = [](RealtimeVec3 v) {
        const float length = std::sqrt(v.X*v.X + v.Y*v.Y + v.Z*v.Z);
        return RealtimeVec3{v.X/length, v.Y/length, v.Z/length};
    };
    const auto cross = [](RealtimeVec3 a, RealtimeVec3 b) {
        return RealtimeVec3{a.Y*b.Z-a.Z*b.Y, a.Z*b.X-a.X*b.Z, a.X*b.Y-a.Y*b.X};
    };
    const auto dot = [](RealtimeVec3 a, RealtimeVec3 b) { return a.X*b.X+a.Y*b.Y+a.Z*b.Z; };
    const auto forward = normalize({camera.Target.X-camera.Position.X, camera.Target.Y-camera.Position.Y,
        camera.Target.Z-camera.Position.Z});
    const auto right = normalize(cross(forward, {0,0,1}));
    const auto up = cross(right, forward);
    RealtimeHudPriceView result;
    result.ModelView = {right.X,up.X,-forward.X,0, right.Y,up.Y,-forward.Y,0, right.Z,up.Z,-forward.Z,0,
        -dot(right,camera.Position),-dot(up,camera.Position),dot(forward,camera.Position),1};
    result.NearClip = .1f; result.FarClip = 1600; result.Fov = 70;
    const float scale = 1/std::tan(70.0f*3.14159265358979323846f/360);
    result.Projection = {scale/(16.0f/9),0,0,0, 0,scale,0,0,
        0,0,-(result.FarClip+result.NearClip)/(result.FarClip-result.NearClip),-1,
        0,0,-2*result.FarClip*result.NearClip/(result.FarClip-result.NearClip),0};
    return result;
}

void Bind(NativeVehiclePool& pool, std::string& error) {
    Require(pool.BindProducer(NativeVehicleProducer::NativeScm, error), error);
    Require(pool.BindProducer(NativeVehicleProducer::NativeGameplayController, error), error);
    Require(pool.BindProducer(NativeVehicleProducer::CarGenerator, error), error);
    Require(pool.SealProducerExtent(error), error);
}
} // namespace

int main(int argc, char** argv) try {
    Require(argc == 2, "game directory required");
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::string error;
    char message[512]{};
    E2ELoadInfo info;
    Require(StreamPager_Init(argv[1], info, message, sizeof(message), {true,300,1200}), message);
    {
        auto context = NativeCollisionContext::LoadBeforeWorker(argv[1], 900, error);
        Require(bool(context), error);
        RealtimeGameplay gameplay;
        RealtimeScriptHost host(gameplay);
        Require(host.InitializeBeforeWorker(argv[1], error, context), error);
        Require(host.RunPass(1000).Status == NativeScriptStatus::Waiting && host.Session().Threads()[0].Commands == 53,
            "actual main53 supplies source-created controller/activity/camera");
        Require(!gameplay.State().CarPresent && !gameplay.State().InVehicle &&
            gameplay.Activity().Authority == NativePlayerActivityAuthority::SourceBacked, "actual new-game no-car owner");
        NativeCarGenerators registry;
        Require(registry.LoadBeforeWorker(argv[1], 0, error), error);
        const auto source = FirstConstructor(argv[1]);
        const auto ref = registry.Create(source, 0).Reference;
        Require(ref.Value == 0 && registry.Resolve(ref)->GenerateCount == 0 && registry.Resolve(ref)->Vehicle.Value == -1,
            "LABELLED legal first014B constructor fixture, not VM execution");
        NativeVehiclePool pool;
        Bind(pool, error); // explicit fixture extent; host binding is parent integration
        realtime_streaming::CpuWorld world;
        const auto root = gameplay.State().PedRoot;
        world.Position = {root.X,root.Y,root.Z}; world.Generation = 1;
        world.Build(true, context, host.InitialPlacementOverrides());
        Require(world.Error.empty() && world.SourceCollision && world.QueryWorld().TriangleCount(), world.Error);
        NativeCarGeneratorRuntime runtime(registry, gameplay, pool, host.State());
        NativeCarGeneratorRuntimeInput input;
        input.Camera = CameraFixture(gameplay.Camera());
        for (std::uint64_t frame = 0; frame < 4; ++frame) {
            input.Frame = frame;
            auto vehicles = pool.Publish(frame, error); Require(bool(vehicles), error);
            Require(runtime.Tick(world, input, vehicles).Status == NativeScriptServiceStatus::Ready, "actual quarter process");
            Require(runtime.Frame().Process.ProcessCounterAfter == (frame+1)%4 && runtime.Frame().Demands.empty(),
                "source Process increments quarter before traversal");
            Require(runtime.Frame().PlayerCenter == NativeScriptPosition{root.X,root.Y,root.Z} &&
                runtime.Frame().GameMs == host.State().TimeMs && runtime.Frame().ClockHour == host.State().Clock.Hours &&
                runtime.Frame().ParkedCars == 0 && runtime.Frame().FreeVehicleSlots == 110,
                "owned player root/clock/native parked census/capacity");
            const auto revision = runtime.Frame().Revision;
            Require(runtime.Tick(world, input, vehicles).Status == NativeScriptServiceStatus::Error &&
                runtime.Frame().Revision == revision, "duplicate frame cannot repeat source quarter side effects");
        }
        Require(runtime.Frame().Process.Actions.size() == 1 &&
            runtime.Frame().Process.Actions[0].Decision == NativeCarGeneratorDecision::Timer,
            "actual constructor time gate precedes disabled gate");
        const auto camera = gameplay.Camera();
        const NativeScriptPosition inFront{camera.Target.X,camera.Target.Y,camera.Target.Z};
        const NativeScriptPosition behind{2*camera.Position.X-camera.Target.X,2*camera.Position.Y-camera.Target.Y,
            2*camera.Position.Z-camera.Target.Z};
        const auto visible = NativeCarGeneratorRuntime::Observe(world, ref, inFront, camera, input.Camera);
        const auto hidden = NativeCarGeneratorRuntime::Observe(world, ref, behind, camera, input.Camera);
        Require(visible.FrustumTested && visible.InFrustum &&
            visible.Observation.Visibility == NativeCarGeneratorVisibility::Unknown && hidden.FrustumTested &&
            !hidden.InFrustum && hidden.Observation.Visibility == NativeCarGeneratorVisibility::HiddenOrOccluded,
            "real controller camera projection excludes behind-eye point but cannot invent COcclusion clearance");
        const auto local = NativeCarGeneratorRuntime::Observe(world, ref, {root.X,root.Y,root.Z}, camera, input.Camera);
        RealtimeVec3 independent;
        NativeCollisionHit provenance;
        Require(world.QueryWorld().Raycast({root.X,root.Y,root.Z+1}, {root.X,root.Y,-1000}, independent, &provenance) &&
            local.VerticalRayHit && local.VerticalHit.Z == independent.Z && local.VerticalSource.Model == provenance.Model &&
            !provenance.Library.empty() && !provenance.Ipl.empty() &&
            local.Observation.Ground == NativeCarGeneratorGround::Unknown &&
            local.Observation.Blockage == NativeCarGeneratorBlockage::Unknown,
            "independently witnessed real source COL ray, not source building-mask or vehicle/ped broadphase completion");
        std::printf("source-col-witness model=%s library=%s ipl=%s record=%u z=%.6f missing=%zu\n",
            provenance.Model.c_str(), provenance.Library.c_str(), provenance.Ipl.c_str(), provenance.Record,
            independent.Z, world.SourceCollision->MissingModels);

        NativeCarGenerators motionRegistry;
        NativeCarGeneratorRuntime motion(motionRegistry, gameplay, pool, host.State());
        const auto beforeMotion = gameplay.State();
        gameplay.Tick(1.0/60, {.Forward = .5f}, world.QueryWorld());
        Require(host.AdvanceTime(16, error), error);
        input.Frame = 4; input.Camera = CameraFixture(gameplay.Camera());
        auto motionVehicles = pool.Publish(input.Frame, error); Require(bool(motionVehicles), error);
        NativeVehiclePool foreignPool;
        Bind(foreignPool, error);
        auto foreign = foreignPool.Publish(input.Frame, error); Require(bool(foreign), error);
        Require(motion.Tick(world, input, foreign).Status == NativeScriptServiceStatus::Error && !motion.Frame().Revision,
            "foreign vehicle authority rejected before Process");
        auto wrongCamera = input;
        wrongCamera.Camera->ModelView[12] += 10;
        Require(motion.Tick(world, wrongCamera, motionVehicles).Status == NativeScriptServiceStatus::Error &&
            !motion.Frame().Revision, "camera matrices must match actual owned gameplay camera");
        NativeScriptServiceStatus workerResult = NativeScriptServiceStatus::Ready;
        std::thread wrongThread([&] { workerResult = motion.Tick(world, input, motionVehicles).Status; });
        wrongThread.join();
        Require(workerResult == NativeScriptServiceStatus::Error && !motion.Frame().Revision,
            "worker cannot query/mutate main-thread Process state");
        Require(motion.Tick(world, input, motionVehicles).Status == NativeScriptServiceStatus::Ready,
            "actual native gameplay motion interval");
        const auto& moved = gameplay.State();
        const auto interval = moved.SimulatedSeconds - beforeMotion.SimulatedSeconds;
        Require(interval > 0 && (moved.PedRoot.X != beforeMotion.PedRoot.X || moved.PedRoot.Y != beforeMotion.PedRoot.Y) &&
            motion.Frame().MeasuredPlayerSpeed[0] == float((moved.PedRoot.X-beforeMotion.PedRoot.X)/interval/50) &&
            motion.Frame().MeasuredPlayerSpeed[1] == float((moved.PedRoot.Y-beforeMotion.PedRoot.Y)/interval/50),
            "measured horizontal native velocity comes from real collision-resolved controller displacement");

        // Move only this labelled probe's source-created player near the legal
        // constructor. This is a source-world runtime fixture, not mission play.
        realtime_streaming::CpuWorld airfield;
        airfield.Position = {325,2537,17.5f}; airfield.Generation = 2;
        airfield.Build(true, context, host.InitialPlacementOverrides());
        Require(airfield.Error.empty(), airfield.Error);
        Require(gameplay.SpawnScriptPlayer(airfield.QueryWorld(), {350,2537,17.5f}, error), error);
        Require(host.AdvanceTime(20, error), error);
        Require(registry.Switch({{7002,1,207050},ref,101}, 0).Status == NativeScriptServiceStatus::Ready, "LABELLED enable fixture");
        NativeCarGeneratorRuntime pending(registry, gameplay, pool, host.State());
        registry.SealStartup(); host.SealStartup(); // all source IO is over
        const auto registryRevision = registry.Revision();
        for (std::uint64_t frame = 5; frame < 9; ++frame) {
            input.Frame = frame; input.Camera = CameraFixture(gameplay.Camera());
            auto vehicles = pool.Publish(frame, error); Require(bool(vehicles), error);
            const auto result = pending.Tick(airfield, input, vehicles);
            Require(result.Status == (frame == 8 ? NativeScriptServiceStatus::Pending : NativeScriptServiceStatus::Ready),
                result.Message);
        }
        const auto& demand = pending.Frame().Demands.at(0);
        Require(demand.Action.Requirement == NativeCarGeneratorRequirement::CollisionBlockage &&
            demand.Action.Request.ModelId == 476 && demand.Action.Request.VehicleType == NativeVehicleType::Plane &&
            demand.GeneratorState.Provenance.ByteOffset == 207007 && demand.WorldGeneration == 2 &&
            demand.RegistryRevision == registryRevision && demand.Vehicles->Owner() == pool.Owner() &&
            demand.Collision == airfield.SourceCollision && pending.ResolveDemand(demand.Id) == &demand,
            "exact identity/generation/source014B/candidate retained pending demand");
        const auto pendingRevision = pending.Frame().Revision;
        const auto counter = registry.ProcessCounter();
        const auto poolRevision = pool.Revision();
        for (unsigned retry = 0; retry < 8; ++retry) {
            ++input.Frame;
            auto vehicles = pool.Publish(input.Frame, error); Require(bool(vehicles), error);
            Require(pending.Tick(airfield, input, vehicles).Status == NativeScriptServiceStatus::Pending &&
                pending.Frame().Revision == pendingRevision && registry.ProcessCounter() == counter &&
                registry.Revision() == registryRevision && pool.Revision() == poolRevision &&
                pending.ResolveDemand(demand.Id) == &demand, "pending retry does not overwrite or repeat Process");
        }
        Require(pool.Census().Alive == 0 && pool.Events().empty() && registry.Resolve(ref)->Vehicle.Value == -1,
            "no detached pool-only vehicle or invented spawned reference");
        NativeCarGenerators randomRegistry;
        auto randomRequest = source;
        randomRequest.Id = {8000,1,1}; randomRequest.ModelId = -1;
        const auto disabled = randomRegistry.Create(randomRequest, 0).Reference;
        randomRequest.Id = {8000,2,2};
        const auto random = randomRegistry.Create(randomRequest, 0).Reference;
        Require(disabled.Value == 0 && random.Value == 1 &&
            randomRegistry.Switch({{8000,3,3},random,101}, 0).Status == NativeScriptServiceStatus::Ready,
            "LABELLED random-population fixture");
        randomRegistry.SealStartup();
        NativeCarGeneratorRuntime randomRuntime(randomRegistry, gameplay, pool, host.State());
        ++input.Frame;
        auto randomVehicles = pool.Publish(input.Frame, error); Require(bool(randomVehicles), error);
        Require(randomRuntime.Tick(airfield, input, randomVehicles).Status == NativeScriptServiceStatus::Unsupported &&
            randomRuntime.Frame().Demands.size() == 1 &&
            randomRuntime.Frame().Demands[0].Action.Requirement == NativeCarGeneratorRequirement::RandomPopulationSelection &&
            randomRuntime.Frame().Demands[0].Id.Owner != demand.Id.Owner &&
            randomRuntime.Frame().Demands[0].Action.Request.ModelId == -1 && randomRegistry.Resolve(random)->ModelId == -1,
            "shared RNG/population selection remains exact owned Unsupported request without selecting a fallback");
        Require(host.Session().Threads()[0].Commands == 53, "fixture made no new VM execution claim");
        std::printf("NativeCarGeneratorRuntimeProbe PASS sourceQuarter=1,2,3,0 first014B=LABELLED-constructor-fixture "
            "activity=actual-main53 pending=model476-source-vehicle-ped-COL demand-retained=8 no-spawn=1 "
            "visibility=actual-camera-projection-fixture source-occlusion=unfulfilled motion=owned-displacement "
            "random=typed-unsupported foreign-authority=reject wrong-thread=reject\n");
    }
    StreamPager_Shutdown();
    return 0;
} catch (const std::exception& exception) {
    std::fprintf(stderr, "NativeCarGeneratorRuntimeProbe FAIL %s\n", exception.what());
    return 2;
}
