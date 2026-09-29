#include "app/platform/linux/NativeCarGeneratorPopulation.h"
#include "app/platform/linux/NativePedStreaming.h"

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
    Check(argc == 2 || (argc == 3 && std::string(argv[2]) == "--cycle-rows"), "game directory argument");
    const bool rows = argc == 3;
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
    NativeCarPopulationSelection preview;
    Check(owner.PreviewAppropriate({2369, -1263, 23}, 12, false, zones.Entries(),
        roster, preview, error) && preview.ModelId == -1 &&
        preview.AppropriateLoadedCars == prior.AppropriateLoadedCars &&
        preview.WeightSum == prior.WeightSum && preview.Draws == 0 &&
        rng.Inspect().Value->DrawCount == before,
        "appropriate count before StreamOneNewCar consumes no RNG");
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
    Check(owner.PreviewAppropriate({2369, -1263, 23}, 12, false, zones.Entries(),
        nonCars, preview, error) && preview.AppropriateLoadedCars.empty() &&
        rng.Inspect().Value->DrawCount == before, "no appropriate member starts source stream gate");
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
    NativeWorldEntityInfo names;
    NativePedModelMetadata metadata;
    Check(names.LoadBeforeWorker(argv[1], {}, error) && metadata.LoadBeforeWorker(argv[1], names, error),
        "same complete IDE and ped metadata authority for cycle binding");
    std::array<NativePedGangCarGroup, 10> gangCars{};
    Check(NativeQualifyPedGangCarGroups(owner, gangCars) == NativePedStreamStatus::QualifiedGroups,
        "gang car identities reuse the owned 34-row cargrp reader");
    for (std::size_t gang = 0; gang < gangCars.size(); ++gang) {
        std::span<const std::int32_t> source;
        Check(owner.ObserveGroupModels(std::uint32_t(18 + gang), source) &&
            gangCars[gang].Known && gangCars[gang].Count == source.size(), "exact source gang car group count");
        if (rows) std::printf("CAR_GROUP %zu %u", 18 + gang, unsigned(gangCars[gang].Count));
        for (std::size_t i = 0; i < source.size(); ++i) {
            Check(gangCars[gang].Models[i].Model == source[i] &&
                !gangCars[gang].Models[i].StreamingKnown && !gangCars[gang].Models[i].Loaded,
                "authored car identity and order never imply a loaded or requested model");
            if (rows) std::printf(" %d", source[i]);
        }
        if (rows) std::printf("\n");
    }
    const std::int32_t keep[]{777};
    std::span<const std::int32_t> borrowed = keep;
    Check(!owner.ObserveGroupModels(34, borrowed) && borrowed.data() == keep,
        "out-of-range source group retains prior span");
    NativeCarGeneratorPopulation unknownGroups;
    const auto priorIdentity = gangCars[0].Models[0].Model;
    Check(NativeQualifyPedGangCarGroups(unknownGroups, gangCars) == NativePedStreamStatus::UnknownGroup &&
        gangCars[0].Models[0].Model == priorIdentity && gangCars[0].Known,
        "unowned car reader cannot publish a fake empty group snapshot");
    auto cycleZones = std::vector<NativeZonePopulationEntry>(zones.Entries().begin(), zones.Entries().end());
    auto cycleZone = std::ranges::find(cycleZones, prior.Zone, &NativeZonePopulationEntry::Label);
    Check(cycleZone != cycleZones.end(), "qualified source zone identity");
    for (std::uint32_t type = 0; type < 20; ++type) {
        for (std::uint32_t weekend = 0; weekend < 2; ++weekend) {
            for (std::uint32_t hour = 0; hour < 24; ++hour) {
                cycleZone->PopulationType = std::uint8_t(type);
                cycleZone->Races = std::uint8_t((type + weekend + hour) % 16);
                cycleZone->DealerStrength = std::uint8_t(type * 3);
                cycleZone->NoCops = hour % 2 != 0;
                for (std::size_t gang = 0; gang < cycleZone->GangStrength.size(); ++gang)
                    cycleZone->GangStrength[gang] = std::uint8_t((type + gang) % 101);
                NativePopulationCycleObservation observed;
                Check(owner.ObserveCycle({2369, -1263, 23}, std::uint8_t(hour), weekend != 0,
                    cycleZones, observed, error) && observed.Known && observed.Zone == *cycleZone &&
                    observed.RowIndex == (type * 2 + weekend) * 12 + hour / 2,
                    "all real source rows bind explicit clock/week and current zone settings");
                NativePedGangWarObservation war;
                war.StateKnown = true; // Explicit NO_ATTACK observation, not an implicit default.
                NativePedGangMaskInput gangDemand;
                gangDemand.MemberKnown = true;
                gangDemand.CurrentMember = 17;
                Check(NativeQualifyPedGangDemand(observed, true, hour % 2 != 0, war, gangDemand) ==
                    NativePedStreamStatus::QualifiedGangDemand && gangDemand.ZoneKnown && gangDemand.HasZone &&
                    gangDemand.DemandKnown && gangDemand.Wanted == (type == 0 && hour % 2 == 0 ? 1022 : 1023) &&
                    gangDemand.MemberKnown && gangDemand.CurrentMember == 17 && !gangDemand.PedGroups[0].Known &&
                    !gangDemand.LoadedCars[0].Known,
                    "shared live zone strengths bind demand, not gang assets/masks/member ownership");
                war.StateKnown = false;
                Check(NativeQualifyPedGangDemand(observed, true, false, war, gangDemand) ==
                    NativePedStreamStatus::UnknownGangWar && gangDemand.DemandKnown && gangDemand.CurrentMember == 17,
                    "unavailable war retains prior demand without inventing a no-attack observation");
                for (std::uint32_t region = 0; region < 3; ++region) {
                    NativePedStreamInput input;
                    Check(NativeQualifyPedCycleSelection(metadata, region, observed, input) ==
                        NativePedStreamStatus::QualifiedCycleInput && input.ZoneKnown &&
                        input.RaceMask == observed.Zone.Races && !input.SlotsKnown &&
                        std::equal(input.Percentages.begin(), input.Percentages.end(), observed.Row.begin() + 6),
                        "actual cycle percentages/race/group binding cannot invent requested or loaded slots");
                }
                if (rows && hour % 2 == 0) {
                    std::printf("CYCLE %u %u %u %u %s %u %u %d", type, weekend, hour,
                        observed.RowIndex, observed.Zone.Label.c_str(), observed.Zone.Races,
                        observed.Zone.DealerStrength, int(observed.Zone.NoCops));
                    for (const auto gang : observed.Zone.GangStrength) std::printf(" %u", unsigned(gang));
                    for (const auto value : observed.Row) std::printf(" %u", unsigned(value));
                    std::printf("\n");
                }
            }
        }
    }
    NativePopulationCycleObservation observed;
    observed.Zone.Label = "retain";
    const auto retained = observed;
    NativePedGangMaskInput demand;
    demand.Wanted = 777;
    NativePedGangWarObservation war;
    Check(NativeQualifyPedGangDemand(observed, true, false, war, demand) == NativePedStreamStatus::UnknownZone &&
        demand.Wanted == 777 && !demand.DemandKnown, "unknown cycle cannot authorize an absent-zone demand");
    Check(!owner.ObserveCycle({2369, -1263, 23}, 24, false, cycleZones, observed, error) && observed == retained,
        "invalid cycle hour retains prior observation");
    Check(!owner.ObserveCycle({0, 0, 9999}, 12, false, cycleZones, observed, error) && observed == retained,
        "unqualified player is not a verified absent zone");
    Check(!owner.ObserveCycle({2369, -1263, 23}, 12, false, altered, observed, error) && observed == retained,
        "changed zone identities reject without publishing a cycle");
    cycleZone->PopulationType = 20;
    Check(!owner.ObserveCycle({2369, -1263, 23}, 12, false, cycleZones, observed, error) && observed == retained,
        "population type outside the actual 480 rows cannot get a default row");
    NativePedStreamInput qualified;
    qualified.RaceMask = 77;
    Check(NativeQualifyPedCycleSelection(metadata, 0, observed, qualified) == NativePedStreamStatus::UnknownZone &&
        qualified.RaceMask == 77, "unknown cycle cannot authorize ped selection");
    observed.Known = true;
    observed.Zone.PopulationType = 1;
    observed.RowIndex = 0;
    Check(NativeQualifyPedCycleSelection(metadata, 0, observed, qualified) == NativePedStreamStatus::InvalidInput &&
        qualified.RaceMask == 77, "mismatched cycle/zone identity cannot publish a selection input");
    observed.RowIndex = 24;
    Check(NativeQualifyPedCycleSelection(metadata, 3, observed, qualified) == NativePedStreamStatus::InvalidInput &&
        qualified.RaceMask == 77, "cycle population type cannot invent current world-region authority");
    NativePedModelMetadata unavailable;
    Check(NativeQualifyPedCycleSelection(unavailable, 0, observed, qualified) == NativePedStreamStatus::UnknownModel &&
        qualified.RaceMask == 77, "actual cycle cannot supply missing ped model authority");
    cycleZone->PopulationType = 19;
    Check(owner.ObserveCycle({2369, -1263, 23}, 12, false, cycleZones, observed, error) &&
        NativeQualifyPedCycleSelection(metadata, 0, observed, qualified) == NativePedStreamStatus::QualifiedCycleInput,
        "qualified actual cycle can be prepared without a requested-slot owner");
    Check(NativeQualifyPedGangDemand(observed, false, false, war, demand) == NativePedStreamStatus::UnknownCheat &&
        demand.Wanted == 777 && !demand.DemandKnown, "actual zone does not infer inactive cheats");
    cycleZone->GangStrength.fill(0);
    Check(owner.ObserveCycle({2369, -1263, 23}, 12, false, cycleZones, observed, error),
        "live zero strengths observed from the same ordered zone owner");
    war = {true, 2, true, true, {2369, -1263}, {2369, -1263}, true, 9};
    Check(NativeQualifyPedGangDemand(observed, true, false, war, demand) == NativePedStreamStatus::QualifiedGangDemand &&
        demand.Wanted == 512 && demand.DemandKnown && !demand.MemberKnown && !demand.PedGroups[9].Known,
        "actual zone plus explicit near war binds demand without constructing a gang or asset");
    NativePedStreamState selectionState;
    NativePedStreamChoice selectionChoice;
    Check(NativePickPedModelToStream(qualified, streamRng.Reference(), selectionState, selectionChoice) ==
        NativePedStreamStatus::UnknownSlots && selectionChoice.Model == -1,
        "cycle binding is not permission to execute selection using fake empty slots");
    Check(streamRng.Inspect().Value->DrawCount == streamDraws, "all cycle observations and group bindings are RNG-free");
    std::printf("native-loaded-cars-ok checks=%d groups=%zu cycle=%zu zone=%s eligible=%zu selected=%d suppressed=ten-draw-no-model next-stream=%d group=%d draws=%u\n",
        s_Checks, owner.CarGroups(), owner.CycleRows(), prior.Zone.c_str(),
        prior.AppropriateLoadedCars.size(), prior.ModelId, postTaxi.ModelId, postTaxi.Group, postTaxi.Draws);
}
