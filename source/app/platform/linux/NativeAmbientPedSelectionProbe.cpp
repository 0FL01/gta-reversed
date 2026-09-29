#include "NativeAmbientPedSelection.h"

#include <cstdio>
#include <cstdlib>
#include <limits>

namespace {
int s_Checks = 0;
void Check(bool condition, const char* message) {
    ++s_Checks;
    if (!condition) { std::fprintf(stderr, "ambient-selection-fail: %s\n", message); std::exit(1); }
}

struct Models final : NativeAmbientPedModelObservations {
    int Mode = 0;
    unsigned PoliceCalls = 0, GangCalls = 0, CivilianCalls = 0;
    bool Police(NativeSourceRngRef, std::int32_t& model) noexcept override {
        ++PoliceCalls;
        if (Mode == 5) return false;
        model = 280;
        return true;
    }
    bool Gang(NativeSourceRngRef, NativeAmbientPedSelection& out) noexcept override {
        ++GangCalls;
        if (Mode == 5) return false;
        out = Mode == 1 ? NativeAmbientPedSelection{7, -1} :
            Mode == 2 ? NativeAmbientPedSelection{0, -1} : NativeAmbientPedSelection{7, 102};
        return true;
    }
    bool Civilian(NativeSourceRngRef, NativeAmbientPedSelection& out) noexcept override {
        ++CivilianCalls;
        if (Mode == 5) return false;
        out = Mode == 3 ? NativeAmbientPedSelection{4, -1} :
            Mode == 4 ? NativeAmbientPedSelection{4, 7} : NativeAmbientPedSelection{4, 9};
        return true;
    }
};
}

int main() {
    const std::array<NativeAmbientDealerModel, 3> dealerModels{{{28, true}, {29, false}, {30, true}}};
    unsigned caseId = 0;
    // The final two seeds produce rand15=22937/22938: the source station's
    // integer /32768 threshold differs from a float /32767 comparison here.
    for (const auto seed : {0u, 1u, 7u, 1792u, 0xffffffffu, 10000u, 2856187457u, 3683841601u}) {
        for (unsigned profile = 0; profile < 18; ++profile) {
            for (int modelMode = 0; modelMode < 5; ++modelMode) {
                NativeAmbientPedSelectionInput input;
                input.HasZone = profile != 0;
                input.Exterior = profile != 8;
                input.PoliceStation = profile == 9;
                input.OnlyGangs = profile == 6 || profile == 7;
                input.NoGangs = profile == 7;
                input.NoCops = profile == 10;
                input.ZoneNoCops = profile == 11;
                input.GangWar = profile == 12;
                input.DealerGroupKnown = true;
                input.DealerGroup = profile == 13 ? std::span<const NativeAmbientDealerModel>{} : dealerModels;
                input.TargetCivilian = profile == 1 ? 0.0f : profile == 2 ? 20.0f : 4.0f;
                input.TargetCop = profile == 1 ? 0.0f : profile == 3 ? 20.0f : 4.0f;
                input.TargetDealer = profile == 1 ? 0.0f : profile == 4 || profile == 13 ? 20.0f : 4.0f;
                input.TargetGang = profile == 1 ? 0.0f : profile == 5 ? 20.0f : 4.0f;
                input.CivilianMale = 1;
                input.CivilianFemale = 1;
                input.Cops = 1;
                input.Dealers = 1;
                input.Gangs[0] = 1;
                if (profile >= 14) {
                    input.TargetCivilian = 2.0f;
                    input.TargetCop = 1.0f;
                    input.TargetDealer = 1.0f;
                    input.TargetGang = 1.0f;
                    if (profile == 14) {
                        input.TargetCivilian += 1.5f;
                        input.TargetCop += 1.999f;
                        input.TargetDealer += 1.125f;
                        input.TargetGang += 1.75f;
                    } else if (profile == 15) {
                        input.TargetCivilian += 2.0f;
                    } else if (profile == 16) {
                        input.TargetCivilian += 2.0f;
                        input.TargetCop += 2.0f;
                        input.TargetDealer += 2.0f;
                        input.TargetGang += 2.0f;
                    } else {
                        input.TargetDealer += 1.5f;
                    }
                }
                NativeSourceRng rng;
                Check(rng.SeedOnce(seed) == NativeSourceRngStatus::Ready, "one source seed");
                Models models;
                models.Mode = modelMode;
                NativeAmbientPedSelection out{31, 123};
                const auto status = NativeSelectAmbientPed(input, rng.Reference(), models, out);
                Check(status == NativeAmbientPedSelectionStatus::Selected ||
                    status == NativeAmbientPedSelectionStatus::NoSelection, "source model decision");
                if (status != NativeAmbientPedSelectionStatus::Selected)
                    Check(out == NativeAmbientPedSelection{31, 123}, "no-selection output retention");
                const auto provenance = rng.Inspect().Value.value();
                std::printf("SELECTION %u %u %u %d %u %d %d %llu %u %u %u %u\n",
                    caseId++, seed, profile, modelMode, unsigned(status), out.PedType, out.Model,
                    static_cast<unsigned long long>(provenance.DrawCount), provenance.State,
                    models.PoliceCalls, models.GangCalls, models.CivilianCalls);
            }
        }
    }
    NativeAmbientPedSelectionInput input;
    input.HasZone = true;
    input.TargetDealer = 20;
    Models models;
    NativeSourceRng rng;
    Check(rng.SeedOnce(0) == NativeSourceRngStatus::Ready, "guard seed");
    NativeAmbientPedSelection out{31, 123};
    Check(NativeSelectAmbientPed(input, rng.Reference(), models, out) ==
        NativeAmbientPedSelectionStatus::UnsupportedModelObservation && out == NativeAmbientPedSelection{31, 123},
        "unknown dealer group cannot masquerade as an empty group");
    input.TargetDealer = 0;
    input.TargetCivilian = 20;
    models.Mode = 5;
    Check(NativeSelectAmbientPed(input, rng.Reference(), models, out) ==
        NativeAmbientPedSelectionStatus::UnsupportedModelObservation && out == NativeAmbientPedSelection{31, 123},
        "missing model observation cannot masquerade as MODEL_INVALID");
    input.TargetCivilian = std::numeric_limits<float>::quiet_NaN();
    const auto before = rng.Inspect().Value;
    Check(NativeSelectAmbientPed(input, rng.Reference(), models, out) ==
        NativeAmbientPedSelectionStatus::InvalidInput && rng.Inspect().Value == before,
        "invalid non-station inputs reject before deficit draws");
    std::printf("native-ambient-ped-selection-ok checks=%d cases=%u model-authority=explicit birth=unowned census=incomplete\n",
        s_Checks, caseId);
}
