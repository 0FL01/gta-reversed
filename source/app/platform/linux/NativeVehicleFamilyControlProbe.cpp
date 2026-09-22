#include "NativeVehicleFamilyControl.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace {
int g_Checks = 0;
void Check(bool value, const char* message) {
    ++g_Checks;
    if (!value) { std::fprintf(stderr, "vehicle-family-control-fail: %s\n", message); std::exit(1); }
}
bool Near(float a, float b) { return std::abs(a - b) < 0.00001f; }
}

int main() {
    using D = NativeVehicleFamilyDependency;
    using S = NativeVehicleFamilyControlStatus;
    NativeVehicleFamilyControlInput input{1.0f, -64, 255, 0, 30.0f};

    NativeVehicleFamilyControlState road{NativeVehicleType::Automobile, D::Road};
    Check(NativeVehicleApplyFamilyControl(road, input) == S::Ok && road.Gas == 1.0f &&
        road.RawSteer > 0.0f && road.AutomobileDelegated && road.CompleteFamilyControl,
        "road automobile transition");

    NativeVehicleFamilyControlState water{NativeVehicleType::Boat, D::Water};
    input.Brake = 255;
    Check(NativeVehicleApplyFamilyControl(water, input) == S::Ok && Near(water.Brake, 0.1f) &&
        Near(water.Gas, -0.03f) && water.SteerRadians > 0.0f && water.CompleteFamilyControl,
        "water boat transition");

    NativeVehicleFamilyControlState rail{NativeVehicleType::Train, D::Rail};
    rail.TrainSpeed = 0.5f; rail.RailDistance = 10.0f;
    input = {}; input.TimeStep = 2.0f;
    Check(NativeVehicleApplyFamilyControl(rail, input) == S::Ok && rail.TrainSpeed < 0.5f &&
        rail.RailDistance > 10.0f && !rail.CompleteFamilyControl, "rail free-carriage transition");
    input.HasPreviousCarriage = true; input.PreviousTrainSpeed = 0.25f;
    input.PreviousRailDistance = 20.0f; input.TrainLength = 5.0f;
    Check(NativeVehicleApplyFamilyControl(rail, input) == S::Ok && rail.TrainSpeed == 0.25f &&
        rail.RailDistance == 25.0f, "rail follower transition");

    NativeVehicleFamilyControlState flight{NativeVehicleType::Helicopter, D::Flight};
    input = {}; input.TimeStep = 1.0f; input.MoveSpeedSquared = 0.0f;
    Check(NativeVehicleApplyFamilyControl(flight, input) == S::Ok && Near(flight.Brake, 0.2f) &&
        !flight.CompleteFamilyControl, "flight abandoned dependency transition");

    NativeVehicleFamilyControlState towing{NativeVehicleType::Trailer, D::Towing};
    input = {}; input.TimeStep = 10.0f; input.TowingVehiclePresent = true;
    Check(NativeVehicleApplyFamilyControl(towing, input) == S::Ok && Near(towing.TrailerExtension, 0.98f),
        "towing retraction transition");
    input.TowingVehiclePresent = false;
    Check(NativeVehicleApplyFamilyControl(towing, input) == S::Ok && towing.TrailerExtension > 0.98f,
        "towing extension transition");

    NativeVehicleFamilyControlState special{NativeVehicleType::Quad, D::Special};
    input = {1.0f, 64, 128, 0, 35.0f};
    Check(NativeVehicleApplyFamilyControl(special, input) == S::Ok && special.AutomobileDelegated &&
        !special.CompleteFamilyControl && special.RawSteer < 0.0f, "special quad delegation transition");

    const auto retained = special;
    input.TimeStep = -1.0f;
    Check(NativeVehicleApplyFamilyControl(special, input) == S::InvalidInput && special == retained,
        "invalid input is atomic");
    special.Family = NativeVehicleType::Bike; special.Dependency = D::Flight; input.TimeStep = 1.0f;
    Check(NativeVehicleApplyFamilyControl(special, input) == S::Unsupported,
        "address-backed family mismatch remains unsupported");

    std::printf("native-vehicle-family-control-ok checks=%d road=1 water=1 rail=1 flight-common=1 towing=1 special-delegated=1 complete-flight=0 complete-bike=0\n", g_Checks);
}
