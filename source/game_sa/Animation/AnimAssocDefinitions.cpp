#include "StdInc.h"

#include "AnimManager.h"
#include "AnimAssocAnimations.h"
#include "AnimAssocDescriptions.h"

std::array<AnimAssocDefinition, NUM_ANIM_ASSOC_GROUPS> CAnimManager::ms_aAnimAssocDefinitionsX{{ // 0x8AA5A8
#define GTA_ANIM_ASSOC_GROUP(group, block, model, animations, descriptors) \
    {group, block, model, std::size(animations), animations, descriptors},
#include "AnimAssocGroups.inc"
#undef GTA_ANIM_ASSOC_GROUP
}};
