// Immutable native-owned ped/vehicle broadphase for CCarGenerator::CheckForBlockage.
#pragma once
#include "NativeGeneratedVehicleAssets.h"
#include "NativeVehiclePool.h"
#include "NativeScriptPeds.h"

class RealtimeScriptHost;
class RealtimeGameplay;

enum class NativeLiveBoundsKnowledge { Unknown, SourceCol, ProvenSourceNull };
struct NativeLiveModelBounds {
    NativeLiveBoundsKnowledge Knowledge = NativeLiveBoundsKnowledge::Unknown;
    NativeCollisionVector Center{}, Min{}, Max{};
    float Radius{};
    bool operator==(const NativeLiveModelBounds&) const = default;
};
enum class NativeLiveEntityKind { Player, Vehicle, MissionPed };
struct NativeLiveEntityBound {
    NativeLiveEntityKind Kind{};
    std::int32_t Reference = -1, ModelId = -1;
    NativeVehicleProducer Producer = NativeVehicleProducer::Count;
    NativeGarageMatrix Transform;
    NativeLiveModelBounds Model;
    NativeCollisionVector WorldCenter{}; // source GetBoundCentre: M * COL.center
    bool operator==(const NativeLiveEntityBound&) const = default;
};
enum class NativeLiveBlockageStatus { Unsupported, Clear, Blocked };
enum class NativeLiveBlockageReason {
    None, InvalidInput, UnknownCandidateModel, UnknownEntityModel,
    UnknownSourceOrdering, StaleProof,
};
struct NativeLiveBlockageResult {
    NativeLiveBlockageStatus Status = NativeLiveBlockageStatus::Unsupported;
    NativeLiveBlockageReason Reason = NativeLiveBlockageReason::None;
    std::size_t XYCandidates{}, PossibleUnknownCandidates{}, ZTests{};
    std::uint64_t Frame{}, WorldGeneration{}, VehicleRevision{}, PlayerRevision{};
    NativeCollisionVector StoredPosition{};
    std::int32_t CandidateModelId = -1; // production typed packet identity
};

// Arithmetic seam for independent fixtures. This does NOT produce an owner
// proof. ProvenSourceNull applies only to the candidate's actual GetColModel()
// result, never a missing retained packet. An entity needs nonnull source COL.
NativeLiveModelBounds NativeLiveBoundsFromCol(const NativeCollisionModel* model);
// Producer-side binding from retained typed asset; no parsing or world insertion.
std::shared_ptr<const NativeVehicleModelCollision> NativeLiveVehicleModelCol(const NativeGeneratedVehicleAsset&);
NativeCollisionVector NativeLiveBoundCenter(const NativeGarageMatrix&, NativeCollisionVector local);
NativeLiveBlockageResult NativeLiveCheckForBlockage(std::span<const NativeLiveEntityBound>,
    NativeCollisionVector storedPosition, const NativeLiveModelBounds& candidate);

class NativeLiveEntityBounds {
public:
    // Captures the registered player plus the host's source mission-ped owner.
    // Capture after ReconcileCarGeneratorsBeforeWorldCommit + world adoption,
    // and PublishVehicles(frame). No IO, no allocation into vehicle pools.
    // Unknown vehicle COL is retained as a possible candidate, never omitted.
    static std::shared_ptr<const NativeLiveEntityBounds> Capture(const RealtimeScriptHost&,
        NativeScriptPedRef, std::shared_ptr<const NativeVehiclePoolSnapshot>, std::string& error);
    bool Matches(const RealtimeScriptHost&, std::uint64_t frame, std::uint64_t worldGeneration) const;
    NativeLiveBlockageResult Query(const RealtimeScriptHost&, std::uint64_t frame,
        std::uint64_t worldGeneration, NativeCollisionVector storedPosition,
        const NativeGeneratedVehicleAsset& candidate) const;
    NativeLiveBlockageResult QueryLoadedModel(const RealtimeScriptHost&, std::uint64_t frame,
        std::uint64_t worldGeneration, NativeCollisionVector storedPosition,
        std::int32_t modelId,
        const std::shared_ptr<const NativeCollisionModel>& sourceModel) const;
    std::span<const NativeLiveEntityBound> Entries() const { return m_Entries; }
    std::uint64_t Frame() const { return m_Vehicles->Frame(); }
    std::uint64_t WorldGeneration() const { return m_WorldGeneration; }
    std::uint64_t VehicleRevision() const { return m_Vehicles->Revision(); }
    std::uint64_t PlayerRevision() const { return m_PlayerRevision; }
    std::uint64_t MissionPedRevision() const { return m_MissionPedRevision; }
    const NativeVehicleCensus& VehicleCensus() const { return m_Vehicles->Census(); }
    // This covers registered producers only. No complete ambient population,
    // sector-link ordering or retail CWorld/CPopulation parity is asserted.
    static constexpr bool SourceParityComplete = false;
private:
    NativeLiveEntityBounds() = default;
    const RealtimeScriptHost* m_Host{};
    const RealtimeGameplay* m_Player{};
    NativeScriptPedRef m_Ped;
    std::shared_ptr<const NativeVehiclePoolSnapshot> m_Vehicles;
    std::shared_ptr<const NativeCollisionSnapshot> m_World;
    std::shared_ptr<const NativePlacementOverrides> m_Overrides;
    std::uint64_t m_WorldGeneration{}, m_HostWorldRevision{}, m_PlayerRevision{}, m_MissionPedRevision{};
    std::vector<NativeScriptPedState> m_MissionPeds;
    std::vector<NativeLiveEntityBound> m_Entries;
};
