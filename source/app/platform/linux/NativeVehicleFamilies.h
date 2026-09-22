#pragma once

#include "app/platform/linux/NativeCarGenerators.h"

#include <cstdint>
#include <string>

enum class NativeVehicleConstructor : std::uint8_t {
    Automobile,
    MonsterTruck,
    QuadBike,
    Helicopter,
    Plane,
    Boat,
    Train,
    Bike,
    Bmx,
    Trailer,
};

struct NativeVehicleFamilyState {
    std::int32_t ModelId = -1;
    std::string ModelName, TextureName, HandlingName;
    NativeVehicleType ModelFamily = NativeVehicleType::Unsupported;
    NativeVehicleConstructor Constructor = NativeVehicleConstructor::Automobile;
    NativeVehicleType RuntimeType = NativeVehicleType::Unsupported;
    NativeVehicleType RuntimeSubType = NativeVehicleType::Unsupported;
    NativeVehicleCreatedBy CreatedBy = NativeVehicleCreatedBy::Unsupported;
    NativeVehicleStatus InitialStatus = NativeVehicleStatus::Simple;
    bool UsesAutomobileBase{}, UsesBikeBase{}, SideStand{}, SourceDefaultBranch{};
    bool operator==(const NativeVehicleFamilyState&) const = default;
};

class NativeVehicleFamilies {
public:
    static bool Construct(const NativeCarGeneratorModelDefinition&, NativeVehicleCreatedBy,
        NativeVehicleFamilyState& out, std::string& error);
    static const char* ConstructorName(NativeVehicleConstructor);
};
