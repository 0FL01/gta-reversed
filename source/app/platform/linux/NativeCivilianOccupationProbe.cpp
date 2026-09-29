#include "NativeCivilianOccupation.h"

#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace {
int s_Checks = 0;
void Check(bool value, const char* message) {
    ++s_Checks;
    if (!value) {
        std::fprintf(stderr, "civilian-occupation-fail: %s\n", message);
        std::exit(1);
    }
}

class Observations final : public NativeCivilianOccupationObservations {
public:
    int Profile = 0, ZoneCalls = 0, AttractorCalls = 0, StatsCalls = 0;
    bool Known = true;
    bool ZoneAccepts(std::int32_t model, bool& accepted) noexcept override {
        ++ZoneCalls;
        accepted = Profile % 3 == 0 || (model + Profile) % 3 != 0;
        return Known;
    }
    bool AttractorAccepts(std::int32_t model, std::string_view, bool& accepted) noexcept override {
        ++AttractorCalls;
        accepted = (model + Profile) % 4 != 0;
        return Known;
    }
    bool StatsCompatible(std::int32_t actual, std::int32_t requested,
        bool& accepted) noexcept override {
        ++StatsCalls;
        accepted = (actual + requested + Profile) % 3 == 0;
        return Known;
    }
};

NativeCivilianOccupationInput Case(int profile, int salt) {
    NativeCivilianOccupationInput input;
    input.Exterior = profile % 4 != 0;
    input.InteriorPedsUsed = profile % 5 == 0 ? 21 : 20;
    input.TestUsedOccupations = profile % 3 != 0;
    input.MustBeMale = profile % 6 == 1 || profile == 19;
    input.MustBeFemale = profile % 6 == 2 || profile == 19;
    input.OnlyOnFoot = profile % 4 == 1;
    input.AtAttractor = profile % 5 == 2;
    input.AnimationGroup = profile % 7 == 2 ? 1 : -1;
    input.CompatibleStats = profile % 6 == 4 ? 3 : -1;
    input.ExcludedModel = profile % 7 == 4 ? 12 : -1;
    input.AttractorScript = "ATM";
    input.Rain = profile % 3 == 0 ? 0.0f :
        profile % 3 == 1 ? std::nextafter(0.1f, 0.0f) : 0.1f;
    for (int i = 0; i < 8; ++i) {
        auto& model = input.LoadedPeds[std::size_t(i)];
        model.Model = (i + salt) % 11 == 0 ? -1 : 10 + i;
        model.Loaded = (i + salt) % 5 != 0;
        model.ModelInfoKnown = true;
        model.References = std::int16_t((i * 3 + salt) % 9);
        model.PedType = (i + salt) % 4 == 0 ? 17 : (i % 2 == 0 ? 4 : 5);
        model.AnimationGroup = (i + salt) % 3;
        model.StatsType = i % 3 == 0 ? 38 : (i % 3 == 1 ? 39 : 2);
        model.CarsCanDrive = (i + salt) % 2 == 0 ? 0x1000u : 0;
    }
    return input;
}

void Matrix() {
    int id = 0;
    for (int profile = 0; profile < 20; ++profile) {
        for (int salt = 0; salt < 32; ++salt) {
            const auto input = Case(profile, salt);
            Observations observations;
            observations.Profile = profile;
            std::int32_t model = -123456;
            const auto status = NativeChooseCivilianOccupation(input, observations, model);
            Check(status == NativeCivilianOccupationStatus::Chosen ||
                status == NativeCivilianOccupationStatus::NoOccupation, "known matrix status");
            Check(status == NativeCivilianOccupationStatus::Chosen || model == -123456,
                "no occupation retains output");
            std::printf("CASE %d %d %d %d %d %d %d %d\n", id++, profile, salt,
                int(status), model, observations.ZoneCalls, observations.AttractorCalls,
                observations.StatsCalls);
        }
    }
}

void Boundaries() {
    NativeCivilianOccupationInput input;
    Observations observations;
    auto& last = input.LoadedPeds[7];
    last = {17, true, true, true, 0, 4, 0, 0, 0};
    std::int32_t out = -123456;
    Check(NativeChooseCivilianOccupation(input, observations, out) ==
        NativeCivilianOccupationStatus::Chosen && out == 17,
        "the eighth slot belongs to every reference pass");
    input.LoadedPeds[0] = {10, true, true, true, 1, 4, 0, 0, 0};
    out = -123456;
    Check(NativeChooseCivilianOccupation(input, observations, out) ==
        NativeCivilianOccupationStatus::Chosen && out == 17,
        "lower reference pass precedes an earlier slot");
    last.References = 3;
    input.LoadedPeds[0].Model = -1;
    out = -123456;
    Check(NativeChooseCivilianOccupation(input, observations, out) ==
        NativeCivilianOccupationStatus::NoOccupation && out == -123456,
        "used occupations test performs three exterior passes");
    input.TestUsedOccupations = false;
    Check(NativeChooseCivilianOccupation(input, observations, out) ==
        NativeCivilianOccupationStatus::Chosen && out == 17, "disabled test has seven passes");
    input.TestUsedOccupations = true;
    input.Exterior = false;
    input.InteriorPedsUsed = 21;
    Check(NativeChooseCivilianOccupation(input, observations, out) ==
        NativeCivilianOccupationStatus::Chosen && out == 17, "interior greater than twenty has five passes");
    input.InteriorPedsUsed = 20;
    out = -123456;
    Check(NativeChooseCivilianOccupation(input, observations, out) ==
        NativeCivilianOccupationStatus::NoOccupation && out == -123456, "interior twenty has three passes");
    input.TestUsedOccupations = false;
    last.Model = -1;
    Check(NativeChooseCivilianOccupation(input, observations, out) ==
        NativeCivilianOccupationStatus::Chosen && out == 7, "source MALE01 fallback is returned only by helper");

    input = {};
    input.LoadedPeds[0] = {10, false, true, true, 0, 4, 0, 0, 0};
    out = -123456;
    Check(NativeChooseCivilianOccupation(input, observations, out) ==
        NativeCivilianOccupationStatus::UnknownStreaming && out == -123456, "unknown streaming is not absent");
    input.LoadedPeds[0].StreamingKnown = true;
    input.LoadedPeds[0].ModelInfoKnown = false;
    Check(NativeChooseCivilianOccupation(input, observations, out) ==
        NativeCivilianOccupationStatus::UnknownModel && out == -123456, "unknown model is not fallback");
    input.LoadedPeds[0].ModelInfoKnown = true;
    observations.Known = false;
    Check(NativeChooseCivilianOccupation(input, observations, out) ==
        NativeCivilianOccupationStatus::UnknownPolicy && out == -123456, "unknown exterior policy rejects");
    input.Exterior = false;
    Check(NativeChooseCivilianOccupation(input, observations, out) ==
        NativeCivilianOccupationStatus::Chosen && out == 10, "interior bypasses zone result, not call");
    input.Rain = std::numeric_limits<float>::quiet_NaN();
    const auto calls = observations.ZoneCalls;
    out = -123456;
    Check(NativeChooseCivilianOccupation(input, observations, out) ==
        NativeCivilianOccupationStatus::InvalidInput && out == -123456 &&
        observations.ZoneCalls == calls, "invalid preflight retains output and observations");
}
}

int main() {
    Boundaries();
    Matrix();
    std::printf("native-civilian-occupation-ok checks=%d cases=640 slots=8 references=3,5,7 birth=unowned census=incomplete\n",
        s_Checks);
}
