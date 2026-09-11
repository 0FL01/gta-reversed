#include "app/platform/linux/NativeVehiclePool.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <exception>
#include <type_traits>
#include <utility>

namespace {
static_assert(std::is_nothrow_move_assignable_v<NativeVehicleRecord>);

bool ValidProducer(NativeVehicleProducer producer) {
    return static_cast<std::size_t>(producer) < NativeVehicleProducerCount;
}

bool Finite(const NativeGarageMatrix& matrix) {
    const auto finite = [](const NativeCollisionVector& vector) {
        return std::ranges::all_of(vector, [](float value) { return std::isfinite(value); });
    };
    return finite(matrix.Position) && std::ranges::all_of(matrix.Basis, finite);
}

bool ValidState(const NativeVehicleState& state, std::string& error) {
    if (state.ModelId < 0) {
        error = "vehicle model ID is required";
        return false;
    }
    if (static_cast<std::uint8_t>(state.Type) > static_cast<std::uint8_t>(NativeVehicleType::Trailer)) {
        error = "vehicle source type is unsupported";
        return false;
    }
    if (state.SubType < 0 || state.SubType > 11) {
        error = "vehicle source subtype is unsupported";
        return false;
    }
    if (static_cast<std::uint8_t>(state.Status) > static_cast<std::uint8_t>(NativeVehicleStatus::PlayerDisabled)) {
        error = "vehicle source status is unsupported";
        return false;
    }
    const auto createdBy = static_cast<std::uint8_t>(state.CreatedBy);
    if (createdBy < static_cast<std::uint8_t>(NativeVehicleCreatedBy::Random) ||
        createdBy > static_cast<std::uint8_t>(NativeVehicleCreatedBy::Permanent)) {
        error = "vehicle created-by owner is required";
        return false;
    }
    if (!Finite(state.Matrix)) {
        error = "vehicle matrix is nonfinite";
        return false;
    }
    if (state.InWorld && !state.Collision) {
        error = "world vehicle requires source collision";
        return false;
    }
    if (state.ModelCollision && state.ModelCollision->ModelId != state.ModelId) {
        error = "vehicle model-info COL binding belongs to another model";
        return false;
    }
    error.clear();
    return true;
}

NativeScriptReferenceResult<NativeVehicleRef> AllocationError(std::string message) {
    return {{NativeScriptServiceStatus::Error, std::move(message)}, {}};
}
} // namespace

NativeVehiclePool::NativeVehiclePool() {
    static std::atomic<std::uint64_t> owners{0};
    m_Owner = ++owners;
}

bool NativeVehiclePool::BindProducer(NativeVehicleProducer producer, std::string& error) {
    if (!ValidProducer(producer)) {
        error = "invalid native vehicle producer";
        return false;
    }
    if (m_ProducerExtentSealed || m_PublicationGeneration) {
        error = "native vehicle producer extent is already published";
        return false;
    }
    auto& owned = m_Producers.Owned[static_cast<std::size_t>(producer)];
    if (owned) {
        error = "native vehicle producer already bound";
        return false;
    }
    owned = true;
    error.clear();
    return true;
}

bool NativeVehiclePool::SealProducerExtent(std::string& error) {
    if (m_ProducerExtentSealed || m_PublicationGeneration) {
        error = "native vehicle producer extent is already sealed or published";
        return false;
    }
    if (std::ranges::none_of(m_Producers.Owned, [](bool owned) { return owned; })) {
        error = "native vehicle producer extent is empty";
        return false;
    }
    m_ProducerExtentSealed = true;
    m_Producers.NativeHostComplete = true;
    error.clear();
    return true;
}

NativeScriptReferenceResult<NativeVehicleRef> NativeVehiclePool::Allocate(const NativeVehicleCreateRequest& request) try {
    if (!m_ProducerExtentSealed) {
        return AllocationError("native vehicle producer extent is incomplete");
    }
    if (!ValidProducer(request.Producer) || !m_Producers.Owned[static_cast<std::size_t>(request.Producer)]) {
        return AllocationError("vehicle producer is not bound to this pool");
    }
    std::string error;
    if (!ValidState(request.State, error)) {
        return AllocationError(std::move(error));
    }
    const auto free = std::ranges::find_if(m_Slots, [](const auto& slot) { return !slot; });
    if (free == m_Slots.end()) {
        return AllocationError("native vehicle pool capacity exhausted");
    }
    const auto slot = static_cast<std::size_t>(std::distance(m_Slots.begin(), free));
    const auto generation = static_cast<std::uint8_t>((m_SlotGenerations[slot] + 1) & 0x7f);
    NativeVehicleRecord record{
        .Reference = {static_cast<std::int32_t>((slot << 8) | generation)},
        .Producer = request.Producer,
        .ScriptRequest = request.ScriptRequest,
        .ProducerIndex = request.ProducerIndex,
        .State = request.State,
    };
    const auto revision = m_Revision + 1;
    m_Events.push_back({m_Events.size() + 1, revision, NativeVehicleEventKind::Allocated, record});
    m_SlotGenerations[slot] = generation;
    m_Slots[slot] = std::move(record);
    m_Revision = revision;
    return {{NativeScriptServiceStatus::Ready, {}}, m_Slots[slot]->Reference};
} catch (const std::exception& exception) {
    return AllocationError(exception.what());
}

bool NativeVehiclePool::Update(NativeVehicleRef reference, const NativeVehicleState& state, std::string& error) try {
    const auto* current = Resolve(reference);
    if (!current) {
        error = "stale native vehicle reference";
        return false;
    }
    if (!ValidState(state, error)) {
        return false;
    }
    auto next = *current;
    next.State = state;
    const auto revision = m_Revision + 1;
    m_Events.push_back({m_Events.size() + 1, revision, NativeVehicleEventKind::Updated, next});
    m_Slots[static_cast<std::size_t>(reference.Value >> 8)] = std::move(next);
    m_Revision = revision;
    error.clear();
    return true;
} catch (const std::exception& exception) {
    error = exception.what();
    return false;
}

bool NativeVehiclePool::Release(NativeVehicleRef reference, std::string& error) try {
    const auto* current = Resolve(reference);
    if (!current) {
        error = "stale native vehicle reference";
        return false;
    }
    const auto slot = static_cast<std::size_t>(reference.Value >> 8);
    const auto revision = m_Revision + 1;
    m_Events.push_back({m_Events.size() + 1, revision, NativeVehicleEventKind::Released, *current});
    m_Slots[slot].reset();
    m_Revision = revision;
    error.clear();
    return true;
} catch (const std::exception& exception) {
    error = exception.what();
    return false;
}

const NativeVehicleRecord* NativeVehiclePool::Resolve(NativeVehicleRef reference) const {
    if (reference.Value < 0) {
        return nullptr;
    }
    const auto slot = static_cast<std::size_t>(reference.Value >> 8);
    if (slot >= m_Slots.size() || !m_Slots[slot] || m_Slots[slot]->Reference != reference) {
        return nullptr;
    }
    return &*m_Slots[slot];
}

const NativeVehicleRecord* NativeVehiclePool::AtSlot(std::size_t slot) const {
    return slot < m_Slots.size() && m_Slots[slot] ? &*m_Slots[slot] : nullptr;
}

NativeVehicleCensus NativeVehiclePool::Census() const {
    NativeVehicleCensus census;
    census.Alive = std::ranges::count_if(m_Slots, [](const auto& slot) { return slot.has_value(); });
    census.Revision = m_Revision;
    census.Producers = m_Producers;
    for (const auto& event : m_Events) {
        switch (event.Kind) {
        case NativeVehicleEventKind::Allocated: ++census.CreatedEvents; break;
        case NativeVehicleEventKind::Updated: ++census.UpdatedEvents; break;
        case NativeVehicleEventKind::Released: ++census.ReleasedEvents; break;
        }
    }
    return census;
}

std::shared_ptr<const NativeVehiclePoolSnapshot> NativeVehiclePool::Publish(std::uint64_t frame, std::string& error) try {
    if (m_LastPublishedFrame && frame <= *m_LastPublishedFrame) {
        error = "native vehicle snapshot frame is duplicate or stale";
        return {};
    }
    auto snapshot = std::shared_ptr<NativeVehiclePoolSnapshot>(new NativeVehiclePoolSnapshot);
    snapshot->m_Owner = m_Owner;
    snapshot->m_Frame = frame;
    snapshot->m_Generation = m_PublicationGeneration + 1;
    snapshot->m_Census = Census();
    snapshot->m_Slots = m_Slots;
    snapshot->m_Events = m_Events;
    m_LastPublishedFrame = frame;
    m_PublicationGeneration = snapshot->m_Generation;
    error.clear();
    return snapshot;
} catch (const std::exception& exception) {
    error = exception.what();
    return {};
}
