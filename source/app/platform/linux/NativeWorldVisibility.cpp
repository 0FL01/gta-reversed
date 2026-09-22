#include "app/platform/linux/NativeWorldVisibility.h"

#include <algorithm>
#include <cmath>

namespace {
bool InTimeRange(std::uint8_t hour, std::uint8_t from, std::uint8_t to) {
    return from > to ? hour >= from || hour < to : hour >= from && hour < to;
}

float Distance(const NativeCollisionVector& first, const NativeCollisionVector& second) {
    const float x = first[0] - second[0], y = first[1] - second[1], z = first[2] - second[2];
    return std::sqrt(x * x + y * y + z * z);
}
}

bool NativeWorldVisibility::Evaluate(const NativeLodCatalog& catalog,
    const NativeCollisionAssets& assets, const NativeCatalogResidency& residency,
    std::span<const float> modelBoundRadii, const NativeWorldVisibilityInput& input,
    NativeWorldVisibilitySnapshot& out, std::string& error) try {
    if (!catalog.DiskValidated() || input.Area < 0 || input.Area > 255 || input.Hour > 23 ||
        !std::isfinite(input.Camera[0]) || !std::isfinite(input.Camera[1]) ||
        !std::isfinite(input.Camera[2]) || !std::isfinite(input.LodMultiplier) ||
        !std::isfinite(input.LowLodScale) || !std::isfinite(input.FarClip) ||
        input.LodMultiplier <= 0.0f || input.LowLodScale <= 0.0f || input.FarClip <= 0.0f) {
        error = "world visibility input is invalid";
        return false;
    }
    std::vector<NativePlacementIdentity> selected = residency.Visible;
    selected.insert(selected.end(), residency.HiddenTargets.begin(), residency.HiddenTargets.end());
    if (selected.empty()) {
        error = "world visibility selection is empty";
        return false;
    }
    if (modelBoundRadii.size() != selected.size()) {
        error = "world visibility model-bound identity count mismatch";
        return false;
    }
    NativeWorldVisibilitySnapshot next;
    next.Input = input;
    next.Decisions.reserve(selected.size());
    for (std::size_t selectedIndex = 0; selectedIndex < selected.size(); ++selectedIndex) {
        const auto& identity = selected[selectedIndex];
        const auto* node = catalog.Find(identity);
        if (!node) {
            error = "world visibility identity is absent from catalog";
            return false;
        }
        const auto metadata = catalog.Metadata(*node);
        if (metadata.Status != NativeWorldInfoStatus::Ready || !metadata.Model ||
            !metadata.Placement || !metadata.Model->DrawDistance || !metadata.Placement->Area) {
            error = "world visibility metadata is incomplete";
            return false;
        }
        NativeWorldVisibilityDecision decision;
        decision.Identity = identity;
        decision.AreaMatch = int(*metadata.Placement->Area) == input.Area;
        decision.TimeInRange = true;
        if (metadata.Model->Kind == NativeWorldModelKind::TimeAtomic) {
            if (!metadata.Model->TimeOn || !metadata.Model->TimeOff) {
                error = "time model lacks authored hours";
                return false;
            }
            decision.TimeInRange = InTimeRange(input.Hour,
                static_cast<std::uint8_t>(*metadata.Model->TimeOn),
                static_cast<std::uint8_t>(*metadata.Model->TimeOff));
        }
        const NativeLodNode* distanceNode = node;
        if (node->Parent && *node->Parent < catalog.Nodes().size()) {
            distanceNode = &catalog.Nodes()[*node->Parent];
            decision.IsLodChild = true;
        }
        decision.IsLodParent = !node->Children.empty();
        decision.Distance = Distance(input.Camera, distanceNode->Placement.Position);
        decision.DrawRadius = *metadata.Model->DrawDistance * input.LodMultiplier;
        const auto collision = assets.LookupModel(identity.Model);
        const float modelBound = collision.Status == NativeCollisionModelStatus::Ready && collision.Model
            ? collision.Model->BoundRadius : modelBoundRadii[selectedIndex];
        if (!std::isfinite(modelBound) || modelBound < 0.0f) {
            error = "world visibility model bound is invalid";
            return false;
        }
        const float farClipRadius = input.FarClip + modelBound;
        decision.DrawRadius = std::min(decision.DrawRadius, farClipRadius);
        if (decision.IsLodParent) decision.DrawRadius *= input.LowLodScale;
        decision.EffectiveDistance = decision.Distance;
        if (decision.EffectiveDistance > 300.0f && decision.DrawRadius > 300.0f &&
            decision.DrawRadius + 20.0f > decision.EffectiveDistance) {
            decision.EffectiveDistance += decision.DrawRadius - 300.0f;
        }
        decision.DistanceInRange = decision.EffectiveDistance <= decision.DrawRadius + 20.0f;
        decision.Present = decision.AreaMatch && decision.TimeInRange && decision.DistanceInRange;
        decision.Reason = !decision.AreaMatch ? "area-mismatch" :
            !decision.TimeInRange ? "time-out-of-range" :
            !decision.DistanceInRange ? "distance-out-of-range" : "source-visible";
        next.Decisions.push_back(std::move(decision));
    }

    for (auto& parent : next.Decisions) {
        if (!parent.Present || !parent.IsLodParent) continue;
        const auto* parentNode = catalog.Find(parent.Identity);
        bool childVisible = false;
        for (const auto childIndex : parentNode->Children) {
            if (childIndex >= catalog.Nodes().size()) continue;
            const auto childIdentity = catalog.Nodes()[childIndex].Identity;
            const auto found = std::find_if(next.Decisions.begin(), next.Decisions.end(),
                [&](const auto& decision) { return decision.Identity == childIdentity; });
            if (found != next.Decisions.end() && found->Present) {
                childVisible = true;
                break;
            }
        }
        if (childVisible) {
            parent.Present = false;
            parent.Reason = "lod-child-visible";
        } else {
            parent.Reason = "lod-parent-fallback";
        }
    }
    for (const auto& decision : next.Decisions) {
        next.Present += decision.Present ? 1u : 0u;
        next.AreaRejected += !decision.AreaMatch ? 1u : 0u;
        next.TimeRejected += decision.AreaMatch && !decision.TimeInRange ? 1u : 0u;
        next.DistanceRejected += decision.AreaMatch && decision.TimeInRange &&
            !decision.DistanceInRange ? 1u : 0u;
        next.LodSuppressed += decision.Reason == "lod-child-visible" ? 1u : 0u;
    }
    next.CompleteSelection = next.Decisions.size() == selected.size();
    next.FrustumAuthority = false;
    next.OcclusionAuthority = false;
    out = std::move(next);
    error.clear();
    return true;
} catch (const std::exception& exception) {
    error = exception.what();
    return false;
}
