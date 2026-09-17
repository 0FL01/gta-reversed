#pragma once

#include "app/platform/linux/NativeCollisionAssets.h"
#include "app/platform/linux/NativeScriptSession.h"

#include <array>
#include <cstddef>
#include <span>

struct NativeWorldObjectVisibility {
    NativePlacementIdentity Identity;
    std::int32_t ModelId = -1;
    NativeScriptPosition Query{};
    float Radius = 0.0f;
    bool Visible = true;
    bool operator==(const NativeWorldObjectVisibility&) const = default;
};

class NativeWorldObjectOverrides {
public:
    static constexpr std::size_t Capacity = 128;
    static constexpr bool RuntimePresentation = false;

    NativeScriptServiceResult SetClosestVisibility(
        const NativeScriptWorldObjectVisibilityRequest& request, const NativeCollisionPopulation& population);
    std::span<const NativeWorldObjectVisibility> Entries() const { return {m_Entries.data(), m_Count}; }

private:
    std::array<NativeWorldObjectVisibility, Capacity> m_Entries{};
    std::size_t m_Count = 0;
};
