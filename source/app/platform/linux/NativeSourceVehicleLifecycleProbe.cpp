#include "NativeSourceVehicleLifecycle.h"
#include "NativeSourceSliceFeedback.h"
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
class SourceInput {
public:
    NativeSourcePadFrame Submit(std::uint64_t sequence, std::uint8_t buttons,
        std::int16_t moveX = 0) {
        NativeSourcePadFrame frame;
        Check(m_Pad.SubmitSample({sequence, sequence * 10, moveX, 0, buttons}, frame) ==
            NativeSourcePadStatus::Ok, "source pad accepts normal lifecycle input");
        return frame;
    }
private:
    NativeSourcePad m_Pad;
};
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
    const auto realWorldModel = collision->LookupModel("gsfreeway7_lan");
    Check(realWorldModel.Status == NativeCollisionModelStatus::Ready && realWorldModel.Model &&
        realWorldModel.Model->HeaderId == 3991 && realWorldModel.Model->Faces.size() == 122,
        "real streamed world COL model ready");
    const auto floor = realWorldModel.Model;
    auto world10 = std::make_shared<NativeCollisionSnapshot>();
    NativeCollisionInstance worldFloor;
    worldFloor.Placement.ModelId = 3991; worldFloor.Placement.Model = "gsfreeway7_lan";
    worldFloor.Placement.Ipl = "bounded-source-lifecycle-world";
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
    SourceInput sourceInput;
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
    NativeSourceSliceFeedback feedback;
    SfxSingleSoundResult retainedSound;
    Check(SfxDecode_Sound(138, 40, retainedSound, error) && retainedSound.sound.dataSize == 11220 &&
        retainedSound.sound.rateHz == 20000 && retainedSound.bufChecksum == 12126532606915493261ULL,
        "exact bank138 sound40 decoder oracle");
    const auto retainedSoundCopy = retainedSound;
    Check(!SfxDecode_Sound(138, 400, retainedSound, error) && retainedSound == retainedSoundCopy,
        "invalid exact SFX identity retains caller output");
    Check(feedback.Initialize(argv[1], initial, error) == NativeSourceSliceFeedbackStatus::Ok, error);
    const auto feedbackInitial = feedback.LastCommitted(); const auto feedbackInitialCopy = *feedbackInitial;
    Check(feedbackInitial->Hud.Health == 100 && feedbackInitial->Hud.MaxHealth == 100 &&
        feedbackInitial->Hud.Armour == 0 && feedbackInitial->Hud.MaxArmour == 100 &&
        feedbackInitial->Hud.Money == 0 && feedbackInitial->Hud.DisplayMoney == 0 &&
        feedbackInitial->Hud.WantedLevel == 0 && feedbackInitial->Hud.ActiveWeapon == 0,
        "source-initial player HUD state");
    Check(feedbackInitial->Actions.size() == 1 && feedbackInitial->Audio.empty() &&
        !feedbackInitial->PresentationFeedback, "spawn feedback has no fabricated audio/presentation authority");
    Check(feedback.Observe(initial, error) == NativeSourceSliceFeedbackStatus::DuplicateIdempotent &&
        feedback.LastCommitted() == feedbackInitial, "duplicate lifecycle observation idempotent");

    Check(lifecycle.Tick(sourceInput.Submit(1, 4), 101, 1, {1, 0, 0}, nullptr, error) ==
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
    Check(feedback.Observe(entering, error) == NativeSourceSliceFeedbackStatus::Ok &&
        feedback.LastCommitted()->Actions.back().Kind == NativeSourceSliceActionKind::EnterVehicle &&
        feedback.LastCommitted()->Actions.back().Flags == NativeSourceSliceActionEnterExit,
        "normal Triangle entry publishes authoritative action");
    Check(lifecycle.ReportDriverDoor(true, 102, error) == NativeSourceVehicleLifecycleStatus::Ok, error);
    Check(feedback.Observe(lifecycle.LastCommitted(), error) == NativeSourceSliceFeedbackStatus::Ok, error);
    Check(feedback.LastCommitted()->Audio.size() == 1 &&
        feedback.LastCommitted()->Audio.back().EventId == 80 &&
        feedback.LastCommitted()->Audio.back().BankId == 138 &&
        feedback.LastCommitted()->Audio.back().BankSlot == 19 &&
        feedback.LastCommitted()->Audio.back().SoundId == 40 &&
        feedback.LastCommitted()->Audio.back().DoorType == 2 &&
        feedback.LastCommitted()->Audio.back().Clip &&
        feedback.LastCommitted()->Audio.back().Clip->sound.bankId == 138 &&
        feedback.LastCommitted()->Audio.back().Clip->sound.soundIndex == 40 &&
        !feedback.LastCommitted()->Audio.back().Clip->sound.pcm.empty(),
        "model400 driver door open routes exact NEW-door PCM");
    const auto entryDoorOpen = lifecycle.LastCommitted();
    Check(lifecycle.CompleteTask(102, {}, error) == NativeSourceVehicleLifecycleStatus::TransitionOutstanding &&
        lifecycle.LastCommitted() == entryDoorOpen, "entry cannot complete before camera transition");
    Check(lifecycle.Tick(sourceInput.Submit(2, 0), 1450, 1, {1, 0, 0}, nullptr, error) ==
        NativeSourceVehicleLifecycleStatus::Ok && lifecycle.LastCommitted()->Camera->Transition.Active,
        "entry transition remains active before exact end");
    Check(lifecycle.Tick(sourceInput.Submit(3, 0), 1451, 1, {1, 0, 0}, nullptr, error) ==
        NativeSourceVehicleLifecycleStatus::Ok && !lifecycle.LastCommitted()->Camera->Transition.Active,
        "entry transition exact completion");
    Check(lifecycle.ReportDriverDoor(false, 1451, error) == NativeSourceVehicleLifecycleStatus::Ok, error);
    Check(feedback.Observe(lifecycle.LastCommitted(), error) == NativeSourceSliceFeedbackStatus::Ok &&
        feedback.LastCommitted()->Audio.size() == 2 &&
        feedback.LastCommitted()->Audio.back().EventId == 86 &&
        feedback.LastCommitted()->Audio.back().SoundId == 33 &&
        feedback.LastCommitted()->Audio.back().Clip->sound.soundIndex == 33,
        "model400 driver door close routes exact NEW-door PCM");
    Check(lifecycle.CompleteTask(1451, {}, error) == NativeSourceVehicleLifecycleStatus::Ok, error);
    auto driving = lifecycle.LastCommitted();
    Check(feedback.Observe(driving, error) == NativeSourceSliceFeedbackStatus::Ok &&
        feedback.LastCommitted()->Hud.InVehicle &&
        feedback.LastCommitted()->Actions.back().Kind == NativeSourceSliceActionKind::DriverAttached,
        "driver callback updates authoritative HUD/action state");
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
    collisionTarget.Target.Identity = 3991; collisionTarget.Target.Kind = NativeSourceAutomobileContactKind::Building;
    collisionTarget.Target.Collision = floor; collisionTarget.Target.InWorld = true;
    collisionTarget.Target.UsesCollision = collisionTarget.Target.Static = collisionTarget.Target.Collidable = true;
    const auto input4 = sourceInput.Submit(4, 2, 64);
    Check(lifecycle.Tick(input4, 1453, NativeTransmission::TimeStep,
        {1, 0, 0}, &collisionTarget, error) ==
        NativeSourceVehicleLifecycleStatus::Ok, error);
    auto accelerated = lifecycle.LastCommitted();
    Check(accelerated->Automobile->GasPedal == 1 && accelerated->Automobile->BrakePedal == 0 &&
        accelerated->Automobile->ForwardSpeed > 0 && accelerated->Automobile->RawSteerAngle < 0,
        "normal Cross/left-X drive input reaches source automobile");
    Check(accelerated->Automobile->Matrix.Position != spawn.VehicleMatrix.Position &&
        accelerated->Automobile->VehicleCollisionProcessed,
        "drive advances through retained source position and loaded collision owners");
    Check(collisionTarget.Target.Collision->HeaderId == 3991 &&
        collisionTarget.Target.Collision->Faces.size() == 122,
        "normal driving collision target is real gsfreeway7_lan COL, not a substitute");
    Check(lifecycle.Pool().Resolve(accelerated->VehicleReference)->State.Matrix ==
        accelerated->Automobile->Matrix, "pool publication follows source vehicle matrix");
    Check(accelerated->Events.back().Kind == NativeSourceVehicleLifecycleEventKind::Drive &&
        accelerated->Events.back().InputSequence == 4, "drive event sampled input sequence");
    Check(feedback.Observe(accelerated, error) == NativeSourceSliceFeedbackStatus::Ok &&
        feedback.LastCommitted()->Actions.back().Kind == NativeSourceSliceActionKind::VehicleControl &&
        feedback.LastCommitted()->Actions.back().Flags ==
            (NativeSourceSliceActionAccelerate | NativeSourceSliceActionSteer) &&
        feedback.LastCommitted()->Actions.back().MoveX == 64 &&
        feedback.LastCommitted()->Hud.VehicleSpeed == accelerated->Automobile->ForwardSpeed,
        "Cross and steering reach action journal and source HUD speed");
    const auto acceleratedCopy = *accelerated;
    auto staleTarget = collisionTarget; staleTarget.WorldGeneration = 9;
    Check(lifecycle.Tick(Frame(5, 2, 2, 64), 1454, 1, {1, 0, 0}, &staleTarget, error) ==
        NativeSourceVehicleLifecycleStatus::StaleWorld && lifecycle.LastCommitted() == accelerated,
        "stale collision generation rejects drive atomically");
    Check(lifecycle.Tick(input4, 1454, 1, {1, 0, 0}, nullptr, error) ==
        NativeSourceVehicleLifecycleStatus::StaleInput && lifecycle.LastCommitted() == accelerated,
        "duplicate normal-input frame rejected atomically");
    Check(*accelerated == acceleratedCopy, "rejected input retains held publication");
    Check(lifecycle.Tick(sourceInput.Submit(5, 1), 1454, NativeTransmission::TimeStep, {1, 0, 0}, nullptr, error) ==
        NativeSourceVehicleLifecycleStatus::Ok, error);
    Check(feedback.Observe(lifecycle.LastCommitted(), error) == NativeSourceSliceFeedbackStatus::Ok &&
        (feedback.LastCommitted()->Actions.back().Flags & NativeSourceSliceActionBrake),
        "Square driving input reaches authoritative brake action");
    Check(lifecycle.LastCommitted()->Automobile->GasPedal <= 0 &&
        lifecycle.LastCommitted()->Automobile->ForwardSpeed < accelerated->Automobile->ForwardSpeed,
        "normal Square input selects source deceleration branch");

    Check(lifecycle.Tick(sourceInput.Submit(6, 0), 1455, 1, {1, 0, 0}, nullptr, error) ==
        NativeSourceVehicleLifecycleStatus::Ok, error);
    Check(feedback.Observe(lifecycle.LastCommitted(), error) == NativeSourceSliceFeedbackStatus::Ok, error);
    Check(lifecycle.Tick(sourceInput.Submit(7, 4), 1456, 1, {1, 0, 0}, nullptr, error) ==
        NativeSourceVehicleLifecycleStatus::Ok, error);
    auto exiting = lifecycle.LastCommitted();
    Check(exiting->Phase == NativeSourceVehicleLifecyclePhase::Exiting && exiting->Task ==
        NativeSourceVehicleLifecycleTask::LeaveCar && exiting->InVehicle && !exiting->PedInWorld,
        "Triangle press starts leave task without instant SetPedOut");
    Check(exiting->Automobile->Handbrake && exiting->Automobile->BrakePedal == 1 &&
        exiting->Automobile->Occupants.Driver == 11, "exit-held source automatic handbrake and retained driver");
    Check(exiting->Camera->Mode == NativeSourceCameraMode::FollowPed && exiting->Camera->Transition.Active,
        "exit source camera transition");
    Check(feedback.Observe(exiting, error) == NativeSourceSliceFeedbackStatus::Ok &&
        feedback.LastCommitted()->Actions.back().Kind == NativeSourceSliceActionKind::ExitVehicle,
        "Triangle exit publishes authoritative action");
    Check(lifecycle.ReportDriverDoor(true, 1457, error) == NativeSourceVehicleLifecycleStatus::Ok, error);
    Check(feedback.Observe(lifecycle.LastCommitted(), error) == NativeSourceSliceFeedbackStatus::Ok &&
        feedback.LastCommitted()->Audio.size() == 3, "exit door open publishes third exact PCM event");
    Check(lifecycle.Destroy(1456, error) == NativeSourceVehicleLifecycleStatus::InvalidPhase,
        "live exit task prevents destruction");
    Check(lifecycle.Tick(sourceInput.Submit(8, 0), 2805, 1, {1, 0, 0}, nullptr, error) ==
        NativeSourceVehicleLifecycleStatus::Ok && lifecycle.LastCommitted()->Camera->Transition.Active,
        "exit remains active before exact end");
    Check(lifecycle.Tick(sourceInput.Submit(9, 0), 2806, 1, {1, 0, 0}, nullptr, error) ==
        NativeSourceVehicleLifecycleStatus::Ok && !lifecycle.LastCommitted()->Camera->Transition.Active,
        "exit camera exact completion");
    Check(lifecycle.ReportDriverDoor(false, 2806, error) == NativeSourceVehicleLifecycleStatus::Ok, error);
    Check(feedback.Observe(lifecycle.LastCommitted(), error) == NativeSourceSliceFeedbackStatus::Ok &&
        feedback.LastCommitted()->Audio.size() == 4, "exit door close publishes fourth exact PCM event");
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
    Check(feedback.Observe(onFoot, error) == NativeSourceSliceFeedbackStatus::Ok &&
        !feedback.LastCommitted()->Hud.InVehicle &&
        feedback.LastCommitted()->Actions.back().Kind == NativeSourceSliceActionKind::DriverDetached,
        "SetPedOut callback returns HUD/action authority on foot");
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
    Check(feedback.Observe(destroyed, error) == NativeSourceSliceFeedbackStatus::Ok, error);
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
    Check(feedback.Observe(evicted, error) == NativeSourceSliceFeedbackStatus::Ok, error);
    Check(evicted->WorldGeneration == 11 && evicted->WorldCollision == world11 &&
        !evicted->VehicleAsset && !evicted->Automobile, "stream generation adopts and releases vehicle asset owner");
    Check(pedWorld10.PedCount() == 0 && pedWorld11.PedCount() == 1,
        "ped migrates only after vehicle pool cleanup");
    Check(evicted->Events.back().Kind == NativeSourceVehicleLifecycleEventKind::WorldEvicted &&
        evicted->Events.size() == 14, "ordered complete lifecycle journal");
    const auto feedbackFinal = feedback.LastCommitted();
    Check(feedbackFinal->Actions.size() == 14 && feedbackFinal->Audio.size() == 4 &&
        feedbackFinal->Actions.back().Kind == NativeSourceSliceActionKind::WorldEvicted,
        "normal-input route publishes complete ordered action/audio journal");
    Check(feedbackFinal->Audio[0].Clip == feedbackFinal->Audio[2].Clip &&
        feedbackFinal->Audio[1].Clip == feedbackFinal->Audio[3].Clip &&
        feedbackFinal->Audio[0].Clip != feedbackFinal->Audio[1].Clip &&
        feedbackFinal->Audio[0].Clip->bufChecksum != feedbackFinal->Audio[1].Clip->bufChecksum,
        "door requests retain two exact shared PCM owners without substitution");
    auto skippedCandidate = *evicted;
    skippedCandidate.Generation += 2;
    skippedCandidate.Events.push_back(skippedCandidate.Events.back());
    skippedCandidate.Events.back().Sequence = 15;
    skippedCandidate.Events.back().Kind = NativeSourceVehicleLifecycleEventKind::Destroyed;
    skippedCandidate.Events.push_back(skippedCandidate.Events.back());
    skippedCandidate.Events.back().Sequence = 16;
    const auto skippedEvent = std::make_shared<const NativeSourceVehicleLifecycleSnapshot>(std::move(skippedCandidate));
    Check(feedback.Observe(skippedEvent, error) == NativeSourceSliceFeedbackStatus::MissingLifecycleEvent &&
        feedback.LastCommitted() == feedbackFinal, "skipped lifecycle event cannot be inferred by feedback owner");
    Check(*feedbackInitial == feedbackInitialCopy && feedbackInitial->Audio.empty(),
        "held feedback snapshot remains immutable after full route");
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
    std::printf("source-slice-feedback-ok actions=%zu audio=%zu hud=100,0,0 door=80/86 bank=138 slot=19 sounds=40/33 open=%u/%u/%llu close=%u/%u/%llu pcm=owned pointer-feedback=0\n",
        feedbackFinal->Actions.size(), feedbackFinal->Audio.size(),
        feedbackFinal->Audio[0].Clip->sound.dataSize, feedbackFinal->Audio[0].Clip->sound.rateHz,
        static_cast<unsigned long long>(feedbackFinal->Audio[0].Clip->bufChecksum),
        feedbackFinal->Audio[1].Clip->sound.dataSize, feedbackFinal->Audio[1].Clip->sound.rateHz,
        static_cast<unsigned long long>(feedbackFinal->Audio[1].Clip->bufChecksum));
} catch (const std::exception& exception) {
    std::fprintf(stderr, "source-vehicle-lifecycle-failed check=%zu %s\n", s_Checks, exception.what());
    return 1;
}
