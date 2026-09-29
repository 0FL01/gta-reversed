#include "NativeGangPedChoice.h"

#include <cstdio>
#include <cstdlib>
#include <limits>

namespace {
int s_Checks = 0;
void Check(bool value, const char* why) {
    ++s_Checks;
    if (!value) {
        std::fprintf(stderr, "gang-ped-choice-fail: %s\n", why);
        std::exit(1);
    }
}
NativeGangPedChoiceInput Fixture(int count, int profile) {
    NativeGangPedChoiceInput input;
    input.GroupKnown = true;
    input.ZoneZeroCount = count;
    if (profile == 5) input.ModelOverride = 17;
    for (int i = 0; i < 21; ++i) {
        input.CurrentGroup[std::size_t(i)] = {std::uint16_t(10 + i), true,
            profile == 0 || (profile == 2 && i % 3 == 0) ||
            (profile == 3 && i == count - 1) || (profile == 4 && i == 0)};
    }
    return input;
}
}

int main() {
    int cases = 0;
    for (const std::uint32_t seed : {0u, 1u, 7u, 1792u, 0xFFFFFFFFu}) {
        for (int count = 1; count <= 21; ++count) {
            for (int profile = 0; profile < 6; ++profile) {
                NativeSourceRng rng;
                Check(rng.SeedOnce(seed) == NativeSourceRngStatus::Ready, "one source RNG");
                NativeGangPedSequence sequence{77, 33, true};
                std::int32_t model = -777;
                const auto status = NativeChooseGangPedModel(Fixture(count, profile),
                    rng.Reference(), sequence, model);
                const auto provenance = rng.Inspect().Value.value();
                Check(provenance.DrawCount == (profile == 5 ? 0u : 2u), "exact two shared draws");
                Check(profile != 1 || (status == NativeGangPedChoiceStatus::NoLoadedModel &&
                    model == -777), "unloaded is not a chosen model");
                Check(profile != 5 || (model == 10 && sequence == NativeGangPedSequence{77, 33, true}),
                    "override returns first model and leaves shared sequence");
                std::printf("CASE %u %d %d %d %d %d %d %d %u %llu\n", seed, count,
                    profile, int(status), model, sequence.Elements, sequence.Offset,
                    int(sequence.Ascending), provenance.State,
                    static_cast<unsigned long long>(provenance.DrawCount));
                ++cases;
            }
        }
    }
    NativeSourceRng rng;
    Check(rng.SeedOnce(1792) == NativeSourceRngStatus::Ready, "guard seed");
    auto input = Fixture(3, 0);
    auto before = rng.Inspect().Value.value();
    NativeGangPedSequence sequence{77, 33, true};
    auto originalSequence = sequence;
    std::int32_t model = -777;
    input.GroupKnown = false;
    Check(NativeChooseGangPedModel(input, rng.Reference(), sequence, model) ==
        NativeGangPedChoiceStatus::UnknownGroup && model == -777 && sequence == originalSequence &&
        rng.Inspect().Value == before, "unknown group is not an empty group");
    input = Fixture(0, 0);
    Check(NativeChooseGangPedModel(input, rng.Reference(), sequence, model) ==
        NativeGangPedChoiceStatus::InvalidInput && model == -777 && sequence == originalSequence &&
        rng.Inspect().Value == before, "zero count cannot modulo-zero");
    input = Fixture(3, 0);
    Check(NativeChooseGangPedModel(input, {}, sequence, model) ==
        NativeGangPedChoiceStatus::UnknownRng && model == -777 && sequence == originalSequence,
        "no fork or fallback RNG");
    for (auto& entry : input.CurrentGroup) entry.StreamingKnown = false;
    Check(NativeChooseGangPedModel(input, rng.Reference(), sequence, model) ==
        NativeGangPedChoiceStatus::UnknownStreaming && model == -777 &&
        rng.Inspect().Value->DrawCount == 2 && sequence.Elements == 3,
        "missing streaming observation preserves consumed source prefix");
    input = Fixture(0, 5);
    Check(NativeChooseGangPedModel(input, {}, sequence, model) ==
        NativeGangPedChoiceStatus::Chosen && model == 10,
        "override does not inspect count or RNG");
    input.ModelOverride = std::numeric_limits<std::int8_t>::min();
    input.CurrentGroup[0].Model = 65535;
    Check(NativeChooseGangPedModel(input, {}, sequence, model) ==
        NativeGangPedChoiceStatus::Chosen && model == 65535,
        "any non-FF override returns the zero-extended first group word");
    NativeSourceRng directionRng;
    Check(directionRng.SeedOnce(0) == NativeSourceRngStatus::Ready, "direction seed");
    input = Fixture(7, 0);
    Check(NativeChooseGangPedModel(input, directionRng.Reference(), sequence, model) ==
        NativeGangPedChoiceStatus::Chosen && !sequence.Ascending && sequence.Offset == 3,
        "original bit4 direction differs from upstream bit2 refactor");
    std::printf("native-gang-ped-choice-ok checks=%d cases=%d slots=21 sequence=shared-rng "
        "direction-bit=4 birth=unowned census=incomplete\n", s_Checks, cases);
}
