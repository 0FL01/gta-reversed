#include "NativeSourceWalkRun.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace {
constexpr auto Ok = NativeSourceWalkRunStatus::Ok;
std::size_t s_Checks = 0;
void Check(bool ok, const char* message) {
    ++s_Checks;
    if (!ok) { std::fprintf(stderr, "source-walk-run FAIL: %s\n", message); std::exit(1); }
}
std::vector<NativeSourceAnimClip> Clips() {
    std::vector<NativeSourceAnimClip> clips;
    for (int id : {0, 1, 3, 5}) {
        NativeSourceAnimClip a;
        a.Key = {0, id}; a.Duration = id == 5 ? 0.25f : 1; a.Sequences = 1;
        a.Synchronised = id < 2; a.Looped = id != 5;
        clips.push_back(a);
    }
    return clips;
}
void Flow() {
    NativeSourceAnimClump clump;
    Check(clump.LoadClips(Clips()) == NativeSourceClumpStatus::Ok, "explicit synthetic ordinary metadata");
    NativeSourceWalkRunInput input;
    NativeSourceWalkRunResult result;
    Check(NativeSourceProcessWalkRun(clump, input, result) == Ok && result.Move == NativeSourceMoveState::Still && clump.Find(3), "idle is authored animation");
    Check(clump.Find(3)->State.BlendAmount == 1, "first full body idle starts fully blended");
    input.MoveRatio = 0.5f;
    Check(NativeSourceProcessWalkRun(clump, input, result) == Ok && result.Starting && result.Move == NativeSourceMoveState::Walk && !clump.Find(3), "idle replaced by full-weight walk_start");
    Check(clump.Find(5)->State.BlendAmount == 1 && !clump.Find(0)->State.Playing && !clump.Find(1)->State.Playing, "source walk/run held during start");
    std::vector<NativeSourceClumpEvent> events;
    Check(clump.Update(0.0625f, events) == NativeSourceClumpStatus::Ok && NativeSourceProcessWalkRun(clump, input, result) == Ok &&
        result.Starting && result.Move == NativeSourceMoveState::Still, "next source tick preserves Still while start association runs");
    Check(clump.Update(0.0625f, events) == NativeSourceClumpStatus::Ok && NativeSourceProcessWalkRun(clump, input, result) == Ok && result.Starting, "not yet at lookahead boundary");
    Check(clump.Update(0.0625f, events) == NativeSourceClumpStatus::Ok && clump.Find(5)->State.CurrentTime == 0.1875f, "start still before its actual end");
    Check(NativeSourceProcessWalkRun(clump, input, result) == Ok && !result.Starting && !clump.Find(5) && result.Move == NativeSourceMoveState::Walk, "source lookahead retires start one timestep before clip end");
    Check(events.empty() && clump.Find(0)->State.Playing && clump.Find(0)->State.BlendAmount == 1, "start retirement is not a fabricated finish event");
    input.MoveRatio = 1;
    Check(NativeSourceProcessWalkRun(clump, input, result) == Ok && result.Move == NativeSourceMoveState::Run && clump.Find(1)->State.BlendAmount == 0, "exact ratio one state/weights");
    input.MoveRatio = 1.5f;
    Check(NativeSourceProcessWalkRun(clump, input, result) == Ok && clump.Find(0)->State.BlendAmount == 0.5f && clump.Find(1)->State.BlendAmount == 0.5f, "source walk/run blend");
    input.MoveRatio = 2;
    Check(NativeSourceProcessWalkRun(clump, input, result) == Ok && clump.Find(1)->State.BlendAmount == 1 && result.RunningActivity, "full run reports source stat activity without fabricating stat mutation");
    Check(clump.Update(0.125f, events) == NativeSourceClumpStatus::Ok && clump.Find(1)->State.CurrentTime > 0, "running association progresses");
    input.ResetWalkAnimations = true;
    Check(NativeSourceProcessWalkRun(clump, input, result) == Ok && result.ResetWalkAnimationsConsumed && clump.Find(1)->State.CurrentTime == 0 && clump.Find(1)->State.Playing, "source reset bit clears phase without stopping playback");
    input.ResetWalkAnimations = false;
    input.MoveRatio = 0;
    Check(NativeSourceProcessWalkRun(clump, input, result) == Ok && result.Move == NativeSourceMoveState::Still && clump.Find(3)->State.BlendDelta == 4 &&
        clump.Find(1)->State.BlendDelta == -4, "ordinary stop blends idle, never manufactures sprint run_stop");
    Check(clump.Update(0.25f, events) == NativeSourceClumpStatus::Ok && !clump.Find(0) && !clump.Find(1) && clump.Find(3)->State.BlendAmount == 1, "source clump retirement completes stop");
    Check(!clump.Find(6) && !clump.Find(7), "sprint-stop animations absent from ordinary path");
}
void Rejection() {
    NativeSourceAnimClump clump;
    NativeSourceWalkRunInput input;
    NativeSourceWalkRunResult out{NativeSourceMoveState::Run, true};
    const auto retained = out;
    Check(NativeSourceProcessWalkRun(clump, input, out) == NativeSourceWalkRunStatus::MissingClip && out == retained && clump.Snapshot().empty(), "missing clips reject before mutations");
    Check(clump.LoadClips(Clips()) == NativeSourceClumpStatus::Ok, "load rejection fixture");
    input.SprintRequested = true;
    Check(NativeSourceProcessWalkRun(clump, input, out) == NativeSourceWalkRunStatus::Unsupported && out == retained && clump.Snapshot().empty(), "sprint cannot silently become ordinary run");
    input.SprintRequested = false; input.TimeCanRun = -1;
    Check(NativeSourceProcessWalkRun(clump, input, out) == NativeSourceWalkRunStatus::Unsupported && out == retained, "exhaustion branch not fabricated");
    input.TimeCanRun = 0; input.Adrenaline = true;
    Check(NativeSourceProcessWalkRun(clump, input, out) == NativeSourceWalkRunStatus::Unsupported && out == retained, "adrenaline clock branch separate");
    input.Adrenaline = false; input.TurningInPlace = true;
    Check(NativeSourceProcessWalkRun(clump, input, out) == NativeSourceWalkRunStatus::Unsupported && out == retained, "special turn branch separate");
    input.TurningInPlace = false; input.MoveRatio = std::numeric_limits<float>::quiet_NaN();
    Check(NativeSourceProcessWalkRun(clump, input, out) == NativeSourceWalkRunStatus::InvalidInput && out == retained, "invalid ratio atomic");
}
}
int main() {
    Flow(); Rejection();
    std::printf("source-walk-run-ok checks=%zu ordinary-branch-only no-full-player-host-claim\n", s_Checks);
}
