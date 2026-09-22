#include "app/platform/linux/NativeCarGeneratorPopulation.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace {
int s_Checks = 0;
void Check(bool valid, const char* message) {
    ++s_Checks;
    if (!valid) { std::fprintf(stderr, "loaded-cars-fail: %s\n", message); std::exit(1); }
}
} // namespace

int main(int argc, char** argv) {
    Check(argc == 2, "game directory argument");
    std::string error;
    NativeCarGenerators generators;
    Check(generators.LoadBeforeWorker(argv[1], 0, error), "vehicles.ide definitions");
    NativeZonePopulation zones;
    Check(zones.LoadBeforeWorker(argv[1], error), "source info.zon registry");
    NativeCarGeneratorPopulation owner;
    Check(owner.LoadBeforeWorker(argv[1], generators.ModelDefinitions(), error), "cargrp/popcycle corpus");
    Check(owner.CarGroups() == 34 && owner.CycleRows() == 480, "all source group/cycle rows");
    NativeSourceRng rng;
    Check(rng.SeedOnce(1792) == NativeSourceRngStatus::Ready, "one source seed");
    const std::array<NativeCarLoadedModel, 5> roster{{{400}, {481}, {537}, {569}, {596}}};
    NativeCarPopulationSelection selected;
    Check(owner.Select({2369, -1263, 23}, 12, false, zones.Entries(), roster,
        rng.Reference(), selected, error), "real zone and ordered roster");
    Check(selected.ModelId == 400 && selected.Draws == 1 &&
        selected.AppropriateLoadedCars.size() == 1 &&
        selected.AppropriateLoadedCars[0] == 400, "no bicycle/train/mission fallback");
    const auto prior = selected;
    const auto before = rng.Inspect().Value->DrawCount;
    Check(!owner.Select({0, 0, 9999}, 12, false, zones.Entries(), roster,
        rng.Reference(), selected, error) && selected.ModelId == prior.ModelId &&
        rng.Inspect().Value->DrawCount == before, "unqualified zone failure atomic");
    auto altered = std::vector<NativeZonePopulationEntry>(zones.Entries().begin(), zones.Entries().end());
    altered[0].Label = "altered";
    Check(!owner.Select({2369, -1263, 23}, 12, false, altered, roster,
        rng.Reference(), selected, error) && selected.ModelId == prior.ModelId &&
        rng.Inspect().Value->DrawCount == before, "changed population-zone owner rejected");
    auto duplicate = roster;
    duplicate[1].ModelId = 400;
    Check(!owner.Select({2369, -1263, 23}, 12, false, zones.Entries(), duplicate,
        rng.Reference(), selected, error) && selected.ModelId == prior.ModelId &&
        rng.Inspect().Value->DrawCount == before, "duplicate loaded source roster rejected");
    Check(!owner.Select({2369, -1263, 23}, 24, false, zones.Entries(), roster,
        rng.Reference(), selected, error) && rng.Inspect().Value->DrawCount == before,
        "invalid source hour cannot draw RNG");
    const std::array<NativeCarLoadedModel, 2> nonCars{{{481}, {537}}};
    Check(owner.Select({2369, -1263, 23}, 12, false, zones.Entries(), nonCars,
        rng.Reference(), selected, error) && selected.ModelId == -1 &&
        selected.Draws == 0 && rng.Inspect().Value->DrawCount == before,
        "no eligible model is MODEL_INVALID without consuming RNG");
    std::array<NativeCarLoadedModel, 5> suppressed = roster;
    suppressed[0].ScriptSuppressed = true;
    Check(owner.Select({2369, -1263, 23}, 12, false, zones.Entries(), suppressed,
        rng.Reference(), selected, error) && selected.ModelId == -1 &&
        selected.Draws == 10, "ten source draws do not fabricate a fallback");
    Check(!owner.LoadBeforeWorker(argv[1], generators.ModelDefinitions(), error), "reload rejection");
    NativeSourceRng streamRng, repeatedRng;
    Check(streamRng.SeedOnce(1792) == NativeSourceRngStatus::Ready &&
        repeatedRng.SeedOnce(1792) == NativeSourceRngStatus::Ready, "source stream seeded once");
    NativeCarStreamChoice stream, repeat;
    Check(owner.ChooseModelToStream({2369, -1263, 23}, 12, false, zones.Entries(),
        roster, 0, streamRng.Reference(), stream, error), "source car stream group selection");
    Check(owner.ChooseModelToStream({2369, -1263, 23}, 12, false, zones.Entries(),
        roster, 0, repeatedRng.Reference(), repeat, error) &&
        stream.Zone == repeat.Zone && stream.ModelId == repeat.ModelId &&
        stream.Group == repeat.Group && stream.Draws == repeat.Draws &&
        streamRng.Inspect().Value->DrawCount == repeatedRng.Inspect().Value->DrawCount,
        "single shared RNG stream repeated deterministically");
    Check(stream.ModelId == -1 ||
        (generators.FindModel(stream.ModelId) &&
         std::ranges::find(roster, stream.ModelId, &NativeCarLoadedModel::ModelId) == roster.end()),
        "stream choice is an unloaded source model request, never a fallback");
    Check(stream.ModelId == 420 && stream.LastCab == 420 && stream.Group == -1 &&
        stream.Draws == 0, "ELS1a source taxi priority does not consume RNG");
    auto taxiLoaded = std::vector<NativeCarLoadedModel>(roster.begin(), roster.end());
    taxiLoaded.push_back({420});
    NativeCarStreamChoice postTaxi;
    Check(owner.ChooseModelToStream({2369, -1263, 23}, 12, false, zones.Entries(),
        taxiLoaded, stream.LastCab, streamRng.Reference(), postTaxi, error) &&
        postTaxi.Draws >= 1 && postTaxi.Group >= 0 && postTaxi.Group < 18 &&
        (postTaxi.ModelId < 0 ||
         std::ranges::find(taxiLoaded, postTaxi.ModelId, &NativeCarLoadedModel::ModelId) == taxiLoaded.end()),
        "subsequent source group selection draws from shared RNG and unloaded roster");
    const auto streamDraws = streamRng.Inspect().Value->DrawCount;
    Check(!owner.ChooseModelToStream({2369, -1263, 23}, 24, false, zones.Entries(),
        roster, 0, streamRng.Reference(), stream, error) &&
        streamRng.Inspect().Value->DrawCount == streamDraws,
        "invalid streaming hour rejects before shared RNG draw");
    Check(!owner.ChooseModelToStream({2369, -1263, 23}, 12, false, zones.Entries(),
        duplicate, 0, streamRng.Reference(), stream, error) &&
        streamRng.Inspect().Value->DrawCount == streamDraws,
        "duplicate loaded roster rejects before shared RNG draw");
    std::printf("native-loaded-cars-ok checks=%d groups=%zu cycle=%zu zone=%s eligible=%zu selected=%d suppressed=ten-draw-no-model next-stream=%d group=%d draws=%u\n",
        s_Checks, owner.CarGroups(), owner.CycleRows(), prior.Zone.c_str(),
        prior.AppropriateLoadedCars.size(), prior.ModelId, postTaxi.ModelId, postTaxi.Group, postTaxi.Draws);
}
