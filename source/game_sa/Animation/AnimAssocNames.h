#pragma once

#include <array>
#include <string_view>
#include <utility>

// The same ordered declarations initialize the DLL association definitions.
// This name-only view has no address-backed state, animation or RW dependency.
inline constexpr std::array<std::pair<std::string_view, std::string_view>, 118> g_AnimAssocNames{{
#define GTA_ANIM_ASSOC_GROUP(group, block, model, animations, descriptors) {group, block},
#include "AnimAssocGroups.inc"
#undef GTA_ANIM_ASSOC_GROUP
}};
