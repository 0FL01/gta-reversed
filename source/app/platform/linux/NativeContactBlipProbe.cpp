// CPU-only contract probe for host radar-consumer registration. Prepared source
// RGBA is real, but no GL context/upload exists and no GPU-readiness claim is made.
#include "app/platform/linux/RealtimeHud.h"
#include "app/platform/linux/RealtimeScriptHost.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>

using int32 = std::int32_t;
using uint32 = std::uint32_t;
using int64 = std::int64_t;
using uint64 = std::uint64_t;
#ifndef __stdcall
#define __stdcall
#endif
#include "oswrapper/oswrapper.h"

namespace {
void Require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "contact-blip probe: %s\n", message);
        std::exit(2);
    }
}

struct CpuRadarConsumerFixture {
    const RealtimeHud& Hud;
    bool AcceptPreparedRgba = false;
    unsigned Calls = 0;

    bool Ready(std::int32_t sprite) {
        ++Calls;
        const auto* image = Hud.PreparedRadarSprite(sprite);
        return AcceptPreparedRgba && sprite == 33 && image && std::strcmp(image->name, "radar_race") == 0
            && image->w == 16 && image->h == 16 && image->filter == 0x1101
            && image->rgba.size() == 16u * 16u * 4u;
    }
};
} // namespace

int main(int argc, char** argv) {
    const char* gameDir = argc > 1 ? argv[1] : "/game";
    OS_SetFilePathOffset(gameDir);
    char error[512]{};
    RealtimeHud hud;
    Require(hud.Load(gameDir, error, sizeof(error)), error);
    const auto* race = hud.PreparedRadarSprite(33);
    Require(race && std::strcmp(race->name, "radar_race") == 0 && race->w == 16 && race->h == 16
        && race->filter == 0x1101 && race->rgba.size() == 16u * 16u * 4u,
        "actual prepared sprite33 RGBA/filter");
    Require(!hud.IsRadarSpriteUploaded(33), "CPU preparation must not imply GPU readiness");

    RealtimeGameplay gameplay;
    RealtimeScriptHost host(gameplay);
    NativeScriptContactBlipRequest request{{7, 11, 205876}, {2495.0f, -1685.0f, 13.0f}, 33};

    request.RadarSpriteReady = true;
    auto result = host.PrepareContactBlipRequest(request);
    Require(result.Status == NativeScriptServiceStatus::Unsupported && !request.RadarSpriteReady,
        "unregistered consumer rejects and clears a stale prepared flag");

    host.SetRadarSpriteReady([&hud](std::int32_t sprite) { return hud.IsRadarSpriteUploaded(sprite); });
    result = host.PrepareContactBlipRequest(request);
    Require(result.Status == NativeScriptServiceStatus::Unsupported && !request.RadarSpriteReady,
        "production HUD callback rejects CPU-only preparation");

    CpuRadarConsumerFixture fixture{hud};
    host.SetRadarSpriteReady([&fixture](std::int32_t sprite) { return fixture.Ready(sprite); });
    result = host.PrepareContactBlipRequest(request);
    Require(result.Status == NativeScriptServiceStatus::Unsupported && !request.RadarSpriteReady && fixture.Calls == 1,
        "registered CPU fixture is queried without implicit acceptance");
    fixture.AcceptPreparedRgba = true;
    result = host.PrepareContactBlipRequest(request);
    Require(result.Status == NativeScriptServiceStatus::Ready && request.RadarSpriteReady && fixture.Calls == 2,
        "explicit consumer acceptance prepares source sprite33 request");

    auto property = request;
    property.Sprite = 32;
    const auto calls = fixture.Calls;
    result = host.PrepareContactBlipRequest(property);
    Require(result.Status == NativeScriptServiceStatus::Ready && !property.RadarSpriteReady && fixture.Calls == calls,
        "entity-owned property sprite keeps its existing preparation path");

    host.SetRadarSpriteReady([](std::int32_t) -> bool { throw std::runtime_error("fixture failure"); });
    request.RadarSpriteReady = true;
    result = host.PrepareContactBlipRequest(request);
    Require(result.Status == NativeScriptServiceStatus::Error && !request.RadarSpriteReady
        && result.Message == "radar sprite readiness exception: fixture failure",
        "callback failure is atomic and diagnostic");

    std::printf("NativeContactBlipProbe PASS actual=0570@205876 sprite=33 image=%s rgba=%zu "
        "cpu-consumer-fixture-only GL-ready-claim=false\n", race->name, race->rgba.size());
    return 0;
}
