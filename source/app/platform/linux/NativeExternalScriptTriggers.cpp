#include "app/platform/linux/NativeExternalScriptTriggers.h"
#include <algorithm>
#include <cmath>
NativeScriptServiceResult NativeExternalScriptTriggers::Add(const NativeScriptExternalTriggerRequest& request,
    std::span<const NativeScriptStreamedState> scripts) {
    if (request.ScriptIndex < 0 || std::size_t(request.ScriptIndex) >= scripts.size() || request.ModelId < 0 ||
        request.Priority < 0 || !std::isfinite(request.Radius) || request.Radius < 0 ||
        request.Type < -1 || request.Type > 127) {
        return {NativeScriptServiceStatus::Error, "invalid external script trigger"};
    }
    if (m_Count == Capacity) {
        return {NativeScriptServiceStatus::Error, "external script trigger capacity70 exhausted"};
    }
    NativeExternalScriptTrigger entry;
    entry.Kind = request.ObjectModel ? NativeExternalTriggerKind::ObjectModel : NativeExternalTriggerKind::PedModel;
    entry.ScriptIndex = std::uint8_t(request.ScriptIndex);
    entry.ModelId = request.ModelId;
    entry.Priority = request.Priority;
    entry.Type = request.Type;
    entry.Radius = request.Radius;
    entry.ModelName = request.ModelName;
    m_Entries[m_Count++] = entry;
    return {NativeScriptServiceStatus::Ready, {}};
}

NativeScriptServiceResult NativeExternalScriptTriggers::AddCodeUse(
    const NativeScriptCodeBrainRequest& request, std::span<const NativeScriptStreamedState> scripts) {
    if (request.ScriptIndex < 0 || std::size_t(request.ScriptIndex) >= scripts.size() ||
        request.Name[0] == '\0' || std::find(request.Name.begin(), request.Name.end(), '\0') == request.Name.end()) {
        return {NativeScriptServiceStatus::Error, "invalid code-use script brain"};
    }
    if (m_Count == Capacity) {
        return {NativeScriptServiceStatus::Error, "external script trigger capacity70 exhausted"};
    }
    NativeExternalScriptTrigger entry;
    entry.Kind = request.Attractor ? NativeExternalTriggerKind::AttractorCodeUse : NativeExternalTriggerKind::CodeUse;
    entry.ScriptIndex = std::uint8_t(request.ScriptIndex);
    entry.Type = request.Attractor ? 5 : 3; // Source code-use attach types.
    std::copy(request.Name.begin(), request.Name.end(), entry.ModelName.begin());
    m_Entries[m_Count++] = entry;
    return {NativeScriptServiceStatus::Ready, {}};
}
