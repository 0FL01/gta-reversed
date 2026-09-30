// Source simple-hierarchy SetClump skin writes, not a Loaded/renderer/ref owner.
#pragma once

#include "NativePedAssets.h"
#include <span>

enum class NativePedSkinSetupStatus {
    Planned, UnknownHierarchy, InvalidInput
};
struct NativePedSkinSetupInput {
    bool HierarchyKnown = false, ComplexHierarchy = false;
    float MorphRadius = 0;
    std::uint32_t HierarchyFlags = 0;
    std::span<const NativePedAssetVertex> Vertices;
};
struct NativePedSkinSetupPlan {
    bool SimpleHierarchy = false;
    float MorphRadius = 0;
    std::uint32_t HierarchyFlags = 0;
    std::vector<std::array<float, 4>> Weights;
    bool operator==(const NativePedSkinSetupPlan&) const = default;
};

// Only the FIRST atomic is normalized; other geometries retain authored weights.
// Complex hierarchy bypasses these writes. No model type/base flags are guessed
// from peds.ide flags. Failure preserves out; no parser packet is mutated.
NativePedSkinSetupStatus NativePlanPedSkinSetup(const NativePedSkinSetupInput& input,
    NativePedSkinSetupPlan& out);
