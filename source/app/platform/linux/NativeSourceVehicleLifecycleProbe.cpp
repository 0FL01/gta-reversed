#include "NativeSourceVehicleLifecycle.h"
#include "NativeCarGenerators.h"
#include "NativeCollisionAssets.h"
#include "RealtimeStreaming.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <ranges>
#include <thread>

namespace {
std::size_t s_Checks;
void Check(bool condition, const std::string& message) {
    ++s_Checks;
    if (!condition) throw std::runtime_error(message);
}
NativeGarageMatrix Matrix(float x = 0, float y = 0, float z = 1) {
    return {{x, y, z}, {{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}}};
}
NativeSourceGroundSnapshot Ground(std::uint64_t generation) {
    NativeSourceGroundSnapshot ground;
    ground.WorldGeneration = generation; ground.MetadataRevision = 7;
    ground.CompleteNormalSector = ground.MembershipAndOverridesVerified = ground.Deduplicated = true;
    ground.MinXY = {-20, -20}; ground.MaxXY = {20, 20};
    return ground;
}
NativeSourcePedWorldPed Ped(std::uint64_t identity, NativeCollisionVector position) {
    NativeSourcePedWorldPed ped;
    ped.Identity = identity; ped.HasPlayerData = true; ped.Physical.Position = position;
    ped.Physical.Mass = 70; ped.Physical.Elasticity = 0.05f;
    ped.Physical.IsPed = ped.Physical.DisableTurnForce = ped.Physical.ApplyGravity = true;
    ped.Physical.UsesCollision = ped.Physical.Collidable = true;
    return ped;
}
NativeSourcePadFrame Frame(std::uint64_t sequence, std::uint8_t buttons, std::uint8_t previous,
    std::int16_t moveX = 0) {
    NativeSourcePadFrame frame;
    frame.Sample = {sequence, sequence * 10, moveX, 0, buttons}; frame.Down = buttons;
    frame.Pressed = buttons & ~previous; frame.Released = previous & ~buttons;
    return frame;
}
std::shared_ptr<NativeCollisionModel> Floor() {
    auto model = std::make_shared<NativeCollisionModel>();
    model->Name = "lifecycle-floor"; model->Version = 2; model->Flags = 2;
    model->Min = {-4, -4, -.125f}; model->Max = {4, 4, .125f}; model->BoundRadius = 6;
    model->Vertices = {{-4, -4, 0}, {4, -4, 0}, {4, 4, 0}, {-4, 4, 0}};
    model->Faces = {{{0, 1, 2}, {1, 0, 0, 10}}, {{0, 2, 3}, {1, 0, 0, 10}}};
    return model;
}
std::shared_ptr<const NativeVehicleAssetCompletion> Completion(std::uint64_t generation,
    std::uint64_t request, const char* gameDir, const NativeCarGeneratorModelDefinition& definition,
    std::shared_ptr<const NativeCollisionAssets> catalog) {
    auto source = std::make_shared<const NativeVehicleAssetSource>(gameDir, std::move(catalog));
    realtime_streaming::Worker worker(false, {}, {}, generation, source);
    const auto submitted = worker.RequestVehicle({99, request, generation, 1, definition});
    Check(submitted.Status == NativeVehicleAssetAdmission::Accepted && submitted.Ticket.Sequence == 1,
        "real parser worker accepts exact vehicle request");
    const auto deadline = realtime_streaming::Milliseconds() + 120000;
    while (worker.VehiclePhase(submitted.Ticket) != NativeVehicleAssetPhase::Ready) {
        if (realtime_streaming::Milliseconds() >= deadline)
            throw std::runtime_error("real parserreal parser19restorer62e59 player27188 retail1,12,21.09,8E6271,52.27.1,299.9 ; originaldata2,255 ; trace580717202,605.97484,2.77514 - final");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    auto completion = worker.TakeVehicleReady(submitted.Ticket);
    worker.Stop({}, {});
    Check(completion && completion->WorkerGeneration == generation && completion->Ticket == submitted.Ticket &&
        completion->Result.Status == NativeGeneratedVehicleAssetStatus::Ready && completion->Asset,
        "real parser worker publishes generation-qualified CPU vehicle packet");
    return completion;
}
}

int main(int argc, char** argv) try {
    Check(argc == 2, "usage: NativeSourceVehicleLifecycleProbe /game");
    std::string error;
    NativeCarGenerators definitions;
    Check(definitions.LoadBeforeWorker(argv[1], 0, error), error);
    const auto* landstal = definitions.FindModel(400);
    Check(landstal && landstal->ModelName == "landstal" && landstal->HandlingName == "LANDSTAL",
        "exact model400 identity");
    NativeCollisionPopulation population; population.IncludesStreamed = true;
    for (const auto& definition : definitions.ModelDefinitions())
        population.Models.emplace(definition.ModelId, NativeCollisionIde{definition.ModelName, false});
    auto collision = std::make_shared<NativeCollisionAssets>();
    Check(collision->Load(argv[1], population, error), error);
    HandlingParams handling; char message[512]{};
    Check(Handling_Load(argv[1], "LANDSTAL", handling, message, sizeof(message)), message);
    CarPoseMeasure suspension{};
    Check(CarPose_Measure(argv[1], "landstal", suspension, message, sizeof(message)), message);

    NativeSourceSurfaces surfaces;
    Check(surfaces.Load(argv[1], error), error);
    auto floor = Floor();
    auto world10 = std::make_shared<NativeCollisionSnapshot>();
    NativeCollisionInstance worldFloor;
    worldFloor.Placement.ModelId = 3991; worldFloor.Placement.Model = "lifecycle-floor";
    worldFloor.Model = floor; worldFloor.Basis = Matrix().Basis;
    worldFloor.Min = floor->Min; worldFloor.Max = floor->Max;
    world10->Instances.push_back(worldFloor);
    NativeSourcePedWorld pedWorld10;
    Check(pedWorld10.Load(Ground(10), error), error);
    const auto completion10 = Completion(10, 1, argv[1], *landstal, collision);
    const auto asset = completion10->Asset;
    Check(asset && asset->Collision && asset->Definition.ModelId == 400 &&
        asset->DffSource == "gta3.img:landstal.dff", "real source asset retained");

    NativeSourceVehicleLifecycle lifecycle;
    Check(!lifecycle.LastCommitted() && lifecycle.Pool().Census().Alive == 0, "fresh lifecycle empty");
    NativeSourceVehicleLifecycleSpawn spawn;
    spawn.WorldGeneration = 10; spawn.PedIdentity = 11; spawn.VehicleIdentity = 22;
    spawn.WorldCollision = world10; spawn.VehicleAsset = completion10; spawn.Handling = handling;
    spawn.Suspension = suspension;
    spawn.VehicleMatrix = Matrix(0, 0, 1); spawn.PedPosition = {1, 0, 1}; spawn.TimeMs = 100;
    const auto player = Ped(11, spawn.PedPosition);
    auto staleSpawn = spawn; staleSpawn.WorldGeneration = 9;
    Check(lifecycle.Spawn(staleSpawn, pedWorld10, player, error) == NativeSourceVehicleLifecycleStatus::InvalidInput &&
        !lifecycle.LastCommitted() && lifecycle.Pool().Census().Alive == 0, "worker/world generation mismatch rejected");
    Check(lifecycle.Spawn(spawn, pedWorld10, player, error) == NativeSourceVehicleLifecycleStatus::Ok, error);
    auto initial = lifecycle.LastCommitted(); const auto initialCopy = *initial;
    Check(initial->Epoch == 1 && initial->Generation == 1 && initial->WorldGeneration == 10,
        "spawn epoch/generation identity");
    Check(initial->Phase == NativeSourceVehicleLifecyclePhase::OnFoot && initial->Task ==
        NativeSourceVehicleLifecycleTask::PlayerOnFoot && initial->PedInWorld && initial->PedUsesCollision &&
        !initial->InVehicle, "spawn source on-foot state");
    Check(initial->VehicleAsset == completion10 && initial->WorldCollision == world10 &&
        initial->Automobile->Assets == asset, "spawn immutable stream/model/world owners");
    Check(initial->PoolAlive == 1 && initial->PoolOwner == lifecycle.Pool().Owner() &&
        initial->PoolRevision == 1 && initial->VehicleReference.Value == 1 &&
        lifecycle.Pool().Resolve(initial->VehicleReference), "spawn source pool allocation");
    Check(pedWorld10.PedCount() == 1 && initial->Camera->Mode == NativeSourceCameraMode::FollowPed,
        "spawn ped-world and camera ownership");
    Check(initial->Events.size() == 1 && initial->Events[0].Kind ==
        NativeSourceVehicleLifecycleEventKind::Spawn, "spawn lifecycle journal");

    Check(lifecycle.Tick(Frame(1, 4, 0), 101, 1, {1, 0, 0}, nullptr, error) ==
        NativeSourceVehicleLifecycleStatus::Ok, error);
    auto entering = lifecycle.LastCommitted(); const auto enteringCopy = *entering;
    Check(entering->Phase == NativeSourceVehicleLifecyclePhase::Entering && entering->Task ==
        NativeSourceVehicleLifecycleTask::EnterCarAsDriver && !entering->InVehicle && entering->PedInWorld,
        "Triangle press starts task without instant warp");
    Check(entering->Camera->Mode == NativeSourceCameraMode::CamOnAString &&
        entering->Camera->Transition.Active && entering->Camera->Transition.DurationMs == 1350,
        "entry starts source camera transition");
    Check(!entering->Automobile->Occupants.Driver && pedWorld10.PedCount() == 1,
        "driver handoff awaits task completion");
    Check(lifecycle.CompleteTask(101, {}, error) == NativeSourceVehicleLifecycleStatus::TransitionOutstanding &&
        lifecycle.LastCommitted() == entering, "entry cannot complete before camera transition");
    Check(lifecycle.Tick(Frame(2, 0, 4), 1450, 1, {1, 0, 0}, nullptr, error) ==
        NativeSourceVehicleLifecycleStatus::Ok && lifecycle.LastCommitted()->Camera->Transition.Active,
        "entry transition remains active before exact end");
    Check(lifecycle.Tick(Frame(3, 0, 0), 1451, 1, {1, 0, 0}, nullptr, error) ==
        NativeSourceVehicleLifecycleStatus::Ok && !lifecycle.LastCommitted()->Camera->Transition.Active,
        "entry transition exact completion");
    Check(lifecycle.CompleteTask(1451, {}, error) == NativeSourceVehicleLifecycleStatus::Ok, error);
    auto driving = lifecycle.LastCommitted();
    Check(driving->Phase == NativeSourceVehicleLifecyclePhase::Driving && driving->Task ==
        NativeSourceVehicleLifecycleTask::CarDrive && driving->InVehicle && !driving->PedInWorld &&
        !driving->PedUsesCollision, "task handoff commits source driver state");
    Check(driving->Automobile->Occupants.Driver == 11 && driving->Automobile->Status ==
        NativeVehicleStatus::Player && pedWorld10.PedCount() == 0, "driver/vehicle/ped-world ownership coherent");
    Check(lifecycle.Pool().Resolve(driving->VehicleReference)->State.Status == NativeVehicleStatus::Player,
        "pool reflects source player vehicle");
    Check(*initial == initialCopy && *entering == enteringCopy, "held lifecycle snapshots immutable");

    NativeSourceVehicleLifecycleWorldTarget collisionTarget;
    collisionTarget.WorldGeneration = 10; collisionTarget.Surfaces = &surfaces;
    collisionTarget.Target.Identity = 9001; collisionTarget.Target.Kind = NativeSourceAutomobileContactKind::Building;
    collisionTarget.Target.Collision = floor; collisionTarget.Target.InWorld = true;
    collisionTarget.Target.UsesCollision = collisionTarget.Target.Static = collisionTarget.Target.Collidable = true;
    Check(lifecycle.Tick(Frame(4, 2, 0, 64), 1453, NativeTransmission::TimeStep,
        {1, 0, 0}, &collisionTarget, error) ==
        NativeSourceVehicleLifecycleStatus::Ok, error);
    auto accelerated = lifecycle.LastCommitted();
    Check(accelerated->Automobile->GasPedal == 1 && accelerated->Automobile->BrakePedal == 0 &&
        accelerated->Automobile->ForwardSpeed > 0 && accelerated->Automobile->RawSteerAngle < 0,
        "normal Cross/left-X drive input reaches source automobile");
    Check(accelerated->Automobile->Matrix.Position != spawn.VehicleMatrix.Position &&
        accelerated->Automobile->VehicleCollisionProcessed,
        "drive advances through retained source position and loaded collision owners");
    Check(lifecycle.Pool().Resolve(accelerated->VehicleReference)->State.Matrix ==
        accelerated->Automobile->Matrix, "pool publication follows source vehicle matrix");
    Check(accelerated->Events.back().Kind == NativeSourceVehicleLifecycleEventKind::Drive &&
        accelerated->Events.back().InputSequence == 4, "drive event sampled input sequence");
    const auto acceleratedCopy = *accelerated;
    auto staleTarget = collisionTarget; staleTarget.WorldGeneration = 9;
    Check(lifecycle.Tick(Frame(5, 2, 2, 64), 1454, 1, {1, 0, 0}, &staleTarget, error) ==
        NativeSourceVehicleLifecycleStatus::StaleWorld && lifecycle.LastCommitted() == accelerated,
        "stale collision generation rejects drive atomically");
    Check(lifecycle.Tick(Frame(4, 2, 0, 64), 1454, 1, {1, 0, 0}, nullptr, error) ==
        NativeSourceVehicleLifecycleStatus::StaleInput && lifecycle.LastCommitted() == accelerated,
        "duplicate normal-input frame rejected atomically");
    Check(*accelerated == acceleratedCopy, "rejected input retains held publication");
    Check(lifecycle.Tick(Frame(5, 1, 2), 1454, NativeTransmission::TimeStep, {1, 0, 0}, nullptr, error) ==
        NativeSourceVehicleLifecycleStatus::Ok, error);
    Check(lifecycle.LastCommitted()->Automobile->GasPedal <= 0 &&
        lifecycle.LastCommitted()->Automobile->ForwardSpeed < accelerated->Automobile->ForwardSpeed,
        "normal Square input selects source deceleration branch");

    Check(lifecycle.Tick(Frame(6, 0, 1), 1455, 1, {1, 0, 0}, nullptr, error) ==
        NativeSourceVehicleLifecycleStatus::Ok, error);
    Check(lifecycle.Tick(Frame(7, 4, 0), 1456, 1, {1, 0, 0}, nullptr, error) ==
        NativeSourceVehicleLifecycleStatus::Ok, error);
    auto exiting = lifecycle.LastCommitted();
    Check(exiting->Phase == NativeSourceVehicleLifecyclePhase::Exiting && exiting->Task ==
        NativeSourceVehicleLifecycleTask::LeaveCar && exiting->InVehicle && !exiting->PedInWorld,
        "Triangle press starts leave task without instant SetPedOut");
    Check(exiting->Automobile->Handbrake && exiting->Automobile->BrakePedal == 1 &&
        exiting->Automobile->Occupants.Driver == 11, "exit-held source automatic handbrake and retained driver");
    Check(exiting->Camera->Mode == NativeSourceCameraMode::FollowPed && exiting->Camera->Transition.Active,
        "exit source camera transition");
    Check(lifecycle.Destroy(1456, error) == NativeSourceVehicleLifecycleStatus::InvalidPhase,
        "live exit task prevents destruction");
    Check(lifecycle.Tick(Frame(8, 0, 4), 2805, 1, {1, 0, 0}, nullptr, error) ==
        NativeSourceVehicleLifecycleStatus::Ok && lifecycle.LastCommitted()->Camera->Transition.Active,
        "exit remains active before exact end");
    Check(lifecycle.Tick(Frame(9, 0, 0), 2806, 1, {1, 0, 0}, nullptr, error) ==
        NativeSourceVehicleLifecycleStatus::Ok && !lifecycle.LastCommitted()->Camera->Transition.Active,
        "exit camera exact completion");
    const auto completedTransition = lifecycle.LastCommitted();
    const auto completedTransitionCopy = *completedTransition;
    Check(lifecycle.CompleteTask(2806, {}, error) == NativeSourceVehicleLifecycleStatus::InvalidInput &&
        lifecycle.LastCommitted() == completedTransition, "exit task requires explicit SetPedOut position");
    Check(lifecycle.CompleteTask(2806, NativeCollisionVector{100, 100, 1}, error) ==
        NativeSourceVehicleLifecycleStatus::InvalidInput && lifecycle.LastCommitted() == completedTransition,
        "foreign SetPedOut position cannot teleport the lifecycle player");
    Check(*completedTransition == completedTransitionCopy,
        "rejected SetPedOut handoffs retain the immutable lifecycle publication");
    const NativeCollisionVector setPedOutPosition{1.25f, 0, 1};
    Check(lifecycle.CompleteTask(2806, setPedOutPosition, error) == NativeSourceVehicleLifecycleStatus::Ok, error);
    auto onFoot = lifecycle.LastCommitted();
    Check(onFoot->Phase == NativeSourceVehicleLifecyclePhase::OnFoot && !onFoot->InVehicle &&
        onFoot->PedInWorld && onFoot->PedUsesCollision && onFoot->Task ==
        NativeSourceVehicleLifecycleTask::PlayerOnFoot, "SetPedOut handoff restores source on-foot owner");
    Check(!onFoot->Automobile->Occupants.Driver && onFoot->Automobile->Status ==
        NativeVehicleStatus::Abandoned && pedWorld10.PedCount() == 1, "RemoveDriver order and ped reinsert");
    NativeSourcePedWorldPed exitedPed;
    Check(pedWorld10.Ped(11, exitedPed) == NativeSourcePedWorldStatus::Ok &&
        exitedPed.Physical.Position == setPedOutPosition && onFoot->PedPosition == setPedOutPosition,
        "exit publishes task-resolved collision-enabled ped position");

    Check(lifecycle.EvictWorld(11, std::make_shared<NativeCollisionSnapshot>(), pedWorld10, 2807, error) ==
        NativeSourceVehicleLifecycleStatus::InvalidPhase, "live pool record prevents world eviction");
    const auto oldReference = onFoot->VehicleReference;
    const std::weak_ptr<const NativeVehicleModelCollision> modelBinding =
        lifecycle.Pool().Resolve(oldReference)->State.ModelCollision;
    Check(lifecycle.Destroy(2807, error) == NativeSourceVehicleLifecycleStatus::Ok, error);
    auto destroyed = lifecycle.LastCommitted(); const auto destroyedCopy = *destroyed;
    Check(destroyed->Phase == NativeSourceVehicleLifecyclePhase::Destroyed &&
        destroyed->PoolAlive == 0 && destroyed->LastPoolEvent == NativeVehicleEventKind::Released &&
        !lifecycle.Pool().Resolve(oldReference),
        "destroy removes world and pool authority");
    Check(lifecycle.Pool().Census().ReleasedEvents == 1 && lifecycle.Pool().Census().UpdatedEvents >= 4,
        "pool lifecycle journal coherent");
    Check(lifecycle.Pool().Events().back().Reference == oldReference &&
        lifecycle.Pool().Events().back().ModelId == 400,
        "released pool journal retains value provenance without resource ownership");
    Check(modelBinding.expired(), "pool release and value-only journal release model-COL binding owner");
    auto world11 = std::make_shared<NativeCollisionSnapshot>(*world10);
    NativeSourcePedWorld pedWorld11;
    Check(pedWorld11.Load(Ground(11), error), error);
    Check(lifecycle.EvictWorld(10, world11, pedWorld11, 2808, error) ==
        NativeSourceVehicleLifecycleStatus::StaleWorld && lifecycle.LastCommitted() == destroyed,
        "stale world eviction rejected");
    Check(lifecycle.EvictWorld(11, world11, pedWorld11, 2808, error) ==
        NativeSourceVehicleLifecycleStatus::Ok, error);
    auto evicted = lifecycle.LastCommitted();
    Check(evicted->WorldGeneration == 11 && evicted->WorldCollision == world11 &&
        !evicted->VehicleAsset && !evicted->Automobile, "stream generation adopts and releases vehicle asset owner");
    Check(pedWorld10.PedCount() == 0 && pedWorld11.PedCount() == 1,
        "ped migrates only after vehicle pool cleanup");
    Check(evicted->Events.back().Kind == NativeSourceVehicleLifecycleEventKind::WorldEvicted &&
        evicted->Events.size() == 10, "ordered complete lifecycle journal");
    Check(*destroyed == destroyedCopy && destroyed->VehicleAsset == completion10,
        "held pre-eviction snapshot remains immutable");

    auto completion11 = Completion(11, 2, argv[1], *landstal, collision);
    auto respawn = spawn; respawn.WorldGeneration = 11; respawn.WorldCollision = world11;
    respawn.VehicleAsset = completion11; respawn.VehicleIdentity = 23; respawn.TimeMs = 2809;
    NativeSourcePedWorldPed migratePed;
    Check(pedWorld11.Ped(11, migratePed) == NativeSourcePedWorldStatus::Ok,
        "respawn fixture retains migrated player");
    respawn.PedPosition = migratePed.Physical.Position;
    Check(lifecycle.Spawn(respawn, pedWorld11, migratePed, error) == NativeSourceVehicleLifecycleStatus::Ok, error);
    auto second = lifecycle.LastCommitted();
    Check(second->Epoch == 2 && second->VehicleReference.Value == 2 &&
        !lifecycle.Pool().Resolve(oldReference) && lifecycle.Pool().Resolve(second->VehicleReference),
        "pool slot reuse increments seven-bit generation and rejects stale ref");
    Check(second->Events.size() == 1 && second->Events[0].Sequence == 1,
        "new lifecycle epoch owns fresh journal");

    std::printf("source-vehicle-lifecycle-v1\n");
    for (const auto& event : evicted->Events)
        std::printf("seq=%llu kind=%u phase=%u task=%u world=%llu ref=%d input=%llu time=%u\n",
            static_cast<unsigned long long>(event.Sequence), unsigned(event.Kind), unsigned(event.Phase),
            unsigned(event.Task), static_cast<unsigned long long>(event.WorldGeneration), event.VehicleReference.Value,
            static_cast<unsigned long long>(event.InputSequence), event.TimeMs);
    std::printf("source-vehicle-lifecycle-ok checks=%zu model=400 phases=spawn,enter,drive,exit,destroy,evict pool=110 refs=7bit normal-input=mode0\n",
        s_Checks);
} catch (const std::exception& exception) {
    std::fprintf(stderr, "source-vehicle-lifecycle-failed check=%zu %s\n", s_Checks, exception.what());
    return 1;
}
