#pragma once

#include "NativePathGraph.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

enum class NativePopulationKind : std::uint8_t { Vehicle, Ped };
struct NativePopulationRef {
    std::uint32_t Value = 0;
    NativePopulationKind Kind = NativePopulationKind::Ped;
    bool operator==(const NativePopulationRef&) const = default;
};
struct NativePopulationEntity {
    NativePopulationRef Reference;
    std::int32_t ModelId = -1;
    NativePathRoute Route;
    NativeCollisionVector Position{};
    std::size_t Segment = 0;
    float SegmentDistance = 0.0f;
    float Speed = 0.0f;
    bool Deletable = true;
    bool Locked = false;
    bool Interesting = false;
    bool Moving = false;
    bool operator==(const NativePopulationEntity&) const = default;
};
enum class NativePopulationStatus : std::uint8_t {
    Ok, InvalidInput, StaleRoute, CapacityExceeded, StaleReference, Overflow,
};

class NativePopulationRuntime {
public:
    static constexpr std::size_t VehicleCapacity = 110;
    static constexpr std::size_t PedCapacity = 140;

    NativePopulationStatus Spawn(NativePopulationKind kind, std::int32_t modelId,
        const NativePathRoute& route, float speed, bool deletable,
        NativePopulationRef& out, std::string& error);
    NativePopulationStatus Tick(float timeStep, const NativePathGraph&, std::string& error);
    NativePopulationStatus ApplyPoolPressure(std::uint32_t frame,
        NativeCollisionVector camera, std::string& error);
    const NativePopulationEntity* Resolve(NativePopulationRef) const noexcept;
    std::size_t Alive(NativePopulationKind) const noexcept;
    std::uint64_t Revision() const noexcept { return m_Revision; }

private:
    struct Slot { std::uint8_t Generation{}; bool Alive{}; NativePopulationEntity Entity; };
    template<std::size_t N> static const NativePopulationEntity* ResolveIn(
        const std::array<Slot, N>&, NativePopulationRef) noexcept;
    template<std::size_t N> static NativePopulationStatus SpawnIn(std::array<Slot, N>&,
        NativePopulationKind, std::int32_t, const NativePathRoute&, float, bool,
        NativePopulationRef&, std::string&);
    template<std::size_t N> static bool RemoveClosest(std::array<Slot, N>&,
        NativeCollisionVector, bool vehicles);

    std::array<Slot, VehicleCapacity> m_Vehicles{};
    std::array<Slot, PedCapacity> m_Peds{};
    std::uint64_t m_Revision = 0;
};
