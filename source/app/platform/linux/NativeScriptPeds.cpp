#include "NativeScriptPeds.h"

#include <cmath>

namespace {
bool Finite(const NativeScriptPosition& p) {
    return std::isfinite(p.X) && std::isfinite(p.Y) && std::isfinite(p.Z);
}

bool ValidRef(NativeScriptPedRef ref) {
    return ref.Value >= 0;
}
}

NativeScriptVehicleOccupancy* NativeScriptPeds::FindOrAllocateOccupancy(NativeScriptVehicleRef vehicle) noexcept {
    for (std::size_t i = 0; i < m_OccupancyCount; ++i) {
        if (m_Occupancy[i].Vehicle.Value == vehicle.Value) return &m_Occupancy[i];
    }
    if (m_OccupancyCount == m_Occupancy.size()) return nullptr;
    auto& occupancy = m_Occupancy[m_OccupancyCount++];
    occupancy = {};
    occupancy.Vehicle = vehicle;
    occupancy.Driver.Value = -1;
    for (auto& passenger : occupancy.Passengers) passenger.Value = -1;
    return &occupancy;
}

NativeScriptPedStatus NativeScriptPeds::CreateDriver(std::int32_t pedType, std::int32_t modelId,
    NativeScriptVehicleRef vehicle, NativeScriptPosition vehiclePosition,
    bool missionCleanup, NativeScriptPedRef& out, std::string& error) {
    if (pedType < 0 || pedType > 31 || modelId < 0 || vehicle.Value < 0 || !Finite(vehiclePosition)) {
        error = "script driver request is invalid";
        return NativeScriptPedStatus::InvalidInput;
    }
    auto* occupancy = FindOrAllocateOccupancy(vehicle);
    if (!occupancy) {
        error = "script vehicle occupancy capacity exceeded";
        return NativeScriptPedStatus::CapacityExceeded;
    }
    if (occupancy->Driver.Value >= 0) {
        error = "script vehicle driver seat is occupied";
        return NativeScriptPedStatus::SeatUnavailable;
    }
    std::size_t slotIndex = Capacity;
    for (std::size_t i = 0; i < m_Slots.size(); ++i) {
        if (!m_Slots[i].Alive) { slotIndex = i; break; }
    }
    if (slotIndex == Capacity) {
        error = "script ped capacity exceeded";
        return NativeScriptPedStatus::CapacityExceeded;
    }
    auto& slot = m_Slots[slotIndex];
    if (slot.Generation == 0xFF) {
        error = "script ped generation exhausted";
        return NativeScriptPedStatus::CapacityExceeded;
    }
    const NativeScriptPedRef reference{
        static_cast<std::int32_t>(((slotIndex + 1u) << 8u) | ++slot.Generation)};
    slot.Alive = true;
    slot.State = {reference, pedType, modelId, vehiclePosition, vehicle, -1, true,
        true, missionCleanup, true, true};
    occupancy->Driver = reference;
    out = reference;
    ++m_Revision;
    error.clear();
    return NativeScriptPedStatus::Ok;
}

NativeScriptPedStatus NativeScriptPeds::WarpPassenger(NativeScriptPedRef ped,
    NativeScriptVehicleRef vehicle, std::int32_t seat, NativeScriptPosition vehiclePosition,
    std::string& error) {
    if (!ValidRef(ped) || vehicle.Value < 0 || seat < 0 || seat >= 8 || !Finite(vehiclePosition)) {
        error = "script passenger request is invalid";
        return NativeScriptPedStatus::InvalidInput;
    }
    auto* occupancy = FindOrAllocateOccupancy(vehicle);
    if (!occupancy) {
        error = "script vehicle occupancy capacity exceeded";
        return NativeScriptPedStatus::CapacityExceeded;
    }
    if (occupancy->Passengers[std::size_t(seat)].Value >= 0) {
        error = "script vehicle passenger seat is occupied";
        return NativeScriptPedStatus::SeatUnavailable;
    }
    if (const auto* state = Resolve(ped); state && state->InVehicle) {
        error = "script ped is already in a vehicle";
        return NativeScriptPedStatus::SeatUnavailable;
    }
    occupancy->Passengers[std::size_t(seat)] = ped;
    if (auto* state = const_cast<NativeScriptPedState*>(Resolve(ped))) {
        state->Position = vehiclePosition;
        state->Vehicle = vehicle;
        state->Seat = seat;
        state->Driver = false;
        state->InVehicle = true;
    }
    ++m_Revision;
    error.clear();
    return NativeScriptPedStatus::Ok;
}

const NativeScriptPedState* NativeScriptPeds::Resolve(NativeScriptPedRef ref) const noexcept {
    if (ref.Value < 0) return nullptr;
    const auto raw = static_cast<std::uint32_t>(ref.Value);
    const auto encodedSlot = raw >> 8u;
    if (!encodedSlot || encodedSlot > Capacity) return nullptr;
    const auto& slot = m_Slots[encodedSlot - 1u];
    return slot.Alive && slot.Generation == (raw & 0xFFu) ? &slot.State : nullptr;
}

const NativeScriptVehicleOccupancy* NativeScriptPeds::Occupancy(NativeScriptVehicleRef ref) const noexcept {
    for (std::size_t i = 0; i < m_OccupancyCount; ++i) {
        if (m_Occupancy[i].Vehicle.Value == ref.Value) return &m_Occupancy[i];
    }
    return nullptr;
}

std::size_t NativeScriptPeds::Alive() const noexcept {
    std::size_t alive = 0;
    for (const auto& slot : m_Slots) alive += slot.Alive ? 1u : 0u;
    return alive;
}
