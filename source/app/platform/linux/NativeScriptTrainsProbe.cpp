#include "NativeScriptTrains.h"

#include <cstdio>
#include <cstdlib>

int main() {
    NativeScriptTrains trains;
    NativeScriptVehicleRef ref;
    int checks = 0;
    auto check = [&](bool value) { ++checks; if (!value) std::exit(1); };
    check(trains.Create(13, {2274.5f, -1257.5f, 23.0f}, true, ref).Status ==
        NativeScriptServiceStatus::Ready && trains.Resolve(ref));
    check(trains.SetSpeed(ref, 0.0f, false).Status == NativeScriptServiceStatus::Ready);
    check(trains.SetSpeed(ref, 10.0f, true).Status == NativeScriptServiceStatus::Ready &&
        trains.Resolve(ref)->CruiseSpeed == 10.0f);
    check(trains.DeleteMissionTrains().Status == NativeScriptServiceStatus::Ready &&
        !trains.Resolve(ref) && trains.Alive() == 0);
    check(!NativeScriptTrains::RuntimeMovement);
    std::printf("native-script-trains-ok checks=%d capacity=%zu movement=0\n",
        checks, NativeScriptTrains::Capacity);
}
