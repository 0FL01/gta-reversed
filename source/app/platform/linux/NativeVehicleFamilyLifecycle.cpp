#include "NativeVehicleFamilyLifecycle.h"

#include <algorithm>
#include <cmath>
#include <ranges>
#include <stdexcept>

namespace {
bool HasCollision(const NativeCollisionModel& collision) {
    return collision.Empty == false && collision.Unsupported.empty() &&
        (!collision.Spheres.empty() || !collision.Boxes.empty() || !collision.Faces.empty());
}
}

NativeVehicleFamilyLifecycle::NativeVehicleFamilyLifecycle() {
    std::string error;
    if (!m_Pool.BindProducer(NativeVehicleProducer::SaveLoad, error) || !m_Pool.SealProducerExtent(error))
        throw std::runtime_error(error);
}

NativeVehicleFamilyLifecycleRecord* NativeVehicleFamilyLifecycle::ResolveMutable(NativeVehicleRef reference) noexcept {
    for (auto& record : m_Records) {
        if (record && record->Reference == reference && !record->Destroyed) return &*record;
    }
    return nullptr;
}

const NativeVehicleFamilyLifecycleRecord* NativeVehicleFamilyLifecycle::Resolve(NativeVehicleRef reference) const noexcept {
    for (const auto& record : m_Records) {
        if (record && record->Reference == reference && !record->Destroyed) return &*record;
    }
    return nullptr;
}

NativeVehicleFamilyLifecycleStatus NativeVehicleFamilyLifecycle::Spawn(
    const NativeCarGeneratorModelDefinition& definition,
    std::shared_ptr<const NativeCollisionModel> collision,
    std::uint8_t maximumPassengers, NativeVehicleRef& out, std::string& error) {
    if (!collision || !HasCollision(*collision) || maximumPassengers > 8) {
        error = "vehicle family spawn requires source collision and bounded seats";
        return NativeVehicleFamilyLifecycleStatus::InvalidInput;
    }
    NativeVehicleFamilyState family;
    if (!NativeVehicleFamilies::Construct(definition, NativeVehicleCreatedBy::Permanent, family, error))
        return NativeVehicleFamilyLifecycleStatus::InvalidInput;
    NativeVehicleState state;
    state.ModelId = definition.ModelId;
    state.Type = family.RuntimeType;
    state.SubType = std::int32_t(family.RuntimeSubType);
    state.Status = family.InitialStatus;
    state.CreatedBy = family.CreatedBy;
    state.InWorld = true;
    state.Collision = collision;
    state.ModelCollision = std::make_shared<const NativeVehicleModelCollision>(
        NativeVehicleModelCollision{definition.ModelId, collision});
    const auto created = m_Pool.Allocate({NativeVehicleProducer::SaveLoad, {}, definition.ModelId, std::move(state)});
    if (created.Result.Status != NativeScriptServiceStatus::Ready) {
        error = created.Result.Message;
        return NativeVehicleFamilyLifecycleStatus::PoolError;
    }
    std::size_t free = m_Records.size();
    for (std::size_t i = 0; i < m_Records.size(); ++i) if (!m_Records[i]) { free = i; break; }
    if (free == m_Records.size()) {
        std::string ignored;
        m_Pool.Release(created.Reference, ignored);
        error = "vehicle lifecycle record capacity exceeded";
        return NativeVehicleFamilyLifecycleStatus::CapacityExceeded;
    }
    m_Records[free] = NativeVehicleFamilyLifecycleRecord{created.Reference, m_Epoch,
        definition.ModelId, definition.ModelName, definition.Type, family.Constructor,
        std::move(collision), 1000.0f, 0, {}, maximumPassengers, 0, 0, false, true};
    out = created.Reference;
    ++m_Revision;
    error.clear();
    return NativeVehicleFamilyLifecycleStatus::Ok;
}

NativeVehicleFamilyLifecycleStatus NativeVehicleFamilyLifecycle::SetDriver(
    NativeVehicleRef reference, std::uint64_t identity, std::string& error) {
    auto* record = ResolveMutable(reference);
    if (!record) { error = "vehicle lifecycle reference is stale"; return NativeVehicleFamilyLifecycleStatus::StaleReference; }
    if (!identity || record->Driver) { error = "vehicle driver identity is invalid or occupied"; return NativeVehicleFamilyLifecycleStatus::Occupied; }
    if (std::ranges::find(record->Passengers, identity) != record->Passengers.end()) {
        error = "vehicle occupant identity is duplicated";
        return NativeVehicleFamilyLifecycleStatus::Occupied;
    }
    record->Driver = identity;
    ++m_Revision;
    error.clear();
    return NativeVehicleFamilyLifecycleStatus::Ok;
}

NativeVehicleFamilyLifecycleStatus NativeVehicleFamilyLifecycle::AddPassenger(
    NativeVehicleRef reference, std::uint64_t identity, std::uint8_t seat, std::string& error) {
    auto* record = ResolveMutable(reference);
    if (!record) { error = "vehicle lifecycle reference is stale"; return NativeVehicleFamilyLifecycleStatus::StaleReference; }
    if (!identity || seat >= record->MaximumPassengers || record->Passengers[seat] || record->Driver == identity ||
        std::ranges::find(record->Passengers, identity) != record->Passengers.end()) {
        error = "vehicle passenger identity is invalid, duplicate or occupied";
        return NativeVehicleFamilyLifecycleStatus::Occupied;
    }
    record->Passengers[seat] = identity;
    ++m_Revision;
    error.clear();
    return NativeVehicleFamilyLifecycleStatus::Ok;
}

NativeVehicleFamilyLifecycleStatus NativeVehicleFamilyLifecycle::ApplyResolvedCollision(
    NativeVehicleRef reference, std::uint32_t contacts, float damage, std::string& error) {
    auto* record = ResolveMutable(reference);
    if (!record) { error = "vehicle lifecycle reference is stale"; return NativeVehicleFamilyLifecycleStatus::StaleReference; }
    if (!contacts || !std::isfinite(damage) || damage < 0.0f) {
        error = "resolved vehicle collision is invalid";
        return NativeVehicleFamilyLifecycleStatus::InvalidInput;
    }
    const float health = std::max(0.0f, record->Health - damage);
    if (!std::isfinite(health)) { error = "vehicle damage overflow"; return NativeVehicleFamilyLifecycleStatus::Overflow; }
    record->Health = health;
    record->CollisionContacts = contacts;
    ++record->DamageRevision;
    ++m_Revision;
    error.clear();
    return NativeVehicleFamilyLifecycleStatus::Ok;
}

NativeVehicleFamilyLifecycleStatus NativeVehicleFamilyLifecycle::Destroy(
    NativeVehicleRef reference, std::string& error) {
    auto* record = ResolveMutable(reference);
    if (!record) { error = "vehicle lifecycle reference is stale"; return NativeVehicleFamilyLifecycleStatus::StaleReference; }
    auto state = m_Pool.Resolve(reference)->State;
    state.Status = NativeVehicleStatus::Wrecked;
    state.InWorld = false;
    if (!m_Pool.Update(reference, state, error) || !m_Pool.Release(reference, error))
        return NativeVehicleFamilyLifecycleStatus::PoolError;
    record->Driver = 0;
    record->Passengers.fill(0);
    record->Destroyed = true;
    record->InWorld = false;
    record->Collision.reset();
    ++m_Revision;
    error.clear();
    return NativeVehicleFamilyLifecycleStatus::Ok;
}

NativeVehicleFamilyLifecycleStatus NativeVehicleFamilyLifecycle::Reload(
    std::uint64_t nextEpoch, std::string& error) {
    if (nextEpoch <= m_Epoch) { error = "vehicle lifecycle reload epoch is stale"; return NativeVehicleFamilyLifecycleStatus::InvalidInput; }
    for (auto& record : m_Records) {
        if (!record) continue;
        if (!record->Destroyed) {
            auto state = m_Pool.Resolve(record->Reference)->State;
            state.Status = NativeVehicleStatus::Wrecked;
            state.InWorld = false;
            if (!m_Pool.Update(record->Reference, state, error) || !m_Pool.Release(record->Reference, error))
                return NativeVehicleFamilyLifecycleStatus::PoolError;
        }
        record.reset();
    }
    m_Epoch = nextEpoch;
    ++m_Revision;
    error.clear();
    return NativeVehicleFamilyLifecycleStatus::Ok;
}

std::shared_ptr<const NativeVehicleFamilyLifecycleSnapshot> NativeVehicleFamilyLifecycle::Publish() const {
    auto snapshot = std::make_shared<NativeVehicleFamilyLifecycleSnapshot>();
    snapshot->Epoch = m_Epoch;
    snapshot->Revision = m_Revision;
    for (const auto& record : m_Records) if (record) snapshot->Records.push_back(*record);
    return snapshot;
}
