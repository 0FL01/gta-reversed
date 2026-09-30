#include "NativePedSkinSetup.h"

#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace {
int g_Checks = 0;
void Check(bool value, const char* message) {
    ++g_Checks;
    if (!value) { std::fprintf(stderr, "ped-skin-setup-fail: %s\n", message); std::exit(1); }
}
void Bits(float value) { std::printf(" %u", std::bit_cast<std::uint32_t>(value)); }
}
int main() {
    std::array<NativePedAssetVertex, 4> vertices;
    std::uint32_t reciprocalDifferences = 0;
    for (std::uint32_t index = 0; index < 4096; ++index) {
        std::uint32_t seed = index * 0x9e3779b9U + 1;
        for (auto& vertex : vertices) {
            for (float& weight : vertex.Weights) {
                seed = seed * 1664525U + 1013904223U;
                const std::uint32_t exponent = index % 4 == 0 ? 0 : index % 4 == 1 ? 126 : 1 + (seed >> 24) % 220;
                weight = std::bit_cast<float>((exponent << 23) | (seed & 0x7fffffU));
            }
        }
        for (bool complex : {false, true}) {
            NativePedSkinSetupInput input{true, complex,
                std::bit_cast<float>((126U << 23) | (index * 2017U & 0x7fffffU)), index ^ 0xf000U, vertices};
            NativePedSkinSetupPlan plan;
            Check(NativePlanPedSkinSetup(input, plan) == NativePedSkinSetupStatus::Planned, "fixture planned");
            Check(plan.SimpleHierarchy == !complex && plan.Weights.size() == (complex ? 0 : vertices.size()), "literal bypass");
            if (complex) Check(plan.MorphRadius == input.MorphRadius && plan.HierarchyFlags == input.HierarchyFlags,
                "complex branch preserves sphere and flags");
            else {
                Check(plan.HierarchyFlags == 0x3000, "literal hierarchy flags");
                for (std::size_t v = 0; v < vertices.size(); ++v) {
                    const auto& weights = vertices[v].Weights;
                    const float reciprocal = 1.f / (weights[0] + weights[1] + weights[2] + weights[3]);
                    for (std::size_t k = 0; k < 4; ++k) if (
                        std::bit_cast<std::uint32_t>(weights[k] * reciprocal) !=
                        std::bit_cast<std::uint32_t>(plan.Weights[v][k])) ++reciprocalDifferences;
                }
            }
            std::printf("SKIN_SETUP %u %u %u %u %zu %u %u %zu", index, unsigned(complex),
                std::bit_cast<std::uint32_t>(input.MorphRadius), input.HierarchyFlags, vertices.size(),
                std::bit_cast<std::uint32_t>(plan.MorphRadius), plan.HierarchyFlags, plan.Weights.size());
            for (const auto& vertex : vertices) for (float weight : vertex.Weights) Bits(weight);
            for (const auto& weights : plan.Weights) for (float weight : weights) Bits(weight);
            std::puts("");
        }
    }
    NativePedSkinSetupPlan retained;
    retained.SimpleHierarchy = true; retained.MorphRadius = 17; retained.HierarchyFlags = 19;
    retained.Weights.resize(1); retained.Weights[0] = {1, 2, 3, 4};
    const auto before = retained;
    NativePedSkinSetupInput input{false, false, 1, 3, vertices};
    Check(NativePlanPedSkinSetup(input, retained) == NativePedSkinSetupStatus::UnknownHierarchy && retained == before,
        "unknown hierarchy cannot guess simple");
    input.HierarchyKnown = true;
    input.Vertices = {};
    Check(NativePlanPedSkinSetup(input, retained) == NativePedSkinSetupStatus::InvalidInput && retained == before,
        "empty geometry preserves output");
    input.Vertices = vertices;
    vertices[3].Weights = {};
    Check(NativePlanPedSkinSetup(input, retained) == NativePedSkinSetupStatus::InvalidInput && retained == before,
        "late invalid weights preserve complete output");
    for (float invalid : {std::numeric_limits<float>::quiet_NaN(), -1.f, std::numeric_limits<float>::infinity()}) {
        vertices[3].Weights = {invalid, 0, 0, 0};
        Check(NativePlanPedSkinSetup(input, retained) == NativePedSkinSetupStatus::InvalidInput && retained == before,
            "invalid weight rejected atomically");
    }
    vertices[3].Weights.fill(std::numeric_limits<float>::max());
    Check(NativePlanPedSkinSetup(input, retained) == NativePedSkinSetupStatus::InvalidInput && retained == before,
        "overflow sum not silently normalized");
    vertices[3].Weights = {1, 0, 0, 0};
    input.MorphRadius = std::numeric_limits<float>::max();
    Check(NativePlanPedSkinSetup(input, retained) == NativePedSkinSetupStatus::InvalidInput && retained == before,
        "overflow radius preserves output");
    input.MorphRadius = -1;
    Check(NativePlanPedSkinSetup(input, retained) == NativePedSkinSetupStatus::InvalidInput && retained == before,
        "invalid radius rejected");
    Check(reciprocalDifferences > 0, "fixture distinguishes literal division from reciprocal refactor");
    std::printf("native-ped-skin-setup-ok checks=%d cases=8192 reciprocal-differences=%u normalization=division first-atomic=only loaded-state=unowned census=incomplete\n",
        g_Checks, reciprocalDifferences);
}
