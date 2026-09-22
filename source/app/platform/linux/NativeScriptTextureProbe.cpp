#include "NativeScriptEntities.h"
#include "RealtimeStreaming.h"
#include "StreamPager.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string_view>
#include <thread>

namespace {
int g_Checks = 0;
void Check(bool value, const char* message) {
    ++g_Checks;
    if (!value) { std::fprintf(stderr, "script-texture-fail: %s\n", message); std::exit(1); }
}
}

int main(int argc, char** argv) {
    Check(argc == 2, "usage: probe GAME_DIR");
    E2ELoadInfo info{};
    char parserError[512]{};
    StreamPagerOptions options;
    options.includeStreamed = true;
    Check(StreamPager_Init(argv[1], info, parserError, sizeof(parserError), options), parserError);
    realtime_streaming::Worker worker(false, {}, {}, 1, {}, {},
        [](const realtime_streaming::ScriptTextureRequest& request,
            NativeScriptTextureDictionaryPacket& packet, std::string& error) {
            return NativeScriptEntities_LoadTextureDictionary(
                request.GameDir.c_str(), request.Name, packet, error);
        });
    Check(worker.RequestScriptTexture({1, argv[1], "LD_NONE"}), "request admission");
    std::optional<realtime_streaming::ScriptTextureCompletion> completion;
    for (unsigned i = 0; i < 1000 && !completion; ++i) {
        completion = worker.TakeScriptTexture(1);
        if (!completion) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    Check(completion.has_value() && completion->Error.empty() && completion->Packet,
        "worker completion");
    const auto& images = completion->Packet->Images;
    constexpr std::array<std::string_view, 24> required{{
        "SHIP", "TVCORN", "SHOOT", "LIGHT",
        "EXPLM01", "EXPLM02", "EXPLM03", "EXPLM04", "EXPLM05", "EXPLM06",
        "EXPLM07", "EXPLM08", "EXPLM09", "EXPLM10", "EXPLM11", "EXPLM12",
        "FORCE", "WARP", "THRUST", "SHPNORM", "SHPWARP", "SHIP2", "SHIP3", "TITLE"
    }};
    for (const auto expected : required) {
        Check(std::ranges::any_of(images, [&](const auto& image) {
            const std::string_view actual(image.name);
            return actual.size() == expected.size() && std::equal(actual.begin(), actual.end(), expected.begin(),
                [](unsigned char a, unsigned char b) { return std::tolower(a) == std::tolower(b); });
        }), "required sprite");
    }
    Check(completion->Packet->Name == "LD_NONE", "dictionary identity");
    Check(std::ranges::all_of(images, [](const auto& image) {
        return image.w > 0 && image.h > 0 && image.mipmaps > 0 && !image.rgba.empty();
    }), "owned image payloads");
    const auto imageCount = images.size();
    completion.reset();
    worker.Stop({}, {});
    StreamPager_Shutdown();
    std::printf("native-script-texture-ok checks=%d dictionary=LD_NONE images=%zu required=24 worker=sole feedback=0\n",
        g_Checks, imageCount);
}
