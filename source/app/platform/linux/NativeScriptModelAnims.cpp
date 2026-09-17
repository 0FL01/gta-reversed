#include "app/platform/linux/NativeScriptModelAnims.h"

#include <algorithm>

NativeScriptServiceResult NativeScriptModelAnims::Add(const NativeScriptModelAnimRequest& request) {
    const auto end = std::find(request.IfpName.begin(), request.IfpName.end(), '\0');
    if (request.ModelId < 0 || request.IfpName[0] == '\0' || end == request.IfpName.end()) {
        return {NativeScriptServiceStatus::Error, "invalid script model animation binding"};
    }
    const auto usedEnd = m_Entries.begin() + std::ptrdiff_t(m_Count);
    const auto duplicate = std::find_if(m_Entries.begin(), usedEnd, [&](const auto& entry) {
        return entry.ModelId == request.ModelId &&
            std::equal(request.IfpName.begin(), request.IfpName.end(), entry.IfpName.begin());
    });
    if (duplicate != usedEnd || m_Count == Capacity) return {NativeScriptServiceStatus::Ready, {}};

    auto& entry = m_Entries[m_Count++];
    entry.ModelId = request.ModelId;
    std::copy(request.IfpName.begin(), request.IfpName.end(), entry.IfpName.begin());
    return {NativeScriptServiceStatus::Ready, {}};
}
