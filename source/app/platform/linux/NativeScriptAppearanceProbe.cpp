#include "app/platform/linux/NativePlayerAssets.h"
#include "app/platform/linux/RealtimeGameplay.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace {
unsigned s_Checks = 0;
void Check(bool condition, const char* description) {
    ++s_Checks;
    if (!condition) {
        std::fprintf(stderr, "source-appearance-fail %s\n", description);
        std::exit(1);
    }
}
}

int main(int argc, char** argv) {
    Check(argc == 2, "game directory required");
    WorldShotScene ground;
    WorldShotMesh floor;
    floor.tris = 2;
    floor.pos = {-100, -100, 0, 100, -100, 0, 100, 100, 0,
                 -100, -100, 0, 100, 100, 0, -100, 100, 0};
    ground.meshes.push_back(std::move(floor));
    RealtimeGameplayWorld world;
    std::string error;
    Check(world.Rebuild(ground, error), "bounded floor collision fixture");

    RealtimeGameplay gameplay;
    const auto sourceClothes = NativePlayerClothes_Startup();
    Check(gameplay.InitializeScriptPlayerAppearance(argv[1], sourceClothes, error) &&
        gameplay.ScriptAppearancePrepared(), "exclusive parser prepares source initial CJ");
    Check(gameplay.SpawnScriptPlayer(world, {0, 0, 0.5f}, error) &&
        !gameplay.ScriptAppearanceVisible() && gameplay.Actors().stats.triangles == 0,
        "SCM player exists but unbuilt source outfit is hidden");
    const auto state = gameplay.State();
    const auto samePlayerState = [&]() {
        const auto& current = gameplay.State();
        return current.Ped.X == state.Ped.X && current.Ped.Y == state.Ped.Y &&
            current.Ped.Z == state.Ped.Z && current.PedRoot.X == state.PedRoot.X &&
            current.PedRoot.Y == state.PedRoot.Y && current.PedRoot.Z == state.PedRoot.Z &&
            current.PedHeading == state.PedHeading && current.Ready == state.Ready &&
            current.MissionCreated == state.MissionCreated && current.Ticks == state.Ticks;
    };
    Check(gameplay.RevealScriptPlayerAppearance(error) && gameplay.ScriptAppearanceVisible() &&
        gameplay.Actors().stats.triangles == 2332 && gameplay.Actors().images.size() == 15 &&
        samePlayerState(), "070D publishes five authored meshes without changing gameplay authority");
    Check(!gameplay.RevealScriptPlayerAppearance(error) && samePlayerState(),
        "second wardrobe reveal is not a duplicate source transition");
    const auto old = gameplay.Actors().meshes[0].pos;
    gameplay.Tick(1.0 / 30, {}, world);
    Check(gameplay.ScriptAppearanceVisible() && gameplay.Actors().stats.triangles == 2332 &&
        gameplay.Actors().meshes[0].pos != old && std::isfinite(gameplay.State().PedRoot.Z),
        "subsequent source IFP pose follows current player state");
    const auto standing = gameplay.State();
    Check(!gameplay.AdoptScriptPlayerPlacement(world, {NAN, 2, 1}, 0.2f, true, error) &&
        gameplay.State().PedRoot.X == standing.PedRoot.X &&
        gameplay.Actors().stats.triangles == 2332,
        "invalid script placement retains the complete prior pose");
    Check(gameplay.AdoptScriptPlayerPlacement(world, {5, 2, 1}, 1.0f, true, error) &&
        gameplay.Actors().stats.triangles == 0 && gameplay.State().PedRoot.X == 5 &&
        gameplay.State().PedRoot.Y == 2 && !gameplay.State().PlayerOnFootTask,
        "script passenger is hidden at the source-owned vehicle transform");
    Check(gameplay.AdoptScriptPlayerPlacement(world, {6, 3, 1}, 1.25f, false, error) &&
        gameplay.Actors().stats.triangles == 2332 && gameplay.State().PedRoot.X == 6 &&
        gameplay.State().PedRoot.Y == 3 && gameplay.State().PedHeading == 1.25f &&
        gameplay.State().PlayerOnFootTask,
        "source task exit restores CJ at the latest scripted position and heading");
    std::printf("native-script-appearance-ok checks=%u outfit=087B/070D tris=2332 textures=15 feedback=0\n",
        s_Checks);
}
