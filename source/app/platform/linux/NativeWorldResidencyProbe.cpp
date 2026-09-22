#include "app/platform/linux/NativeWorldResidency.h"
#include "app/platform/linux/StreamPager.h"

#include <cstdio>
#include <cstdlib>

namespace {
int g_Checks = 0;
void Check(bool value, const std::string& message) {
    ++g_Checks;
    if (!value) { std::fprintf(stderr, "world-residency-fail: %s\n", message.c_str()); std::exit(1); }
}
}

int main(int argc, char** argv) {
    Check(argc == 2, "usage: probe GAME_DIR");
    char pagerError[512]{};
    E2ELoadInfo info;
    Check(StreamPager_Init(argv[1], info, pagerError, sizeof(pagerError), {true, 300.0f, 80}), pagerError);
    NativeCollisionPopulation population;
    std::string error;
    Check(StreamPager_CollisionPopulation(population, error), error);
    auto catalog = NativeLodCatalog::LoadBeforeWorker(argv[1], population, error);
    Check(bool(catalog) && catalog->DiskValidated(), error);
    NativeCollisionAssets assets;
    Check(assets.Load(argv[1], population, error), error);
    NativePathResidencyCatalog paths;
    Check(paths.LoadBeforeWorker(argv[1], error) && paths.Areas().size() == 64, error);

    NativeCatalogResidency grove;
    Check(catalog->SelectResidency(2495.0f, -1685.0f, 140.0f, 0, grove, error), error);
    std::vector<NativePlacementIdentity> rendered = grove.Visible;
    rendered.insert(rendered.end(), grove.HiddenTargets.begin(), grove.HiddenTargets.end());
    NativeWorldResidencyCandidate first;
    Check(NativeWorldResidency::Prepare(*catalog, assets, population, paths, 1,
        2495.0f, -1685.0f, 140.0f, 0, rendered, first, error), error);
    Check(first.Paths.size() >= 1 && first.Collision && !first.Collision->Instances.empty(),
        "paired path and collision residency");

    NativeWorldResidency owner;
    NativeDynamicWorldRef dynamic;
    Check(owner.SpawnDynamic(400, {2495.0f, -1685.0f, 12.0f}, 0, dynamic, error), error);
    Check(owner.Adopt(first, error), error);
    const auto held = owner.LastCommitted();
    Check(held && held->Rendered == rendered && held->Dynamic.size() == 1 &&
        held->Dynamic[0].Reference == dynamic && held->PairedRenderCollision &&
        held->CompletePlacementIdentity && !held->PathSearchAuthority, "first immutable publication");

    NativeCatalogResidency interior;
    Check(catalog->SelectResidency(0.0f, 0.0f, 140.0f, 16, interior, error), error);
    std::vector<NativePlacementIdentity> interiorRendered = interior.Visible;
    interiorRendered.insert(interiorRendered.end(), interior.HiddenTargets.begin(), interior.HiddenTargets.end());
    NativeWorldResidencyCandidate second;
    Check(NativeWorldResidency::Prepare(*catalog, assets, population, paths, 2,
        0.0f, 0.0f, 140.0f, 16, interiorRendered, second, error), error);
    Check(owner.MoveDynamic(dynamic, {0.0f, 0.0f, 5.0f}, 16, error) && owner.Adopt(second, error), error);
    Check(owner.LastCommitted()->Generation == 2 && owner.LastCommitted()->Area == 16 &&
        owner.LastCommitted()->Dynamic.size() == 1 && owner.LastCommitted()->Dynamic[0].Reference == dynamic &&
        held->Generation == 1 && held->Area == 0, "area itinerary identity and immutable prior generation");

    const auto before = owner.LastCommitted();
    auto bad = second;
    bad.Generation = 3;
    bad.Rendered.pop_back();
    Check(!owner.Adopt(bad, error) && owner.LastCommitted() == before, "mismatched render identity rejection");
    Check(!owner.Adopt(second, error) && owner.LastCommitted() == before, "stale generation rejection");
    Check(owner.RemoveDynamic(dynamic, error) && !owner.Resolve(dynamic), error);
    StreamPager_Shutdown();
    std::printf("native-world-residency-ok checks=%d exterior=%zu interior=%zu paths=64 dynamic=stable paired-generation=1 search-authority=0\n",
        g_Checks, rendered.size(), interiorRendered.size());
}
