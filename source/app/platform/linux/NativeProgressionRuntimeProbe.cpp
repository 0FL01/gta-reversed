#include "NativeProgressionRuntime.h"

#include <cstdio>
#include <cstdlib>

namespace {
int g_Checks = 0;
void Check(bool value, const char* message) {
    ++g_Checks;
    if (!value) { std::fprintf(stderr, "progression-fail: %s\n", message); std::exit(1); }
}
}

int main() {
    NativeProgressionRuntime original;
    std::string error;
    Check(original.SetMoney(10000, error) == NativeProgressionStatus::Ok, "money");
    Check(original.RegisterPurchase("SAFEHOUSE", NativeInteractionKind::Property, 5000, false, error) == NativeProgressionStatus::Ok &&
        original.Buy("SAFEHOUSE", error) == NativeProgressionStatus::Ok &&
        original.Use("SAFEHOUSE", error) == NativeProgressionStatus::Ok, "property");
    Check(original.RegisterPurchase("AMMUNATION", NativeInteractionKind::Shop, 1000, false, error) == NativeProgressionStatus::Ok &&
        original.Buy("AMMUNATION", error) == NativeProgressionStatus::Ok, "shop");
    Check(original.SetIntegerStat(146, 7, error) == NativeProgressionStatus::Ok &&
        original.SetFloatStat(0, 42.5f, error) == NativeProgressionStatus::Ok, "stats");
    Check(original.GrantReward(3, 2500, error) == NativeProgressionStatus::Ok &&
        original.SetUnlock(5, true, error) == NativeProgressionStatus::Ok, "reward unlock");
    Check(original.RegisterInterior(1, false, error) == NativeProgressionStatus::Ok &&
        original.EnterInterior(1, error) == NativeProgressionStatus::Locked &&
        original.UnlockInterior(1, error) == NativeProgressionStatus::Ok &&
        original.EnterInterior(1, error) == NativeProgressionStatus::Ok, "interior");
    std::vector<std::uint8_t> bytes;
    Check(original.Encode(bytes, error) == NativeProgressionStatus::Ok, "encode");
    NativeProgressionRuntime restarted;
    Check(restarted.Restore(bytes, error) == NativeProgressionStatus::Ok, "restart restore");
    Check(restarted.Interactions().Money() == 4000 && restarted.Interactions().Find("SAFEHOUSE")->Owned &&
        restarted.IntegerStat(146) == 7 && restarted.FloatStat(0) == 42.5f &&
        restarted.Reward(3) == 2500 && restarted.Unlocked(5) && restarted.Interior(1)->Visits == 1,
        "all owner families restored");
    Check(restarted.Use("SAFEHOUSE", error) == NativeProgressionStatus::Ok &&
        restarted.SetIntegerStat(146, 8, error) == NativeProgressionStatus::Ok,
        "reapplied after restart");
    const auto before = restarted.Revision();
    auto corrupt = bytes;
    corrupt.back() ^= 1;
    Check(restarted.Restore(corrupt, error) == NativeProgressionStatus::CorruptSave &&
        restarted.Revision() == before, "corrupt atomic");
    Check(!bytes.empty() && bytes.size() < 4096, "bounded canonical envelope");
    std::printf("native-progression-runtime-ok checks=%d money=4000 property=owned shop=1 "
        "stats=7,42.5 reward=2500 unlock=5 interior=1 restart=reapplied envelope=%zu\n",
        g_Checks, bytes.size());
}
