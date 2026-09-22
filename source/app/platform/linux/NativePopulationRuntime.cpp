#include "NativePopulationRuntime.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace {
bool Finite(const NativeCollisionVector& value) {
    return std::isfinite(value[0]) && std::isfinite(value[1]) && std::isfinite(value[2]);
}
float Distance(const NativeCollisionVector& a, const NativeCollisionVector& b) {
    const float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    return std::sqrt(x*x + y*y + z*z);
}
}

template<std::size_t N>
const NativePopulationEntity* NativePopulationRuntime::ResolveIn(
    const std::array<Slot, N>& slots, NativePopulationRef reference) noexcept {
    if (!reference.Value) return nullptr;
    const auto index = std::size_t(reference.Value >> 8u);
    const auto generation = std::uint8_t(reference.Value & 0xFFu);
    return index < slots.size() && slots[index].Alive && slots[index].Generation == generation
        ? &slots[index].Entity : nullptr;
}

template<std::size_t N>
NativePopulationStatus NativePopulationRuntime::SpawnIn(std::array<Slot, N>& slots,
    NativePopulationKind kind, std::int32_t modelId, const NativePathRoute& route,
    float speed, bool deletable, NativePopulationRef& out, std::string& error) {
    const auto slot = std::ranges::find_if(slots, [](const Slot& value) { return !value.Alive; });
    if (slot == slots.end()) { error = "population pool capacity exceeded"; return NativePopulationStatus::CapacityExceeded; }
    const auto index = std::size_t(slot - slots.begin());
    slot->Generation = std::uint8_t(slot->Generation + 1u);
    if (!slot->Generation) slot->Generation = 1;
    slot->Alive = true;
    const NativePopulationRef reference{std::uint32_t(index << 8u) | slot->Generation, kind};
    slot->Entity = {reference, modelId, route, {}, 0, 0.0f, speed, deletable, false, false, false};
    out = reference;
    error.clear();
    return NativePopulationStatus::Ok;
}

NativePopulationStatus NativePopulationRuntime::Spawn(NativePopulationKind kind,
    std::int32_t modelId, const NativePathRoute& route, float speed, bool deletable,
    NativePopulationRef& out, std::string& error) {
    if (modelId < 0 || !route.Generation || route.Nodes.size() < 2 ||
        route.Nodes.front() != route.Start || route.Nodes.back() != route.End ||
        !std::isfinite(speed) || speed <= 0.0f) {
        error = "population spawn route is invalid";
        return NativePopulationStatus::InvalidInput;
    }
    const auto status = kind == NativePopulationKind::Vehicle
        ? SpawnIn(m_Vehicles, kind, modelId, route, speed, deletable, out, error)
        : SpawnIn(m_Peds, kind, modelId, route, speed, deletable, out, error);
    if (status == NativePopulationStatus::Ok) ++m_Revision;
    return status;
}

NativePopulationStatus NativePopulationRuntime::Tick(float timeStep,
    const NativePathGraph& graph, std::string& error) {
    if (!std::isfinite(timeStep) || timeStep < 0.0f) {
        error = "population timestep is invalid";
        return NativePopulationStatus::InvalidInput;
    }
    auto tick = [&](auto& slots) -> NativePopulationStatus {
        for (auto& slot : slots) {
            if (!slot.Alive) continue;
            auto next = slot.Entity;
            if (next.Route.Generation != graph.Generation()) {
                error = "population route generation is stale";
                return NativePopulationStatus::StaleRoute;
            }
            float remaining = next.Speed * timeStep;
            while (remaining > 0.0f && next.Segment + 1 < next.Route.Nodes.size()) {
                const auto* from = graph.Resolve(next.Route.Nodes[next.Segment]);
                const auto* to = graph.Resolve(next.Route.Nodes[next.Segment + 1]);
                if (!from || !to) { error = "population route node is unavailable"; return NativePopulationStatus::StaleRoute; }
                const float length = Distance(from->Position, to->Position);
                if (!std::isfinite(length) || length <= 0.0f) { error = "population route segment is invalid"; return NativePopulationStatus::InvalidInput; }
                const float available = length - next.SegmentDistance;
                const float step = std::min(remaining, available);
                next.SegmentDistance += step;
                remaining -= step;
                const float alpha = next.SegmentDistance / length;
                for (std::size_t axis = 0; axis < 3; ++axis)
                    next.Position[axis] = from->Position[axis] + (to->Position[axis] - from->Position[axis]) * alpha;
                if (next.SegmentDistance >= length) {
                    ++next.Segment;
                    next.SegmentDistance = 0.0f;
                    next.Position = to->Position;
                }
                next.Moving = true;
            }
            if (!Finite(next.Position)) { error = "population movement overflow"; return NativePopulationStatus::Overflow; }
            slot.Entity = std::move(next);
        }
        return NativePopulationStatus::Ok;
    };
    auto status = tick(m_Vehicles);
    if (status == NativePopulationStatus::Ok) status = tick(m_Peds);
    if (status == NativePopulationStatus::Ok) { ++m_Revision; error.clear(); }
    return status;
}

template<std::size_t N>
bool NativePopulationRuntime::RemoveClosest(std::array<Slot, N>& slots,
    NativeCollisionVector camera, bool vehicles) {
    std::size_t selected = N;
    float closest = std::numeric_limits<float>::max();
    for (std::size_t i = 0; i < slots.size(); ++i) {
        const auto& slot = slots[i];
        if (!slot.Alive || !slot.Entity.Deletable ||
            (vehicles && (slot.Entity.Locked || slot.Entity.Interesting))) continue;
        const float distance = Distance(slot.Entity.Position, camera);
        if (distance < closest) { closest = distance; selected = i; }
    }
    if (selected == N) return false;
    slots[selected].Alive = false;
    return true;
}

NativePopulationStatus NativePopulationRuntime::ApplyPoolPressure(std::uint32_t frame,
    NativeCollisionVector camera, std::string& error) {
    if (!Finite(camera)) { error = "population pressure camera is invalid"; return NativePopulationStatus::InvalidInput; }
    bool removed = false;
    if (frame % 8u == 3u && VehicleCapacity - Alive(NativePopulationKind::Vehicle) < 8)
        removed |= RemoveClosest(m_Vehicles, camera, true);
    if (frame % 8u == 5u && PedCapacity - Alive(NativePopulationKind::Ped) < 8)
        removed |= RemoveClosest(m_Peds, camera, false);
    if (removed) ++m_Revision;
    error.clear();
    return NativePopulationStatus::Ok;
}

const NativePopulationEntity* NativePopulationRuntime::Resolve(NativePopulationRef reference) const noexcept {
    return reference.Kind == NativePopulationKind::Vehicle
        ? ResolveIn(m_Vehicles, reference) : ResolveIn(m_Peds, reference);
}

std::size_t NativePopulationRuntime::Alive(NativePopulationKind kind) const noexcept {
    const auto count = [](const auto& slots) {
        return std::count_if(slots.begin(), slots.end(), [](const Slot& slot) { return slot.Alive; });
    };
    return kind == NativePopulationKind::Vehicle ? count(m_Vehicles) : count(m_Peds);
}
