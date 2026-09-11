#include "NativeLiveEntityBounds.h"
#include "RealtimeScriptHost.h"

#include <algorithm>
#include <cmath>

namespace {
bool Finite(NativeCollisionVector v) {
    return std::ranges::all_of(v, [](float f) { return std::isfinite(f); });
}
bool Valid(const NativeLiveModelBounds& b) {
    return b.Knowledge == NativeLiveBoundsKnowledge::SourceCol && Finite(b.Center) &&
        Finite(b.Min) && Finite(b.Max) && std::isfinite(b.Radius) && b.Radius >= 0 &&
        b.Min[0] <= b.Max[0] && b.Min[1] <= b.Max[1] && b.Min[2] <= b.Max[2];
}
NativeLiveEntityBound PlayerBound(const RealtimeScriptHost& host, NativeScriptPedRef ref,
    const RealtimeGameplay& player) {
    const auto& state = player.State();
    NativeLiveEntityBound entry;
    entry.Kind = NativeLiveEntityKind::Player;
    entry.Reference = ref.Value;
    entry.ModelId = 0; // FileLoader::LoadPedObject MODEL_PLAYER -> TempColModels ped1
    entry.Transform.Position = {state.PedRoot.X, state.PedRoot.Y, state.PedRoot.Z};
    const auto c = std::cos(state.PedCurrentRotation), s = std::sin(state.PedCurrentRotation);
    entry.Transform.Basis = {{{c, s, 0}, {-s, c, 0}, {0, 0, 1}}};
    entry.Model = NativeLiveBoundsFromCol(host.Garages().Ped1Collision().get());
    entry.WorldCenter = NativeLiveBoundCenter(entry.Transform, entry.Model.Center);
    return entry;
}
NativeLiveEntityBound VehicleBound(const NativeVehicleRecord& record) {
    NativeLiveEntityBound entry;
    entry.Kind = NativeLiveEntityKind::Vehicle;
    entry.Reference = record.Reference.Value;
    entry.ModelId = record.State.ModelId;
    entry.Producer = record.Producer;
    entry.Transform = record.State.Matrix;
    const auto& model = record.State.ModelCollision;
    entry.Model = NativeLiveBoundsFromCol(model && model->ModelId == record.State.ModelId ? model->Collision.get() : nullptr);
    entry.WorldCenter = NativeLiveBoundCenter(entry.Transform, entry.Model.Center);
    return entry;
}
bool SameExtent(const NativeVehicleProducerExtent& a, const NativeVehicleProducerExtent& b) {
    return a.Owned == b.Owned && a.NativeHostComplete == b.NativeHostComplete &&
        a.SourceParityComplete == b.SourceParityComplete;
}
bool CurrentVehicles(const NativeVehiclePool& owner, const NativeVehiclePoolSnapshot& packet) {
    const auto census = owner.Census();
    const auto& previous = packet.Census();
    if (owner.Owner() != packet.Owner() || owner.Revision() != packet.Revision() ||
        owner.PublicationGeneration() != packet.Generation() || !previous.Producers.NativeHostComplete ||
        !SameExtent(census.Producers, previous.Producers) || census.Alive != previous.Alive ||
        census.CreatedEvents != previous.CreatedEvents || census.UpdatedEvents != previous.UpdatedEvents ||
        census.ReleasedEvents != previous.ReleasedEvents) return false;
    for (std::size_t i = 0; i < NativeVehiclePoolCapacity; ++i) {
        const auto* a = owner.AtSlot(i);
        const auto* b = packet.AtSlot(i);
        if (bool(a) != bool(b) || (a && *a != *b)) return false;
    }
    return true;
}
} // namespace

NativeLiveModelBounds NativeLiveBoundsFromCol(const NativeCollisionModel* model) {
    if (!model) return {}; // Missing ownership is NOT proven GetColModel()==nullptr.
    NativeLiveModelBounds bounds{NativeLiveBoundsKnowledge::SourceCol,
        model->BoundCenter, model->Min, model->Max, model->BoundRadius};
    if (!Valid(bounds)) return {};
    return bounds; // Primitive/render support is irrelevant to these COL bounds.
}
std::shared_ptr<const NativeVehicleModelCollision> NativeLiveVehicleModelCol(const NativeGeneratedVehicleAsset& packet) {
    if (!packet || !packet->Collision) return {};
    return std::make_shared<const NativeVehicleModelCollision>(NativeVehicleModelCollision{
        packet->Definition.ModelId, packet->Collision});
}
NativeCollisionVector NativeLiveBoundCenter(const NativeGarageMatrix& matrix, NativeCollisionVector local) {
    NativeCollisionVector result{};
    for (std::size_t i = 0; i < 3; ++i) {
        result[i] = matrix.Basis[0][i] * local[0] + matrix.Basis[1][i] * local[1] +
            matrix.Basis[2][i] * local[2] + matrix.Position[i];
    }
    return result;
}
NativeLiveBlockageResult NativeLiveCheckForBlockage(std::span<const NativeLiveEntityBound> entries,
    NativeCollisionVector pos, const NativeLiveModelBounds& candidate) {
    NativeLiveBlockageResult result;
    result.StoredPosition = pos;
    const auto unsupported = [&](NativeLiveBlockageReason reason) {
        result.Reason = reason;
        return result;
    };
    if (!Finite(pos)) return unsupported(NativeLiveBlockageReason::InvalidInput);
    const bool nullModel = candidate.Knowledge == NativeLiveBoundsKnowledge::ProvenSourceNull;
    if (!nullModel && !Valid(candidate)) return unsupported(NativeLiveBlockageReason::UnknownCandidateModel);
    const float radius = nullModel ? 2.0f : candidate.Radius;
    std::array<const NativeLiveEntityBound*, 8> selected{};
    for (const auto& entry : entries) {
        if (!Valid(entry.Model) || !Finite(entry.WorldCenter) || !Finite(entry.Transform.Position)) {
            ++result.PossibleUnknownCandidates;
            continue;
        }
        const float dx = entry.WorldCenter[0] - pos[0], dy = entry.WorldCenter[1] - pos[1];
        // Vector2D::Magnitude: float square/sum then sqrt. Strict circle, not AABB.
        const float distance = std::sqrt(dx * dx + dy * dy);
        if (distance >= entry.Model.Radius + radius) continue;
        if (result.XYCandidates < selected.size()) selected[result.XYCandidates] = &entry;
        ++result.XYCandidates;
    }
    if (result.PossibleUnknownCandidates) return unsupported(NativeLiveBlockageReason::UnknownEntityModel);
    // No verified CWorld sector/list insertion-order owner exists in this native
    // track. Even an early blocker cannot authorize a result when overflow exists.
    if (result.XYCandidates > selected.size()) return unsupported(NativeLiveBlockageReason::UnknownSourceOrdering);
    const float lower = pos[2] + (nullModel ? -1.0f : candidate.Min[2]);
    const float upper = pos[2] + (nullModel ? 1.0f : candidate.Max[2]);
    for (std::size_t i = 0; i < result.XYCandidates; ++i) {
        const auto& entry = *selected[i];
        ++result.ZTests;
        // Deliberately origin Z + UNROTATED local bbox, not world AABB/center Z.
        if (entry.Transform.Position[2] + entry.Model.Max[2] + 1.0f > lower &&
            entry.Transform.Position[2] + entry.Model.Min[2] - 1.0f < upper) {
            result.Status = NativeLiveBlockageStatus::Blocked;
            return result;
        }
    }
    result.Status = NativeLiveBlockageStatus::Clear;
    return result;
}

std::shared_ptr<const NativeLiveEntityBounds> NativeLiveEntityBounds::Capture(const RealtimeScriptHost& host,
    NativeScriptPedRef ped, std::shared_ptr<const NativeVehiclePoolSnapshot> vehicles, std::string& error) {
    const auto* player = host.ResolvePed(ped);
    const auto& residency = host.CarGeneratorResidency();
    if (!player || !player->State().Ready || player->State().CarPresent || !vehicles ||
        !CurrentVehicles(host.Vehicles(), *vehicles) || !residency.Generation() || !residency.Snapshot()) {
        error = "live bounds require registered Ready player, complete current native pool, committed world and no unregistered preview car";
        return {};
    }
    if (host.Publication().SourceCollision != residency.Snapshot() ||
        host.Publication().Overrides != residency.Snapshot()->Overrides) {
        error = "live bounds require exact adopted host world/override identities after residency commit";
        return {};
    }
    const auto pedModel = NativeLiveBoundsFromCol(host.Garages().Ped1Collision().get());
    if (pedModel.Knowledge != NativeLiveBoundsKnowledge::SourceCol || pedModel.Radius != 1.0f ||
        pedModel.Center != NativeCollisionVector{} || pedModel.Min[2] != -1.0f || pedModel.Max[2] != 0.95f) {
        error = "live player requires actual source TempColModels ped1 ownership";
        return {};
    }
    auto proof = std::shared_ptr<NativeLiveEntityBounds>(new NativeLiveEntityBounds);
    proof->m_Host = &host;
    proof->m_Player = player;
    proof->m_Ped = ped;
    proof->m_Vehicles = std::move(vehicles);
    proof->m_World = residency.Snapshot();
    proof->m_Overrides = proof->m_World->Overrides;
    proof->m_WorldGeneration = residency.Generation();
    proof->m_HostWorldRevision = host.WorldRevision();
    proof->m_PlayerRevision = player->Activity().Revision;
    proof->m_Entries.push_back(PlayerBound(host, ped, *player));
    for (std::size_t slot = 0; slot < NativeVehiclePoolCapacity; ++slot) {
        const auto* record = proof->m_Vehicles->AtSlot(slot);
        if (record && record->State.InWorld) proof->m_Entries.push_back(VehicleBound(*record));
    }
    error.clear();
    return proof;
}
bool NativeLiveEntityBounds::Matches(const RealtimeScriptHost& host, std::uint64_t frame,
    std::uint64_t worldGeneration) const {
    if (&host != m_Host || frame != Frame() || worldGeneration != m_WorldGeneration ||
        host.WorldRevision() != m_HostWorldRevision || host.CarGeneratorResidency().Generation() != m_WorldGeneration ||
        host.CarGeneratorResidency().Snapshot() != m_World || m_World->Overrides != m_Overrides ||
        host.Publication().SourceCollision != m_World || host.Publication().Overrides != m_Overrides ||
        !CurrentVehicles(host.Vehicles(), *m_Vehicles)) return false;
    const auto* player = host.ResolvePed(m_Ped);
    if (player != m_Player || !player || !player->State().Ready || player->State().CarPresent ||
        player->Activity().Revision != m_PlayerRevision || PlayerBound(host, m_Ped, *player) != m_Entries.front()) return false;
    std::size_t index = 1;
    for (std::size_t slot = 0; slot < NativeVehiclePoolCapacity; ++slot) {
        const auto* record = host.Vehicles().AtSlot(slot);
        if (record && record->State.InWorld) {
            if (index >= m_Entries.size() || VehicleBound(*record) != m_Entries[index++]) return false;
        }
    }
    return index == m_Entries.size();
}
NativeLiveBlockageResult NativeLiveEntityBounds::Query(const RealtimeScriptHost& host, std::uint64_t frame,
    std::uint64_t worldGeneration, NativeCollisionVector pos, const NativeGeneratedVehicleAsset& candidate) const {
    NativeLiveBlockageResult result;
    if (!Matches(host, frame, worldGeneration)) result.Reason = NativeLiveBlockageReason::StaleProof;
    else result = NativeLiveCheckForBlockage(m_Entries, pos,
        NativeLiveBoundsFromCol(candidate ? candidate->Collision.get() : nullptr));
    result.Frame = Frame();
    result.WorldGeneration = m_WorldGeneration;
    result.VehicleRevision = VehicleRevision();
    result.PlayerRevision = PlayerRevision();
    result.StoredPosition = pos;
    result.CandidateModelId = candidate ? candidate->Definition.ModelId : -1;
    return result;
}
