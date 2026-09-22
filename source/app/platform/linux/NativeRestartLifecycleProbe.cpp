#include "NativeRestartLifecycle.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace {
int g_Checks = 0;
void Check(bool value, const std::string& message) {
    ++g_Checks;
    if (!value) { std::fprintf(stderr, "restart-lifecycle-fail: %s\n", message.c_str()); std::exit(1); }
}
NativeRestartActorState Broken(NativeScriptPosition position) {
    NativeRestartActorState state;
    state.Position = position;
    state.Health = 0.0f;
    state.Armour = 50.0f;
    state.WantedLevel = 4;
    state.ControlEnabled = false;
    state.CameraBehindPlayer = false;
    return state;
}
}

int main(int argc, char** argv) {
    Check(argc == 2, "usage: probe GAME_DIR");
    NativeRestarts restarts;
    std::string error;
    Check(restarts.LoadBeforeWorker(argv[1], error), error);
    Check(restarts.Add({{}, NativeRestartKind::Hospital, {100, 0, 10}, 90, 0}).Status ==
        NativeScriptServiceStatus::Ready, "register hospital fixture");
    Check(restarts.Add({{}, NativeRestartKind::Police, {-100, 0, 20}, -90, 0}).Status ==
        NativeScriptServiceStatus::Ready, "register police fixture");
    restarts.SealStartup();

    NativeRestartLifecycle lifecycle(restarts);
    auto death = Broken({90, 0, 0});
    Check(lifecycle.Recover({NativeRestartKind::Hospital, death.Position, 0, {}, 0}, death, error) ==
        NativeRestartLifecycleStatus::Ok, error);
    Check(death.Position == NativeScriptPosition{100, 0, 11} && death.Area == 0 &&
        death.Health == 100.0f && death.Armour == 0.0f && death.WantedLevel == 0,
        "death hospital resurrection state");
    Check(death.ControlEnabled && death.CameraBehindPlayer && death.WorldCleared &&
        death.EntryExitReset && death.SceneStreamed && death.GameplayReset &&
        death.WorldGeneration == 2 && death.TaskGeneration == 2,
        "death returns world control camera and tasks");

    auto arrest = Broken({-90, 0, 0});
    Check(lifecycle.Recover({NativeRestartKind::Police, arrest.Position, 0, {}, 0}, arrest, error) ==
        NativeRestartLifecycleStatus::Ok, error);
    Check(arrest.Position == NativeScriptPosition{-100, 0, 21} && arrest.ControlEnabled &&
        arrest.CameraBehindPlayer && arrest.WorldGeneration == 2 && arrest.TaskGeneration == 2,
        "arrest police resurrection and return-to-play");
    Check(lifecycle.Revision() == 2, "separate restart commits");
    const auto retained = arrest;
    Check(lifecycle.Recover({NativeRestartKind::Police, {100000, 0, 0}, 0, {}, 0}, arrest, error) ==
        NativeRestartLifecycleStatus::Unsupported && arrest == retained,
        "unsupported restart selection retains actor");

    std::printf("native-restart-lifecycle-ok checks=%d death=hospital arrest=police world=restored control=1 camera=behind tasks=reset generation=2\n",
        g_Checks);
}
