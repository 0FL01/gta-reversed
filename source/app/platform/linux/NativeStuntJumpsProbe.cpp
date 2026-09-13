#include "app/platform/linux/NativeStuntJumps.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace {
std::size_t s_Checks;
void Check(bool value, const char* message) {
    ++s_Checks;
    if (!value) { std::fprintf(stderr, "native-stunt-jumps FAIL: %s\n", message); std::exit(1); }
}
}

int main() {
    NativeStuntJumps jumps;
    std::string error;
    std::size_t index = 999;
    Check(NativeStuntJumps::Capacity == 256 && NativeStuntJumps::Coverage ==
        NativeStuntJumpCoverage{true, false, false, false, false}, "registration-only coverage is explicit");
    Check(jumps.Add({10, 20, 30}, {1, 2, 3}, {-10, -20, -30}, {4, 5, 6},
        {100, 200, 300}, 500, index, error) == NativeStuntJumpStatus::Ok && index == 0,
        "register source center and half-size values");
    const NativeStuntJump expected{{{9, 18, 27}, {11, 22, 33}},
        {{-14, -25, -36}, {-6, -15, -24}}, {100, 200, 300}, 500, false, false};
    Check(jumps.Entries().size() == 1 && jumps.Entries()[0] == expected && jumps.Revision() == 1,
        "registration derives exact boxes and initial flags");
    const auto retained = jumps.Entries()[0];
    const auto badIndex = index;
    Check(jumps.Add({}, {-1, 0, 0}, {}, {}, {}, 0, index, error) == NativeStuntJumpStatus::InvalidInput &&
        index == badIndex && jumps.Entries().size() == 1 && jumps.Entries()[0] == retained,
        "negative radius rejects atomically");
    Check(jumps.Add({std::numeric_limits<float>::max(), 0, 0},
        {std::numeric_limits<float>::max(), 0, 0}, {}, {}, {}, 0, index, error) ==
        NativeStuntJumpStatus::Overflow && jumps.Entries().size() == 1,
        "derived bounds overflow rejects atomically");
    for (std::size_t i = 1; i < NativeStuntJumps::Capacity; ++i)
        Check(jumps.Add({float(i), 0, 0}, {}, {}, {}, {}, std::int32_t(i), index, error) ==
            NativeStuntJumpStatus::Ok && index == i, "fill source stunt-jump pool in order");
    Check(jumps.Add({}, {}, {}, {}, {}, 0, index, error) == NativeStuntJumpStatus::Full &&
        jumps.Entries().size() == 256 && jumps.Revision() == 256 && index == 255,
        "full source pool rejects without changing identity");
    std::printf("native-stunt-jumps-ok checks=%zu capacity=256 registration=1 update=0 reward=0 reset=0 save=0\n", s_Checks);
    return 0;
}
