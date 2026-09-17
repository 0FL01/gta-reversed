#include "NativeScriptPeds.h"

#include <cstdio>
#include <cstdlib>

namespace {
int g_Checks = 0;
void Check(bool value, const char* message) {
    ++g_Checks;
    if (!value) { std::fprintf(stderr, "script-peds-fail: %s\n", message); std::exit(1); }
}
}

int main() {
    NativeScriptPeds peds;
    std::string error;
    NativeScriptPedRef driver;
    const NativeScriptVehicleRef vehicle{0x101};
    const NativeScriptPosition position{1, 2, 3};
    Check(peds.CreateDriver(23, 291, vehicle, position, true, driver, error) ==
        NativeScriptPedStatus::Ok, "create mission driver");
    Check(driver.Value > 0 && peds.Resolve(driver) && peds.Resolve(driver)->Driver &&
        peds.Resolve(driver)->InVehicle && peds.Alive() == 1, "driver value owner");
    Check(peds.CreateDriver(23, 291, vehicle, position, true, driver, error) ==
        NativeScriptPedStatus::SeatUnavailable, "occupied driver rejection");
    const auto before = peds.Revision();
    Check(peds.WarpPassenger({1}, vehicle, 2, position, error) == NativeScriptPedStatus::Ok &&
        peds.Occupancy(vehicle)->Passengers[2].Value == 1, "external player passenger");
    Check(peds.WarpPassenger({1}, vehicle, 2, position, error) ==
        NativeScriptPedStatus::SeatUnavailable && peds.Revision() == before + 1,
        "duplicate passenger atomic rejection");
    Check(!NativeScriptPeds::RuntimePresentation, "no presentation claim");
    std::printf("native-script-peds-ok checks=%d capacity=%zu driver=%d passenger-seat=2 presentation=0\n",
        g_Checks, NativeScriptPeds::Capacity, driver.Value);
}
