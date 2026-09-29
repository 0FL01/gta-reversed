#include "NativePedCreationCoordinates.h"

#include <algorithm>
#include <cmath>

namespace {
bool Finite(const NativeCollisionVector& p) {
    return std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(p[2]);
}

float SquaredXY(const NativeCollisionVector& p, float x, float y) {
    // Original x87 subtractions are not spilled until the sum; compressed node
    // coordinates and binary32 input differences fit these double intermediates.
    const double dx = double(p[0]) - double(x);
    const double dy = double(p[1]) - double(y);
    return float(dx * dx + dy * dy);
}

int Area(float coordinate) {
    return int(std::clamp((double(coordinate) + 3000.0) / 750.0, 0.0, 7.0));
}

bool Ring(float distance, float min, float max) {
    return min < distance && distance < max;
}
}

NativePedCreationStatus NativeGeneratePedCreationCoordinates(const NativePathGraph& graph,
    const NativePedCreationInput& input, NativeSourceRngRef rng,
    NativePedCreationObservations& observations, NativePedCreationPosition& out) {
    if (!input.Generation || input.Generation != graph.Generation()) return NativePedCreationStatus::StaleGraph;
    if (!std::isfinite(input.X) || !std::isfinite(input.Y) ||
        !std::isfinite(input.VisibleMin) || !std::isfinite(input.VisibleMax) ||
        !std::isfinite(input.HiddenMin) || !std::isfinite(input.HiddenMax) ||
        input.VisibleMin < 0 || input.VisibleMax < input.VisibleMin ||
        input.HiddenMin < 0 || input.HiddenMax < input.HiddenMin) return NativePedCreationStatus::InvalidInput;
    if (rng.Readiness() != NativeSourceRngStatus::Ready) return NativePedCreationStatus::UnknownRng;
    const auto densityDraw = rng.NextRand15();
    if (!densityDraw.Value) return NativePedCreationStatus::UnknownRng;
    const float densityUnit = float(double(*densityDraw.Value) * (1.0 / 32768.0));
    const int density = int(double(densityUnit) * 15.0);
    const float enlarged = float(double(input.VisibleMax) + 30.0);
    const float maxSquared = enlarged * enlarged;
    if (!std::isfinite(maxSquared)) return NativePedCreationStatus::Overflow;
    const auto area = std::uint8_t(Area(input.X) + Area(input.Y) * 8);
    const auto nodes = graph.Nodes(area);
    const auto* metadata = graph.Metadata(area);
    for (int nodeTrial = 0; nodeTrial < 300; ++nodeTrial) {
        if (nodes.empty() || !metadata || !metadata->PedNodes) continue;
        const auto nodeDraw = rng.NextRand15();
        if (!nodeDraw.Value) return NativePedCreationStatus::UnknownRng;
        // Source discards six low bits BEFORE modulo, not uniform over the
        // entire node pool. Do not replace this with an unbiased distribution.
        const auto index = metadata->VehicleNodes + ((*nodeDraw.Value >> 6u) % metadata->PedNodes);
        if (index >= nodes.size()) return NativePedCreationStatus::InvalidInput;
        const auto& first = nodes[index];
        const float firstSquared = SquaredXY(first.Position, input.X, input.Y);
        if (!std::isfinite(firstSquared)) return NativePedCreationStatus::Overflow;
        if (!(firstSquared < maxSquared) || first.PedDensity <= density) continue;
        float firstDistanceState = firstSquared;
        for (std::uint8_t linkIndex = 0; linkIndex < first.Links; ++linkIndex) {
            NativePathGraphLink link;
            if (!graph.Link(first.Address, linkIndex, link)) return NativePedCreationStatus::InvalidInput;
            if (link.CrossesRoad) continue;
            const auto* second = graph.Resolve(link.Address);
            if (!second) continue; // Source unloaded/invalid exterior-area link.
            if ((first.SwitchedOff || second->SwitchedOff) && !input.AllowSwitchedOff) continue;
            if (second->PedDensity <= density) continue;
            // Original reuses the same spilled local for squared distance AND
            // distance. Every qualifying later link square-roots it again.
            // Do not silently repair this source control-flow quirk.
            firstDistanceState = float(std::sqrt(double(firstDistanceState)));
            if (!(firstDistanceState < input.VisibleMax)) {
                const float secondSquared = SquaredXY(second->Position, input.X, input.Y);
                if (!std::isfinite(secondSquared)) return NativePedCreationStatus::Overflow;
                const float secondDistance = float(std::sqrt(double(secondSquared)));
                if (!(secondDistance < input.VisibleMax)) continue;
            }
            for (int pointTrial = 0; pointTrial < 5; ++pointTrial) {
                const auto pointDraw = rng.NextRand15();
                if (!pointDraw.Value) return NativePedCreationStatus::UnknownRng;
                const float fraction = float(double(*pointDraw.Value & 0xFFu) * (1.0 / 256.0));
                const float inverse = 1.0f - fraction;
                NativePedCreationPosition candidate;
                candidate.First = first.Address;
                candidate.Second = second->Address;
                candidate.Fraction = fraction;
                for (std::size_t axis = 0; axis < 3; ++axis) {
                    const float scaledSecond = second->Position[axis] * fraction;
                    const float scaledFirst = first.Position[axis] * inverse;
                    candidate.Position[axis] = scaledFirst + scaledSecond;
                }
                const float dx = candidate.Position[0] - input.X;
                const float dy = candidate.Position[1] - input.Y;
                const float squared = float(double(dx) * double(dx) + double(dy) * double(dy));
                const float distance = float(std::sqrt(double(squared)));
                if (!Finite(candidate.Position) || !std::isfinite(distance)) return NativePedCreationStatus::Overflow;
                bool visible = false;
                if (!observations.SphereVisible(candidate.Position, 2.0f, input.AlternateCamera, visible))
                    return NativePedCreationStatus::UnsupportedObservation;
                if (visible) {
                    if (!Ring(distance, input.VisibleMin, input.VisibleMax)) continue;
                } else {
                    if (!Ring(distance, input.HiddenMin, input.HiddenMax)) continue;
                    const auto parity = rng.NextRand15();
                    if (!parity.Value) return NativePedCreationStatus::UnknownRng;
                    if (!(*parity.Value & 1u)) continue;
                }
                auto rayStart = candidate.Position;
                rayStart[2] = float(double(rayStart[2]) + 2.0);
                if (!std::isfinite(rayStart[2])) return NativePedCreationStatus::Overflow;
                bool found = false;
                float height = 0;
                if (!observations.GroundZ(rayStart, found, height))
                    return NativePedCreationStatus::UnsupportedObservation;
                if (!found) continue;
                if (!std::isfinite(height)) return NativePedCreationStatus::Overflow;
                const float difference = std::abs(height - candidate.Position[2]);
                // Source rejects this entire request, not just this point, if
                // the found floor is more than three units away from the node.
                if (difference > 3.0f) return NativePedCreationStatus::NoPosition;
                candidate.Position[2] = height;
                out = candidate;
                return NativePedCreationStatus::Position;
            }
        }
    }
    return NativePedCreationStatus::NoPosition;
}

NativePedCreationStatus NativeJitterPedCreationCoordinates(const NativePathGraph& graph,
    std::uint64_t generation, NativePathAddress first, NativePathAddress second,
    std::uint16_t seed, NativeCollisionVector& position) {
    if (!generation || generation != graph.Generation()) return NativePedCreationStatus::StaleGraph;
    if (!Finite(position)) return NativePedCreationStatus::InvalidInput;
    const auto* a = graph.Resolve(first);
    const auto* b = graph.Resolve(second);
    if (!a || !b) return NativePedCreationStatus::InvalidInput;
    const int width = std::min(a->Width, b->Width);
    const int x = (int(seed) & 15) - 7;
    const int y = ((int(seed) >> 4) & 15) - 7;
    constexpr double scale = double(0.00775f);
    auto next = position;
    next[0] = float(double(position[0]) + double(x * width) * scale);
    next[1] = float(double(position[1]) + double(y * width) * scale);
    if (!Finite(next)) return NativePedCreationStatus::Overflow;
    position = next;
    return NativePedCreationStatus::Position;
}
