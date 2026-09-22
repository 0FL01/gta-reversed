#include "NativeSpecialStateManifest.h"

#include <algorithm>
#include <limits>

namespace {
bool ImplementedReplay(eReplayPacket type) {
    return type == REPLAY_PACKET_END || type == REPLAY_PACKET_GENERAL || type == REPLAY_PACKET_CLOCK ||
        type == REPLAY_PACKET_WEATHER || type == REPLAY_PACKET_TIMER || type == REPLAY_PACKET_MISC ||
        type == REPLAY_PACKET_END_OF_FRAME;
}
}

NativeSpecialStateManifest::NativeSpecialStateManifest() {
    static_assert(TOTAL_CHEATS == 92);
    static_assert(NUM_REPLAY_PACKETS == 20);
    for (std::size_t i = 0; i < m_Cheats.size(); ++i) m_Cheats[i] = {std::uint16_t(i), NativeSpecialCoverage::Pending, false};
    constexpr eCheats implemented[]{CHEAT_HEALTH_ARMOR_250K, CHEAT_WANTED_LEVEL_2STARS,
        CHEAT_CLEAR_WANTED_LEVEL, CHEAT_SUNNY_WEATHER, CHEAT_VERY_SUNNY_WEATHER,
        CHEAT_OVERCAST_WEATHER, CHEAT_RAINY_WEATHER, CHEAT_FOGGY_WEATHER,
        CHEAT_FASTER_GAMEPLAY, CHEAT_SLOWER_GAMEPLAY, CHEAT_RIOT_MODE, CHEAT_ADRENALINE_MODE};
    for (const auto cheat : implemented) m_Cheats[std::size_t(cheat)].Coverage = NativeSpecialCoverage::ValueImplemented;
    for (std::size_t i = 0; i < m_ReplayRows.size(); ++i) {
        m_ReplayRows[i] = {std::uint16_t(i), ImplementedReplay(eReplayPacket(i))
            ? NativeSpecialCoverage::ValueImplemented : NativeSpecialCoverage::Pending, false};
    }
    for (std::size_t i = 0; i < m_ScriptRows.size(); ++i)
        m_ScriptRows[i] = {std::uint16_t(i), NativeSpecialCoverage::ValueImplemented, false};
}

NativeSpecialStatus NativeSpecialStateManifest::ApplyCheat(eCheats cheat, std::string& error) {
    if (cheat < 0 || cheat >= TOTAL_CHEATS) { error = "cheat id is invalid"; return NativeSpecialStatus::InvalidInput; }
    auto& row = m_Cheats[std::size_t(cheat)];
    if (row.Coverage == NativeSpecialCoverage::Pending) { error = "cheat remains explicitly pending"; return NativeSpecialStatus::Pending; }
    switch (cheat) {
    case CHEAT_HEALTH_ARMOR_250K: m_Money += 250000; m_Health = 100; m_Armour = 100; break;
    case CHEAT_WANTED_LEVEL_2STARS: m_Wanted = std::min(6, m_Wanted + 2); break;
    case CHEAT_CLEAR_WANTED_LEVEL: m_Wanted = 0; break;
    case CHEAT_SUNNY_WEATHER: m_Weather = 0; break;
    case CHEAT_VERY_SUNNY_WEATHER: m_Weather = 1; break;
    case CHEAT_OVERCAST_WEATHER: m_Weather = 2; break;
    case CHEAT_RAINY_WEATHER: m_Weather = 8; break;
    case CHEAT_FOGGY_WEATHER: m_Weather = 9; break;
    case CHEAT_FASTER_GAMEPLAY: m_TimeScale = 2.0f; break;
    case CHEAT_SLOWER_GAMEPLAY: m_TimeScale = 0.5f; break;
    case CHEAT_RIOT_MODE: m_CheatActive[std::size_t(cheat)] = !m_CheatActive[std::size_t(cheat)]; break;
    case CHEAT_ADRENALINE_MODE: m_CheatActive[std::size_t(cheat)] = !m_CheatActive[std::size_t(cheat)]; break;
    default: error = "implemented cheat route is missing"; return NativeSpecialStatus::InvalidInput;
    }
    row.Tested = true;
    error.clear();
    return NativeSpecialStatus::Ok;
}

NativeSpecialStatus NativeSpecialStateManifest::SetScriptState(NativeScriptSpecialKind kind,
    bool value, std::string& error) {
    if (std::size_t(kind) >= m_ScriptRows.size()) { error = "script special state is invalid"; return NativeSpecialStatus::InvalidInput; }
    m_ScriptState[std::size_t(kind)] = value;
    m_ScriptRows[std::size_t(kind)].Tested = true;
    error.clear();
    return NativeSpecialStatus::Ok;
}

NativeSpecialStatus NativeSpecialStateManifest::BeginReplay(std::string& error) {
    m_Replay.clear(); m_Playback = 0; m_Recording = true; m_Playing = false; error.clear(); return NativeSpecialStatus::Ok;
}
NativeSpecialStatus NativeSpecialStateManifest::Record(eReplayPacket type, std::uint64_t value, std::string& error) {
    if (!m_Recording) { error = "replay is not recording"; return NativeSpecialStatus::InvalidPhase; }
    if (type >= NUM_REPLAY_PACKETS) { error = "replay packet type is invalid"; return NativeSpecialStatus::InvalidInput; }
    auto& row = m_ReplayRows[std::size_t(type)];
    if (row.Coverage == NativeSpecialCoverage::Pending) { error = "replay packet remains explicitly pending"; return NativeSpecialStatus::Pending; }
    if (m_Replay.size() == std::numeric_limits<std::uint32_t>::max()) { error = "replay packet capacity exhausted"; return NativeSpecialStatus::Overflow; }
    m_Replay.push_back({type, m_Replay.size() + 1, value});
    row.Tested = true;
    error.clear();
    return NativeSpecialStatus::Ok;
}
NativeSpecialStatus NativeSpecialStateManifest::BeginPlayback(std::string& error) {
    if (!m_Recording || m_Replay.empty()) { error = "replay has no recorded packets"; return NativeSpecialStatus::InvalidPhase; }
    m_Recording = false; m_Playing = true; m_Playback = 0; error.clear(); return NativeSpecialStatus::Ok;
}
NativeSpecialStatus NativeSpecialStateManifest::Next(NativeReplayValuePacket& out, std::string& error) {
    if (!m_Playing || m_Playback >= m_Replay.size()) { error = "replay playback is complete/inactive"; return NativeSpecialStatus::InvalidPhase; }
    out = m_Replay[m_Playback++];
    error.clear();
    return NativeSpecialStatus::Ok;
}
