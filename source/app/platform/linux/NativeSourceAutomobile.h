#pragma once

#include "Handling.h"
#include "NativeGeneratedVehicleAssets.h"
#include "NativeVehiclePool.h"

#include <array>
#include <memory>
#include <string>

enum class NativeSourceAutomobileStatus { Ready, InvalidInput, Unsupported, Error, Occupied, Full };

struct NativeSourceAutomobileOccupants {
    std::uint64_t Driver{};
    std::array<std::uint64_t, 8> Passengers{};
    std::uint8_t PassengerCount{}, MaxPassengers{};
    bool operator==(const NativeSourceAutomobileOccupants&) const = default;
};
struct NativeSourceAutomobileState {
    std::uint64_t Identity{}, Revision{};
    std::int32_t ModelId = -1;
    std::string ModelName, HandlingName, TextureName;
    NativeVehicleType Type = NativeVehicleType::Unsupported;
    NativeVehicleStatus Status = NativeVehicleStatus::Simple;
    NativeVehicleCreatedBy CreatedBy = NativeVehicleCreatedBy::Parked;
    NativeGarageMatrix Matrix;
    float Mass{}, TurnMass{}, Drag{}, MaxVelocityKmh{}, EngineAcceleration{}, EngineInertia{};
    std::array<float, 3> CentreOfMass{};
    float PercentSubmerged{}, TractionMult{}, TractionLoss{}, TractionBias{};
    float Elasticity = 0.05f, BrakeCount = 20, TireTemperature = 1;
    std::uint8_t Gears{};
    char DriveType{}, EngineType{};
    std::array<float, 4> WheelRotation{}, WheelSpeed{}, SuspensionCompression{1,1,1,1};
    NativeSourceAutomobileOccupants Occupants;
    std::shared_ptr<const NativeGeneratedVehiclePacket> Assets;
    bool operator==(const NativeSourceAutomobileState&) const = default;
};

class NativeSourceAutomobile {
public:
    // Composes existing exact IDE/DFF/TXD/COL and handling owners into the
    // ordinary CAutomobile constructor state. No numeric model fallback and no
    // vehicle pool/world publication; P3-A06 owns those lifetimes.
    NativeSourceAutomobileStatus Construct(std::uint64_t identity,
        const NativeCarGeneratorModelDefinition&, NativeGeneratedVehicleAsset,
        const HandlingParams&, NativeVehicleCreatedBy, const NativeGarageMatrix&,
        std::string& error);
    NativeSourceAutomobileStatus SetDriver(std::uint64_t occupant, std::string& error);
    NativeSourceAutomobileStatus AddPassenger(std::uint64_t occupant, std::uint8_t seat, std::string& error);
    NativeSourceAutomobileStatus RemoveOccupant(std::uint64_t occupant, std::string& error);
    std::shared_ptr<const NativeSourceAutomobileState> LastCommitted() const noexcept { return m_State; }

private:
    NativeSourceAutomobileStatus Publish(NativeSourceAutomobileState&&, std::string&);
    std::shared_ptr<const NativeSourceAutomobileState> m_State;
};
