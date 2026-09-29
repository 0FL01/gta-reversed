#include "NativePedModelPolicies.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
int s_Checks = 0;
void Check(bool value, const char* message) {
    ++s_Checks;
    if (!value) {
        std::fprintf(stderr, "ped-model-policies-fail: %s\n", message);
        std::exit(1);
    }
}
}

int main(int argc, char** argv) {
    using Status = NativePedModelPolicyStatus;
    const bool cases = argc == 2 && std::strcmp(argv[1], "--cases") == 0;
    bool decision = true;
    Check(NativePedStatsCompatible(15, 1, decision) == Status::Decided && !decision,
        "second blocked stat is not ignored");
    Check(NativePedStatsCompatible(18, 15, decision) == Status::Decided && !decision,
        "old first requires old second");
    Check(NativePedStatsCompatible(15, 18, decision) == Status::Decided && decision,
        "age compatibility is asymmetric");
    Check(NativePedStatsCompatible(11, 13, decision) == Status::Decided && decision,
        "original blocks only first seven gang stats");
    Check(NativePedStatsCompatible(43, 15, decision) == Status::InvalidInput && decision,
        "invalid stat retains previous decision");
    const NativePedPolicyModel unknown;
    Check(NativePedZoneAccepts(false, false, 0, unknown, decision) == Status::Decided && !decision,
        "no zone does not read model");
    Check(NativePedZoneAccepts(true, true, 0, unknown, decision) == Status::Decided && decision,
        "streaming cheat does not read model");
    Check(NativePedZoneAccepts(true, false, 15, unknown, decision) == Status::UnknownModel && decision,
        "unknown race is not default race");
    Check(NativePedAttractorAccepts(unknown, std::nullopt, decision) == Status::Decided && decision,
        "null attractor has source early guard");
    Check(NativePedAttractorAccepts(unknown, "ATM", decision) == Status::UnknownModel && decision,
        "unknown model is not a civilian");
    Check(NativePedAttractorAccepts({true, 7, 4, 0}, "ATM", decision) == Status::Decided && decision,
        "unknown named attractor source default");
    Check(NativePedAttractorAccepts({true, 7, 5, 0}, "STRIPM", decision) == Status::Decided && !decision,
        "original strip-male inequality");
    Check(NativePedAttractorAccepts({true, 280, 6, 0}, "ATM", decision) == Status::Decided && !decision,
        "cop exclusion precedes default name");
    decision = true;
    Check(NativePedAttractorAccepts({true, 7, 4, 0}, std::string_view("bad\0name", 8), decision) ==
        Status::InvalidInput && decision, "invalid source string retains decision");

    int comparisons = 0;
    for (int first = -1; first <= 42; ++first) {
        for (int second = -1; second <= 42; ++second) {
            Check(NativePedStatsCompatible(first, second, decision) == Status::Decided, "stat matrix");
            if (cases) std::printf("STATS %d %d %d\n", first, second, decision ? 1 : 0);
            ++comparisons;
        }
    }
    for (int hasZone = 0; hasZone != 2; ++hasZone) {
        for (int cheat = 0; cheat != 2; ++cheat) {
            for (int race = 0; race <= 4; ++race) {
                for (int mask = 0; mask <= 255; ++mask) {
                    Check(NativePedZoneAccepts(hasZone != 0, cheat != 0, std::uint8_t(mask),
                        {true, 7, 4, race}, decision) == Status::Decided, "zone matrix");
                    if (cases) std::printf("ZONE %d %d %d %d %d\n", hasZone, cheat, race, mask, decision ? 1 : 0);
                    ++comparisons;
                }
            }
        }
    }
    constexpr std::array<std::string_view, 13> names{
        "COPSIT", "coplook", "BROWSE", "dancer", "BARGUY", "pedroul", "PEDCARD",
        "PEDSLOT", "STRIPW", "stripm", "ATM", "", "UNKNOWN"};
    constexpr std::array types{0, 4, 5, 6, 17, 31};
    for (int model = 0; model < 312; ++model) {
        for (int type : types) {
            for (std::size_t name = 0; name < names.size(); ++name) {
                Check(NativePedAttractorAccepts({true, model, type, 0}, names[name], decision) ==
                    Status::Decided, "attractor matrix");
                if (cases) std::printf("ATTRACTOR %d %d %zu %d\n", model, type, name, decision ? 1 : 0);
                ++comparisons;
            }
        }
    }
    std::printf("native-ped-model-policies-ok checks=%d cases=%d zone=race-mask stats=both attractor=original birth=unowned census=incomplete\n",
        s_Checks, comparisons);
}
