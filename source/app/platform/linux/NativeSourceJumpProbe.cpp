#include "NativeSourceJump.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <limits>

namespace {
constexpr auto Ok = NativeSourceJumpStatus::Ok;
using Phase = NativeSourceJumpPhase;
using Priority = NativeSourceAbortPriority;
using Event = NativeSourceAbortEvent;
std::size_t s_Checks = 0;
void Check(bool ok, const char* message) {
    ++s_Checks;
    if (!ok) { std::fprintf(stderr, "source-jump FAIL: %s\n", message); std::exit(1); }
}
NativeSourceJumpStart FixtureStart() {
    NativeSourceJumpStart start;
    start.LaunchDuration = 1; // generated test clip, NOT a source asset duration
    return start;
}
void CompleteLaunch(NativeSourceJump& jump, const NativeSourceJumpStart& start = FixtureStart()) {
    Check(jump.Begin(start) == Ok, "begin source jump");
    Check(jump.AdvanceAnimation(jump.State().Generation, start.LaunchDuration) == Ok &&
        jump.State().Phase == Phase::AwaitWorld, "finish callback precedes world/force");
}
void Flow() {
    NativeSourceJump jump;
    Check(jump.Begin({}) == NativeSourceJumpStatus::InvalidInput && jump.State().Generation == 0, "missing authored clip duration does not create a task");
    NativeSourceJumpStart start; start.LaunchDuration = 0.25f;
    start.Run = {true, 1, 0.2f, 1};
    Check(jump.Begin(start) == Ok && jump.State().RightLaunch, "source .367 phase selects right launch");
    const auto generation = jump.State().Generation;
    Check(jump.AdvanceAnimation(generation, 0.125f) == Ok && !jump.State().LaunchFinished && !jump.State().InAir, "no premature jump launch");
    Check(jump.AdvanceAnimation(generation, 0.125f) == Ok && jump.State().LaunchFinished && !jump.State().InAir, "animation finish alone is not physical launch");
    Check(jump.State().Animation.BlendAutoRemove && jump.State().Animation.BlendDelta == -4, "source launch association default finish-auto-remove flag");
    NativeSourceJumpWorld world;
    const auto waiting = jump.State();
    Check(jump.ResolveWorld(generation, world) == NativeSourceJumpStatus::Unsupported && jump.State() == waiting, "missing world authority stays unsupported");
    world.Status = NativeSourceJumpWorldStatus::Pending;
    Check(jump.ResolveWorld(generation, world) == NativeSourceJumpStatus::PendingWorld && jump.State() == waiting, "unknown collision retains pending task");
    world.Status = NativeSourceJumpWorldStatus::Error;
    Check(jump.ResolveWorld(generation, world) == NativeSourceJumpStatus::WorldError && jump.State() == waiting, "world error atomic");
    world.Status = NativeSourceJumpWorldStatus::Ready;
    world.Run = start.Run;
    world.MoveX = 0.2f; // squared .04 < source run threshold .17, preserve source quirk
    Check(jump.ResolveWorld(generation, world) == Ok && jump.State().InAir && jump.State().UpwardForce == 8.5f, "source player impulse");
    Check(jump.State().ClearStanding && jump.State().AirAnimation == NativeSourceJumpAirAnimation::JumpGlide && !jump.State().AirAnimationFinishAutoRemove, "source standing/glide requests");
    Check(jump.State().WriteHorizontalSpeed && jump.State().MoveX == 0 && std::abs(jump.State().MoveY - 0.17f) < 0.000001f, "squared versus unsquared launch threshold");
    Check(jump.ResolveWorld(generation, world) == NativeSourceJumpStatus::InvalidInput, "force output cannot commit twice");
    Check(jump.BeginLanding(generation, 0.5f, 2, true, false, false, 1) == Ok && jump.State().RunningLand && !jump.State().InAir, "source running land selection");
    Check(jump.AdvanceAnimation(generation, 0.1f) == Ok && jump.State().RightFoot && !jump.State().LeftFoot, "right foot at .1 seconds");
    const auto marker = jump.State();
    Check(jump.State().RightFoot && jump.State() == marker, "foot predicate nonconsuming");
    Check(jump.AdvanceAnimation(generation, 0.1f) == Ok && !jump.State().RightFoot && jump.State().LeftFoot, "left foot at .2 seconds");
    Check(jump.AdvanceAnimation(generation, 0.3f) == Ok && jump.State().LandFinished && jump.State().Landing && !jump.State().ResetLocomotionTime, "land callback waits for ProcessPed");
    Check(jump.State().Animation.BlendDelta == -100 && !jump.State().HasAnimation, "running land finish blend/detach");
    Check(jump.CompleteLanding(generation) == Ok && jump.State().Phase == Phase::Finished && jump.State().ResetLocomotionTime && !jump.State().Landing, "source land process resets movement associations");
    Check(jump.Begin(start) == Ok && jump.State().Generation == generation + 1, "restart changes task generation");
    const auto restarted = jump.State();
    Check(jump.AdvanceAnimation(generation, 1) == NativeSourceJumpStatus::Stale && jump.State() == restarted, "stale callback frame rejected");
}
void Aborts() {
    NativeSourceJump jump;
    NativeSourceJumpStart start; start.LaunchDuration = 2;
    Check(jump.Begin(start) == Ok, "begin abort fixture");
    const auto generation = jump.State().Generation;
    Check(jump.AdvanceAnimation(generation, 0.125f) == Ok, "blend launch in");
    bool accepted = true;
    Check(jump.Abort(generation, Priority::Urgent, Event::Other, false, accepted) == Ok && !accepted, "urgent simple jump abort denied");
    Check(jump.State().Animation.BlendDelta == -4 && jump.State().Animation.BlendAutoRemove, "denied abort still requests fade per source");
    Check(jump.AdvanceAnimation(generation, 0.25f) == Ok && jump.State().Phase == Phase::AwaitWorld && jump.State().Animation.CurrentTime == 0.125f, "blend deletion finish callback before animation time end");
    Check(jump.Abort(generation, Priority::Urgent, Event::Death, false, accepted) == Ok && accepted && jump.State().Phase == Phase::Aborted, "complex urgent death bypasses simple refusal");
    Check(jump.Begin(start) == Ok, "restart abort fixture");
    const auto next = jump.State().Generation;
    Check(jump.Abort(next, Priority::Immediate, Event::None, false, accepted) == Ok && accepted && !jump.State().HasAnimation && !jump.State().Animation.FinishToken, "immediate task destruction detaches callback");
    CompleteLaunch(jump);
    NativeSourceJumpWorld world; world.Status = NativeSourceJumpWorldStatus::Ready;
    Check(jump.ResolveWorld(jump.State().Generation, world) == Ok, "air fixture");
    Check(jump.Abort(jump.State().Generation, Priority::Urgent, Event::Other, false, accepted) == Ok && !accepted, "ordinary in-air urgent refusal");
    Check(jump.Abort(jump.State().Generation, Priority::Urgent, Event::ScriptCommand71, false, accepted) == Ok && accepted, "source priority71 script abort");
}
void Decisions() {
    for (const bool above : {false, true}) {
        NativeSourceJump jump; NativeSourceJumpStart start = FixtureStart();
        start.Sprint = {true, above ? std::nextafter(0.3f, 1.0f) : 0.3f, 0.2f, 1};
        start.Run = {true, 1, 0.3f, 1};
        Check(jump.Begin(start) == Ok && jump.State().RightLaunch == above, "strict .3 association fallback/phase boundary");
    }
    NativeSourceJump denied; NativeSourceJumpStart start = FixtureStart(); start.ExistingLaunch = true;
    Check(denied.Begin(start) == Ok && denied.State().Phase == Phase::Finished && !denied.State().LaunchStarted, "existing launch prevents duplicate task animation");
    start.ExistingLaunch = false; start.SteepSlopeAgainstForward = true;
    Check(denied.Begin(start) == Ok && denied.State().SlopePitchRequested && !denied.State().CopyCurrentRotationToAim, "source rejected slope requests IK pitch, not aim rotation copy");
    NativeSourceJump blocked; CompleteLaunch(blocked);
    NativeSourceJumpWorld world; world.Status = NativeSourceJumpWorldStatus::Ready; world.JumpBlocked = true;
    Check(blocked.ResolveWorld(blocked.State().Generation, world) == Ok && blocked.State().Phase == Phase::HitHead && blocked.State().Landing && blocked.State().QuietBlockedSound && !blocked.State().UpwardForce, "blocked jump emits hit-head not force");
    Check(blocked.BeginHitHeadAnimation(blocked.State().Generation, 0.4f) == Ok, "source hit-wall association");
    Check(blocked.AdvanceAnimation(blocked.State().Generation, 0.4f) == Ok && blocked.State().HitHeadFinished && blocked.State().Landing, "head callback waits for task process");
    Check(blocked.CompleteHitHead(blocked.State().Generation) == Ok && blocked.State().Phase == Phase::Finished && !blocked.State().Landing, "head child finishes parent without locomotion reset");
    NativeSourceJump npc; start = FixtureStart(); start.Player = start.HasPlayerData = false;
    CompleteLaunch(npc, start); world.JumpBlocked = false;
    Check(npc.ResolveWorld(npc.State().Generation, world) == Ok && npc.State().UpwardForce == 4.5f, "source ordinary ped impulse");
    const auto saved = npc.State();
    Check(npc.BeginLanding(npc.State().Generation, std::numeric_limits<float>::quiet_NaN(), 1, false, false, false, 1) == NativeSourceJumpStatus::InvalidInput && npc.State() == saved, "invalid clip cannot partially transition");
    Check(npc.BeginLanding(npc.State().Generation, 1, 2, true, true, true, 1) == Ok && !npc.State().RunningLand && npc.State().Animation.Speed == 1, "NPC fall-land no player sprint modifier");
    NativeSourceJump changing; start = FixtureStart(); start.StatModifier = 0.5f; start.Sprint = {true, 1, 0, 1};
    Check(changing.Begin(start) == Ok && changing.AdvanceAnimation(changing.State().Generation, 2) == Ok, "initial slow launch animation");
    world = {}; world.Status = NativeSourceJumpWorldStatus::Ready; world.StatModifier = 2; world.MegaJump = true;
    world.Run = {true, 1, 0, 1};
    Check(changing.ResolveWorld(changing.State().Generation, world) == Ok && changing.State().UpwardForce == 170 &&
        std::abs(changing.State().MoveY - 0.34f) < 0.000001f, "launch refreshes current stats, cheat and movement associations");
    Check(changing.BeginLanding(changing.State().Generation, 1, 2, true, true, true, 0.75f) == Ok &&
        changing.State().Animation.Speed == 1.5f, "landing reads current stat modifier, not launch snapshot");
}
std::vector<NativeSourceAnimClip> SharedClips() {
    std::vector<NativeSourceAnimClip> clips;
    for (const int id : {0, 1, 2, 38, 116, 117, 119, 122}) {
        NativeSourceAnimClip clip;
        clip.Key = {0, id}; clip.Duration = id < 3 ? 1.0f : 0.5f; clip.Sequences = 1;
        clip.Synchronised = clip.Looped = id < 3;
        clip.Partial = clip.FinishAutoRemove = id >= 3;
        clips.push_back(clip);
    }
    return clips;
}
void SharedOwnership() {
    NativeSourceAnimClump clump;
    Check(clump.LoadClips(SharedClips()) == NativeSourceClumpStatus::Ok, "load shared synthetic associations");
    NativeSourceAnimHandle walk;
    Check(clump.Add({0, 0}, walk) == NativeSourceClumpStatus::Ok && clump.SetSpeed(walk, 0.5f) == NativeSourceClumpStatus::Ok, "clump owns movement association with independent cycle");
    std::vector<NativeSourceClumpEvent> events;
    NativeSourceAnimHandle launch;
    {
        NativeSourceJump jump(clump);
        Check(jump.Begin({}) == Ok && jump.State().Animation.TotalTime == 0.5f, "shared begin reads authored clip, no duration argument");
        const auto generation = jump.State().Generation;
        launch = clump.Find(116)->Handle;
        Check(jump.AdvanceAnimation(generation, 0.5f) == NativeSourceJumpStatus::InvalidInput && clump.Get(launch)->State.CurrentTime == 0, "task cannot advance the shared clump a second time");
        Check(clump.Update(0.5f, events) == NativeSourceClumpStatus::Ok && jump.ObserveAnimation(generation) == Ok && jump.State().LaunchFinished, "clump event drives task finish");
        Check(clump.Get(launch) && clump.Get(launch)->State.Alive && clump.Get(launch)->State.BlendDelta == -4, "task callback leaves live fading association");
        const auto held = jump.State();
        Check(jump.ObserveAnimation(generation) == Ok && jump.State() == held, "repeat observation idempotent");
        NativeSourceJumpWorld world; world.Status = NativeSourceJumpWorldStatus::Ready;
        Check(jump.ResolveWorld(generation, world) == Ok, "shared launch world predicate");
        Check(jump.BeginLanding(generation, 0, 2, true, false, false, 1) == Ok, "land reads shared authored duration");
        Check(clump.Update(0.1f, events) == NativeSourceClumpStatus::Ok && jump.ObserveAnimation(generation) == Ok && jump.State().RightFoot, "shared right foot");
        Check(clump.Update(0.1f, events) == NativeSourceClumpStatus::Ok && jump.ObserveAnimation(generation) == Ok && jump.State().LeftFoot, "shared left foot");
        Check(clump.Update(0.3f, events) == NativeSourceClumpStatus::Ok && jump.ObserveAnimation(generation) == Ok && jump.State().LandFinished, "shared land callback");
        Check(clump.Find(119)->State.BlendDelta == -100, "land callback mutates actual owner fade");
        Check(clump.Get(walk)->State.CurrentTime > 0, "walk clock continued independently");
        Check(jump.CompleteLanding(generation) == Ok && clump.Get(walk)->State.CurrentTime == 0, "source ProcessPed resets actual movement time");
    }
    Check(clump.Find(119) && clump.Get(walk), "task destructor does not own clump associations");
    Check(clump.Update(0.25f, events) == NativeSourceClumpStatus::Ok && !clump.Find(119), "clump later retires land animation");
    {
        NativeSourceJump jump(clump);
        Check(jump.Begin({}) == Ok, "second shared task");
        const int id = jump.State().RightLaunch ? 117 : 116;
        launch = clump.Find(id)->Handle;
        bool accepted = true;
        Check(jump.Abort(jump.State().Generation, Priority::Leisure, Event::None, false, accepted) == Ok && !accepted &&
            clump.Get(launch)->State.BlendDelta == -4, "denied abort fades actual clump association");
    }
    Check(clump.Get(launch) && !clump.Get(launch)->State.FinishToken, "task destruction detaches callback only");
    Check(clump.Update(0.25f, events) == NativeSourceClumpStatus::Ok && events.size() == 1 && !events[0].Event.FinishToken, "late fade has no dangling task callback");
    NativeSourceJump missed(clump);
    Check(missed.Begin({}) == Ok, "missed-update fixture");
    Check(clump.Update(0.1f, events) == NativeSourceClumpStatus::Ok && clump.Update(0.1f, events) == NativeSourceClumpStatus::Ok &&
        missed.ObserveAnimation(missed.State().Generation) == NativeSourceJumpStatus::Stale, "skipped callback delivery cannot silently advance task");
}
}
int main() {
    Flow(); Aborts(); Decisions(); SharedOwnership();
    std::printf("source-jump-ok checks=%zu source-task-flow synthetic-world-predicates no-physics-host\n", s_Checks);
}
