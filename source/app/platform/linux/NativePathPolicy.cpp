#include "app/platform/linux/NativePathPolicy.h"
#include <algorithm>
#include <cmath>

NativeScriptServiceResult NativePathPolicy::Add(const NativeScriptPathPolicyRequest& request) try {
    for (float value : request.Coordinates) if (!std::isfinite(value)) {
        return {NativeScriptServiceStatus::Error, "nonfinite path policy box"};
    }
    if (request.Kind > NativePathPolicyKind::PedOriginal) {
        return {NativeScriptServiceStatus::Error, "unknown path policy kind"};
    }
    NativePathPolicyEntry entry;
    entry.Kind = request.Kind;
    for (std::size_t i = 0; i < 3; ++i) {
        entry.Min[i] = std::min(request.Coordinates[i], request.Coordinates[i + 3]);
        entry.Max[i] = std::max(request.Coordinates[i], request.Coordinates[i + 3]);
    }
    m_Entries.push_back(entry);
    ++m_Revision;
    return {NativeScriptServiceStatus::Ready, {}};
} catch (const std::exception& exception) {
    return {NativeScriptServiceStatus::Error, exception.what()};
}
