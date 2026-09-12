#include "NativeSourceWalkRun.h"
#include <cmath>
#include <stdexcept>

namespace {
void Require(NativeSourceClumpStatus status) {
    if (status != NativeSourceClumpStatus::Ok) throw std::logic_error("source walk/run clump preflight invariant violated");
}
}

NativeSourceWalkRunStatus NativeSourceProcessWalkRun(NativeSourceAnimClump& clump,
    const NativeSourceWalkRunInput& input, NativeSourceWalkRunResult& out) {
    if (input.Group < 0 || !std::isfinite(input.MoveRatio) || input.MoveRatio < 0 || !std::isfinite(input.TimeCanRun))
        return NativeSourceWalkRunStatus::InvalidInput;
    // Explicit source branches not yet adopted by this normal-flow primitive.
    if (input.SprintRequested || input.TurningInPlace || input.Adrenaline || input.TimeCanRun < 0)
        return NativeSourceWalkRunStatus::Unsupported;
    for (int id : {2, 6, 7, 10, 137, 138}) if (clump.Find(id)) return NativeSourceWalkRunStatus::Unsupported;
    for (int id : {0, 1, 3, 5}) {
        const auto* clip = clump.Clip({input.Group, id});
        if (!clip) return NativeSourceWalkRunStatus::MissingClip;
        if (clip->Partial || clip->Facial || clip->Synchronised != (id < 2) || clip->Looped != (id != 5))
            return NativeSourceWalkRunStatus::Unsupported;
    }
    // Normal association phase domain, not UpdateTime's oversized-single-wrap
    // corner. Reject rather than manufacture a wrapped/clamped phase here.
    for (const auto& a : clump.Snapshot()) {
        if (a.State.BlendAmount < 0 || a.State.BlendAmount > 1 || a.State.CurrentTime < 0 ||
            a.State.CurrentTime > a.State.TotalTime || !std::isfinite(a.State.CurrentTime + a.State.TimeStep))
            return NativeSourceWalkRunStatus::Unsupported;
    }
    NativeSourceWalkRunResult result;
    NativeSourceAnimHandle idle, start, walk, run;
    if (const auto* a = clump.Find(3)) idle = a->Handle;
    if (const auto* a = clump.Find(5)) start = a->Handle;
    if (const auto* a = clump.Find(0)) walk = a->Handle;
    if (const auto* a = clump.Find(1)) run = a->Handle;
    // Retail 0x627820..0x627872: consume the ped's animation-reset bit, without
    // restarting the Playing flag. Sprint association is outside this profile.
    if (input.ResetWalkAnimations) {
        if (walk.Serial) Require(clump.ResetTime(walk));
        if (run.Serial) Require(clump.ResetTime(run));
        result.ResetWalkAnimationsConsumed = true;
    }
    // Retail 0x627967..0x627c9c, TimeCanRun >= 0 and no turning predicate.
    if (input.MoveRatio == 0) {
        if (!idle.Serial) Require(clump.Blend({input.Group, 3}, 4.0f, idle));
        out = result;
        return NativeSourceWalkRunStatus::Ok;
    }
    // Retail 0x627cab..0x627d50. Starting from idle immediately creates a
    // full-weight WALK_start (AddAnimation, not BlendAnimation).
    if (idle.Serial) {
        if (!start.Serial) Require(clump.Add({input.Group, 5}, start));
        else Require(clump.SetBlend(start, 1, 0));
        if (walk.Serial) Require(clump.ResetTime(walk));
        if (run.Serial) Require(clump.ResetTime(run));
        Require(clump.Remove(idle));
        result.Move = NativeSourceMoveState::Walk;
    }
    // Source retains walk/run associations with zero weights during start.
    if (!walk.Serial) {
        Require(clump.Add({input.Group, 0}, walk));
        Require(clump.SetBlend(walk, 0, 0));
    }
    if (!run.Serial) {
        Require(clump.Add({input.Group, 1}, run));
        Require(clump.SetBlend(run, 0, 0));
    }
    if (start.Serial) {
        const auto state = clump.Get(start)->State;
        // Retail 0x627e0c..0x627e59: a LOOKAHEAD by the last source TimeStep,
        // not a callback or a guessed percentage of WALK_start.
        if (!state.Playing || state.CurrentTime + state.TimeStep >= state.TotalTime) {
            Require(clump.Remove(start));
            Require(clump.SetPlaying(walk, true));
            Require(clump.SetPlaying(run, true));
            start = {};
        }
    }
    // Retail 0x627fea..0x628010: keep the existing MoveState during start;
    // caller's ordinary PlayerOnFoot branch begins at Still each source tick.
    if (start.Serial) {
        Require(clump.SetPlaying(walk, false));
        Require(clump.SetPlaying(run, false));
        Require(clump.SetBlend(walk, 0, clump.Get(walk)->State.BlendDelta));
        Require(clump.SetBlend(run, 0, clump.Get(run)->State.BlendDelta));
        result.Starting = true;
    } else {
        NativeSourceWalkRunWeights weights;
        if (NativeSourceSelectWalkRun(input.MoveRatio, weights) != NativePedControlStatus::Ok)
            throw std::logic_error("source walk/run ratio preflight invariant violated");
        Require(clump.SetBlend(walk, weights.Walk, 0));
        Require(clump.SetBlend(run, weights.Run, 0));
        result.Move = weights.Move;
        result.RunningActivity = input.MoveRatio >= 2; // retail 0x62819a callsite
    }
    out = result;
    return NativeSourceWalkRunStatus::Ok;
}
