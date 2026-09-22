#include "app/platform/linux/NativeVehicleFamilies.h"

#include <array>
#include <cstdio>
#include <cstdlib>

namespace {
int g_Checks = 0;
void Check(bool value, const std::string& message) {
    ++g_Checks;
    if (!value) { std::fprintf(stderr, "vehicle-families-fail: %s\n", message.c_str()); std::exit(1); }
}
}

int main(int argc, char** argv) {
    Check(argc == 2, "usage: probe GAME_DIR");
    NativeCarGenerators definitions;
    std::string error;
    Check(definitions.LoadBeforeWorker(argv[1], 0, error), error);
    Check(definitions.ModelDefinitions().size() == 212, "shipped vehicle census");
    std::array<std::size_t, 12> families{};
    std::array<bool, 10> constructors{};
    for (const auto& definition : definitions.ModelDefinitions()) {
        NativeVehicleFamilyState state;
        Check(NativeVehicleFamilies::Construct(definition, NativeVehicleCreatedBy::Random, state, error), error);
        Check(state.ModelId == definition.ModelId && state.ModelName == definition.ModelName &&
            state.TextureName == definition.TextureName && state.ModelFamily == definition.Type,
            "exact definition identity");
        Check(state.ModelId != 400 || definition.ModelId == 400, "no model400 fallback");
        Check(state.ModelId != 476 || definition.ModelId == 476, "no model476 fallback");
        switch (definition.Type) {
        case NativeVehicleType::Automobile:
            Check(state.Constructor == NativeVehicleConstructor::Automobile &&
                state.RuntimeType == NativeVehicleType::Automobile && state.RuntimeSubType == NativeVehicleType::Automobile,
                "automobile constructor identity");
            break;
        case NativeVehicleType::MonsterTruck:
            Check(state.Constructor == NativeVehicleConstructor::MonsterTruck &&
                state.RuntimeType == NativeVehicleType::Automobile && state.RuntimeSubType == NativeVehicleType::MonsterTruck,
                "monster constructor identity");
            break;
        case NativeVehicleType::Quad:
            Check(state.Constructor == NativeVehicleConstructor::QuadBike && state.RuntimeSubType == NativeVehicleType::Quad,
                "quad constructor identity");
            break;
        case NativeVehicleType::Helicopter:
            Check(state.Constructor == NativeVehicleConstructor::Helicopter && state.RuntimeSubType == NativeVehicleType::Helicopter,
                "helicopter constructor identity");
            break;
        case NativeVehicleType::Plane:
            Check(state.Constructor == NativeVehicleConstructor::Plane && state.RuntimeSubType == NativeVehicleType::Plane,
                "plane constructor identity");
            break;
        case NativeVehicleType::Boat:
            Check(state.Constructor == NativeVehicleConstructor::Boat && state.RuntimeType == NativeVehicleType::Boat,
                "boat constructor identity");
            break;
        case NativeVehicleType::Train:
            Check(state.Constructor == NativeVehicleConstructor::Train && state.RuntimeType == NativeVehicleType::Train,
                "train constructor identity");
            break;
        case NativeVehicleType::Bike:
            Check(state.Constructor == NativeVehicleConstructor::Bike && state.SideStand,
                "bike constructor identity");
            break;
        case NativeVehicleType::Bmx:
            Check(state.Constructor == NativeVehicleConstructor::Bmx && state.RuntimeType == NativeVehicleType::Bike && state.SideStand,
                "BMX constructor identity");
            break;
        case NativeVehicleType::Trailer:
            Check(state.Constructor == NativeVehicleConstructor::Trailer && state.RuntimeType == NativeVehicleType::Automobile &&
                state.RuntimeSubType == NativeVehicleType::Trailer && state.InitialStatus == NativeVehicleStatus::Abandoned,
                "trailer constructor identity");
            break;
        default: Check(false, "unexpected shipped fake family");
        }
        const auto family = static_cast<std::uint8_t>(definition.Type);
        Check(family < families.size(), "family range");
        ++families[family];
        constructors[static_cast<std::size_t>(state.Constructor)] = true;
    }
    for (std::size_t family = 0; family < families.size(); ++family) {
        const bool absentFakeFamily = family == static_cast<std::size_t>(NativeVehicleType::FakeHelicopter) ||
            family == static_cast<std::size_t>(NativeVehicleType::FakePlane);
        Check(absentFakeFamily ? families[family] == 0 : families[family] > 0,
            "shipped model-family census");
    }
    for (std::size_t constructor = 0; constructor < constructors.size(); ++constructor) {
        Check(constructors[constructor], "every source constructor represented");
    }
    NativeVehicleFamilyState retained;
    retained.ModelId = 123;
    auto invalid = definitions.ModelDefinitions().front();
    invalid.Type = NativeVehicleType::Unsupported;
    Check(!NativeVehicleFamilies::Construct(invalid, NativeVehicleCreatedBy::Random, retained, error) &&
        retained.ModelId == 123, "invalid family rejection retains output");
    for (const auto fake : {NativeVehicleType::FakeHelicopter, NativeVehicleType::FakePlane}) {
        auto definition = definitions.ModelDefinitions().front();
        definition.Type = fake;
        NativeVehicleFamilyState state;
        Check(NativeVehicleFamilies::Construct(definition, NativeVehicleCreatedBy::Random, state, error) &&
            state.Constructor == NativeVehicleConstructor::Automobile && state.SourceDefaultBranch &&
            state.ModelId == definition.ModelId, "source fake-family default constructor");
    }
    std::printf("native-vehicle-families-ok checks=%d models=212 shipped-model-families=10 source-types=12 constructors=10 fake-types-absent=2 fallback-400=0 fallback-476=0\n",
        g_Checks);
}
