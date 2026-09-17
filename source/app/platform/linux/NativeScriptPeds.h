#pragma once

#include "NativeScriptSession.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

enum class NativeScriptPedStatus : std::uint8_t {
    Ok,
    InvalidInput,
    CapacityExceeded,
    StaleReference,
    SeatUnavailable,
};

struct NativeScriptPedState {
    NativeScriptPedRef Reference;
    std::int32_t PedType = -1;
    std::int32_t ModelId = -1;
    NativeScriptPosition Position;
    NativeScriptVehicleRef Vehicle;
    std::int32_t Seat = -1;
    bool Driver = false;
    bool MissionCreated = false;
    bool MissionCleanupRegistered = false;
    bool InWorld = false;
    bool InVehicle = false;
    bool operator==(const NativeScriptPedState&) const = default;
};

struct NativeScriptVehicleOccupancy {
    NativeScriptVehicleRef Vehicle;
    NativeScriptPedRef Driver;
    std::array<NativeScriptPedRef, 8> Passengers{};
    bool operator==(const NativeScriptVehicleOccupancy&) const = default;
};

// Value-only source mission-ped and vehicle-occupancy owner. It does not pose,
// animate, pathfind or render peds; those remain later presentation/AI owners.
class NativeScriptPeds {
public:
    static constexpr std::size_t Capacity = 140;

    NativeScriptPedStatus CreateDriver(std::int32_t pedType, std::int32_t modelId,
        NativeScriptVehicleRef vehicle, NativeScriptPosition vehiclePosition,
        bool missionCleanup, NativeScriptPedRef& out, std::string& error);
    NativeScriptPedStatus WarpPassenger(NativeScriptPedRef ped, NativeScriptVehicleRef vehicle,
        std::int32_t seat, NativeScriptPosition vehiclePosition, std::string& error);

    const NativeScriptPedState* Resolve(NativeScriptPedRef) const noexcept;
    const NativeScriptVehicleOccupancy* Occupancy(NativeScriptVehicleRef) const noexcept;
    std::size_t Alive() const noexcept;
    std::uint64_t Revision() const noexcept { return m_Revision; }
    static constexpr bool RuntimePresentation = false;

private:
    struct Slot {
        std::uint8_t Generation = 0;
        bool Alive = false;
        NativeScriptPedState State;
    };

    NativeScriptVehicleOccupancy* FindOrAllocateOccupancy(NativeScriptVehicleRef) noexcept;

    std::array<Slot, Capacity> m_Slots{};
    std::array<NativeScriptVehicleOccupancy, Capacity> m_Occupancy{};
    std::size_t m_OccupancyCount = 0;
    std::uint64_t m_Revision = 0;
};
