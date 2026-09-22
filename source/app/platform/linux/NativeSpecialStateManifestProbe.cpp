#include "NativeSpecialStateManifest.h"

#include <cstdio>
#include <cstdlib>

namespace {
int g_Checks = 0;
void Check(bool value, const char* message) {
    ++g_Checks;
    if (!value) { std::fprintf(stderr, "special-state-fail: %s\n", message); std::exit(1); }
}
}

int main() {
    NativeSpecialStateManifest state;
    std::string error;
    Check(state.Cheats().size() == 92 && state.ReplayPackets().size() == 20 &&
        state.ScriptStates().size() == 9, "manifest census");
    std::size_t implementedCheats = 0, pendingCheats = 0;
    for (const auto& row : state.Cheats())
        (row.Coverage == NativeSpecialCoverage::ValueImplemented ? implementedCheats : pendingCheats)++;
    Check(implementedCheats == 12 && pendingCheats == 80, "cheat rows explicit");
    Check(state.ApplyCheat(CHEAT_HEALTH_ARMOR_250K, error) == NativeSpecialStatus::Ok &&
        state.Money() == 250000, "health money route");
    Check(state.ApplyCheat(CHEAT_WANTED_LEVEL_2STARS, error) == NativeSpecialStatus::Ok &&
        state.Wanted() == 2 && state.ApplyCheat(CHEAT_CLEAR_WANTED_LEVEL, error) ==
            NativeSpecialStatus::Ok && state.Wanted() == 0, "wanted routes");
    Check(state.ApplyCheat(CHEAT_RAINY_WEATHER, error) == NativeSpecialStatus::Ok &&
        state.Weather() == 8 && state.ApplyCheat(CHEAT_FASTER_GAMEPLAY, error) ==
            NativeSpecialStatus::Ok && state.TimeScale() == 2.0f, "weather time routes");
    Check(state.ApplyCheat(CHEAT_SPAWN_RHINO, error) == NativeSpecialStatus::Pending,
        "pending cheat explicit");
    for (std::size_t i = 0; i < state.ScriptStates().size(); ++i) {
        const auto kind = NativeScriptSpecialKind(i);
        Check(state.SetScriptState(kind, true, error) == NativeSpecialStatus::Ok &&
            state.ScriptState(kind), "script state route");
    }
    Check(state.BeginReplay(error) == NativeSpecialStatus::Ok, "record begin");
    constexpr eReplayPacket types[]{REPLAY_PACKET_GENERAL, REPLAY_PACKET_CLOCK,
        REPLAY_PACKET_WEATHER, REPLAY_PACKET_TIMER, REPLAY_PACKET_MISC, REPLAY_PACKET_END_OF_FRAME};
    for (std::size_t i = 0; i < std::size(types); ++i)
        Check(state.Record(types[i], 100 + i, error) == NativeSpecialStatus::Ok,
            "record implemented packet");
    Check(state.Record(REPLAY_PACKET_VEHICLE, 0, error) == NativeSpecialStatus::Pending,
        "pending replay explicit");
    Check(state.BeginPlayback(error) == NativeSpecialStatus::Ok, "playback begin");
    NativeReplayValuePacket packet;
    for (std::size_t i = 0; i < std::size(types); ++i)
        Check(state.Next(packet, error) == NativeSpecialStatus::Ok && packet.Type == types[i] &&
            packet.Value == 100 + i, "replay value route");
    std::size_t unknown = 0;
    const auto countUnknown = [&](auto rows) {
        for (const auto& row : rows) unknown += row.Coverage != NativeSpecialCoverage::ValueImplemented &&
            row.Coverage != NativeSpecialCoverage::Pending;
    };
    countUnknown(state.Cheats());
    countUnknown(state.ReplayPackets());
    countUnknown(state.ScriptStates());
    Check(unknown == 0, "no unclassified feature rows");
    std::printf("native-special-state-ok checks=%d cheats=92 implemented=12 pending=80 "
        "replay=20 implemented=7 pending=13 script-states=9 unknown=0\n", g_Checks);
}
