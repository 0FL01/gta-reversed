#include "NativeModelStreaming.h"

#include <cstdio>
#include <cstdlib>
#include <limits>

namespace {
unsigned Checks = 0;
void Check(bool ok) {
    ++Checks;
    if (!ok) { std::fprintf(stderr, "model-streaming-check-failed %u\n", Checks); std::abort(); }
}
void Row(char operation, const NativeModelStreamObservation& o, std::uint32_t flags,
    const NativeModelStreamPlan& p) {
    std::printf("MODEL %c %d %u %u %u %u %d %d %d %u", operation, o.Model,
        unsigned(o.LoadState), unsigned(o.Flags), unsigned(o.InList), unsigned(o.ModelType),
        o.TxdSlot, o.AnimationSlot, o.ParentTxdSlot, flags);
    std::printf(" %u", unsigned(p.Count));
    for (unsigned i = 0; i < p.Count; ++i) {
        const auto& e = p.Effects[i];
        std::printf(" %u %d %u", unsigned(e.Kind), e.Argument, e.Flags);
    }
    std::putchar('\n');
}
}
int main() {
    NativeModelStreamObservation o;
    NativeModelStreamPlan p;
    p.Count = 1; p.Effects[0].Argument = 777;
    const auto saved = p;
    Check(NativePlanModelRequest(o, 8, p) == NativeModelStreamStatus::UnknownState && p == saved);
    Check(NativePlanModelDeletable(o, p) == NativeModelStreamStatus::UnknownState && p == saved);
    o.Known = true; o.Model = 7;
    Check(NativePlanModelRequest(o, 8, p) == NativeModelStreamStatus::UnknownDependencies && p == saved);
    o.LoadState = 1;
    Check(NativePlanModelRequest(o, 8, p) == NativeModelStreamStatus::UnknownList && p == saved);
    Check(NativePlanModelDeletable(o, p) == NativeModelStreamStatus::UnknownList && p == saved);
    o.ListKnown = true; o.InList = true;
    Check(NativePlanModelRequest(o, 8, p) == NativeModelStreamStatus::UnknownModelType && p == saved);
    o.InList = false;
    Check(NativePlanModelRequest(o, 8, p) == NativeModelStreamStatus::Planned && p.Count == 1);
    p = saved; o.Model = 26312;
    Check(NativePlanModelRequest(o, 8, p) == NativeModelStreamStatus::InvalidInput && p == saved);
    o.Model = 7; o.LoadState = 5;
    Check(NativePlanModelDeletable(o, p) == NativeModelStreamStatus::InvalidInput && p == saved);
    o.LoadState = 0; o.DependenciesKnown = true; o.TxdSlot = -1;
    Check(NativePlanModelRequest(o, 8, p) == NativeModelStreamStatus::InvalidInput && p == saved);
    o.TxdSlot = 0; o.AnimationSlot = 180;
    Check(NativePlanModelRequest(o, 8, p) == NativeModelStreamStatus::InvalidInput && p == saved);
    o.Model = 20000; o.ParentTxdSlot = 5000;
    Check(NativePlanModelRequest(o, 8, p) == NativeModelStreamStatus::InvalidInput && p == saved);
    o.Flags = 4; o.ListKnown = false; o.LoadState = 1;
    Check(NativePlanModelDeletable(o, p) == NativeModelStreamStatus::Planned && p.Count == 1);
    unsigned requests = 0, deletable = 0;
    constexpr std::uint32_t inputs[]{0, 2, 4, 8, 10, 16, 31, std::numeric_limits<std::uint32_t>::max()};
    for (unsigned load = 0; load < 5; ++load) {
        for (unsigned old = 0; old < 67; ++old) {
            for (const auto flags : inputs) {
                for (unsigned profile = 0; profile < 5; ++profile) {
                    o = {}; o.Known = true; o.LoadState = load;
                    o.Flags = old < 64 ? old : old == 64 ? 128 : old == 65 ? 192 : 255;
                    o.Model = profile < 3 ? 7 : profile == 3 ? 20042 : 25000;
                    o.ModelTypeKnown = true; o.ModelType = profile == 1 ? 6 : profile == 2 ? 7 : 1;
                    o.ListKnown = true; o.InList = load == 1 && (old + profile) % 2;
                    o.DependenciesKnown = true; o.TxdSlot = 42;
                    o.AnimationSlot = old % 2 ? 17 : -1; o.ParentTxdSlot = old % 2 ? 8 : -1;
                    Check(NativePlanModelRequest(o, flags, p) == NativeModelStreamStatus::Planned);
                    Check(p.Count && p.Count <= p.Effects.size());
                    Row('R', o, flags, p); ++requests;
                }
            }
        }
        for (unsigned old = 0; old < 256; ++old) {
            for (unsigned linked = 0; linked < 2; ++linked) {
                o = {}; o.Known = true; o.Model = 7; o.LoadState = load;
                o.Flags = old; o.ListKnown = true; o.InList = load == 1 && linked;
                Check(NativePlanModelDeletable(o, p) == NativeModelStreamStatus::Planned);
                Check(p.Count == 1 || p.Count == 2);
                Row('D', o, 0, p); ++deletable;
            }
        }
    }
    std::printf("native-model-streaming-ok checks=%u requests=%u deletable=%u effects=planned assets=unowned census=incomplete\n",
        Checks, requests, deletable);
}
