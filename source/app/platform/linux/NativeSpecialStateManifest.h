#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <span>
#include <vector>

using int8 = std::int8_t;
using uint8 = std::uint8_t;
#include "game_sa/Enums/eCheats.h"
#include "game_sa/Enums/eReplay.h"

enum class NativeSpecialCoverage : std::uint8_t { ValueImplemented, Pending };
enum class NativeSpecialStatus : std::uint8_t { Ok, Pending, InvalidInput, InvalidPhase, Overflow };
enum class NativeScriptSpecialKind : std::uint8_t {
    Riot, Adrenaline, Widescreen, PlayerControl, ZoneNames,
    UpdateStats, RandomTrains, Density, Weather, Count,
};
struct NativeSpecialManifestRow {
    std::uint16_t Id = 0;
    NativeSpecialCoverage Coverage = NativeSpecialCoverage::Pending;
    bool Tested = false;
    bool operator==(const NativeSpecialManifestRow&) const = default;
};
struct NativeReplayValuePacket {
    eReplayPacket Type = REPLAY_PACKET_END;
    std::uint64_t Sequence = 0, Value = 0;
    bool operator==(const NativeReplayValuePacket&) const = default;
};

class NativeSpecialStateManifest {
public:
    NativeSpecialStateManifest();
    NativeSpecialStatus ApplyCheat(eCheats, std::string& error);
    NativeSpecialStatus SetScriptState(NativeScriptSpecialKind, bool value, std::string& error);
    NativeSpecialStatus BeginReplay(std::string& error);
    NativeSpecialStatus Record(eReplayPacket, std::uint64_t value, std::string& error);
    NativeSpecialStatus BeginPlayback(std::string& error);
    NativeSpecialStatus Next(NativeReplayValuePacket& out, std::string& error);

    std::span<const NativeSpecialManifestRow> Cheats() const noexcept { return m_Cheats; }
    std::span<const NativeSpecialManifestRow> ReplayPackets() const noexcept { return m_ReplayRows; }
    std::span<const NativeSpecialManifestRow> ScriptStates() const noexcept { return m_ScriptRows; }
    std::int32_t Money() const noexcept { return m_Money; }
    std::int32_t Wanted() const noexcept { return m_Wanted; }
    std::int32_t Weather() const noexcept { return m_Weather; }
    float TimeScale() const noexcept { return m_TimeScale; }
    bool ScriptState(NativeScriptSpecialKind kind) const noexcept { return m_ScriptState[std::size_t(kind)]; }

private:
    std::array<NativeSpecialManifestRow, TOTAL_CHEATS> m_Cheats{};
    std::array<NativeSpecialManifestRow, NUM_REPLAY_PACKETS> m_ReplayRows{};
    std::array<NativeSpecialManifestRow, std::size_t(NativeScriptSpecialKind::Count)> m_ScriptRows{};
    std::array<bool, TOTAL_CHEATS> m_CheatActive{};
    std::array<bool, std::size_t(NativeScriptSpecialKind::Count)> m_ScriptState{};
    std::vector<NativeReplayValuePacket> m_Replay;
    std::size_t m_Playback = 0;
    bool m_Recording = false, m_Playing = false;
    std::int32_t m_Money = 0, m_Health = 100, m_Armour = 0, m_Wanted = 0, m_Weather = 0;
    float m_TimeScale = 1.0f;
};
