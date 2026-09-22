#include "NativeRestartLifecycle.h"

#include <cmath>
#include <limits>

NativeRestartLifecycleStatus NativeRestartLifecycle::Recover(const NativeRestartQuery& query,
    NativeRestartActorState& state, std::string& error) {
    if (!std::isfinite(state.Health) || !std::isfinite(state.Armour) ||
        !std::isfinite(state.HeadingRadians) || !state.WorldGeneration || !state.TaskGeneration) {
        error = "restart actor state is invalid";
        return NativeRestartLifecycleStatus::InvalidInput;
    }
    const auto selection = m_Restarts.Query(query);
    if (selection.Status == NativeRestartStatus::Unsupported) {
        error = selection.Message;
        return NativeRestartLifecycleStatus::Unsupported;
    }
    if (selection.Status != NativeRestartStatus::RestartRequired || !selection.Required) {
        error = selection.Message.empty() ? "restart selection failed" : selection.Message;
        return NativeRestartLifecycleStatus::InvalidInput;
    }
    const auto& required = *selection.Required;
    if (required.Effects != 63 || required.RegistryRevision != m_Restarts.Revision()) {
        error = "restart selection is stale or incomplete";
        return NativeRestartLifecycleStatus::StaleSelection;
    }
    if (state.WorldGeneration == std::numeric_limits<std::uint64_t>::max() ||
        state.TaskGeneration == std::numeric_limits<std::uint64_t>::max() ||
        m_Revision == std::numeric_limits<std::uint64_t>::max()) {
        error = "restart lifecycle generation exhausted";
        return NativeRestartLifecycleStatus::Overflow;
    }
    auto next = state;
    next.Health = 100.0f;
    next.Armour = 0.0f;
    next.WantedLevel = 0;
    ++next.TaskGeneration;
    next.WorldCleared = true;
    next.EntryExitReset = true;
    next.Area = required.DestinationArea;
    next.SceneStreamed = true;
    ++next.WorldGeneration;
    next.Position = required.ResurrectionPosition;
    next.HeadingRadians = required.HeadingRadians;
    next.CameraBehindPlayer = true;
    next.ControlEnabled = true;
    next.GameplayReset = true;
    if (!m_Restarts.ConsumeSelection(required)) {
        error = "restart selection became stale before commit";
        return NativeRestartLifecycleStatus::StaleSelection;
    }
    state = next;
    ++m_Revision;
    error.clear();
    return NativeRestartLifecycleStatus::Ok;
}
