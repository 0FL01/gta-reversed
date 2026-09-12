#include "IfpAnim.h"
#include "NativeSourceAnimClump.h"
#include "NativeSourceJump.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace {
std::size_t s_Checks = 0;
void Check(bool ok, const char* message) {
    ++s_Checks;
    if (!ok) { std::fprintf(stderr, "source-clump-assets FAIL: %s\n", message); std::exit(1); }
}
}
int main(int argc, char** argv) {
    if (argc != 2) { std::fprintf(stderr, "usage: sa_source_clump_assets_probe owned-game-dir\n"); return 2; }
    IfpAnimPlayerBank bank;
    char error[512]{};
    IfpAnimClipInfo info{"retained", 7, 8};
    const auto initial = info;
    Check(!bank.Describe("JUMP_launch", info, error, sizeof(error)) && info == initial, "unloaded bank preserves output");
    Check(bank.Load(argv[1], "ped", error, sizeof(error)), error);
    // Source aStdAnimations/aStdAnimDescs; no name-keyword flag inference.
    struct Row { int Id; const char* Name; bool Sync, Loop, Partial, FinishRemove, BlendRemove; };
    const Row rows[] = {
        {0, "Walk_civi", true, true, false, false, false},
        {1, "run_civi", true, true, false, false, false},
        {2, "sprint_panic", true, true, false, false, false},
        {3, "idle_stance", false, true, false, false, false},
        {5, "walk_start", false, false, false, false, false},
        {6, "run_stop", false, false, false, false, true},
        {7, "run_stopR", false, false, false, false, true},
        {38, "HIT_wall", false, false, true, true, false},
        {116, "JUMP_launch", false, false, true, true, false},
        {117, "JUMP_launch_R", false, false, true, true, false},
        {118, "JUMP_glide", false, false, true, false, true},
        {119, "JUMP_land", false, false, true, true, false},
        {122, "FALL_land", false, false, true, true, false},
    };
    std::vector<NativeSourceAnimClip> clips;
    for (const auto& row : rows) {
        Check(bank.Describe(row.Name, info, error, sizeof(error)), error);
        Check(std::isfinite(info.Duration) && info.Duration > 0 && info.Sequences > 0, "real clip supports task timing");
        NativeSourceAnimClip clip;
        clip.Key = {0, row.Id}; clip.Duration = float(info.Duration); clip.Sequences = std::uint32_t(info.Sequences);
        clip.Synchronised = row.Sync; clip.Looped = row.Loop; clip.Partial = row.Partial;
        clip.FinishAutoRemove = row.FinishRemove; clip.BlendAutoRemove = row.BlendRemove;
        clips.push_back(clip);
        std::printf("source-clip id=%d name=%s duration=%.9g sequences=%zu\n", row.Id, info.Name.c_str(), info.Duration, info.Sequences);
    }
    const auto retained = info;
    Check(!bank.Describe("not-a-source-clip", info, error, sizeof(error)) && info == retained, "missing clip cannot select default");
    Check(!bank.Describe(nullptr, info, error, sizeof(error)) && info == retained, "null lookup atomic");
    NativeSourceAnimClump clump;
    Check(clump.LoadClips(clips) == NativeSourceClumpStatus::Ok, "real reader metadata adopted by core");
    NativeSourceAnimHandle launch;
    Check(clump.Blend({0, 116}, 8, launch) == NativeSourceClumpStatus::Ok && clump.BindFinish(launch, 17) == NativeSourceClumpStatus::Ok, "source launch association");
    const float duration = clump.Get(launch)->State.TotalTime;
    std::vector<NativeSourceClumpEvent> events;
    Check(clump.Update(duration / 2, events) == NativeSourceClumpStatus::Ok && events.empty(), "real clip no invented half-time marker");
    Check(clump.Update(duration / 2, events) == NativeSourceClumpStatus::Ok && events.size() == 1 && events[0].Event.FinishToken == 17, "real clip exact finish");
    Check(clump.Update(0.25f, events) == NativeSourceClumpStatus::Ok && !clump.Get(launch), "real association post-task retirement");
    {
        NativeSourceJump jump(clump);
        Check(jump.Begin({}) == NativeSourceJumpStatus::Ok && jump.State().Animation.TotalTime == duration, "task uses real shared launch metadata");
        const auto generation = jump.State().Generation;
        Check(clump.Update(duration, events) == NativeSourceClumpStatus::Ok && jump.ObserveAnimation(generation) == NativeSourceJumpStatus::Ok &&
            jump.State().Phase == NativeSourceJumpPhase::AwaitWorld, "real clump callback reaches task");
        NativeSourceJumpWorld world;
        Check(jump.ResolveWorld(generation, world) == NativeSourceJumpStatus::Unsupported, "real animation does not supply missing physics authority");
        world.Status = NativeSourceJumpWorldStatus::Ready; // isolated synthetic world observation
        Check(jump.ResolveWorld(generation, world) == NativeSourceJumpStatus::Ok &&
            jump.BeginLanding(generation, 0, 2, true, false, false, 1) == NativeSourceJumpStatus::Ok, "synthetic world observation selects real land clip");
        const float landDuration = jump.State().Animation.TotalTime;
        Check(landDuration == float(7.0 / 30.0), "authored jump land duration, not launch duration");
        Check(clump.Update(0.1f, events) == NativeSourceClumpStatus::Ok && jump.ObserveAnimation(generation) == NativeSourceJumpStatus::Ok && jump.State().RightFoot, "real land right-foot marker");
        Check(clump.Update(0.1f, events) == NativeSourceClumpStatus::Ok && jump.ObserveAnimation(generation) == NativeSourceJumpStatus::Ok && jump.State().LeftFoot, "real land left-foot marker");
        Check(clump.Update(landDuration - 0.2f, events) == NativeSourceClumpStatus::Ok && jump.ObserveAnimation(generation) == NativeSourceJumpStatus::Ok && jump.State().LandFinished, "real land finish callback");
        Check(jump.CompleteLanding(generation) == NativeSourceJumpStatus::Ok && clump.Find(119), "finished task leaves real clump animation alive");
    }
    Check(clump.Update(0.25f, events) == NativeSourceClumpStatus::Ok && !clump.Find(119), "actual clump retires real land after task destruction");
    std::printf("source-clump-assets-ok checks=%zu clips=%zu reader-metadata-only no-physics-or-pose-claim\n", s_Checks, clips.size());
}
