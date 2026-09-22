#pragma once
#include "app/platform/linux/NativeGarages.h"
#include "app/platform/linux/NativeVehiclePool.h"
#include "app/platform/linux/RealtimeGameplay.h"

namespace realtime_streaming { struct CpuWorld; }
struct NativeGaragesRuntimeInput {
    std::uint64_t Frame = 0; // unpaused source frame counter, once per update
    bool Replay = false, Coop = false;
    std::optional<NativeVehicleRef> ScriptPlayerVehicle;
};
enum class NativeGaragesRuntimeBarrier { None, GarageUpdate, GarageCamera };
enum class NativeGarageTidyMode : std::uint8_t { Far, Near };
enum class NativeGarageTidyRequirement : std::uint8_t { None, VehicleAuthority, VehicleDestruction, NearVehicleCollision };
enum class NativeGarageTidyReason : std::uint8_t { Wrecked, UpVectorBelowHalf };
struct NativeGarageTidyCandidate {
    std::size_t SourceSlot = 0;
    NativeVehicleRef Vehicle;
    NativeGarageTidyReason Reason = NativeGarageTidyReason::Wrecked;
    NativeVehicleCreatedBy CreatedBy = NativeVehicleCreatedBy::Unsupported;
    bool MissionCleanupRegistered = false;
    bool ScriptLocked = false;
};
struct NativeGarageTidyPlan {
    NativeScriptServiceStatus Status = NativeScriptServiceStatus::Error;
    NativeGarageTidyRequirement Requirement = NativeGarageTidyRequirement::None;
    NativeGarageTidyMode Mode = NativeGarageTidyMode::Far;
    NativeGarageRef Garage;
    std::uint64_t Frame = 0;
    std::uint64_t VehicleOwner = 0, VehicleGeneration = 0, VehicleRevision = 0;
    std::size_t FirstSlot = 109, LastSlot = 1, Examined = 0, Destroyed = 0;
    std::vector<NativeGarageTidyCandidate> Candidates;
};
struct NativeGaragesRuntimeFrame {
    std::uint64_t Revision = 0, Generation = 0;
    std::uint64_t VehicleOwner = 0, VehicleGeneration = 0, VehicleRevision = 0;
    NativeVehicleCensus VehicleCensus;
    NativeGarageView View;
    NativeGarageCamera Camera;
    // Real existing follow camera, never a fabricated garage-fixed camera.
    RealtimeGameplayCamera BaselineCamera;
    NativeGaragesRuntimeBarrier Barrier = NativeGaragesRuntimeBarrier::None;
    NativeGarageRequirement Requirement = NativeGarageRequirement::None;
    std::optional<NativeGarageRef> Garage;
    std::optional<NativeGarageTidyPlan> TidyPlan;
    std::size_t UnsupportedUpdates = 0;
};

class NativeGaragesRuntime {
public:
    NativeGaragesRuntime(NativeGarages& garages,const RealtimeGameplay& gameplay) : m_Garages(garages),m_Gameplay(gameplay) {}
    NativeGaragesRuntime(NativeGarages& garages,const RealtimeGameplay& gameplay,const NativeVehiclePool& vehicles)
        : m_Garages(garages),m_Gameplay(gameplay),m_Vehicles(&vehicles) {}
    // Pass the ACTIVE committed CpuWorld, after its GPU upload/scene swap, not a
    // worker result still uploading. Parent owns that commit and both lifetimes.
    // Validates the coupled source-COL/placement generation before touching the
    // registry. Tick consumes actual gameplay state; no caller eligibility bool.
    NativeScriptServiceResult Tick(const realtime_streaming::CpuWorld& published, NativeGaragesRuntimeInput input,
        std::shared_ptr<const NativeVehiclePoolSnapshot> vehicles = {});
    // Validation failures leave this unchanged. Required source camera/type/
    // maintenance work latches Unsupported; later frames cannot skip that work.
    const NativeGaragesRuntimeFrame& Frame() const { return m_Frame; }
    // Pure adapter exposed for input fixtures; production Tick calls this with
    // its bound live gameplay owner. Vehicle render bounds are never substituted.
    static NativeScriptServiceResult MakeView(const RealtimeGameplayState&, const RealtimeGameplayCamera&,
        const NativeGarages&, NativeGaragesRuntimeInput, NativeGarageView& out,
        const NativeVehicleRecord* scriptVehicle = nullptr);
    // Pure immutable source-slot reader. Far mode never reads collision or any
    // world/render owner and never mutates/releases a candidate.
    static NativeGarageTidyPlan PlanTidy(const NativeGarageEntry&, NativeGarageRef, std::uint64_t frame,
        bool tidyClose, const NativeVehiclePoolSnapshot& vehicles);
private:
    NativeGarages& m_Garages;
    const RealtimeGameplay& m_Gameplay;
    const NativeVehiclePool* m_Vehicles = nullptr;
    NativeGaragesRuntimeFrame m_Frame;
    std::shared_ptr<const NativeCollisionSnapshot> m_PublishedCollision;
    std::shared_ptr<const NativeVehiclePoolSnapshot> m_PublishedVehicles;
    std::optional<NativeScriptServiceResult> m_Fault;
};
