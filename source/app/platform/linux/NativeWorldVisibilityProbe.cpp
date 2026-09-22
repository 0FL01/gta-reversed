#include "app/platform/linux/NativeWorldVisibility.h"
#include "app/platform/linux/StreamPager.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace {
int g_Checks = 0;
void Check(bool value, const std::string& message) {
    ++g_Checks;
    if (!value) { std::fprintf(stderr, "world-visibility-fail: %s\n", message.c_str()); std::exit(1); }
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
    Check(bool(catalog), error);
    NativeCollisionAssets assets;
    Check(assets.Load(argv[1], population, error), error);
    NativeCatalogResidency roads;
    Check(catalog->SelectResidency(1532.054688f, -1662.289063f, 190.0f, 0, roads, error), error);
    WorldShotScene scene;
    E2EPagerFrame frame;
    char selectedError[512]{};
    std::vector<NativePlacementIdentity> rendered;
    Check(StreamPager_UpdateSelected(roads.Visible, roads.HiddenTargets, scene, frame,
        selectedError, sizeof(selectedError), &rendered), selectedError);
    std::vector<float> bounds;
    bounds.reserve(scene.meshes.size());
    for (const auto& mesh : scene.meshes) {
        NativeCollisionVector minimum{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
        NativeCollisionVector maximum{-minimum[0], -minimum[1], -minimum[2]};
        for (std::size_t i = 0; i + 2 < mesh.pos.size(); i += 3) {
            for (int axis = 0; axis < 3; ++axis) {
                minimum[axis] = std::min(minimum[axis], mesh.pos[i + std::size_t(axis)]);
                maximum[axis] = std::max(maximum[axis], mesh.pos[i + std::size_t(axis)]);
            }
        }
        const float x = (maximum[0] - minimum[0]) * 0.5f;
        const float y = (maximum[1] - minimum[1]) * 0.5f;
        const float z = (maximum[2] - minimum[2]) * 0.5f;
        bounds.push_back(std::sqrt(x * x + y * y + z * z));
    }
    Check(rendered.size() == bounds.size() && rendered.size() == 326, "selected model-bound identities");
    NativeWorldVisibilitySnapshot noon, night;
    Check(NativeWorldVisibility::Evaluate(*catalog, assets, roads, bounds,
        {{1532.054688f, -1662.289063f, 12.460938f}, 0, 12, 1.0f, 1.0f, 300.0f}, noon, error), error);
    Check(NativeWorldVisibility::Evaluate(*catalog, assets, roads, bounds,
        {{1532.054688f, -1662.289063f, 12.460938f}, 0, 23, 1.0f, 1.0f, 300.0f}, night, error), error);
    Check(noon.CompleteSelection && night.CompleteSelection && noon.Decisions.size() == 326 &&
        night.Decisions.size() == 326 && !noon.FrustumAuthority && !noon.OcclusionAuthority,
        "complete source-reason coverage");
    Check(noon.TimeRejected != night.TimeRejected || noon.Present != night.Present,
        "authored time route changes visibility");
    bool timeReason = false, lodReason = false;
    for (const auto& decision : noon.Decisions) {
        timeReason |= decision.Reason == "time-out-of-range";
        lodReason |= decision.Reason == "lod-child-visible" || decision.Reason == "lod-parent-fallback";
    }
    for (const auto& decision : night.Decisions) timeReason |= decision.Reason == "time-out-of-range";
    Check(timeReason && lodReason, "time and LOD reasons present");
    const auto before = noon;
    Check(!NativeWorldVisibility::Evaluate(*catalog, assets, roads, bounds,
        {{0, 0, 0}, 256, 12, 1.0f, 1.0f, 300.0f}, noon, error) && noon == before,
        "invalid area rejection is atomic");
    StreamPager_Shutdown();
    std::printf("native-world-visibility-ok checks=%d residents=326 noon=%zu night=%zu time-reasons=1 lod-reasons=1 frustum=external occlusion=external\n",
        g_Checks, before.Present, night.Present);
}
