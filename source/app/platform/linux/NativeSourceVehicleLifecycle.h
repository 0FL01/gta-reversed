#pragma once

#include "NativeSourceAutomobile.h"
#include "NativeSourceCamera.h"
#include "NativeSourcePad.h"
#include "NativeSourcePedWorld.h"
#include "NativeVehicleAssetQueue.h"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

enum class NativeSourceVehicleLifecycleStatus : std::uint8_t {
    Ok, NotLoaded, InvalidInput, InvalidPhase, StaleWorld, StaleInput,
    TransitionOutstanding, PoolError, AutomobileError, CameraError, Overflow,
};
enum class NativeSourceVehicleLifecyclePhase : std::uint8_t {
    Empty, OnFoot, Entering, Driving, Exiting, Destroyed,
};
enum class NativeSourceVehicleLifecycleTask : std::uint16_t {
    PlayerOnFoot = 0, EnterCarAsDriver = 701, LeaveCar = 704, CarDrive = 710,
};
enum class NativeSourceVehicleLifecycleEventKind : std::uint8_t {
    Spawn, EnterRequested, DriverAttached, Drive, ExitRequested,
    DriverDetached, Destroyed, WorldEvicted,
};

struct NativeSourceVehicleLifecycleEvent {
    std::uint64_t Sequence{}, WorldGeneration{}, InputSequence{}, PedIdentity{}, VehicleIdentity{};
    NativeSourceVehicleLifecycleEventKind Kind{};
    NativeSourceVehicleLifecyclePhase Phase{};
    NativeVehicleRef VehicleReference;
    NativeSourceVehicleLifecycleTask Task = NativeSourceVehicleLifecycleTask::PlayerOnFoot;
    std::uint32_t TimeMs{};
    bool operator==(const NativeSourceVehicleLifecycleEvent&) const = default;
};

struct NativeSourceVehicleLifecycleSpawn {
    std::uint64_t WorldGeneration{}, PedIdentity{}, VehicleIdentity{};
    std::shared_ptr<const NativeCollisionSnapshot> WorldCollision;
    std::shared_ptr<const NativeVehicleAssetCompletion> VehicleAsset;
    CarPoseMeasure Suspension;
    HandlingParams Handling;
    NativeGarageMatrix VehicleMatrix;
    NativeVehicleCreatedBy CreatedBy = NativeVehicleCreatedBy::Mission;
    NativeCollisionVector PedPosition{};
    std::uint32_t TimeMs{};
};

struct NativeSourceVehicleLifecycleWorldTarget {
    std::uint64_t WorldGeneration{};
    NativeSourceAutomobileTarget Target;
    const NativeSourceSurfaces* Surfaces{};
};

struct NativeSourceVehicleLifecycleSnapshot {
    std::uint64_t Epoch{}, Generation{}, WorldGeneration{}, PedIdentity{}, VehicleIdentity{};
    NativeSourceVehicleLifecyclePhase Phase = NativeSourceVehicleLifecyclePhase::Empty;
    NativeSourceVehicleLifecycleTask Task = NativeSourceVehicleLifecycleTask::PlayerOnFoot;
    NativeVehicleRef VehicleReference;
    NativeVehicleEventKind LastPoolEvent = NativeVehicleEventKind::Allocated;
    std::uint64_t PoolOwner{}, PoolRevision{};
    std::size_t PoolAlive{};
    NativeCollisionVector PedPosition{};
    bool PedInWorld{}, PedUsesCollision{}, InVehicle{};
    std::uint8_t PadButtons{};
    std::uint64_t InputSequence{};
    std::uint32_t TimeMs{};
    std::shared_ptr<const NativeCollisionSnapshot> WorldCollision;
    std::shared_ptr<const NativeVehicleAssetCompletion> VehicleAsset;
    std::shared_ptr<const NativeSourceAutomobileState> Automobile;
    std::shared_ptr<const NativeSourceCameraSnapshot> Camera;
    std::vector<NativeSourceVehicleLifecycleEvent> Events;
    bool operator==(const NativeSourceVehicleLifecycleSnapshot&) const = default;
};

// Bounded main-thread owner for one ordinary player/model400 lifecycle. Normal
// source Pad samples select enter/drive/exit; the address-backed door animation
// trees report their final SetPedIn/SetPedOut handoff through CompleteTask().
// Neither transition is an instant input warp. World generation is an immutable
// source-COL identity and cannot be evicted until the vehicle leaves world/pool.
// No Godot transform, diagnostic gameplay or render pointer is an input.
class NativeSourceVehicleLifecycle {
public:
    NativeSourceVehicleLifecycle();
    NativeSourceVehicleLifecycle(const NativeSourceVehicleLifecycle&) = delete;
    NativeSourceVehicleLifecycle& operator=(const NativeSourceVehicleLifecycle&) = delete;

    NativeSourceVehicleLifecycleStatus Spawn(const NativeSourceVehicleLifecycleSpawn&,
        NativeSourcePedWorld&, const NativeSourcePedWorldPed&, std::string& error);
    NativeSourceVehicleLifecycleStatus Tick(const NativeSourcePadFrame&, std::uint32_t nowMs,
        float timeStep, std::array<float, 3> activeSourceFront,
        const NativeSourceVehicleLifecycleWorldTarget*, std::string& error);
    // Enter completion has no world position. Exit completion must supply the
    // finite, nearby SetPedOut position produced by the task boundary; this
    // owner never invents a door/world result or reads a presentation node.
    NativeSourceVehicleLifecycleStatus CompleteTask(std::uint32_t nowMs,
        std::optional<NativeCollisionVector> setPedOutPosition, std::string& error);
    NativeSourceVehicleLifecycleStatus Destroy(std::uint32_t nowMs, std::string& error);
    NativeSourceVehicleLifecycleStatus EvictWorld(std::uint64_t nextGeneration,
        std::shared_ptr<const NativeCollisionSnapshot> nextWorld, NativeSourcePedWorld& nextPedWorld,
        std::uint32_t nowMs, std::string& error);

    std::shared_ptr<const NativeSourceVehicleLifecycleSnapshot> LastCommitted() const noexcept { return m_Published; }
    const NativeVehiclePool& Pool() const noexcept { return m_Pool; }
    std::span<const NativeSourceVehicleLifecycleEvent> Events() const noexcept { return m_Events; }

private:
    NativeSourceVehicleLifecycleStatus Publish(NativeSourceVehicleLifecycleSnapshot&&,
        std::vector<NativeSourceVehicleLifecycleEvent>&&, std::string& error);
    NativeSourceVehicleLifecycleStatus AutomobileStatus(NativeSourceAutomobileStatus, std::string& error);
    NativeSourceVehicleLifecycleStatus CameraStatus(NativeSourceCameraStatus, std::string& error);
    NativeVehicleState VehicleState(bool inWorld) const;
    bool Append(const NativeSourceVehicleLifecycleSnapshot&, std::vector<NativeSourceVehicleLifecycleEvent>&,
        NativeSourceVehicleLifecycleEventKind,
        NativeSourceVehicleLifecyclePhase, NativeSourceVehicleLifecycleTask, std::uint32_t, std::uint64_t,
        std::string& error);

    NativeVehiclePool m_Pool;
    std::unique_ptr<NativeSourceAutomobile> m_Automobile;
    std::shared_ptr<const NativeVehicleModelCollision> m_ModelCollision;
    NativeSourcePedWorld* m_PedWorld{};
    NativeSourcePedWorldPed m_Ped;
    NativeSourceCamera m_Camera;
    std::shared_ptr<const NativeSourceVehicleLifecycleSnapshot> m_Published;
    std::vector<NativeSourceVehicleLifecycleEvent> m_Events;
    std::uint64_t m_NextEvent = 1, m_NextEpoch = 1;
};
