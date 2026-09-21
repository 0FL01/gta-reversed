#include "NativeScriptTrains.h"

#include <cmath>

NativeScriptServiceResult NativeScriptTrains::Create(std::int32_t type,
    NativeScriptPosition position, bool clockwise, NativeScriptVehicleRef& out) {
    if (type < 0 || type > 15 || !std::isfinite(position.X) ||
        !std::isfinite(position.Y) || !std::isfinite(position.Z))
        return {NativeScriptServiceStatus::Error, "mission train request is invalid"};
    for (std::size_t i = 0; i < m_Slots.size(); ++i) {
        auto& slot = m_Slots[i];
        if (slot.State.Alive) continue;
        if (slot.Generation == 0xFF)
            return {NativeScriptServiceStatus::Error, "mission train generation exhausted"};
        out.Value = static_cast<std::int32_t>(0x40000000u | (std::uint32_t(i) << 8u) | ++slot.Generation);
        slot.State = {out, type, position, clockwise, 0.0f, 0.0f, true, true};
        return {NativeScriptServiceStatus::Ready, {}};
    }
    return {NativeScriptServiceStatus::Error, "mission train capacity exceeded"};
}

NativeScriptServiceResult NativeScriptTrains::SetSpeed(NativeScriptVehicleRef ref,
    float speed, bool cruise) {
    if (!std::isfinite(speed)) return {NativeScriptServiceStatus::Error, "mission train speed is invalid"};
    auto* state = const_cast<NativeScriptTrainState*>(Resolve(ref));
    if (!state) return {NativeScriptServiceStatus::Error, "mission train reference is stale"};
    (cruise ? state->CruiseSpeed : state->Speed) = speed;
    return {NativeScriptServiceStatus::Ready, {}};
}

NativeScriptServiceResult NativeScriptTrains::DeleteMissionTrains() {
    for (auto& slot : m_Slots) if (slot.State.Mission) slot.State.Alive = false;
    return {NativeScriptServiceStatus::Ready, {}};
}

const NativeScriptTrainState* NativeScriptTrains::Resolve(NativeScriptVehicleRef ref) const noexcept {
    const auto raw = static_cast<std::uint32_t>(ref.Value);
    if ((raw & 0xFF000000u) != 0x40000000u) return nullptr;
    const auto slotIndex = (raw >> 8u) & 0xFFFFu;
    if (slotIndex >= m_Slots.size()) return nullptr;
    const auto& slot = m_Slots[slotIndex];
    return slot.State.Alive && slot.Generation == (raw & 0xFFu) ? &slot.State : nullptr;
}

std::size_t NativeScriptTrains::Alive() const noexcept {
    std::size_t result = 0;
    for (const auto& slot : m_Slots) result += slot.State.Alive ? 1u : 0u;
    return result;
}
