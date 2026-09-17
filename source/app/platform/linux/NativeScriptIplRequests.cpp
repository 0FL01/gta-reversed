#include "app/platform/linux/NativeScriptIplRequests.h"

#include <algorithm>

NativeScriptServiceResult NativeScriptIplRequests::Set(const NativeScriptIplRequest& request) {
    if (request.Name[0] == '\0' || std::find(request.Name.begin(), request.Name.end(), '\0') == request.Name.end()) {
        return {NativeScriptServiceStatus::Error, "invalid script IPL name"};
    }
    auto end = m_Entries.begin() + std::ptrdiff_t(m_Count);
    const auto found = std::find_if(m_Entries.begin(), end, [&](const auto& entry) { return entry.Name == request.Name; });
    if (found != end) {
        found->Requested = request.Requested;
        return {NativeScriptServiceStatus::Ready, {}};
    }
    if (!request.Requested) return {NativeScriptServiceStatus::Ready, {}};
    if (m_Count == Capacity) return {NativeScriptServiceStatus::Error, "script IPL request capacity256 exhausted"};
    m_Entries[m_Count++] = {request.Name, true};
    return {NativeScriptServiceStatus::Ready, {}};
}
