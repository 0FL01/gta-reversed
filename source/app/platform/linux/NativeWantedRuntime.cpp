#include "NativeWantedRuntime.h"

#include <algorithm>
#include <limits>

void NativeWantedRuntime::Record(const char* kind) {
    m_Events.push_back({++m_Sequence, m_State, kind});
}

void NativeWantedRuntime::UpdateLevel(std::uint64_t nowMs) {
    const auto old = m_State.Level;
    if (m_State.Chaos >= 4600) { m_State.Level=6; m_State.ChanceOnRoadblock=30; m_State.MaximumCops=10; m_State.MaximumCopCars=3; }
    else if (m_State.Chaos >= 2400) { m_State.Level=5; m_State.ChanceOnRoadblock=24; m_State.MaximumCops=8; m_State.MaximumCopCars=3; }
    else if (m_State.Chaos >= 1200) { m_State.Level=4; m_State.ChanceOnRoadblock=18; m_State.MaximumCops=6; m_State.MaximumCopCars=2; }
    else if (m_State.Chaos >= 550) { m_State.Level=3; m_State.ChanceOnRoadblock=12; m_State.MaximumCops=4; m_State.MaximumCopCars=2; }
    else if (m_State.Chaos >= 180) { m_State.Level=2; m_State.ChanceOnRoadblock=0; m_State.MaximumCops=3; m_State.MaximumCopCars=2; }
    else if (m_State.Chaos >= 50) { m_State.Level=1; m_State.ChanceOnRoadblock=0; m_State.MaximumCops=1; m_State.MaximumCopCars=1; }
    else { m_State.Level=0; m_State.ChanceOnRoadblock=0; m_State.MaximumCops=0; m_State.MaximumCopCars=0; }
    if (old != m_State.Level) (void)nowMs;
    while (m_State.PursuitCount > m_State.MaximumCops) {
        m_State.PursuitCops[--m_State.PursuitCount] = 0;
    }
}

NativeWantedStatus NativeWantedRuntime::RegisterOffense(std::uint32_t chaos,
    std::uint64_t nowMs, std::string& error) {
    if (!chaos) { error = "wanted offense must add chaos"; return NativeWantedStatus::InvalidInput; }
    if (m_State.Chaos > 9200u - std::min(chaos, 9200u)) m_State.Chaos = 9200;
    else m_State.Chaos = std::min(9200u, m_State.Chaos + chaos);
    UpdateLevel(nowMs);
    ++m_State.Revision;
    Record("offense");
    error.clear();
    return NativeWantedStatus::Ok;
}

NativeWantedStatus NativeWantedRuntime::JoinPursuit(std::uint64_t identity, std::string& error) {
    if (!identity) { error = "pursuit cop identity is invalid"; return NativeWantedStatus::InvalidInput; }
    if (std::find(m_State.PursuitCops.begin(), m_State.PursuitCops.end(), identity) != m_State.PursuitCops.end()) {
        error = "pursuit cop is duplicated";
        return NativeWantedStatus::Duplicate;
    }
    if (m_State.PursuitCount >= m_State.MaximumCops || m_State.PursuitCount == m_State.PursuitCops.size()) {
        error = "pursuit cop limit reached";
        return NativeWantedStatus::CapacityExceeded;
    }
    m_State.PursuitCops[m_State.PursuitCount++] = identity;
    ++m_State.Revision;
    Record("pursuit");
    error.clear();
    return NativeWantedStatus::Ok;
}

NativeWantedStatus NativeWantedRuntime::EscapeTick(std::uint64_t nowMs,
    bool policePresent, bool elusiveZone, bool elusiveVehicle, std::string& error) {
    if (nowMs < m_State.LastDecreaseMs) { error = "wanted escape clock is non-monotonic"; return NativeWantedStatus::InvalidInput; }
    if (nowMs - m_State.LastDecreaseMs <= 1000) { error.clear(); return NativeWantedStatus::Ok; }
    if (m_State.Level > 1 && elusiveZone && elusiveVehicle) {
        m_State.LastDecreaseMs = nowMs;
        ++m_State.Revision;
        Record("elusive-hold");
        error.clear();
        return NativeWantedStatus::Ok;
    }
    if (!policePresent) {
        m_State.LastDecreaseMs = nowMs;
        const auto decrease = elusiveZone ? 2u : 1u;
        m_State.Chaos = m_State.Chaos > decrease ? m_State.Chaos - decrease : 0;
        UpdateLevel(nowMs);
        ++m_State.Revision;
        Record("escape");
    }
    error.clear();
    return NativeWantedStatus::Ok;
}

bool NativeWantedRuntime::ShouldCreateRoadblock(std::uint8_t randomPercent) const noexcept {
    return randomPercent < 100 && randomPercent < m_State.ChanceOnRoadblock;
}
