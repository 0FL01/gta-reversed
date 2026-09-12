#include "NativeSourcePedControl.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace {
std::size_t s_Checks = 0;
void Check(bool ok, const char* message) {
    ++s_Checks;
    if (!ok) { std::fprintf(stderr, "source-ped-control FAIL: %s\n", message); std::exit(1); }
}
constexpr auto Ok = NativePedControlStatus::Ok;
void Movement() {
    NativeSourceMoveBlend blend;
    NativeSourceMoveSample sample;
    sample.UpDown = -120; sample.TimeStep = 1;
    Check(NativeSourceUpdateMoveBlend(sample, blend) == Ok && blend.Ratio == 0.07f, "source blend ramp .07 per normalized step");
    Check(blend.AimingRotation == 0, "source forward heading");
    for (int i = 0; i < 40; ++i) Check(NativeSourceUpdateMoveBlend(sample, blend) == Ok, "ramp");
    Check(blend.Ratio == 2, "run target reached, not smoothed forever");
    sample.WalkModifier = true;
    for (int i = 0; i < 40; ++i) Check(NativeSourceUpdateMoveBlend(sample, blend) == Ok, "decelerate to walk modifier");
    Check(blend.Ratio == 1, "walk key clamps desired ratio");
    NativeSourceWalkRunWeights weights;
    Check(NativeSourceSelectWalkRun(1, weights) == Ok && weights.Move == NativeSourceMoveState::Run && weights.Walk == 1 && weights.Run == 0, "retail EXACT one is RUN state with walk weight");
    Check(NativeSourceSelectWalkRun(std::nextafter(1.0f, 0.0f), weights) == Ok && weights.Move == NativeSourceMoveState::Walk, "strict below one walk");
    Check(NativeSourceSelectWalkRun(1.5f, weights) == Ok && weights.Walk == 0.5f && weights.Run == 0.5f, "retail walk/run blend weights");
    Check(NativeSourceSelectWalkRun(2, weights) == Ok && weights.Walk == 0 && weights.Run == 1, "two full run");
    const auto savedWeights = weights;
    Check(NativeSourceSelectWalkRun(0, weights) != Ok && weights == savedWeights, "idle is outside normal locomotion branch");
    sample.DirectionAllowed = false;
    Check(NativeSourceUpdateMoveBlend(sample, blend) == Ok && blend.Ratio == 0, "direction predicate blocks ratio");
    sample.DirectionAllowed = true; sample.Attached = true;
    Check(NativeSourceUpdateMoveBlend(sample, blend) == Ok && blend.Ratio == 0, "attached zero");
    const auto saved = blend;
    sample.LeftRight = 129;
    Check(NativeSourceUpdateMoveBlend(sample, blend) != Ok && blend == saved, "invalid source axis atomic");
}
void Animation() {
    NativeSourceAnimAssociation state;
    NativeSourceAnimEvent missingEvent{7, false};
    Check(NativeSourceAnimUpdateTime(state, missingEvent) != Ok && missingEvent.FinishToken == 7, "missing clip duration cannot advance or invent a marker");
    state.TotalTime = 1; // explicit synthetic clip duration
    state.FinishToken = 42; state.FinishAutoRemove = true; state.BlendAmount = 1;
    NativeSourceAnimEvent event;
    Check(NativeSourceAnimUpdateStep(state, 0.5f, 1) == Ok && state.TimeStep == 0.5f, "unsynchronised step");
    Check(NativeSourceAnimUpdateTime(state, event) == Ok && !event.FinishToken && state.CurrentTime == 0.5f, "no invented early finish marker");
    Check(NativeSourceAnimUpdateTime(state, event) == Ok && event.FinishToken == 42 && state.CurrentTime == 1 && state.Playing, "exact end fires once, playing clears NEXT call");
    Check(state.BlendAutoRemove && state.BlendDelta == -4, "finish auto fade");
    Check(NativeSourceAnimUpdateTime(state, event) == Ok && !event.FinishToken && !state.Playing, "source late playing clear");
    Check(NativeSourceAnimUpdateBlend(state, 0.25f, event) == Ok && event.Removed && !event.FinishToken && !state.Alive, "fade deletion cannot refire callback");
    const auto deleted = state; const auto deletedEvent = event;
    Check(NativeSourceAnimUpdateTime(state, event) != Ok && state == deleted && event == deletedEvent, "deleted association rejects atomically");
    state = {}; state.TotalTime = 1; state.FinishToken = 77; state.BlendAmount = 0.5f; state.BlendDelta = -4; state.BlendAutoRemove = true;
    Check(NativeSourceAnimUpdateBlend(state, 0.125f, event) == Ok && event.Removed && event.FinishToken == 77, "abort fade invokes finish callback even before time end");
    state = {}; state.TotalTime = 1; state.Looped = true; state.TimeStep = 2.25f; state.FinishToken = 99;
    Check(NativeSourceAnimUpdateTime(state, event) == Ok && state.CurrentTime == 1.25f && !event.FinishToken, "loop wraps ONCE, not modulo");
    Check(NativeSourceAnimUpdateTime(state, event) == Ok && !state.Playing && state.CurrentTime == 1.25f, "oversized loop stops next source update");
    state = {}; state.Synchronised = true; state.TotalTime = 2;
    Check(NativeSourceAnimUpdateStep(state, 0.25f, 3) == Ok && state.TimeStep == 1.5f, "synchronised step operation order");
    state.Playing = false;
    Check(NativeSourceAnimUpdateStep(state, 0.1f, 1) == Ok && state.TimeStep == 1.5f, "nonplaying retains source timestep");
    const auto saved = state;
    event = {123, true}; const auto savedEvent = event;
    Check(NativeSourceAnimUpdateBlend(state, std::numeric_limits<float>::quiet_NaN(), event) != Ok && state == saved && event == savedEvent, "invalid blend sample atomic");
    state = {}; state.TotalTime = 1; state.BlendAmount = 0.9f; state.BlendDelta = 8;
    Check(NativeSourceAnimUpdateBlend(state, 0.1f, event) == Ok && state.BlendAmount == 1 && state.BlendDelta == 0, "blend upper clamp");
}
}
int main() {
    Movement(); Animation();
    std::printf("source-ped-control-ok checks=%zu source-primitives-only task-owner-not-yet-hosted\n", s_Checks);
}
