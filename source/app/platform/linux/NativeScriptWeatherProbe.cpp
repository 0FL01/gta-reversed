#include "app/platform/linux/NativeScriptWeather.h"

#include <cstdlib>
#include <iostream>

namespace {
unsigned s_Checks;
void Check(bool condition) {
    ++s_Checks;
    if (!condition) std::abort();
}
}

int main() {
    NativeScriptWeather weather;
    Check(weather.State() == NativeScriptWeatherState{});
    Check(weather.ForceNow(4).Status == NativeScriptServiceStatus::Ready);
    Check(weather.State().Forced == 4 && weather.State().Old == 4 && weather.State().New == 4 &&
        weather.State().Revision == 1);
    const auto retained = weather.State();
    Check(weather.ForceNow(-1).Status == NativeScriptServiceStatus::Error && weather.State() == retained);
    Check(weather.ForceNow(23).Status == NativeScriptServiceStatus::Error && weather.State() == retained);
    Check(weather.Release().Status == NativeScriptServiceStatus::Ready && weather.State().Forced == -1 &&
        weather.State().Old == 4 && weather.State().New == 4 && weather.State().Revision == 2);
    std::cout << "native-script-weather-ok checks=" << s_Checks
              << " force-now=forced,old,new range=0..22\n";
}
