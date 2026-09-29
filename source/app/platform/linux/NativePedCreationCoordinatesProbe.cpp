#include "NativePedCreationCoordinates.h"

#include <bit>
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace {
int s_Checks = 0;

void Check(bool condition, const char* message) {
    ++s_Checks;
    if (!condition) {
        std::fprintf(stderr, "ped-creation-fail: %s\n", message);
        std::exit(1);
    }
}

struct Observations final : NativePedCreationObservations {
    int VisibilityMode = 0;
    int GroundMode = 0;
    std::uint32_t VisibilityCalls = 0, GroundCalls = 0;

    bool SphereVisible(const NativeCollisionVector&, float radius,
        bool alternate, bool& visible) noexcept override {
        ++VisibilityCalls;
        if (radius != 2.0f || VisibilityMode == 3) return false;
        visible = VisibilityMode == 0 || (VisibilityMode == 2 && alternate);
        return true;
    }

    bool GroundZ(const NativeCollisionVector& point, bool& found, float& height) noexcept override {
        ++GroundCalls;
        if (GroundMode == 4) return false;
        found = GroundMode != 1;
        height = point[2] - 2.0f;
        if (GroundMode == 2) height += 3.0f;
        if (GroundMode == 3) height += 4.0f;
        return true;
    }
};

NativePedCreationPosition Sentinel() {
    return {{-123, -456, -789}, {99, 10}, {98, 11}, -1};
}
}

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    NativePathGraph graph;
    std::string error;
    Check(graph.LoadBeforeWorker(argv[1], error), error.c_str());
    std::vector<NativePathAreaResidency> areas;
    for (std::uint8_t area = 0; area < 64; ++area) {
        const auto* metadata = graph.Metadata(area);
        Check(metadata != nullptr, "all source path areas");
        areas.push_back(*metadata);
    }
    Check(graph.Adopt(1, areas, error) == NativePathGraphStatus::Ok, error.c_str());
    std::size_t succeeded = 0, hiddenParity = 0, groundRejections = 0;
    std::uint32_t caseId = 0;
    for (const auto seed : {0u, 1u, 7u, 1792u, 0xffffffffu}) {
        for (const auto origin : {std::array<float, 2>{2488.562255859375f, -1666.864501953125f},
            std::array<float, 2>{2229.5f, -1342.0f}, std::array<float, 2>{0, 0},
            std::array<float, 2>{3001, 3001}, std::array<float, 2>{-3001, -3001}}) {
            for (int mode = 0; mode < 10; ++mode) {
                NativePedCreationInput input{1, origin[0], origin[1], 0, 200, 0, 200,
                    mode == 6, mode == 5};
                if (mode >= 7) {
                    input.VisibleMin = 42.5f;
                    input.VisibleMax = 50.5f;
                    input.HiddenMin = 15.0f;
                    input.HiddenMax = 25.0f;
                }
                if (mode == 9) input.VisibleMax = input.VisibleMin;
                NativeSourceRng rng;
                Check(rng.SeedOnce(seed) == NativeSourceRngStatus::Ready, "one source seed");
                Observations observations;
                observations.VisibilityMode = mode == 1 || mode == 8 ? 1 : mode == 5 ? 2 : 0;
                observations.GroundMode = mode >= 2 && mode <= 4 ? mode - 1 : 0;
                auto position = Sentinel();
                const auto status = NativeGeneratePedCreationCoordinates(graph, input,
                    rng.Reference(), observations, position);
                Check(status == NativePedCreationStatus::Position || status == NativePedCreationStatus::NoPosition,
                    "source coordinate decision");
                if (status == NativePedCreationStatus::NoPosition) {
                    Check(position == Sentinel(), "failed decision preserves output");
                    groundRejections += mode == 4 && observations.GroundCalls;
                } else {
                    ++succeeded;
                    hiddenParity += mode == 1;
                    Check(graph.Resolve(position.First) && graph.Resolve(position.Second), "live selected nodes");
                }
                const auto provenance = rng.Inspect().Value.value();
                std::printf("CASE %u %u %.9g %.9g %.9g %.9g %.9g %.9g %d %d %d %d %u %llu %u %u %u %u %u %u %u %u %u %u %u\n",
                    caseId++, seed, origin[0], origin[1], input.VisibleMin, input.VisibleMax,
                    input.HiddenMin, input.HiddenMax, int(input.AllowSwitchedOff), int(input.AlternateCamera),
                    observations.VisibilityMode, observations.GroundMode, unsigned(status),
                    static_cast<unsigned long long>(provenance.DrawCount), provenance.State,
                    position.First.Area, position.First.Node, position.Second.Area, position.Second.Node,
                    std::bit_cast<std::uint32_t>(position.Fraction),
                    std::bit_cast<std::uint32_t>(position.Position[0]),
                    std::bit_cast<std::uint32_t>(position.Position[1]),
                    std::bit_cast<std::uint32_t>(position.Position[2]),
                    observations.VisibilityCalls, observations.GroundCalls);
                if (status == NativePedCreationStatus::Position) {
                    for (const auto jitterSeed : {std::uint16_t(0), std::uint16_t(0x77), std::uint16_t(0xffff)}) {
                        auto jittered = position.Position;
                        Check(NativeJitterPedCreationCoordinates(graph, 1, position.First, position.Second,
                            jitterSeed, jittered) == NativePedCreationStatus::Position, "source width jitter");
                        std::printf("JITTER %u %u %u %u %u\n", caseId - 1, jitterSeed,
                            std::bit_cast<std::uint32_t>(jittered[0]), std::bit_cast<std::uint32_t>(jittered[1]),
                            std::bit_cast<std::uint32_t>(jittered[2]));
                    }
                }
            }
        }
    }
    Check(succeeded > 0 && hiddenParity > 0 && groundRejections > 0, "success/parity/ground branches exercised");
    NativeSourceRng rng;
    Check(rng.SeedOnce(0) == NativeSourceRngStatus::Ready, "validation seed");
    Observations observations;
    NativePedCreationInput input{1, 2488.562255859375f, -1666.864501953125f, 0, 200, 0, 200};
    auto out = Sentinel();
    const auto before = rng.Inspect().Value;
    input.Generation = 2;
    Check(NativeGeneratePedCreationCoordinates(graph, input, rng.Reference(), observations, out) ==
        NativePedCreationStatus::StaleGraph && out == Sentinel() && rng.Inspect().Value == before,
        "stale graph before RNG");
    input.Generation = 1;
    input.X = std::numeric_limits<float>::quiet_NaN();
    Check(NativeGeneratePedCreationCoordinates(graph, input, rng.Reference(), observations, out) ==
        NativePedCreationStatus::InvalidInput && out == Sentinel() && rng.Inspect().Value == before,
        "invalid input before RNG");
    input.X = 2488.562255859375f;
    observations.VisibilityMode = 3;
    Check(NativeGeneratePedCreationCoordinates(graph, input, rng.Reference(), observations, out) ==
        NativePedCreationStatus::UnsupportedObservation && out == Sentinel(), "missing camera is not invisible");
    NativeSourceRng unseeded;
    const auto calls = observations.VisibilityCalls;
    Check(NativeGeneratePedCreationCoordinates(graph, input, unseeded.Reference(), observations, out) ==
        NativePedCreationStatus::UnknownRng && out == Sentinel() && observations.VisibilityCalls == calls,
        "unknown RNG cannot observe the world");
    NativeSourceRng missingGroundRng;
    Check(missingGroundRng.SeedOnce(0) == NativeSourceRngStatus::Ready, "missing ground seed");
    observations = {};
    observations.GroundMode = 4;
    Check(NativeGeneratePedCreationCoordinates(graph, input, missingGroundRng.Reference(), observations, out) ==
        NativePedCreationStatus::UnsupportedObservation && out == Sentinel() && observations.GroundCalls == 1,
        "missing building-world authority is not a ground miss");
    auto held = Sentinel().Position;
    Check(NativeJitterPedCreationCoordinates(graph, 2, {15, 733}, {15, 734}, 0, held) ==
        NativePedCreationStatus::StaleGraph && held == Sentinel().Position,
        "jitter retains position for a retired generation");
    std::printf("native-ped-creation-coordinates-ok checks=%d cases=%u successful=%zu census=incomplete birth=unowned\n",
        s_Checks, caseId, succeeded);
}
