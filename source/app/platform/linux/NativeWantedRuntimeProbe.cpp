#include "NativeWantedRuntime.h"

#include <cstdio>
#include <cstdlib>

namespace {
int g_Checks = 0;
void Check(bool value, const char* message) {
    ++g_Checks;
    if (!value) { std::fprintf(stderr, "wanted-runtime-fail: %s\n", message); std::exit(1); }
}
std::vector<NativeWantedEvent> Scenario() {
    NativeWantedRuntime wanted;
    std::string error;
    Check(wanted.RegisterOffense(600, 1, error) == NativeWantedStatus::Ok &&
        wanted.State().Level == 3 && wanted.State().ChanceOnRoadblock == 12 &&
        wanted.State().MaximumCops == 4 && wanted.State().MaximumCopCars == 2,
        "offense escalates to source level3");
    for (std::uint64_t cop = 1; cop <= 4; ++cop)
        Check(wanted.JoinPursuit(cop, error) == NativeWantedStatus::Ok, "join bounded pursuit");
    Check(wanted.JoinPursuit(5, error) == NativeWantedStatus::CapacityExceeded,
        "pursuit limit rejection");
    Check(wanted.ShouldCreateRoadblock(11) && !wanted.ShouldCreateRoadblock(12),
        "source roadblock percentage threshold");
    Check(wanted.EscapeTick(1002, true, true, false, error) == NativeWantedStatus::Ok &&
        wanted.State().Chaos == 600, "police presence retains chaos");
    Check(wanted.EscapeTick(2004, false, true, true, error) == NativeWantedStatus::Ok &&
        wanted.State().Chaos == 600, "elusive law vehicle holds high wanted");
    std::uint64_t now = 2004;
    while (wanted.State().Chaos) {
        now += 1001;
        Check(wanted.EscapeTick(now, false, true, false, error) == NativeWantedStatus::Ok,
            "source escape decrement");
    }
    Check(wanted.State().Level == 0 && wanted.State().PursuitCount == 0 &&
        !wanted.ShouldCreateRoadblock(0), "escape clears pursuit and roadblocks");
    return wanted.Events();
}
}

int main() {
    const auto first = Scenario();
    const auto second = Scenario();
    Check(first.size() == second.size(), "scenario event count deterministic");
    for (std::size_t i = 0; i < first.size(); ++i) {
        Check(first[i].Sequence == second[i].Sequence && first[i].State == second[i].State,
            "scenario states deterministic");
    }
    std::printf("native-wanted-runtime-ok checks=%d offense=600 level=3 pursuit=4 roadblock=12 escape=clean deterministic=twice events=%zu\n",
        g_Checks, first.size());
}
