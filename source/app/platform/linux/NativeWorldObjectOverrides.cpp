#include "app/platform/linux/NativeWorldObjectOverrides.h"

#include <algorithm>
#include <cmath>
#include <limits>

NativeScriptServiceResult NativeWorldObjectOverrides::SetClosestVisibility(
    const NativeScriptWorldObjectVisibilityRequest& request, const NativeCollisionPopulation& population) {
    if (request.ModelId < 0 || !std::isfinite(request.Position.X) || !std::isfinite(request.Position.Y) ||
        !std::isfinite(request.Position.Z) || !std::isfinite(request.Radius) || request.Radius < 0.0f) {
        return {NativeScriptServiceStatus::Error, "invalid closest world-object visibility request"};
    }
    const NativeCollisionPlacement* closest = nullptr;
    float closestDistance = std::numeric_limits<float>::max();
    const float radiusSquared = request.Radius * request.Radius;
    for (const auto& placement : population.Instances) {
        if (placement.ModelId != request.ModelId) continue;
        const float dx = placement.Position[0] - request.Position.X;
        const float dy = placement.Position[1] - request.Position.Y;
        const float dz = placement.Position[2] - request.Position.Z;
        const float distance = dx * dx + dy * dy + dz * dz;
        if (distance <= radiusSquared && distance < closestDistance) {
            closest = &placement;
            closestDistance = distance;
        }
    }
    if (!closest) return {NativeScriptServiceStatus::Ready, {}};
    const auto identity = NativePlacementIdentity::From(*closest);
    auto end = m_Entries.begin() + std::ptrdiff_t(m_Count);
    auto found = std::find_if(m_Entries.begin(), end, [&](const auto& entry) { return entry.Identity == identity; });
    if (found == end) {
        if (m_Count == Capacity) return {NativeScriptServiceStatus::Error, "world-object override capacity128 exhausted"};
        found = m_Entries.begin() + std::ptrdiff_t(m_Count++);
    }
    *found = {identity, request.ModelId, request.Position, request.Radius, request.Visible};
    return {NativeScriptServiceStatus::Ready, {}};
}
