#include "NativePedModelMetadata.h"
#include "NativePedStreaming.h"

#include <bit>
#include <cstdio>
#include <cstdlib>

namespace {
int s_Checks = 0;
void Check(bool value, const char* message) {
    ++s_Checks;
    if (!value) {
        std::fprintf(stderr, "ped-metadata-fail: %s\n", message);
        std::exit(1);
    }
}
NativeWorldEntitySourceText Stats() {
    NativeWorldEntitySourceText result{"fixture.stats", "# source stat order\n"};
    for (int i = 0; i < 43; ++i) {
        const auto name = i == 4 || i == 9 ? "MATCH" : "STAT_" + std::to_string(i);
        result.Text += name + " 20 15 257 -1 258 -2 1.25 0.75 65537 255\n";
    }
    return result;
}
NativeWorldEntitySourceText Groups() {
    NativeWorldEntitySourceText result{"fixture.groups", "# source group order\nmissing_model\n"};
    for (int i = 0; i < 57; ++i) {
        if (i == 0) {
            for (int slot = 0; slot < 21; ++slot) result.Text += "male01, ";
            result.Text += "null\n"; // Source stops at21 before consuming the player key.
        } else result.Text += "missing_model male01 Hxexample # trailing comment\n";
    }
    return result;
}
}

int main(int argc, char** argv) {
    if (argc < 2 || argc > 3 || (argc == 3 && std::string_view(argv[2]) != "--rows")) return 2;
    const bool rows = argc == 3;
    std::string error;
    NativeCollisionPopulation population;
    NativeWorldEntityInfo names;
    NativePedModelMetadata metadata;
    NativeCivilianLoadedPed slot;
    slot.Model = 1234;
    Check(metadata.QualifyCivilianSlot(7, true, true, 0, slot) == NativePedMetadataStatus::NotLoaded &&
        slot.Model == 1234, "unloaded namespace cannot qualify streaming");
    const std::array<NativeWorldEntitySourceText, 1> ide{{{"fixture.ide", "peds\n"
        "0 null null PLAYER1 MISSING PLAYER 0 0 null 0 0 PED_TYPE_GEN V1 V2\n"
        "7 male01 male01 CIVMALE MATCH PLAYER 120C 10001 man 1 4 PED_TYPE_GEN V1 V2\n"
        "8 Hxexample generic CIVFEMALE missing custom 0 0 woman 0 0 PED_TYPE_GEN V1 V2\nend\n"}}};
    Check(names.LoadSources(population, ide, {"object.dat", ""}, error), error.c_str());
    const auto stats = Stats(), groups = Groups();
    const NativeWorldEntitySourceText animations{"fixture.animations",
        "custom ped man 1\nWALK_civi\nend\nPLAYER ped man 0\nend\n"};
    Check(metadata.LoadSources(names, stats, animations, groups, error), error.c_str());
    Check(metadata.Find(7)->PedType == 4 && metadata.Find(7)->StatsType == 4 &&
        metadata.Find(7)->AnimationGroup == 54 && metadata.Find(7)->Race == 0 &&
        !metadata.Find(7)->StatsUsedSourceFallback, "type/stat first match and case-insensitive builtin animation precedence");
    Check(metadata.Find(8)->PedType == 5 && metadata.Find(8)->StatsType == 16 &&
        metadata.Find(8)->StatsUsedSourceFallback && metadata.Find(8)->AnimationGroup == 118 &&
        metadata.Find(8)->Race == 4, "exact source stat fallback and Hispanic race4");
    const auto& stat = metadata.Stats()[4];
    Check(stat.Fear == 1 && stat.Temper == 255 && stat.Lawfulness == 2 && stat.Sexiness == 254 &&
        stat.Flags == 1 && stat.DefaultDecisionMaker == -1, "source byte/word stat narrowing");
    Check(metadata.Groups()[0].Count == 21 && metadata.Groups()[0].Models.back() == 7 &&
        metadata.Groups()[1].Count == 2 && metadata.Groups()[1].Models[1] == 8 &&
        metadata.Groups()[1].Models[2] == 2000 && metadata.Groups()[0].Source.Line == 3,
        "source group cap, skipped unmatched line and unused sentinel");
    Check(!metadata.LoadSources(names, {"invalid.stats", "MATCH 20\n"}, animations, groups, error) &&
        metadata.Find(7)->StatsType == 4, "malformed stat reload retains prior snapshot");
    for (const auto invalid : {"nan", "inf", "1e999"}) {
        auto nonfinite = stats;
        nonfinite.Text.replace(nonfinite.Text.find("20 15"), 2, invalid);
        Check(!metadata.LoadSources(names, nonfinite, animations, groups, error) &&
            metadata.Find(7)->StatsType == 4, "nonfinite/out-of-range source stat cannot publish");
    }
    auto unknownType = ide;
    unknownType[0].Text.replace(unknownType[0].Text.find("CIVMALE"), 7, "civmale");
    Check(names.LoadSources(population, unknownType, {"object.dat", ""}, error) &&
        !metadata.LoadSources(names, stats, animations, groups, error) &&
        metadata.Find(7)->PedType == 4, "case-sensitive unknown source type has no guessed enum default");
    auto unknownAnimation = ide;
    unknownAnimation[0].Text.replace(unknownAnimation[0].Text.find("MATCH PLAYER"), 12, "MATCH missing_anim");
    Check(names.LoadSources(population, unknownAnimation, {"object.dat", ""}, error) &&
        !metadata.LoadSources(names, stats, animations, groups, error) &&
        metadata.Find(7)->AnimationGroup == 54, "unknown source animation group cannot become a default group");
    Check(names.LoadSources(population, ide, {"object.dat", ""}, error), "restore valid source namespace");
    Check(!metadata.LoadSources(names, stats, {"invalid.anim", "custom ped man 1\nWALK_civi\n"}, groups, error) &&
        metadata.AnimationGroups().size() == 120, "unterminated animation reload is atomic");
    Check(!metadata.LoadSources(names, stats, animations, {"invalid.groups", "null\n"}, error) &&
        metadata.Groups()[0].Count == 21, "player group key cannot become an ambient model");
    Check(metadata.QualifyCivilianSlot(7, false, true, 0, slot) == NativePedMetadataStatus::UnknownStreaming &&
        slot.Model == 1234, "IDE row does not imply a streaming observation");
    Check(metadata.QualifyCivilianSlot(999, true, true, 0, slot) == NativePedMetadataStatus::UnknownModel &&
        slot.Model == 1234, "missing model cannot become a male01 fallback");
    Check(metadata.QualifyCivilianSlot(-1, true, true, 0, slot) == NativePedMetadataStatus::InvalidInput &&
        slot.Model == 1234, "empty slot cannot be declared loaded");
    Check(metadata.QualifyCivilianSlot(7, true, false, 2, slot) == NativePedMetadataStatus::Ready &&
        !slot.Loaded && slot.References == 2 && slot.ModelInfoKnown && slot.CarsCanDrive == 0x120C,
        "qualification retains caller-owned unloaded state and references");

    Check(names.LoadBeforeWorker(argv[1], population, error), error.c_str());
    Check(metadata.LoadBeforeWorker(argv[1], names, error), error.c_str());
    Check(metadata.Models().size() == 276 && metadata.AnimationGroups().size() > 118,
        "actual whole ped namespace and appended animation groups");
    std::array<NativePedStreamGroup, 18> streamingGroups;
    Check(NativeQualifyPedStreamingGroups(metadata, 3, streamingGroups) == NativePedStreamStatus::InvalidInput,
        "world zone cannot be guessed outside the source three regions");
    for (std::uint32_t worldZone = 0; worldZone < 3; ++worldZone) {
        Check(NativeQualifyPedStreamingGroups(metadata, worldZone, streamingGroups) ==
            NativePedStreamStatus::QualifiedGroups, "real group identities qualify for each source world zone");
        for (std::size_t group = 0; group < streamingGroups.size(); ++group) {
            const auto& source = metadata.Groups()[NativePedGroupTranslation[group][worldZone]];
            const auto& bound = streamingGroups[group];
            Check(bound.Known && bound.Count == source.Count, "actual translated group count");
            for (std::size_t slot = 0; slot < source.Count; ++slot)
                Check(bound.Models[slot].Model == source.Models[slot] && bound.Models[slot].RaceKnown &&
                    bound.Models[slot].Race == metadata.Find(source.Models[slot])->Race,
                    "actual source ordered model and race bound without loaded assumption");
        }
        NativePedStreamInput input;
        input.ZoneKnown = input.SlotsKnown = true;
        input.RaceMask = 15;
        input.Groups = streamingGroups;
        input.Percentages.fill(6); // Explicit clock/zone fixture, not actual runtime percentages.
        NativeSourceRng rng;
        Check(rng.SeedOnce(worldZone) == NativeSourceRngStatus::Ready, "one source RNG for actual-data adapter fixture");
        NativePedStreamState state;
        NativePedStreamChoice choice;
        Check(NativePickPedModelToStream(input, rng.Reference(), state, choice) == NativePedStreamStatus::Selected &&
            metadata.Find(choice.Model) && choice.Draws == rng.Inspect().Value->DrawCount,
            "original picker consumes real model groups and explicit requested-slot observations");
    }
    NativePedMetadataPolicies policies(metadata, NativePedZonePolicy{true, false, 15});
    NativePedMetadataPolicies unknownZone(metadata, std::nullopt);
    bool accepted = true;
    Check(!unknownZone.ZoneAccepts(7, accepted) && accepted, "unavailable zone policy retains the decision");
    Check(policies.ZoneAccepts(7, accepted) && accepted, "actual male01 metadata feeds the source zone helper");
    Check(policies.AttractorAccepts(280, "COPSIT", accepted) && accepted, "actual cop type feeds the source attractor helper");
    accepted = true;
    Check(!policies.AttractorAccepts(999, "COPSIT", accepted) && accepted,
        "unknown model policy stays unavailable rather than rejected");
    NativeCivilianOccupationInput occupation;
    Check(metadata.QualifyCivilianSlot(7, true, true, 0, occupation.LoadedPeds[7]) == NativePedMetadataStatus::Ready,
        "explicit loaded-slot observation binds actual source fields");
    occupation.MustBeMale = true;
    occupation.OnlyOnFoot = true;
    int chosen = -1;
    Check(NativeChooseCivilianOccupation(occupation, policies, chosen) == NativeCivilianOccupationStatus::NoOccupation && chosen == -1,
        "source on-foot bit1000 rejects the actual male01 zero mask");
    occupation.OnlyOnFoot = false;
    Check(NativeChooseCivilianOccupation(occupation, policies, chosen) == NativeCivilianOccupationStatus::Chosen && chosen == 7,
        "real metadata/policies feed the verified eight-slot source traversal");
    for (const auto& [id, model] : metadata.Models()) {
        Check(id == model.Source.ModelId && model.PedType >= 0 && model.PedType < 32 &&
            model.StatsType >= 0 && model.StatsType < 43 && model.AnimationGroup >= 0 &&
            std::size_t(model.AnimationGroup) < metadata.AnimationGroups().size() && model.Race >= 0 && model.Race <= 4,
            "every actual model retains qualified source identities");
        if (rows) {
            const auto& s = model.Source;
            std::printf("PED\t%d\t%s\t%s\t%d\t%d\t%d\t%d\t%d\t%u\t%u\t%s\t%d\t%d\t%s\t%s\t%s\t%s\t%u\n",
                id, s.Name.c_str(), s.TxdName.c_str(), model.PedType, model.StatsType, model.AnimationGroup, model.Race,
                model.StatsUsedSourceFallback, s.CarsCanDriveMask, s.PedFlags, s.AnimationFileName.c_str(), s.Radio1, s.Radio2,
                s.AudioTypeName.c_str(), s.VoiceMinName.c_str(), s.VoiceMaxName.c_str(), s.Ide.Source.c_str(), s.Ide.Line);
        }
    }
    if (rows) {
        for (std::size_t i = 0; i < metadata.Stats().size(); ++i) {
            const auto& s = metadata.Stats()[i];
            std::printf("STAT\t%zu\t%s\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%d\t%u\n", i, s.Name.c_str(),
                std::bit_cast<std::uint32_t>(s.FleeDistance), std::bit_cast<std::uint32_t>(s.HeadingChangeRate),
                s.Fear, s.Temper, s.Lawfulness, s.Sexiness, std::bit_cast<std::uint32_t>(s.AttackStrength),
                std::bit_cast<std::uint32_t>(s.DefendWeakness), s.Flags, s.DefaultDecisionMaker, s.Source.Line);
        }
        for (std::size_t i = 0; i < metadata.AnimationGroups().size(); ++i)
            std::printf("ANIM\t%zu\t%s\n", i, metadata.AnimationGroups()[i].c_str());
        for (std::size_t i = 0; i < metadata.Groups().size(); ++i) {
            const auto& g = metadata.Groups()[i];
            std::printf("GROUP\t%zu\t%u\t%u", i, g.Count, g.Source.Line);
            for (const auto model : g.Models) std::printf("\t%d", model);
            std::printf("\n");
        }
    }
    Check(!metadata.LoadBeforeWorker("/nonexistent-ped-metadata", names, error) && metadata.Models().size() == 276,
        "failed disk reload retains the complete resolved snapshot");
    std::printf("native-ped-metadata-ok checks=%d models=%zu stats=43 animations=%zu groups=57 loaded-state=caller-owned census=incomplete\n",
        s_Checks, metadata.Models().size(), metadata.AnimationGroups().size());
}
