#include "NativeEnvironmentLifecycle.h"

#include <cstdio>
#include <cstdlib>

namespace {
int g_Checks = 0;
void Check(bool condition, const char* message) {
    ++g_Checks;
    if (!condition) { std::fprintf(stderr, "environment-lifecycle-fail: %s\n", message); std::exit(1); }
}
}

int main(int argc, char** argv) {
    const char* game = argc > 1 ? argv[1] : "/game";
    NativeEnvironmentLifecycle owner;
    std::string error;
    Check(owner.Initialize(game, 2500, -1600, 12, 0, error) == NativeEnvironmentStatus::Ok,
        "Los Santos noon initialization");
    const auto losSantos = owner.LastCommitted();
    Check(losSantos && losSantos->Region == NativeWeatherRegion::LosSantos &&
        losSantos->Generation == 1 && losSantos->ExtraSunnyness == 1.0f,
        "Los Santos source region/weather");
    Check(owner.Advance(-1600, 0, 7, 0, 8, 0.5f, 1000, 1.0f, {0.5f, -0.25f}, error) ==
        NativeEnvironmentStatus::Ok, "LA to rainy SF transition");
    const auto transition = owner.LastCommitted();
    Check(transition->Region == NativeWeatherRegion::SanFierro && transition->Rain == 0.5f &&
        transition->CloudCoverage == 0.5f && transition->Wind == 0.5f &&
        transition->WaterWavyness == 0.8f, "weather transition factors");
    Check(transition->FirstFlowUv[0] > 0 && transition->SecondFlowUv[0] > 0 &&
        transition->LowCloudColours != losSantos->LowCloudColours, "cloud and water time state");
    Check(owner.Advance(0, 1500, 19, 19, 19, 0, 2000, 3.0f, {1, 1}, error) ==
        NativeEnvironmentStatus::Ok, "desert sandstorm transition");
    const auto desert = owner.LastCommitted();
    Check(desert->Region == NativeWeatherRegion::Desert && desert->Foggyness == 1 &&
        desert->CloudCoverage == 1 && desert->Wind == 1.5f && desert->WaterWavyness == 1,
        "desert source region/weather");
    Check(losSantos->Generation == 1 && losSantos->Events.size() == 1,
        "held environment snapshot immutable");
    const auto retained = desert;
    Check(owner.Advance(0, 0, 24, 0, 0, 0, 3, 0, {}, error) ==
        NativeEnvironmentStatus::InvalidInput && owner.LastCommitted() == retained,
        "invalid transition retains publication");
    Check(!NativeEnvironmentLifecycle::OwnsFrustum && !NativeEnvironmentLifecycle::OwnsCloudGeometry &&
        !NativeEnvironmentLifecycle::OwnsWaterGeometry && !desert->PresentationFeedback,
        "presentation authorities remain external");
    Check(desert->Events.size() == 3 && desert->FarClip > 0 && desert->Water[3] >= 0,
        "ordered complete environment publications");
    std::printf("native-environment-lifecycle-ok checks=%d route=los-santos,san-fierro,desert transitions=3 cloud=timecyc water=source-flow geometry=external feedback=0\n", g_Checks);
}
