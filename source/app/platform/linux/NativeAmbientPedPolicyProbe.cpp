#include "NativeAmbientPedPolicy.h"

#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

#ifdef NATIVE_AMBIENT_PED_ORACLE
#include NATIVE_AMBIENT_PED_ORACLE
#endif

namespace {
static std::size_t s_Checks{};
static void Check(bool value, const char* message) {
    ++s_Checks;
    if (!value) {
        std::fprintf(stderr, "ambient-ped-policy-fail: %s\n", message);
        std::exit(1);
    }
}

static void Literals() {
    NativeAmbientPedState state;
    NativeAmbientPedInput input;
    NativeAmbientPedDecision decision;
    const auto run = [&] {
        Check(NativeAmbientManagePed(state, input, decision) == NativeAmbientPedStatus::Ok,
            "valid source policy");
    };
    input.GameMs = 100;
    run();
    Check(state.NeededUntilMs == 4100 && decision.DeadlineRefreshed, "civilian close refresh");
    state.PedType = 6;
    run();
    Check(state.NeededUntilMs == 10100, "cop close refresh");
    state = {};
    state.Position[0] = 54.5f;
    run();
    Check(decision.Action == NativeAmbientPedAction::Remove, "far threshold equality removes");
    input.OnScreen = true;
    run();
    Check(state.FadeOut && decision.Action == NativeAmbientPedAction::Keep &&
        !decision.DeadlineRefreshed, "visible far ped fades instead of refresh");
    state.ClumpAlpha = 0;
    run();
    Check(decision.Action == NativeAmbientPedAction::Remove && !decision.DistanceEvaluated,
        "fully faded removal before distance");
    state = {};
    state.PedType = 7;
    state.Position[0] = 25;
    state.DeadInFrontOfCar = state.HasDeadInFrontVehicle = true;
    run();
    Check(decision.RemovalDistance == -5.0f && state.NeededUntilMs == 4100,
        "gang offset precedes dead-front shortcut and is not clamped");
    state.PedType = 4;
    run();
    Check(decision.RemovalDistance == 0, "dead-front source zero distance");
    state = {};
    state.Position[0] = 30;
    state.NeededUntilMs = 100;
    input.OnScreen = false;
    run();
    Check(decision.Action == NativeAmbientPedAction::Keep, "deadline equality retains");
    input.GameMs = 101;
    for (const auto mode : {7, 39, 46}) {
        input.CameraMode = mode;
        run();
        Check(decision.Action == NativeAmbientPedAction::Keep, "protected camera mode");
    }
    input.CameraMode = 4;
    input.LookingBehind = true;
    run();
    Check(decision.Action == NativeAmbientPedAction::Keep, "camera look retains");
    input.LookingBehind = false;
    run();
    Check(decision.Action == NativeAmbientPedAction::Remove, "expired unseen mid-range ped removes");
    for (const auto threshold : {8000u, 15000u, 30000u}) {
        state = {};
        state.Dead = true;
        state.DeathTimeMs = 20;
        input = {};
        input.GangWarFighting = threshold == 8000;
        input.Frenzy = threshold == 15000;
        input.GameMs = threshold + 20;
        run();
        Check(!state.FadeOut, "death fade threshold is strict");
        ++input.GameMs;
        run();
        Check(state.FadeOut, "death fade immediately after threshold");
    }
    state = {};
    input = {};
    input.GameMs = std::numeric_limits<std::uint32_t>::max() - 2000u;
    run();
    Check(state.NeededUntilMs == 1999u, "source refresh wraps uint32");
    state = {};
    state.Dead = true;
    state.DeathTimeMs = std::numeric_limits<std::uint32_t>::max() - 100u;
    input.GameMs = 30000;
    run();
    Check(state.FadeOut, "source death elapsed wraps uint32");
    state = {};
    state.Dead = true;
    const auto before = state;
    const auto previousDecision = decision;
    input = {};
    input.GameMs = 30001;
    input.CreationDistanceMultiplier = 0;
    Check(NativeAmbientManagePed(state, input, decision) == NativeAmbientPedStatus::InvalidInput &&
        state == before && decision == previousDecision, "invalid math retains fade prefix and output");
    input = {};
    state.Position[0] = std::numeric_limits<float>::max();
    const auto huge = state;
    Check(NativeAmbientManagePed(state, input, decision) == NativeAmbientPedStatus::Overflow &&
        state == huge && decision == previousDecision, "overflow retains both outputs");
    for (const auto guard : {0, 1, 2, 3}) {
        state = {};
        state.Player = guard == 0;
        state.Deletable = guard != 1;
        state.InVehicle = guard == 2;
        state.AttachedToVehicle = guard == 3;
        state.Position[0] = std::numeric_limits<float>::quiet_NaN();
        run();
        Check(!decision.DistanceEvaluated && decision.Action == NativeAmbientPedAction::Keep,
            "source early guard does not evaluate distance");
    }
}
}

int main() {
    Literals();
#ifdef NATIVE_AMBIENT_PED_ORACLE
    const auto oracleChecks = RunOriginalAmbientPedPolicyOracle();
    std::printf("native-ambient-ped-policy-ok checks=%zu source-oracle=%zu creation=unowned census=incomplete\n",
        s_Checks, oracleChecks);
#else
    std::printf("native-ambient-ped-policy-ok checks=%zu creation=unowned census=incomplete\n", s_Checks);
#endif
}
