#include "NativeWorldEntityInfo.h"

#include <cstdio>
#include <cstdlib>

namespace {
int s_Checks = 0;

void Check(bool value, const char* message) {
    ++s_Checks;
    if (!value) {
        std::fprintf(stderr, "ped-namespace-fail: %s\n", message);
        std::exit(1);
    }
}
}

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    NativeWorldEntityInfo metadata;
    NativeCollisionPopulation population;
    std::string error;
    const std::array<NativeWorldEntitySourceText, 1> fixtures{{{"fixture.ide",
        "peds\n7 male01 male01 CIVMALE STAT_SENSIBLE_GUY man 120C 10001 man 1 4 PED_TYPE_GEN VOICE_ONE VOICE_TWO\n"
        "8 incomplete generic\nend\n"}}};
    Check(metadata.LoadSources(population, fixtures, {"object.dat", ""}, error), "fixture load");
    Check(metadata.PedModels().size() == 1 && metadata.PedModels().contains(7), "only complete row is known");
    const auto& fixture = metadata.PedModels().at(7);
    Check(fixture.CarsCanDriveMask == 0x120C && fixture.PedFlags == 1 &&
        fixture.Radio1 == 1 && fixture.Radio2 == 4, "source hexadecimal narrowing and radios");
    Check(fixture.Name == "male01" && fixture.TxdName == "male01" &&
        fixture.PedTypeName == "CIVMALE" && fixture.StatName == "STAT_SENSIBLE_GUY" &&
        fixture.AnimationGroupName == "man" && fixture.AnimationFileName == "man" &&
        fixture.AudioTypeName == "PED_TYPE_GEN" && fixture.VoiceMinName == "VOICE_ONE" &&
        fixture.VoiceMaxName == "VOICE_TWO" && fixture.Ide.Line == 2, "complete source row provenance");

    const std::array<NativeWorldEntitySourceText, 2> replacement{{fixtures[0],
        {"replacement.ide", "cars\n7 vehicle vehicle\nend\n"}}};
    Check(metadata.LoadSources(population, replacement, {"object.dat", ""}, error) &&
        metadata.PedModels().empty(), "later non-ped definition retires old ped row");
    int modelId = 999;
    Check(metadata.FindNamespaceModelId("VEHICLE", modelId) == NativeWorldNameStatus::Found && modelId == 7,
        "lookup covers non-ped models outside the static population");
    const std::array<NativeWorldEntitySourceText, 2> incompleteOverride{{fixtures[0],
        {"override.ide", "peds\n7 replacement generic\nend\n"}}};
    Check(metadata.LoadSources(population, incompleteOverride, {"object.dat", ""}, error) &&
        metadata.PedModels().empty(), "incomplete override cannot retain the old ped properties");
    Check(metadata.FindNamespaceModelId("replacement", modelId) == NativeWorldNameStatus::Found && modelId == 7,
        "incomplete row retains only its source identity");
    const std::array<NativeWorldEntitySourceText, 1> ambiguous{{{"duplicate.ide",
        "cars\n7 duplicate generic\n8 DUPLICATE generic\nend\n"}}};
    Check(metadata.LoadSources(population, ambiguous, {"object.dat", ""}, error), "ambiguous unconsumed namespace load");
    modelId = 999;
    Check(metadata.FindNamespaceModelId("duplicate", modelId) == NativeWorldNameStatus::Ambiguous && modelId == 999,
        "ambiguous lookup preserves output instead of inventing source cursor order");
    Check(metadata.FindNamespaceModelId("missing", modelId) == NativeWorldNameStatus::Missing && modelId == 999,
        "proven absent namespace key is distinct from unknown");
    Check(metadata.FindNamespaceModelId("", modelId) == NativeWorldNameStatus::InvalidName && modelId == 999,
        "invalid namespace lookup retains output");
    Check(metadata.LoadBeforeWorker(argv[1], population, error), error.c_str());
    Check(metadata.PedModels().size() == 276, "complete owned IDE ped census");
    const auto& male = metadata.PedModels().at(7);
    Check(male.Name == "male01" && male.TxdName == "male01" && male.PedTypeName == "CIVMALE" &&
        male.StatName == "STAT_SENSIBLE_GUY" && male.AnimationGroupName == "man" &&
        male.CarsCanDriveMask == 0, "real male01 definition, not a fallback");
    const auto& player = metadata.PedModels().at(0);
    Check(player.Name == "null" && player.PedTypeName == "PLAYER1" &&
        player.AnimationGroupName == "player", "source player declaration is not an ambient alias");
    for (const auto& [id, model] : metadata.PedModels()) {
        Check(id == model.ModelId && !model.Name.empty() && !model.TxdName.empty() &&
            !model.PedTypeName.empty() && !model.StatName.empty() &&
            !model.AnimationGroupName.empty() && !model.Ide.Source.empty() && model.Ide.Line > 0,
            "every ped row retains complete source identity");
    }
    Check(!metadata.LoadBeforeWorker("/nonexistent-ped-namespace", population, error) &&
        metadata.PedModels().size() == 276 && metadata.PedModels().at(7).Name == "male01",
        "failed reload retains complete ped namespace");
    NativeWorldEntitySourceText retained{"retained", "text"};
    size_t bytes = 123;
    Check(!NativeWorldEntitySourceText::ReadBeforeWorker(argv[1], "../outside", bytes, retained, error) &&
        bytes == 123 && retained.Source == "retained" && retained.Text == "text", "asset path rejection is atomic");
    std::printf("native-ped-namespace-ok checks=%d models=276 source=ordered-IDE rows=14 loaded-state=unowned census=incomplete\n", s_Checks);
}
