#include "NativePedSkinSetup.h"

#include <cmath>
#include <utility>

NativePedSkinSetupStatus NativePlanPedSkinSetup(const NativePedSkinSetupInput& input,
    NativePedSkinSetupPlan& out) {
    if (!input.HierarchyKnown) return NativePedSkinSetupStatus::UnknownHierarchy;
    if (!std::isfinite(input.MorphRadius) || input.MorphRadius < 0 || input.Vertices.empty() ||
        input.Vertices.size() > 65536) return NativePedSkinSetupStatus::InvalidInput;
    NativePedSkinSetupPlan candidate;
    candidate.SimpleHierarchy = !input.ComplexHierarchy;
    candidate.MorphRadius = input.MorphRadius;
    candidate.HierarchyFlags = input.HierarchyFlags;
    if (input.ComplexHierarchy) {
        // Source bypass: per-atomic hierarchy binding remains a separate effect.
        out = std::move(candidate);
        return NativePedSkinSetupStatus::Planned;
    }
    // Original qword constant is exactly widened 1.2F, not binary64 1.2.
    candidate.MorphRadius = float(static_cast<long double>(input.MorphRadius) * static_cast<long double>(1.2F));
    if (!std::isfinite(candidate.MorphRadius)) return NativePedSkinSetupStatus::InvalidInput;
    candidate.Weights.reserve(input.Vertices.size());
    for (const auto& vertex : input.Vertices) {
        const auto& weights = vertex.Weights;
        for (float weight : weights) if (!std::isfinite(weight) || weight < 0)
            return NativePedSkinSetupStatus::InvalidInput;
        // Original x87 adds without intermediate float spills, then spills SUM.
        long double sum = weights[0];
        sum += weights[1]; sum += weights[2]; sum += weights[3];
        const float denominator = float(sum);
        if (!std::isfinite(denominator) || denominator <= 0) return NativePedSkinSetupStatus::InvalidInput;
        std::array<float, 4> normalized;
        for (std::size_t k = 0; k < 4; ++k) {
            // Literal division, NOT the upstream rounded-reciprocal refactor.
            normalized[k] = float(static_cast<long double>(weights[k]) / denominator);
        }
        candidate.Weights.push_back(normalized);
    }
    candidate.HierarchyFlags = 0x3000; // UPDATEMODELLINGMATRICES | UPDATELTMS.
    out = std::move(candidate);
    return NativePedSkinSetupStatus::Planned;
}
