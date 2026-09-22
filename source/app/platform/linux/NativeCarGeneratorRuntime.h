// Main-thread adapter for the bounded exterior native new-game owner.
#pragma once

#include "app/platform/linux/NativeCarGenerators.h"
#include "app/platform/linux/RealtimeGameplay.h"
#include "app/platform/linux/RealtimeHud.h"

#include <thread>

namespace realtime_streaming { struct CpuWorld; }
struct NativeScriptState;

struct NativeCarGeneratorRuntimeInput {
    std::uint64_t Frame = 0; // actual unpaused host update, not a retry counter
    std::uint8_t Area = 0; // current native extent is exclusively exterior 0
    float GenerationDistanceMultiplier = 1.0f; // source camera owner value
    // Exact player vehicle position from the current NativeScm pool owner,
    // never from the diagnostic controller's unused car transform.
    std::optional<NativeScriptPosition> ScriptVehiclePosition;
    // CapturePriceView after installing the world camera, before actor transforms.
    // Missing matrices leave visibility Unknown; never assume visible/unoccluded.
    std::optional<RealtimeHudPriceView> Camera;
};

struct NativeCarGeneratorQueryWitness {
    NativeCarGeneratorObservation Observation;
    bool FrustumTested = false, InFrustum = false;
    bool VerticalRayTested = false, VerticalRayHit = false;
    RealtimeVec3 VerticalHit;
    NativeCollisionHit VerticalSource;
    bool CameraRayTested = false, CameraRayHit = false;
    RealtimeVec3 CameraHit;
    NativeCollisionHit CameraSource;
    // These are diagnostic source-COL witnesses, NOT COcclusion or the
    // building-only ProcessVerticalLine result. CpuWorld has no entity mask.
};

struct NativeCarGeneratorDemandId {
    std::uint64_t Owner = 0, Sequence = 0;
    bool operator==(const NativeCarGeneratorDemandId&) const = default;
};

struct NativeCarGeneratorRuntimeDemand {
    NativeCarGeneratorDemandId Id;
    std::uint64_t Frame = 0, WorldGeneration = 0, RegistryRevision = 0;
    std::uint64_t VehicleOwner = 0, VehicleGeneration = 0, VehicleRevision = 0;
    std::uint64_t ActivityRevision = 0;
    std::uint32_t GameMs = 0;
    NativeCarGeneratorAction Action; // exact unmodified source Process request
    NativeCarGeneratorState GeneratorState; // source slot + provenance at issue
    std::shared_ptr<const NativeCollisionSnapshot> Collision;
    std::shared_ptr<const NativeVehiclePoolSnapshot> Vehicles;
};

struct NativeCarGeneratorRuntimeFrame {
    std::uint64_t Revision = 0, Frame = 0, WorldGeneration = 0, RegistryRevision = 0;
    std::uint64_t ActivityRevision = 0, VehicleOwner = 0, VehicleGeneration = 0, VehicleRevision = 0;
    std::uint32_t GameMs = 0, ParkedCars = 0;
    std::size_t FreeVehicleSlots = 0;
    std::uint8_t ClockHour = 0, Area = 0;
    bool ScriptVehicleActive = false;
    NativeScriptPosition PlayerCenter, Camera;
    // Actual root displacement / controller simulated interval, converted to
    // source units (m/s / 50). This is native interval motion, not a claim that
    // the controller exposes original CPed::m_vecMoveSpeed (it does not).
    std::array<float, 3> MeasuredPlayerSpeed{};
    double MotionIntervalSeconds = 0;
    NativeCarGeneratorProcessFrame Process;
    std::vector<NativeCarGeneratorQueryWitness> Queries;
    std::vector<NativeCarGeneratorRuntimeDemand> Demands;
};

class NativeCarGeneratorRuntime {
public:
    // Construct on the main thread after source-player construction. References
    // must be the host's actual owners and outlive this adapter.
    NativeCarGeneratorRuntime(NativeCarGenerators&, const RealtimeGameplay&,
        const NativeVehiclePool&, const NativeScriptState&);
    NativeCarGeneratorRuntime(const NativeCarGeneratorRuntime&) = delete;
    NativeCarGeneratorRuntime& operator=(const NativeCarGeneratorRuntime&) = delete;

    // Only the ACTIVE scene after upload/swap. SourceCollision and Collision
    // must be the coupled build; never pass a worker/uploading packet. No IO,
    // parsers, model loads, BVH rebuilds or vehicle allocation occur here.
    NativeScriptServiceResult Tick(const realtime_streaming::CpuWorld&, NativeCarGeneratorRuntimeInput,
        std::shared_ptr<const NativeVehiclePoolSnapshot>);
    const NativeCarGeneratorRuntimeFrame& Frame() const { return m_Frame; }
    const NativeCarGeneratorRuntimeDemand* ResolveDemand(NativeCarGeneratorDemandId) const;

    // Pure query adapter (except main-thread BVH counters). Also useful for
    // independently witnessing source-world geometry away from the player.
    static NativeCarGeneratorQueryWitness Observe(const realtime_streaming::CpuWorld&,
        NativeCarGeneratorRef, NativeScriptPosition, const RealtimeGameplayCamera&,
        const std::optional<RealtimeHudPriceView>&);

    // Outstanding work is intentionally retained. There is no acknowledge,
    // fake completion, pool-only spawn or retry-Process API. A future fulfillment
    // transaction must verify this demand's identities/generations, commit real
    // model/render/COL/physics/world/pool effects and consume each request once.
private:
    NativeCarGenerators& m_Registry;
    const RealtimeGameplay& m_Gameplay;
    const NativeVehiclePool& m_Vehicles;
    const NativeScriptState& m_Script;
    const std::thread::id m_Thread;
    const std::uint64_t m_Owner;
    RealtimeGameplayState m_Previous;
    NativeCarGeneratorRuntimeFrame m_Frame;
    std::shared_ptr<const NativeCollisionSnapshot> m_Collision;
    std::uint64_t m_NextDemand = 0;
};
