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
    slot.Generation = std::uint8_t((slot.Generation + 1u) & 0x7Fu);
    const NativeScriptPedRef reference{
        static_cast<std::int32_t>(((slotIndex + 1u) << 8u) | slot.Generation)};
    slot.Alive = true;
    slot.State = {reference, pedType, modelId, vehiclePosition, vehicle, -1, true,
        true, missionCleanup, true, true};
    occupancy->Driver = reference;
    out = reference;
    ++m_Revision;
    error.clear();
    return NativeScriptPedStatus::Ok;
}

NativeScriptPedStatus NativeScriptPeds::CreateOnFoot(std::int32_t pedType, std::int32_t modelId,
    NativeScriptPosition position, bool missionCleanup, NativeScriptPedRef& out, std::string& error) {
    if (pedType < 0 || pedType > 31 || modelId < 0 || !Finite(position)) {
        error = "script on-foot ped request is invalid";
        return NativeScriptPedStatus::InvalidInput;
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
    slot.Generation = std::uint8_t((slot.Generation % 0x7F) + 1);
    const NativeScriptPedRef reference{
        static_cast<std::int32_t>(((slotIndex + 1u) << 8u) | slot.Generation)};
    slot.Alive = true;
    slot.State = {reference, pedType, modelId, position, {}, -1, false,
        true, missionCleanup, true, false};
    slot.State.Vehicle.Value = -1;
    out = reference;
    ++m_Revision;
    error.clear();
    return NativeScriptPedStatus::Ok;
}

NativeScriptPedStatus NativeScriptPeds::CreatePassenger(std::int32_t pedType, std::int32_t modelId,
    NativeScriptVehicleRef vehicle, std::int32_t seat, NativeScriptPosition vehiclePosition,
    bool missionCleanup, NativeScriptPedRef& out, std::string& error) {
    if (pedType < 0 || pedType > 31 || modelId < 0 || vehicle.Value < 0 || seat < 0 || seat >= 8 ||
        !Finite(vehiclePosition)) {
        error = "script passenger creation request is invalid";
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
    std::size_t slotIndex = Capacity;
    for (std::size_t i = 0; i < m_Slots.size(); ++i) {
        if (!m_Slots[i].Alive) { slotIndex = i; break; }
    }
    if (slotIndex == Capacity) {
        error = "script ped capacity exceeded";
        return NativeScriptPedStatus::CapacityExceeded;
    }
    auto& slot = m_Slots[slotIndex];
    slot.Generation = std::uint8_t((slot.Generation + 1u) & 0x7Fu);
    const NativeScriptPedRef reference{
        static_cast<std::int32_t>(((slotIndex + 1u) << 8u) | slot.Generation)};
    slot.Alive = true;
    slot.State = {reference, pedType, modelId, vehiclePosition, vehicle, seat, false,
        true, missionCleanup, true, true};
    occupancy->Passengers[std::size_t(seat)] = reference;
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

NativeScriptPedStatus NativeScriptPeds::LeaveVehicle(NativeScriptPedRef ped,
    NativeScriptVehicleRef vehicle, std::string& error) {
    for (std::size_t i = 0; i < m_OccupancyCount; ++i) {
        auto& occupancy = m_Occupancy[i];
        if (occupancy.Vehicle.Value != vehicle.Value) continue;
        bool found = false;
        if (occupancy.Driver.Value == ped.Value) {
            occupancy.Driver.Value = -1;
            found = true;
        }
        for (auto& passenger : occupancy.Passengers) {
            if (passenger.Value == ped.Value) {
                passenger.Value = -1;
                found = true;
            }
        }
        if (!found) break;
        if (auto* state = const_cast<NativeScriptPedState*>(Resolve(ped))) {
            state->Vehicle.Value = -1;
            state->Seat = -1;
            state->Driver = false;
            state->InVehicle = false;
        }
        ++m_Revision;
        error.clear();
        return NativeScriptPedStatus::Ok;
    }
    error = "script ped is not an occupant of the supplied vehicle";
    return NativeScriptPedStatus::StaleReference;
}

NativeScriptPedStatus NativeScriptPeds::Release(NativeScriptPedRef ref, std::string& error) {
    auto* state = const_cast<NativeScriptPedState*>(Resolve(ref));
    if (!state) { error = "script ped reference is stale"; return NativeScriptPedStatus::StaleReference; }
    for (std::size_t i = 0; i < m_OccupancyCount; ++i) {
        auto& occupancy = m_Occupancy[i];
        if (occupancy.Driver.Value == ref.Value) occupancy.Driver.Value = -1;
        for (auto& passenger : occupancy.Passengers)
            if (passenger.Value == ref.Value) passenger.Value = -1;
    }
    const auto slot = (std::uint32_t(ref.Value) >> 8u) - 1u;
    m_Slots[slot].Alive = false;
    state->InWorld = state->InVehicle = false;
    ++m_Revision;
    error.clear();
    return NativeScriptPedStatus::Ok;
}

NativeScriptPedStatus NativeScriptPeds::SetHealth(NativeScriptPedRef ref, float health,
    std::string& error) {
    if (!std::isfinite(health) || health < 0.0f) {
        error = "script ped health is invalid";
        return NativeScriptPedStatus::InvalidInput;
    }
    auto* state = const_cast<NativeScriptPedState*>(Resolve(ref));
    if (!state) {
        error = "script ped reference is stale";
        return NativeScriptPedStatus::StaleReference;
    }
    state->Health = health;
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
    return slot.Alive && slot.Generation == (raw & 0x7Fu) ? &slot.State : nullptr;
}

const NativeScriptVehicleOccupancy* NativeScriptPeds::Occupancy(NativeScriptVehicleRef ref) const noexcept {
    for (std::size_t i = 0; i < m_OccupancyCount; ++i) {
        if (m_Occupancy[i].Vehicle.Value == ref.Value) return &m_Occupancy[i];
    }
    return nullptr;
}

NativeScriptVehicleRef NativeScriptPeds::VehicleForPed(NativeScriptPedRef ped) const noexcept {
    for (std::size_t i = 0; i < m_OccupancyCount; ++i) {
        const auto& occupancy = m_Occupancy[i];
        if (occupancy.Driver.Value == ped.Value) return occupancy.Vehicle;
        for (const auto passenger : occupancy.Passengers) {
            if (passenger.Value == ped.Value) return occupancy.Vehicle;
        }
    }
    return {-1};
}

std::size_t NativeScriptPeds::Alive() const noexcept {
    std::size_t alive = 0;
    for (const auto& slot : m_Slots) alive += slot.Alive ? 1u : 0u;
    return alive;
}
