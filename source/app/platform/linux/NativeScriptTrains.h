#pragma once

#include "NativeScriptSession.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

struct NativeScriptTrainState {
    NativeScriptVehicleRef Reference;
    std::int32_t Type = -1;
    NativeScriptPosition Position;
    bool Clockwise = false;
    float Speed = 0.0f, CruiseSpeed = 0.0f;
    bool Mission = false, Alive = false;
    bool operator==(const NativeScriptTrainState&) const = default;
};

class NativeScriptTrains {
public:
    static constexpr std::size_t Capacity = 4;
    static constexpr bool RuntimeMovement = false;

    NativeScriptServiceResult Create(std::int32_t type, NativeScriptPosition,
        bool clockwise, NativeScriptVehicleRef& out);
    NativeScriptServiceResult SetSpeed(NativeScriptVehicleRef, float speed, bool cruise);
    NativeScriptServiceResult DeleteMissionTrains();
    const NativeScriptTrainState* Resolve(NativeScriptVehicleRef) const noexcept;
    std::size_t Alive() const noexcept;

private:
    struct Slot {
        std::uint8_t Generation = 0;
        NativeScriptTrainState State;
    };
    std::array<Slot, Capacity> m_Slots{};
};
