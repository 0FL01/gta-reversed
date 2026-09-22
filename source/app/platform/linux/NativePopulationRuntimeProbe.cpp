#include "NativePopulationRuntime.h"

#include <cstdio>
#include <cstdlib>

namespace {
int g_Checks = 0;
void Check(bool value, const std::string& message) {
    ++g_Checks;
    if (!value) { std::fprintf(stderr, "population-runtime-fail: %s\n", message.c_str()); std::exit(1); }
}
NativePathRoute PedRoute(const NativePathGraph& graph, std::string& error) {
    const auto nodes = graph.Nodes(14);
    NativePathRoute route;
    for (std::size_t first = 0; first < nodes.size(); ++first) {
        if (nodes[first].Vehicle || nodes[first].SwitchedOff) continue;
        for (std::size_t last = first + 1; last < std::min(nodes.size(), first + 64); ++last) {
            if (nodes[last].Vehicle || nodes[last].SwitchedOff) continue;
            if (graph.Search(nodes[first].Address, nodes[last].Address, false, route, error) == NativePathGraphStatus::Ok)
                return route;
        }
    }
    return {};
}
}

int main(int argc, char** argv) {
    Check(argc == 2, "usage: probe GAME_DIR");
    std::string error;
    NativePathGraph graph;
    Check(graph.LoadBeforeWorker(argv[1], error), error);
    std::vector<NativePathAreaResidency> areas;
    for (const std::uint8_t area : {std::uint8_t(14), std::uint8_t(15), std::uint8_t(22), std::uint8_t(23)})
        areas.push_back(*graph.Metadata(area));
    Check(graph.Adopt(1, areas, error) == NativePathGraphStatus::Ok, error);
    NativePathRoute vehicleRoute;
    Check(graph.Search({14, 0}, {14, 1}, true, vehicleRoute, error) == NativePathGraphStatus::Ok,
        "known traffic route");
    const auto pedRoute = PedRoute(graph, error);
    Check(pedRoute.Nodes.size() > 1 && pedRoute.Generation == 1, "source ped route");

    NativePopulationRuntime population;
    NativePopulationRef firstVehicle, firstPed;
    Check(population.Spawn(NativePopulationKind::Vehicle, 400, vehicleRoute, 12.0f, true,
        firstVehicle, error) == NativePopulationStatus::Ok, error);
    Check(population.Spawn(NativePopulationKind::Ped, 7, pedRoute, 2.0f, true,
        firstPed, error) == NativePopulationStatus::Ok, error);
    Check(population.Tick(1.0f, graph, error) == NativePopulationStatus::Ok &&
        population.Resolve(firstVehicle)->Moving && population.Resolve(firstPed)->Moving,
        "traffic and ped move on source routes");
    const auto vehiclePosition = population.Resolve(firstVehicle)->Position;
    const auto pedPosition = population.Resolve(firstPed)->Position;
    Check(population.Tick(1.0f, graph, error) == NativePopulationStatus::Ok &&
        population.Resolve(firstVehicle)->Position != vehiclePosition &&
        population.Resolve(firstPed)->Position != pedPosition, "moving population advances");

    for (std::size_t i = 1; i < NativePopulationRuntime::VehicleCapacity - 7; ++i) {
        NativePopulationRef ignored;
        Check(population.Spawn(NativePopulationKind::Vehicle, 400, vehicleRoute, 1.0f, true,
            ignored, error) == NativePopulationStatus::Ok, error);
    }
    for (std::size_t i = 1; i < NativePopulationRuntime::PedCapacity - 7; ++i) {
        NativePopulationRef ignored;
        Check(population.Spawn(NativePopulationKind::Ped, 7, pedRoute, 1.0f, true,
            ignored, error) == NativePopulationStatus::Ok, error);
    }
    Check(population.Alive(NativePopulationKind::Vehicle) == 103 &&
        population.Alive(NativePopulationKind::Ped) == 133, "forced source free-space threshold");
    Check(population.ApplyPoolPressure(2, {}, error) == NativePopulationStatus::Ok &&
        population.Alive(NativePopulationKind::Vehicle) == 103, "vehicle pressure frame gate");
    Check(population.ApplyPoolPressure(3, vehiclePosition, error) == NativePopulationStatus::Ok &&
        population.Alive(NativePopulationKind::Vehicle) == 102 && !population.Resolve(firstVehicle),
        "vehicle closest-deletable pressure cleanup");
    Check(population.ApplyPoolPressure(5, pedPosition, error) == NativePopulationStatus::Ok &&
        population.Alive(NativePopulationKind::Ped) == 132 && !population.Resolve(firstPed),
        "ped closest pressure cleanup");
    Check(population.ApplyPoolPressure(5, {INFINITY, 0, 0}, error) == NativePopulationStatus::InvalidInput,
        "invalid pressure input rejection");

    std::printf("native-population-runtime-ok checks=%d traffic=moving peds=moving vehicle-pressure=103-to-102 ped-pressure=133-to-132 paths=source\n",
        g_Checks);
}
