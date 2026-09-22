#pragma once

#include "app/platform/linux/NativeLodCatalog.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

struct NativeWorldVisibilityInput {
    NativeCollisionVector Camera{};
    int Area{};
    std::uint8_t Hour{};
    float LodMultiplier{1.0f};
    float LowLodScale{1.0f};
    float FarClip{300.0f};
    bool operator==(const NativeWorldVisibilityInput&) const = default;
};

struct NativeWorldVisibilityDecision {
    NativePlacementIdentity Identity;
    bool Present{}, AreaMatch{}, TimeInRange{}, DistanceInRange{};
    bool IsLodChild{}, IsLodParent{};
    float Distance{}, EffectiveDistance{}, DrawRadius{};
    std::string Reason;
    bool operator==(const NativeWorldVisibilityDecision&) const = default;
};

struct NativeWorldVisibilitySnapshot {
    NativeWorldVisibilityInput Input;
    std::vector<NativeWorldVisibilityDecision> Decisions;
    std::size_t Present{}, AreaRejected{}, TimeRejected{}, DistanceRejected{}, LodSuppressed{};
    bool CompleteSelection{}, FrustumAuthority{}, OcclusionAuthority{};
    bool operator==(const NativeWorldVisibilitySnapshot&) const = default;
};

class NativeWorldVisibility {
public:
    static bool Evaluate(const NativeLodCatalog&, const NativeCollisionAssets&,
        const NativeCatalogResidency&, std::span<const float> modelBoundRadii,
        const NativeWorldVisibilityInput&, NativeWorldVisibilitySnapshot& out, std::string& error);
};
