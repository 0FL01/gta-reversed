#include "NativePathGraph.h"

#include <cstdio>
#include <cstdlib>

namespace {
int g_Checks = 0;
void Check(bool value, const std::string& message) {
    ++g_Checks;
    if (!value) { std::fprintf(stderr, "path-graph-fail: %s\n", message.c_str()); std::exit(1); }
}
}

int main(int argc, char** argv) {
    Check(argc == 2, "usage: probe GAME_DIR");
    std::string error;
    NativePathGraph graph;
    Check(graph.LoadBeforeWorker(argv[1], error), error);
    std::vector<NativePathAreaResidency> selected;
    for (const std::uint8_t area : {std::uint8_t(14), std::uint8_t(15), std::uint8_t(22), std::uint8_t(23)}) {
        Check(graph.Metadata(area) != nullptr, "path area metadata");
        selected.push_back(*graph.Metadata(area));
    }
    Check(graph.Adopt(1, selected, error) == NativePathGraphStatus::Ok, error);
    Check(selected.size() > 1 && graph.Generation() == 1, "source path residency adoption");

    constexpr NativePathAddress start{14, 0}, end{14, 1};
    constexpr std::array<NativePathAddress, 17> expected{{
        {14, 0}, {14, 305}, {14, 306}, {14, 5}, {14, 4}, {14, 3}, {14, 2}, {14, 92},
        {14, 102}, {14, 101}, {14, 93}, {14, 95}, {14, 96}, {14, 97}, {14, 98}, {14, 94}, {14, 1},
    }};
    NativePathRoute route;
    Check(graph.Search(start, end, true, route, error) == NativePathGraphStatus::Ok, error);
    Check(route.Generation == 1 && route.Start == start && route.End == end &&
        route.Nodes == std::vector<NativePathAddress>(expected.begin(), expected.end()) && route.Distance == 313,
        "known source graph route");
    const auto retained = route;
    Check(graph.Adopt(1, selected, error) == NativePathGraphStatus::StaleGeneration && route.Nodes == retained.Nodes,
        "stale adoption retains route value");
    auto changed = selected;
    ++changed.front().Fingerprint;
    Check(graph.Adopt(2, changed, error) == NativePathGraphStatus::InvalidInput && graph.Generation() == 1,
        "changed path payload rejection");
    Check(graph.Adopt(2, selected, error) == NativePathGraphStatus::Ok && graph.Generation() == 2,
        "path residency generation transition");
    NativePathRoute repeated;
    Check(graph.Search(start, end, graph.Resolve(start)->Vehicle, repeated, error) == NativePathGraphStatus::Ok &&
        repeated.Nodes == route.Nodes && repeated.Distance == route.Distance && repeated.Generation == 2,
        "deterministic route survives residency generation");
    Check(NativePathGraph::DeterministicOwnership, "owned search authority");

    std::printf("native-path-graph-ok checks=%d areas=%zu route=%u:%u-%u:%u nodes=%zu distance=%u generation=2 deterministic=1\n",
        g_Checks, selected.size(), start.Area, start.Node, end.Area, end.Node,
        route.Nodes.size(), route.Distance);
}
