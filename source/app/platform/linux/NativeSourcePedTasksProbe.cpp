#include "NativeSourcePedTasks.h"
#include <cstdio>
#include <cstdlib>
#include <algorithm>

namespace {
std::size_t s_Checks = 0;
void Check(bool ok, const char* message) {
    ++s_Checks;
    if (!ok) { std::fprintf(stderr, "source-ped-tasks FAIL: %s\n", message); std::exit(1); }
}
std::vector<NativeSourceAnimClip> Clips() {
    std::vector<NativeSourceAnimClip> clips;
    for (int id : {0, 1, 3, 5, 38, 116, 117, 118, 119, 122}) {
        NativeSourceAnimClip a;
        a.Key = {0, id}; a.Duration = id == 119 || id == 122 ? 0.4f : id >= 38 ? 0.2f : 1;
        a.Sequences = 1;
        a.Synchronised = id < 2;
        a.Looped = id < 4;
        a.Partial = id >= 38;
        a.FinishAutoRemove = id >= 38 && id != 118;
        a.BlendAutoRemove = id == 118;
        clips.push_back(a);
    }
    return clips;
}
struct Fixture {
    NativeSourcePedTaskContext Context;
    NativeSourceAnimClump Clump;
    NativeSourceTaskManager Tasks; // destroys tasks before clump/context
    Fixture() {
        Check(Clump.LoadClips(Clips()) == NativeSourceClumpStatus::Ok, "explicit synthetic source clip profile");
        Check(Tasks.SetPrimary(NativePlayerPrimarySlot::Default, NativeSourceMakePlayerOnFootTask(Context, Clump, Tasks)) == NativeSourceTaskStatus::Ok, "install production ordinary on-foot task");
        Context.MoveSample.TimeStep = 1;
    }
    void Tick(float seconds) {
        // Explicit fixture phases; no claim about a hosted full-game frame.
        std::vector<NativeSourceClumpEvent> events;
        Check(Clump.Update(seconds, events) == NativeSourceClumpStatus::Ok, "single clump advance");
        Check(Tasks.NotifyAnimations() == NativeSourceTaskStatus::Ok, "notify every attached task");
        Check(Tasks.Manage() == NativeSourceTaskStatus::Ok, "one source task pass");
    }
    void RequestJump() {
        Context.JumpJustDown = true;
        Tick(0.05f);
        Context.JumpJustDown = false;
        Check(Tasks.Active()->Type() == 211 && NativeSourceTaskManager::Last(Tasks.Active())->Type() == 210, "on-foot installs source complex/simple jump tree");
        Tick(0.05f);
        Check(Context.Jump.Phase == NativeSourceJumpPhase::Launch, "simple jump starts on next task pass");
    }
};
void Route() {
    Fixture f;
    f.Tick(0);
    Check(f.Clump.Find(3) && f.Context.Movement.Move == NativeSourceMoveState::Still, "on-foot authored idle");
    f.Context.MoveSample.UpDown = -120;
    for (int i = 0; i < 35; ++i) f.Tick(0.05f);
    Check(f.Context.MoveBlend.Ratio == 2 && f.Context.Movement.Move == NativeSourceMoveState::Run && f.Clump.Find(1)->State.BlendAmount == 1, "source ramp through start/walk to run");
    f.RequestJump();
    // Five quarter-clip ticks avoid relying on summed .05 reaching .2 exactly.
    for (int i = 0; i < 5; ++i) f.Tick(0.05f);
    Check(f.Context.Jump.LaunchFinished && f.Context.Frontier == NativeSourcePedFrontier::UnsupportedWorld && !f.Context.Jump.UpwardForce,
        "missing source collision authority stops after animation, before launch force");
    f.Context.JumpWorld.Status = NativeSourceJumpWorldStatus::Ready; // explicit synthetic observation
    f.Context.AirStatus = NativeSourceJumpWorldStatus::Pending;
    f.Tick(0.05f);
    Check(f.Context.Jump.UpwardForce == 8.5f && f.Context.InAir && f.Tasks.Active()->Type() == 211 &&
        f.Tasks.Active()->Child()->Type() == 240 && NativeSourceTaskManager::Last(f.Tasks.Active())->Type() == 241,
        "source jump -> in-air-and-land -> in-air tree");
    Check(f.Context.Frontier == NativeSourcePedFrontier::PendingWorld, "in-air physical query remains pending");
    Check(f.Clump.Find(118) && f.Clump.Find(118)->State.DeleteToken && f.Clump.Find(118)->State.BlendDelta == 8, "launch creates real clump glide; in-air child owns delete callback");
    f.Context.AirStatus = NativeSourceJumpWorldStatus::Ready;
    f.Context.Landed = true;
    f.Tick(0.05f);
    Check(f.Context.Jump.Landing && !f.Context.InAir && NativeSourceTaskManager::Last(f.Tasks.Active())->Type() == 242, "source contact decision advances to landing in same task pass");
    Check(f.Clump.Find(118) && !f.Clump.Find(118)->State.DeleteToken, "air child destruction detaches callback without deleting fading glide");
    f.Tick(0.1f);
    Check(f.Context.Jump.RightFoot && !f.Context.Jump.LeftFoot, "integrated right-foot marker");
    f.Tick(0.1f);
    Check(f.Context.Jump.LeftFoot && !f.Context.Jump.RightFoot, "integrated left-foot marker");
    f.Tick(0.2f);
    Check(f.Tasks.Active()->Type() == 0 && f.Context.Jump.Phase == NativeSourceJumpPhase::Finished && f.Context.Jump.ResetLocomotionTime,
        "finished child chain unwinds, retained default task resumes later");
    Check(f.Clump.Find(119) && !f.Clump.Find(119)->State.FinishToken, "finished task leaves clump-owned landing fade");
    f.Context.MoveSample.UpDown = 0;
    f.Tick(0.25f);
    Check(f.Context.Movement.Move == NativeSourceMoveState::Still && f.Clump.Find(3) && !f.Clump.Find(119), "ordinary stop resumes through source idle");
}
void InterruptionAndGuards() {
    Fixture f;
    f.Tick(0);
    f.RequestJump();
    const int launchId = f.Context.Jump.RightLaunch ? 117 : 116;
    Check(!f.Tasks.Active()->MakeAbortable(NativeSourceAbortPriority::Urgent, nullptr) && f.Clump.Find(launchId)->State.BlendDelta == -4,
        "source urgent jump refusal still fades actual animation");
    f.Tick(0.1f);
    Check(f.Context.Jump.LaunchFinished && f.Context.Frontier == NativeSourcePedFrontier::UnsupportedWorld, "interruption fade deletion delivers callback through task manager");
    Check(f.Tasks.Active()->MakeAbortable(NativeSourceAbortPriority::Immediate, nullptr) && !f.Context.InAir, "immediate jump abort accepted");
    Check(f.Tasks.SetPrimary(NativePlayerPrimarySlot::Primary, nullptr) == NativeSourceTaskStatus::Ok && f.Tasks.Active()->Type() == 0, "caller destroys aborted primary, retains on-foot");
    f.Context.JumpJustDown = true; f.Context.HeavyWeapon = true;
    f.Tick(0.25f);
    Check(f.Tasks.Active()->Type() == 0, "heavy weapon blocks source jump request");
    f.Context.HeavyWeapon = false; f.Context.Targeting = true;
    f.Tick(0.05f);
    Check(f.Tasks.Active()->Type() == 0, "targeting blocks jump");
    f.Context.Targeting = false; f.Context.CameraPreventsJump = true;
    f.Tick(0.05f);
    Check(f.Tasks.Active()->Type() == 0, "source camera mode blocks jump");
    f.Context.CameraPreventsJump = false; f.Context.MoveSample.Attached = true;
    f.Tick(0.05f);
    Check(f.Tasks.Active()->Type() == 0, "attachment blocks jump");
    f.Context.MoveSample.Attached = false; f.Context.JumpJustDown = false; f.Context.Locomotion.SprintRequested = true;
    const auto before = f.Context.MoveBlend;
    f.Tick(0.05f);
    Check(f.Context.Frontier == NativeSourcePedFrontier::UnsupportedControl && f.Context.MoveBlend == before, "unsupported sprint branch does not become ordinary source run");
}
void TypedDeath() {
    for (const bool started : {false, true}) {
        Fixture f;
        Check(f.Tasks.SetPrimary(NativePlayerPrimarySlot::Primary, NativeSourceMakeJumpTask(f.Context, f.Clump)) == NativeSourceTaskStatus::Ok, "typed death jump task");
        if (started) f.Tick(0);
        NativeSourceTaskEvent event;
        event.JumpAbort = NativeSourceAbortEvent::Death; // synthetic, explicitly typed source observation
        Check(f.Tasks.Active()->MakeAbortable(NativeSourceAbortPriority::Urgent, &event) && !f.Context.InAir, "urgent death bypasses launch abort refusal, even before ProcessPed");
        Check(f.Tasks.Flush() == NativeSourceTaskStatus::Ok && (!f.Clump.Find(116) || !f.Clump.Find(116)->State.FinishToken), "death teardown leaves no dangling animation callback");
    }
}
class PhysicalResponse final : public NativeSourceSimpleTask {
public:
    int Type() const override { return 230; }
    bool ProcessPed() override { return false; }
    bool MakeAbortable(NativeSourceAbortPriority, const NativeSourceTaskEvent*) override { return true; }
};
void InactiveProductionTask() {
    Fixture f;
    f.Tick(0);
    f.RequestJump();
    Check(f.Tasks.SetPrimary(NativePlayerPrimarySlot::PhysicalResponse, std::make_unique<PhysicalResponse>()) == NativeSourceTaskStatus::Ok, "physical response preempts production jump");
    f.Tick(0.25f);
    Check(f.Tasks.Active()->Type() == 230 && f.Context.Jump.LaunchFinished && f.Context.Jump.Phase == NativeSourceJumpPhase::AwaitWorld,
        "production jump receives finish while its task slot is inactive");
    Check(f.Tasks.SetPrimary(NativePlayerPrimarySlot::PhysicalResponse, nullptr) == NativeSourceTaskStatus::Ok, "remove physical response");
    f.Tick(0);
    Check(f.Context.Frontier == NativeSourcePedFrontier::UnsupportedWorld && f.Tasks.Active()->Type() == 211, "resumed task reaches real missing-world frontier, not stale animation");
    Check(f.Tasks.Flush() == NativeSourceTaskStatus::Ok && !f.Tasks.Active(), "flush integrated task families");
}
void BlockedAndAirAbort() {
    for (const bool blocked : {false, true}) {
        Fixture f;
        f.Context.JumpWorld.Status = NativeSourceJumpWorldStatus::Ready; // synthetic source query result
        f.Context.JumpWorld.JumpBlocked = blocked;
        f.RequestJump();
        f.Tick(0.2f);
        if (blocked) {
            Check(NativeSourceTaskManager::Last(f.Tasks.Active())->Type() == 500 && f.Context.Jump.QuietBlockedSound && !f.Context.Jump.UpwardForce,
                "source blocked jump installs hit-head child, not an in-air force");
            f.Tick(0.2f);
            Check(f.Tasks.Active()->Type() == 0 && !f.Context.Jump.Landing && !f.Context.Jump.ResetLocomotionTime, "hit-head completion returns to retained on-foot without landing reset");
        } else {
            Check(NativeSourceTaskManager::Last(f.Tasks.Active())->Type() == 241 && f.Clump.Find(118)->State.DeleteToken, "air abort fixture owns glide callback");
            Check(f.Tasks.Active()->MakeAbortable(NativeSourceAbortPriority::Immediate, nullptr) &&
                f.Clump.Find(118)->State.BlendDelta == -8 && !f.Clump.Find(118)->State.DeleteToken, "source in-air abort fades glide at minus eight and detaches callback");
            Check(f.Tasks.SetPrimary(NativePlayerPrimarySlot::Primary, nullptr) == NativeSourceTaskStatus::Ok, "destroy aborted air subtree");
            f.Tick(0.25f);
            Check(!f.Clump.Find(118), "aborted air animation retired by clump");
        }
    }
    Fixture missing;
    auto clips = Clips();
    std::erase_if(clips, [](const auto& clip) { return clip.Key.Animation == 118; });
    Check(missing.Clump.LoadClips(clips) == NativeSourceClumpStatus::Ok, "fixture without required glide metadata");
    missing.Context.JumpWorld.Status = NativeSourceJumpWorldStatus::Ready;
    missing.RequestJump();
    missing.Tick(0.2f);
    Check(missing.Context.Frontier == NativeSourcePedFrontier::MissingClip && !missing.Context.Jump.UpwardForce &&
        NativeSourceTaskManager::Last(missing.Tasks.Active())->Type() == 210, "missing glide blocks before launch force and child transition");
}
}
int main() {
    Route(); InterruptionAndGuards(); TypedDeath(); InactiveProductionTask(); BlockedAndAirAbort();
    std::printf("source-ped-tasks-ok checks=%zu ordinary-task-integration synthetic-contact-observations no-gameplay-host\n", s_Checks);
}
