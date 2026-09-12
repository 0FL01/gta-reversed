#include "NativeSourceVehicleLifecycle.h"

#include <cmath>
#include <exception>
#include <limits>
#include <ranges>
#include <stdexcept>
#include <string_view>

namespace {
using Status = NativeSourceVehicleLifecycleStatus;
constexpr std::uint8_t Square = 1, Cross = 2, Triangle = 4;
bool Finite(const NativeCollisionVector& value) {
    return std::ranges::all_of(value, [](float component) { return std::isfinite(component); });
}
bool Finite(const NativeGarageMatrix& matrix) {
    return Finite(matrix.Position) && std::ranges::all_of(matrix.Basis, [](const auto& axis) { return Finite(axis); });
}
bool SameDefinition(const NativeCarGeneratorModelDefinition& a, const NativeCarGeneratorModelDefinition& b) {
    return a.ModelId == b.ModelId && a.ModelName == b.ModelName && a.TextureName == b.TextureName &&
        a.Type == b.Type && a.TypeName == b.TypeName && a.HandlingName == b.HandlingName &&
        a.GameName == b.GameName && a.AnimationGroup == b.AnimationGroup && a.ClassName == b.ClassName &&
        a.Frequency == b.Frequency && a.Flags == b.Flags && a.ComponentRules == b.ComponentRules &&
        a.Misc == b.Misc && a.WheelSizeFront == b.WheelSizeFront &&
        a.WheelSizeRear == b.WheelSizeRear && a.WheelUpgradeClass == b.WheelUpgradeClass &&
        a.Source == b.Source && a.Line == b.Line;
}
}

NativeSourceVehicleLifecycle::NativeSourceVehicleLifecycle() {
    std::string error;
    if (!m_Pool.BindProducer(NativeVehicleProducer::NativeGameplayController, error) ||
        !m_Pool.SealProducerExtent(error)) throw std::runtime_error(error);
}

NativeSourceVehicleLifecycleStatus NativeSourceVehicleLifecycle::Publish(
    NativeSourceVehicleLifecycleSnapshot&& next, std::vector<NativeSourceVehicleLifecycleEvent>&& events,
    std::string& error) try {
    next.Events = events;
    auto published = std::make_shared<const NativeSourceVehicleLifecycleSnapshot>(std::move(next));
    m_Events = std::move(events);
    m_Published = std::move(published);
    error.clear();
    return Status::Ok;
} catch (const std::exception& exception) {
    error = exception.what();
    return Status::Overflow;
}

NativeSourceVehicleLifecycleStatus NativeSourceVehicleLifecycle::AutomobileStatus(
    NativeSourceAutomobileStatus status, std::string& error) {
    if (status == NativeSourceAutomobileStatus::Ready) return Status::Ok;
    if (error.empty()) error = "source automobile rejected lifecycle transition";
    return status == NativeSourceAutomobileStatus::Error ? Status::Overflow : Status::AutomobileError;
}

NativeSourceVehicleLifecycleStatus NativeSourceVehicleLifecycle::CameraStatus(
    NativeSourceCameraStatus status, std::string& error) {
    if (status == NativeSourceCameraStatus::Ok) return Status::Ok;
    if (error.empty()) error = "source camera rejected lifecycle transition";
    if (status == NativeSourceCameraStatus::TransitionOutstanding) return Status::TransitionOutstanding;
    return status == NativeSourceCameraStatus::Overflow ? Status::Overflow : Status::CameraError;
}

NativeVehicleState NativeSourceVehicleLifecycle::VehicleState(bool inWorld) const {
    const auto& source = *m_Automobile->LastCommitted();
    NativeVehicleState state;
    state.ModelId = source.ModelId; state.Type = source.Type; state.SubType = 0;
    state.Status = source.Status; state.CreatedBy = source.CreatedBy; state.InWorld = inWorld;
    state.Matrix = source.Matrix; state.Collision = source.Assets->Collision;
    state.ModelCollision = m_ModelCollision;
    return state;
}

bool NativeSourceVehicleLifecycle::Append(const NativeSourceVehicleLifecycleSnapshot& state,
    std::vector<NativeSourceVehicleLifecycleEvent>& events,
    NativeSourceVehicleLifecycleEventKind kind, NativeSourceVehicleLifecyclePhase phase,
    NativeSourceVehicleLifecycleTask task, std::uint32_t time, std::uint64_t input, std::string& error) try {
    if (m_NextEvent == std::numeric_limits<std::uint64_t>::max()) {
        error = "source lifecycle event sequence exhausted";
        return false;
    }
    events.push_back({m_NextEvent, state.WorldGeneration, input, state.PedIdentity, state.VehicleIdentity,
        kind, phase, state.VehicleReference, task, time});
    return true;
} catch (const std::exception& exception) {
    error = exception.what();
    return false;
}

NativeSourceVehicleLifecycleStatus NativeSourceVehicleLifecycle::Spawn(
    const NativeSourceVehicleLifecycleSpawn& request, NativeSourcePedWorld& pedWorld,
    const NativeSourcePedWorldPed& ped, std::string& error) {
    if (m_Published && m_Published->Phase != NativeSourceVehicleLifecyclePhase::Destroyed) {
        error = "source lifecycle already owns a live actor";
        return Status::InvalidPhase;
    }
    if (!request.WorldGeneration || !request.PedIdentity || !request.VehicleIdentity ||
        !request.WorldCollision || !request.VehicleAsset || !request.VehicleAsset->Asset ||
        !request.VehicleAsset->Result || !request.VehicleAsset->Ticket.Identity ||
        request.VehicleAsset->WorkerGeneration != request.WorldGeneration ||
        request.VehicleAsset->Ticket.Identity->Definition.ModelId != 400 ||
        request.VehicleAsset->Asset->Definition.ModelId != 400 ||
        request.VehicleAsset->Ticket.Identity->Definition.ModelName != "landstal" ||
        request.VehicleAsset->Asset->Definition.ModelName != "landstal" ||
        request.Suspension.wheels != 4 || std::string_view(request.Suspension.model) != "landstal" ||
        !SameDefinition(request.VehicleAsset->Ticket.Identity->Definition, request.VehicleAsset->Asset->Definition) ||
        request.WorldCollision->Instances.empty() ||
        std::ranges::none_of(request.WorldCollision->Instances, [](const NativeCollisionInstance& instance) {
            return instance.Model && !instance.Model->Empty && instance.Model->Unsupported.empty();
        }) ||
        ped.Identity != request.PedIdentity || ped.Physical.Position != request.PedPosition ||
        pedWorld.WorldGeneration() != request.WorldGeneration || !Finite(request.PedPosition) ||
        !Finite(request.VehicleMatrix)) {
        error = "invalid generation-qualified lifecycle spawn";
        return Status::InvalidInput;
    }
    if (m_Published && request.WorldGeneration < m_Published->WorldGeneration) {
        error = "source lifecycle spawn world is stale";
        return Status::StaleWorld;
    }
    NativeSourcePedWorldPed existingPed;
    const auto existingStatus = pedWorld.Ped(ped.Identity, existingPed);
    if (existingStatus != NativeSourcePedWorldStatus::Ok &&
        existingStatus != NativeSourcePedWorldStatus::PedNotFound) {
        error = "source ped-world cannot verify lifecycle spawn";
        return Status::InvalidInput;
    }
    if (existingStatus == NativeSourcePedWorldStatus::Ok && existingPed != ped) {
        error = "source ped-world player state conflicts with lifecycle spawn";
        return Status::InvalidInput;
    }
    auto automobile = std::make_unique<NativeSourceAutomobile>();
    if (const auto status = AutomobileStatus(automobile->Construct(request.VehicleIdentity,
            request.VehicleAsset->Ticket.Identity->Definition, request.VehicleAsset->Asset,
            request.Handling, request.CreatedBy, request.VehicleMatrix, error), error); status != Status::Ok)
        return status;
    if (const auto status = AutomobileStatus(automobile->SetupSuspension(request.Suspension, error), error);
        status != Status::Ok) return status;
    const bool insertedPed = existingStatus == NativeSourcePedWorldStatus::PedNotFound;
    if (insertedPed && pedWorld.AddPed(ped) != NativeSourcePedWorldStatus::Ok) {
        error = "source ped-world rejected lifecycle spawn";
        return Status::InvalidInput;
    }
    std::shared_ptr<const NativeVehicleModelCollision> modelCollision;
    try {
        modelCollision = std::make_shared<const NativeVehicleModelCollision>(
            NativeVehicleModelCollision{400, automobile->LastCommitted()->Assets->Collision});
    } catch (const std::exception& exception) {
        if (insertedPed) pedWorld.RemovePed(ped.Identity);
        error = exception.what();
        return Status::Overflow;
    }
    m_Automobile = std::move(automobile); m_ModelCollision = std::move(modelCollision);
    const auto allocated = m_Pool.Allocate({NativeVehicleProducer::NativeGameplayController, {}, 0, VehicleState(true)});
    if (allocated.Result.Status != NativeScriptServiceStatus::Ready) {
        if (insertedPed) pedWorld.RemovePed(ped.Identity);
        m_Automobile.reset(); m_ModelCollision.reset(); error = allocated.Result.Message; return Status::PoolError;
    }
    const auto epoch = m_NextEpoch;
    if (const auto status = CameraStatus(m_Camera.Initialize(epoch, request.PedIdentity, request.TimeMs), error);
        status != Status::Ok) {
        std::string ignored; m_Pool.Release(allocated.Reference, ignored);
        if (insertedPed) pedWorld.RemovePed(ped.Identity);
        m_Automobile.reset(); m_ModelCollision.reset(); return status;
    }
    std::vector<NativeSourceVehicleLifecycleEvent> events;
    NativeSourceVehicleLifecycleSnapshot next;
    try {
        next.Epoch = epoch; next.Generation = 1; next.WorldGeneration = request.WorldGeneration;
        next.PedIdentity = request.PedIdentity; next.VehicleIdentity = request.VehicleIdentity;
        next.Phase = NativeSourceVehicleLifecyclePhase::OnFoot;
        next.Task = NativeSourceVehicleLifecycleTask::PlayerOnFoot; next.VehicleReference = allocated.Reference;
        next.PedPosition = request.PedPosition; next.PedInWorld = next.PedUsesCollision = true;
        next.TimeMs = request.TimeMs; next.WorldCollision = request.WorldCollision;
        next.VehicleAsset = request.VehicleAsset; next.Automobile = m_Automobile->LastCommitted();
        next.Camera = m_Camera.LastCommitted();
        next.PoolOwner = m_Pool.Owner(); next.PoolRevision = m_Pool.Revision();
        next.PoolAlive = m_Pool.Census().Alive; next.LastPoolEvent = m_Pool.Events().back().Kind;
        events.push_back({1, next.WorldGeneration, 0, next.PedIdentity, next.VehicleIdentity,
            NativeSourceVehicleLifecycleEventKind::Spawn, next.Phase, next.VehicleReference, next.Task, next.TimeMs});
    }
    catch (const std::exception& exception) {
        std::string ignored; m_Pool.Release(allocated.Reference, ignored);
        if (insertedPed) pedWorld.RemovePed(ped.Identity);
        m_Automobile.reset(); m_ModelCollision.reset();
        error = exception.what(); return Status::Overflow;
    }
    const auto status = Publish(std::move(next), std::move(events), error);
    if (status == Status::Ok) {
        m_PedWorld = &pedWorld; m_Ped = ped; m_NextEvent = 2; ++m_NextEpoch;
    }
    return status;
}

NativeSourceVehicleLifecycleStatus NativeSourceVehicleLifecycle::Tick(const NativeSourcePadFrame& input,
    std::uint32_t nowMs, float timeStep, std::array<float, 3> front,
    const NativeSourceVehicleLifecycleWorldTarget* worldTarget, std::string& error) {
    if (!m_Published || !m_Automobile) { error = "source lifecycle not loaded"; return Status::NotLoaded; }
    if (!input.Sample.Seq || input.Sample.Seq <= m_Published->InputSequence || nowMs < m_Published->TimeMs) {
        error = "stale lifecycle input or time"; return Status::StaleInput;
    }
    if (!std::isfinite(timeStep) || timeStep <= 0 || !Finite(front) ||
        (front[0] == 0 && front[1] == 0) || input.Sample.Buttons > 7 || input.Down != input.Sample.Buttons ||
        input.Pressed != std::uint8_t(input.Down & ~m_Published->PadButtons) ||
        input.Released != std::uint8_t(m_Published->PadButtons & ~input.Down)) {
        error = "invalid lifecycle input"; return Status::InvalidInput;
    }
    if (worldTarget && (worldTarget->WorldGeneration != m_Published->WorldGeneration ||
        !worldTarget->Surfaces || worldTarget->Target.Kind != NativeSourceAutomobileContactKind::Building ||
        !worldTarget->Target.Collision || !m_Published->WorldCollision ||
        std::ranges::none_of(m_Published->WorldCollision->Instances, [&](const NativeCollisionInstance& instance) {
            return instance.Model == worldTarget->Target.Collision;
        }))) {
        error = "stale or invalid lifecycle collision target"; return Status::StaleWorld;
    }
    auto next = *m_Published; auto events = m_Events;
    next.Generation++; next.TimeMs = nowMs; next.InputSequence = input.Sample.Seq; next.PadButtons = input.Down;
    const bool exitPressed = (input.Pressed & Triangle) != 0;
    bool emitted = false;
    if (next.Phase == NativeSourceVehicleLifecyclePhase::OnFoot && exitPressed) {
        const auto dx = next.PedPosition[0] - next.Automobile->Matrix.Position[0];
        const auto dy = next.PedPosition[1] - next.Automobile->Matrix.Position[1];
        const auto dz = next.PedPosition[2] - next.Automobile->Matrix.Position[2];
        if (!std::isfinite(dx * dx + dy * dy + dz * dz) || dx * dx + dy * dy > 16.0f || std::abs(dz) > 2.0f) {
            error = "no qualified nearby lifecycle vehicle"; return Status::InvalidInput;
        }
        next.Phase = NativeSourceVehicleLifecyclePhase::Entering;
        next.Task = NativeSourceVehicleLifecycleTask::EnterCarAsDriver;
        if (!Append(next, events, NativeSourceVehicleLifecycleEventKind::EnterRequested, next.Phase,
                next.Task, nowMs, input.Sample.Seq, error)) return Status::Overflow;
        emitted = true;
        NativeSourceCameraPlayer player{NativeSourceCameraPlayerState::EnterCar, next.PedIdentity,
            next.VehicleIdentity, true, false};
        if (const auto status = CameraStatus(m_Camera.Restore(nowMs, player, front,
                NativeSourceCameraSwitch::Interpolation, input.Sample.Seq), error); status != Status::Ok) return status;
        next.Camera = m_Camera.LastCommitted();
    } else if (next.Phase == NativeSourceVehicleLifecyclePhase::Driving && exitPressed) {
        next.Phase = NativeSourceVehicleLifecyclePhase::Exiting;
        next.Task = NativeSourceVehicleLifecycleTask::LeaveCar;
        if (!Append(next, events, NativeSourceVehicleLifecycleEventKind::ExitRequested, next.Phase,
                next.Task, nowMs, input.Sample.Seq, error)) return Status::Overflow;
        emitted = true;
        // GetExitVehicle is also the automatic handbrake condition. The source
        // leave task still owns SetPedOut completion, so driver remains attached.
        if (const auto status = AutomobileStatus(m_Automobile->ProcessPlayerControls(0, 255,
                input.Sample.MoveX, false, true, next.Automobile->ForwardSpeed,
                timeStep, error), error); status != Status::Ok) return status;
        NativeSourceCameraPlayer player{NativeSourceCameraPlayerState::ExitCar, next.PedIdentity,
            next.VehicleIdentity, true, false};
        if (const auto status = CameraStatus(m_Camera.Restore(nowMs, player, front,
                NativeSourceCameraSwitch::Interpolation, input.Sample.Seq), error); status != Status::Ok) return status;
        next.Automobile = m_Automobile->LastCommitted(); next.Camera = m_Camera.LastCommitted();
        if (!m_Pool.Update(next.VehicleReference, VehicleState(true), error)) return Status::PoolError;
    } else if (next.Phase == NativeSourceVehicleLifecyclePhase::Driving) {
        const std::uint8_t accelerate = input.Down & Cross ? 255 : 0;
        const std::uint8_t brake = input.Down & Square ? 255 : 0;
        if (!Append(next, events, NativeSourceVehicleLifecycleEventKind::Drive, next.Phase,
                next.Task, nowMs, input.Sample.Seq, error)) return Status::Overflow;
        emitted = true;
        if (const auto status = AutomobileStatus(m_Automobile->EndControlFrame(error), error);
            status != Status::Ok) return status;
        if (const auto status = AutomobileStatus(m_Automobile->ProcessPlayerControls(accelerate, brake,
                input.Sample.MoveX, false, false, m_Automobile->LastCommitted()->ForwardSpeed,
                timeStep, error), error); status != Status::Ok) return status;
        if (const auto status = AutomobileStatus(m_Automobile->AdvanceDrive(timeStep, true, error), error);
            status != Status::Ok) return status;
        if (worldTarget) {
            if (const auto status = AutomobileStatus(m_Automobile->ProcessCollision(worldTarget->Target,
                    *worldTarget->Surfaces, timeStep, error), error); status != Status::Ok) return status;
        }
        if (const auto status = AutomobileStatus(m_Automobile->AdvancePosition(timeStep, error), error);
            status != Status::Ok) return status;
        next.Automobile = m_Automobile->LastCommitted();
        if (!m_Pool.Update(next.VehicleReference, VehicleState(true), error)) return Status::PoolError;
        NativeSourceCameraPlayer player{NativeSourceCameraPlayerState::InVehicle, next.PedIdentity,
            next.VehicleIdentity, true, false};
        if (const auto status = CameraStatus(m_Camera.Restore(nowMs, player, front,
                NativeSourceCameraSwitch::Interpolation, input.Sample.Seq), error); status != Status::Ok) return status;
        next.Camera = m_Camera.LastCommitted();
    } else {
        if (const auto status = CameraStatus(m_Camera.Advance(nowMs), error); status != Status::Ok) return status;
        next.Camera = m_Camera.LastCommitted();
    }
    next.PoolRevision = m_Pool.Revision(); next.PoolAlive = m_Pool.Census().Alive;
    if (!m_Pool.Events().empty()) next.LastPoolEvent = m_Pool.Events().back().Kind;
    const auto status = Publish(std::move(next), std::move(events), error);
    if (status == Status::Ok && emitted) ++m_NextEvent;
    return status;
}

NativeSourceVehicleLifecycleStatus NativeSourceVehicleLifecycle::CompleteTask(
    std::uint32_t nowMs, std::optional<NativeCollisionVector> setPedOutPosition, std::string& error) {
    if (!m_Published || !m_Automobile) { error = "source lifecycle not loaded"; return Status::NotLoaded; }
    if (nowMs < m_Published->TimeMs) { error = "backward lifecycle task time"; return Status::StaleInput; }
    if (m_Published->Phase != NativeSourceVehicleLifecyclePhase::Entering &&
        m_Published->Phase != NativeSourceVehicleLifecyclePhase::Exiting) {
        error = "no source vehicle task awaits handoff"; return Status::InvalidPhase;
    }
    if (!m_Camera.LastCommitted() || m_Camera.LastCommitted()->Transition.Active) {
        error = "source camera transition still outstanding"; return Status::TransitionOutstanding;
    }
    const bool entering = m_Published->Phase == NativeSourceVehicleLifecyclePhase::Entering;
    if ((entering && setPedOutPosition) || (!entering && (!setPedOutPosition || !Finite(*setPedOutPosition)))) {
        error = "invalid source task position handoff"; return Status::InvalidInput;
    }
    if (!entering) {
        const auto& position = *setPedOutPosition;
        const auto& vehicle = m_Automobile->LastCommitted()->Matrix.Position;
        const float dx = position[0] - vehicle[0], dy = position[1] - vehicle[1], dz = position[2] - vehicle[2];
        if (!std::isfinite(dx * dx + dy * dy + dz * dz) || dx * dx + dy * dy > 16.0f || std::abs(dz) > 2.0f) {
            error = "source SetPedOut position is outside the bounded vehicle handoff";
            return Status::InvalidInput;
        }
    }
    auto next = *m_Published; auto events = m_Events;
    next.Generation++; next.TimeMs = nowMs;
    next.Phase = entering ? NativeSourceVehicleLifecyclePhase::Driving : NativeSourceVehicleLifecyclePhase::OnFoot;
    next.Task = entering ? NativeSourceVehicleLifecycleTask::CarDrive : NativeSourceVehicleLifecycleTask::PlayerOnFoot;
    if (!Append(next, events, entering ? NativeSourceVehicleLifecycleEventKind::DriverAttached :
            NativeSourceVehicleLifecycleEventKind::DriverDetached, next.Phase, next.Task,
            nowMs, next.InputSequence, error)) return Status::Overflow;
    if (entering) {
        NativeSourcePedWorldPed ped;
        if (!m_PedWorld || m_PedWorld->Ped(next.PedIdentity, ped) != NativeSourcePedWorldStatus::Ok ||
            ped != m_Ped) {
            error = "source ped-world player changed during entry"; return Status::InvalidPhase;
        }
        if (const auto status = AutomobileStatus(m_Automobile->SetDriver(next.PedIdentity, error), error); status != Status::Ok) return status;
        if (const auto status = AutomobileStatus(m_Automobile->SetStatus(NativeVehicleStatus::Player, error), error); status != Status::Ok) return status;
        if (m_PedWorld->RemovePed(next.PedIdentity) != NativeSourcePedWorldStatus::Ok) {
            error = "source ped-world rejected driver handoff"; return Status::InvalidPhase;
        }
        next.PedInWorld = next.PedUsesCollision = false; next.InVehicle = true;
    } else {
        // CVehicle::RemoveDriver sets Abandoned before clearing the driver ref.
        if (const auto status = AutomobileStatus(m_Automobile->SetStatus(NativeVehicleStatus::Abandoned, error), error); status != Status::Ok) return status;
        if (const auto status = AutomobileStatus(m_Automobile->RemoveOccupant(next.PedIdentity, error), error); status != Status::Ok) return status;
        next.PedInWorld = next.PedUsesCollision = true; next.InVehicle = false;
        next.PedPosition = *setPedOutPosition;
        m_Ped.Physical.Position = next.PedPosition; m_Ped.Physical.MoveSpeed = {};
        m_Ped.Physical.FrictionMoveSpeed = {}; m_Ped.Contact = {}; m_Ped.ControlPrepared = false;
        if (!m_PedWorld || m_PedWorld->AddPed(m_Ped) != NativeSourcePedWorldStatus::Ok) {
            error = "source ped-world rejected exit handoff"; return Status::InvalidPhase;
        }
    }
    next.Automobile = m_Automobile->LastCommitted();
    if (!m_Pool.Update(next.VehicleReference, VehicleState(true), error)) return Status::PoolError;
    next.PoolRevision = m_Pool.Revision(); next.PoolAlive = m_Pool.Census().Alive;
    next.LastPoolEvent = m_Pool.Events().back().Kind;
    const auto status = Publish(std::move(next), std::move(events), error);
    if (status == Status::Ok) ++m_NextEvent;
    return status;
}

NativeSourceVehicleLifecycleStatus NativeSourceVehicleLifecycle::Destroy(
    std::uint32_t nowMs, std::string& error) {
    if (!m_Published || !m_Automobile) { error = "source lifecycle not loaded"; return Status::NotLoaded; }
    if (m_Published->Phase != NativeSourceVehicleLifecyclePhase::OnFoot ||
        m_Automobile->LastCommitted()->Occupants.Driver || m_Automobile->LastCommitted()->Occupants.PassengerCount) {
        error = "live occupants or vehicle task prevent destruction"; return Status::InvalidPhase;
    }
    if (nowMs < m_Published->TimeMs) { error = "backward lifecycle destruction time"; return Status::StaleInput; }
    auto next = *m_Published; auto events = m_Events;
    next.Generation++; next.TimeMs = nowMs;
    next.Phase = NativeSourceVehicleLifecyclePhase::Destroyed;
    if (!Append(next, events, NativeSourceVehicleLifecycleEventKind::Destroyed, next.Phase,
            next.Task, nowMs, next.InputSequence, error)) return Status::Overflow;
    if (!m_Pool.Update(next.VehicleReference, VehicleState(false), error) ||
        !m_Pool.Release(next.VehicleReference, error)) return Status::PoolError;
    next.Automobile = m_Automobile->LastCommitted();
    next.PoolRevision = m_Pool.Revision(); next.PoolAlive = m_Pool.Census().Alive;
    next.LastPoolEvent = m_Pool.Events().back().Kind;
    const auto status = Publish(std::move(next), std::move(events), error);
    if (status == Status::Ok) {
        ++m_NextEvent;
        m_Automobile.reset(); m_ModelCollision.reset();
    }
    return status;
}

NativeSourceVehicleLifecycleStatus NativeSourceVehicleLifecycle::EvictWorld(std::uint64_t generation,
    std::shared_ptr<const NativeCollisionSnapshot> world, NativeSourcePedWorld& nextPedWorld,
    std::uint32_t nowMs, std::string& error) {
    if (!m_Published) { error = "source lifecycle not loaded"; return Status::NotLoaded; }
    if (m_Published->Phase != NativeSourceVehicleLifecyclePhase::Destroyed || m_Pool.Census().Alive) {
        error = "live vehicle prevents source world eviction"; return Status::InvalidPhase;
    }
    if (!world || generation <= m_Published->WorldGeneration || nowMs < m_Published->TimeMs ||
        &nextPedWorld == m_PedWorld || nextPedWorld.WorldGeneration() != generation || world->Instances.empty() ||
        std::ranges::none_of(world->Instances, [](const NativeCollisionInstance& instance) {
            return instance.Model && !instance.Model->Empty && instance.Model->Unsupported.empty();
        })) {
        error = "invalid or stale source world eviction"; return Status::StaleWorld;
    }
    auto next = *m_Published; auto events = m_Events;
    next.Generation++; next.WorldGeneration = generation; next.WorldCollision = std::move(world);
    next.TimeMs = nowMs; next.VehicleAsset.reset(); next.Automobile.reset();
    if (!Append(next, events, NativeSourceVehicleLifecycleEventKind::WorldEvicted, next.Phase,
            next.Task, nowMs, next.InputSequence, error)) return Status::Overflow;
    next.PoolRevision = m_Pool.Revision(); next.PoolAlive = m_Pool.Census().Alive;
    next.LastPoolEvent = m_Pool.Events().back().Kind;
    NativeSourcePedWorldPed ped;
    if (!m_PedWorld || m_PedWorld->Ped(next.PedIdentity, ped) != NativeSourcePedWorldStatus::Ok ||
        nextPedWorld.AddPed(ped) != NativeSourcePedWorldStatus::Ok) {
        error = "next source ped-world rejected migration"; return Status::InvalidPhase;
    }
    if (m_PedWorld->RemovePed(next.PedIdentity) != NativeSourcePedWorldStatus::Ok) {
        nextPedWorld.RemovePed(next.PedIdentity);
        error = "old source ped-world rejected migration"; return Status::InvalidPhase;
    }
    const auto status = Publish(std::move(next), std::move(events), error);
    if (status == Status::Ok) {
        ++m_NextEvent;
        m_PedWorld = &nextPedWorld; m_Ped = ped;
    }
    else {
        nextPedWorld.RemovePed(ped.Identity);
        m_PedWorld->AddPed(ped);
    }
    return status;
}
