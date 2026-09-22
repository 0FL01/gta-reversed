#include "NativeVehicleFamilyLifecycle.h"

#include <array>
#include <cstdio>
#include <cstdlib>

namespace {
int g_Checks = 0;
void Check(bool value, const std::string& message) {
    ++g_Checks;
    if (!value) { std::fprintf(stderr, "vehicle-family-lifecycle-fail: %s\n", message.c_str()); std::exit(1); }
}

std::shared_ptr<const NativeCollisionModel> Collision(const NativeCarGeneratorModelDefinition& definition) {
    auto collision = std::make_shared<NativeCollisionModel>();
    collision->Name = definition.ModelName;
    collision->ValidatedHeaderId = true;
    collision->HeaderId = static_cast<std::uint16_t>(definition.ModelId);
    collision->BoundRadius = 1.0f;
    collision->Spheres.push_back({{}, 1.0f, {}});
    return collision;
}
}

int main(int argc, char** argv) {
    Check(argc == 2, "usage: probe GAME_DIR");
    NativeCarGenerators definitions;
    std::string error;
    Check(definitions.LoadBeforeWorker(argv[1], 0, error), error);
    std::array<std::optional<NativeCarGeneratorModelDefinition>, 12> representatives;
    for (const auto& definition : definitions.ModelDefinitions()) {
        const auto family = static_cast<std::size_t>(definition.Type);
        if (family < representatives.size() && !representatives[family]) representatives[family] = definition;
    }
    representatives[static_cast<std::size_t>(NativeVehicleType::FakeHelicopter)] = definitions.ModelDefinitions().front();
    representatives[static_cast<std::size_t>(NativeVehicleType::FakeHelicopter)]->Type = NativeVehicleType::FakeHelicopter;
    representatives[static_cast<std::size_t>(NativeVehicleType::FakePlane)] = definitions.ModelDefinitions().front();
    representatives[static_cast<std::size_t>(NativeVehicleType::FakePlane)]->Type = NativeVehicleType::FakePlane;

    NativeVehicleFamilyLifecycle lifecycle;
    std::array<NativeVehicleRef, 12> references;
    for (std::size_t family = 0; family < representatives.size(); ++family) {
        Check(representatives[family].has_value(), "family representative");
        const auto& definition = *representatives[family];
        Check(lifecycle.Spawn(definition, Collision(definition), 3, references[family], error) ==
            NativeVehicleFamilyLifecycleStatus::Ok, error);
        const auto* record = lifecycle.Resolve(references[family]);
        Check(record && record->ModelId == definition.ModelId && record->ModelName == definition.ModelName &&
            record->Family == definition.Type && record->Collision && record->Health == 1000.0f,
            "spawn exact family/collision identity");
        Check(lifecycle.SetDriver(references[family], 1000 + family, error) ==
            NativeVehicleFamilyLifecycleStatus::Ok, error);
        Check(lifecycle.AddPassenger(references[family], 2000 + family, 1, error) ==
            NativeVehicleFamilyLifecycleStatus::Ok, error);
        Check(lifecycle.ApplyResolvedCollision(references[family], 2, 125.0f, error) ==
            NativeVehicleFamilyLifecycleStatus::Ok && lifecycle.Resolve(references[family])->Health == 875.0f,
            "resolved collision damage state");
    }
    const auto beforeDestroy = lifecycle.Publish();
    for (std::size_t family = 0; family < references.size(); family += 2) {
        Check(lifecycle.Destroy(references[family], error) == NativeVehicleFamilyLifecycleStatus::Ok, error);
        Check(!lifecycle.Resolve(references[family]), "destroy clears live reference");
    }
    Check(lifecycle.Pool().Census().Alive == 6, "half matrix remains before reload");
    const auto retainedRevision = lifecycle.Revision();
    Check(lifecycle.Reload(1, error) == NativeVehicleFamilyLifecycleStatus::InvalidInput &&
        lifecycle.Revision() == retainedRevision, "stale reload rejection atomic");
    Check(lifecycle.Reload(2, error) == NativeVehicleFamilyLifecycleStatus::Ok &&
        lifecycle.Pool().Census().Alive == 0 && lifecycle.Epoch() == 2, "reload cleans all live owners");
    Check(beforeDestroy->Records.size() == 12 && beforeDestroy->Records.front().Collision &&
        beforeDestroy->Records.front().Driver != 0, "held immutable lifecycle snapshot");
    Check(!NativeVehicleFamilyLifecycle::ResolvesFamilyDamageFormula,
        "damage formula remains source response authority");

    std::printf("native-vehicle-family-lifecycle-ok checks=%d families=12 collision=owned occupants=clean damage=resolved destruction=all reload=epoch2 formula-authority=external\n",
        g_Checks);
}
