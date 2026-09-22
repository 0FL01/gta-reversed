#include "app/platform/linux/NativeVehicleFamilies.h"

#include <utility>

namespace {
bool ValidCreatedBy(NativeVehicleCreatedBy value) {
    return value >= NativeVehicleCreatedBy::Random && value <= NativeVehicleCreatedBy::Permanent;
}
}

bool NativeVehicleFamilies::Construct(const NativeCarGeneratorModelDefinition& definition,
    NativeVehicleCreatedBy createdBy, NativeVehicleFamilyState& out, std::string& error) {
    if (definition.ModelId < 0 || definition.ModelName.empty() || definition.TextureName.empty() ||
        definition.Type == NativeVehicleType::Unsupported || !ValidCreatedBy(createdBy)) {
        error = "vehicle family definition is incomplete";
        return false;
    }
    NativeVehicleFamilyState next;
    next.ModelId = definition.ModelId;
    next.ModelName = definition.ModelName;
    next.TextureName = definition.TextureName;
    next.HandlingName = definition.HandlingName;
    next.ModelFamily = definition.Type;
    next.CreatedBy = createdBy;
    next.InitialStatus = NativeVehicleStatus::Simple;
    switch (definition.Type) {
    case NativeVehicleType::Automobile:
        next.Constructor = NativeVehicleConstructor::Automobile;
        next.RuntimeType = next.RuntimeSubType = NativeVehicleType::Automobile;
        next.UsesAutomobileBase = true;
        break;
    case NativeVehicleType::MonsterTruck:
        next.Constructor = NativeVehicleConstructor::MonsterTruck;
        next.RuntimeType = NativeVehicleType::Automobile;
        next.RuntimeSubType = NativeVehicleType::MonsterTruck;
        next.UsesAutomobileBase = true;
        break;
    case NativeVehicleType::Quad:
        next.Constructor = NativeVehicleConstructor::QuadBike;
        next.RuntimeType = NativeVehicleType::Automobile;
        next.RuntimeSubType = NativeVehicleType::Quad;
        next.UsesAutomobileBase = true;
        break;
    case NativeVehicleType::Helicopter:
        next.Constructor = NativeVehicleConstructor::Helicopter;
        next.RuntimeType = NativeVehicleType::Automobile;
        next.RuntimeSubType = NativeVehicleType::Helicopter;
        next.UsesAutomobileBase = true;
        break;
    case NativeVehicleType::Plane:
        next.Constructor = NativeVehicleConstructor::Plane;
        next.RuntimeType = NativeVehicleType::Automobile;
        next.RuntimeSubType = NativeVehicleType::Plane;
        next.UsesAutomobileBase = true;
        break;
    case NativeVehicleType::Boat:
        next.Constructor = NativeVehicleConstructor::Boat;
        next.RuntimeType = next.RuntimeSubType = NativeVehicleType::Boat;
        break;
    case NativeVehicleType::Train:
        next.Constructor = NativeVehicleConstructor::Train;
        next.RuntimeType = next.RuntimeSubType = NativeVehicleType::Train;
        break;
    case NativeVehicleType::FakeHelicopter:
    case NativeVehicleType::FakePlane:
        // CPools::LoadVehiclePool's source switch uses CAutomobile for both
        // fake model-info families; this is an explicit source default branch.
        next.Constructor = NativeVehicleConstructor::Automobile;
        next.RuntimeType = next.RuntimeSubType = NativeVehicleType::Automobile;
        next.UsesAutomobileBase = true;
        next.SourceDefaultBranch = true;
        break;
    case NativeVehicleType::Bike:
        next.Constructor = NativeVehicleConstructor::Bike;
        next.RuntimeType = next.RuntimeSubType = NativeVehicleType::Bike;
        next.UsesBikeBase = true;
        next.SideStand = true;
        break;
    case NativeVehicleType::Bmx:
        next.Constructor = NativeVehicleConstructor::Bmx;
        next.RuntimeType = NativeVehicleType::Bike;
        next.RuntimeSubType = NativeVehicleType::Bmx;
        next.UsesBikeBase = true;
        next.SideStand = true;
        break;
    case NativeVehicleType::Trailer:
        next.Constructor = NativeVehicleConstructor::Trailer;
        next.RuntimeType = NativeVehicleType::Automobile;
        next.RuntimeSubType = NativeVehicleType::Trailer;
        next.UsesAutomobileBase = true;
        next.InitialStatus = NativeVehicleStatus::Abandoned;
        break;
    default:
        error = "vehicle family has no source constructor";
        return false;
    }
    out = std::move(next);
    error.clear();
    return true;
}

const char* NativeVehicleFamilies::ConstructorName(NativeVehicleConstructor value) {
    switch (value) {
    case NativeVehicleConstructor::Automobile: return "Automobile";
    case NativeVehicleConstructor::MonsterTruck: return "MonsterTruck";
    case NativeVehicleConstructor::QuadBike: return "QuadBike";
    case NativeVehicleConstructor::Helicopter: return "Helicopter";
    case NativeVehicleConstructor::Plane: return "Plane";
    case NativeVehicleConstructor::Boat: return "Boat";
    case NativeVehicleConstructor::Train: return "Train";
    case NativeVehicleConstructor::Bike: return "Bike";
    case NativeVehicleConstructor::Bmx: return "Bmx";
    case NativeVehicleConstructor::Trailer: return "Trailer";
    }
    return "Unknown";
}
