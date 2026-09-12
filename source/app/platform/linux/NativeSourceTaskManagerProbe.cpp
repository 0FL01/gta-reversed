#include "NativeSourceTaskManager.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace {
constexpr auto Ok = NativeSourceTaskStatus::Ok;
using Primary = NativePlayerPrimarySlot;
using Secondary = NativePlayerSecondarySlot;
std::size_t s_Checks = 0;
void Check(bool ok, const char* message) {
    ++s_Checks;
    if (!ok) { std::fprintf(stderr, "source-task-manager FAIL: %s\n", message); std::exit(1); }
}
struct Audit {
    std::vector<int> Processed, Destroyed, Aborted, Stopped;
};
class Leaf : public NativeSourceSimpleTask {
public:
    Leaf(Audit& audit, int type, bool finish, bool abort = true) : m_Audit(audit), m_Type(type), m_Finish(finish), m_Abort(abort) {}
    ~Leaf() override { m_Audit.Destroyed.push_back(m_Type); }
    int Type() const override { return m_Type; }
    bool ProcessPed() override { m_Audit.Processed.push_back(m_Type); return m_Finish; }
    bool MakeAbortable(NativeSourceAbortPriority, const NativeSourceTaskEvent*) override { m_Audit.Aborted.push_back(m_Type); return m_Abort; }
    void StopTimer(const NativeSourceTaskEvent*) override { m_Audit.Stopped.push_back(m_Type); }
protected:
    Audit& m_Audit;
private:
    int m_Type;
    bool m_Finish, m_Abort;
};
class Chain : public NativeSourceComplexTask {
public:
    Chain(Audit& audit, int count) : m_Audit(audit), m_Count(count) {}
    int Type() const override { return 100; }
    NativeSourceTaskPtr CreateFirstSubTask() override { return Next(); }
    NativeSourceTaskPtr CreateNextSubTask() override { return Next(); }
private:
    NativeSourceTaskPtr Next() { return m_Next <= m_Count ? std::make_unique<Leaf>(m_Audit, m_Next++, true) : nullptr; }
    Audit& m_Audit;
    int m_Count, m_Next = 1;
};
class Replacing : public NativeSourceComplexTask {
public:
    explicit Replacing(Audit& audit) : m_Audit(audit) {}
    int Type() const override { return 101; }
    NativeSourceTaskPtr CreateFirstSubTask() override { return std::make_unique<Leaf>(m_Audit, 20, false, false); }
    NativeSourceTaskPtr CreateNextSubTask() override { return nullptr; }
    NativeSourceTaskControl ControlSubTask() override {
        if (!m_Replaced) { m_Replaced = true; return {true, std::make_unique<Leaf>(m_Audit, 21, false)}; }
        return {};
    }
private:
    Audit& m_Audit;
    bool m_Replaced = false;
};
class Installing : public Leaf {
public:
    Installing(Audit& audit, NativeSourceTaskManager& owner) : Leaf(audit, 80, false), m_Owner(owner) {}
    bool ProcessPed() override {
        Check(m_Owner.Manage() == NativeSourceTaskStatus::Busy, "recursive Manage rejected");
        NativeSourceTaskPtr replacement = std::make_unique<Leaf>(m_Audit, 81, false);
        Check(m_Owner.SetPrimary(Primary::Default, std::move(replacement)) == NativeSourceTaskStatus::Busy && replacement, "executing task cannot destroy itself; rejected replacement remains owned");
        Check(m_Owner.SetPrimary(Primary::Primary, std::move(replacement)) == Ok, "default on-foot may install higher primary during ProcessPed");
        return Leaf::ProcessPed();
    }
private:
    NativeSourceTaskManager& m_Owner;
};
class Nested : public NativeSourceComplexTask {
public:
    Nested(Audit& audit, int count) : m_Audit(audit), m_Count(count) {}
    int Type() const override { return 102; }
    NativeSourceTaskPtr CreateFirstSubTask() override { return std::make_unique<Chain>(m_Audit, m_Count); }
    NativeSourceTaskPtr CreateNextSubTask() override {
        if (!m_Next) { m_Next = true; return std::make_unique<Leaf>(m_Audit, 60, true); }
        return nullptr;
    }
private:
    Audit& m_Audit;
    int m_Count;
    bool m_Next = false;
};
class FinishingControl : public NativeSourceComplexTask {
public:
    explicit FinishingControl(Audit& audit) : m_Audit(audit) {}
    int Type() const override { return 103; }
    NativeSourceTaskPtr CreateFirstSubTask() override { return std::make_unique<Leaf>(m_Audit, 70, false); }
    NativeSourceTaskPtr CreateNextSubTask() override { return nullptr; }
    NativeSourceTaskControl ControlSubTask() override { return {true, nullptr}; }
private:
    Audit& m_Audit;
};
void Scheduling() {
    Audit audit;
    NativeSourceTaskManager tasks;
    Check(tasks.SetPrimary(Primary::Default, std::make_unique<Leaf>(audit, 90, false)) == Ok, "default slot");
    Check(tasks.SetPrimary(Primary::Primary, std::make_unique<Chain>(audit, 12)) == Ok, "complex primary children initialized immediately");
    Check(tasks.Active()->Type() == 100 && NativeSourceTaskManager::Last(tasks.Active())->Type() == 1 &&
        NativeSourceTaskManager::Last(tasks.Active())->Parent() == tasks.Active(), "root/child parent ownership");
    Check(tasks.FindActive(90) == nullptr && tasks.FindActive(1), "active search excludes dormant primaries");
    Check(tasks.Manage() == Ok && audit.Processed.size() == 10 && audit.Processed.front() == 1 && audit.Processed.back() == 10, "source ten-transition primary ceiling");
    Check(NativeSourceTaskManager::Last(tasks.Active())->Type() == 11, "eleventh child exists but waits for next Manage");
    Check(tasks.Manage() == Ok && audit.Processed.size() == 12 && tasks.Active()->Type() == 90, "complete root without processing default same frame");
    Check(tasks.Manage() == Ok && audit.Processed.back() == 90, "default resumes next call");
    Check(tasks.SetSecondary(Secondary::Attack, std::make_unique<Chain>(audit, 12)) == Ok, "secondary chain");
    const auto before = audit.Processed.size();
    Check(tasks.Manage() == Ok && audit.Processed.size() == before + 13 && !tasks.Secondary(Secondary::Attack), "secondary drains all twelve transitions, no invented ten-step cap");
    Check(tasks.SetPrimary(Primary::PhysicalResponse, std::make_unique<Leaf>(audit, 10, false)) == Ok && tasks.Active()->Type() == 10, "source primary slot order");
    Check(tasks.SetSecondary(Secondary::Attack, std::make_unique<Leaf>(audit, 30, false)) == Ok &&
        tasks.SetSecondary(Secondary::Duck, std::make_unique<Leaf>(audit, 31, false)) == Ok, "attack and duck slots");
    audit.Processed.clear();
    Check(tasks.Manage() == Ok && audit.Processed == std::vector<int>{10, 30, 31}, "primary then attack then duck order");
    Check(tasks.FindActive(31) == tasks.Secondary(Secondary::Duck), "secondary active lookup");
    NativeSourceTaskPtr rejected = std::make_unique<Leaf>(audit, 99, false);
    Check(tasks.SetPrimary(Primary::Count, std::move(rejected)) == NativeSourceTaskStatus::InvalidSlot && rejected, "invalid index preserves caller owner");
}
void ControlAndTeardown() {
    Audit audit;
    NativeSourceTaskManager tasks;
    Check(tasks.SetPrimary(Primary::Primary, std::make_unique<Replacing>(audit)) == Ok && tasks.Manage() == Ok, "parent replacement processed");
    Check(audit.Aborted == std::vector<int>{20} && audit.Destroyed == std::vector<int>{20} && audit.Processed == std::vector<int>{21}, "source parent replaces child even after abort refusal");
    Check(tasks.SetPrimary(Primary::EventResponseTemp, std::make_unique<Leaf>(audit, 40, false)) == Ok &&
        tasks.SetPrimary(Primary::EventResponseNonTemp, std::make_unique<Leaf>(audit, 41, false)) == Ok, "event response tasks");
    Check(tasks.ClearTaskEventResponse() == Ok && !tasks.Primary(Primary::EventResponseTemp) && tasks.Primary(Primary::EventResponseNonTemp), "source clear removes temp only on first call");
    Check(tasks.ClearTaskEventResponse() == Ok && !tasks.Primary(Primary::EventResponseNonTemp), "second clear removes nontemp");
    Check(tasks.Flush() == Ok && tasks.SetPrimary(Primary::Default, std::make_unique<Leaf>(audit, 50, false, false)) == Ok &&
        tasks.SetSecondary(Secondary::Attack, std::make_unique<Leaf>(audit, 51, false)) == Ok, "flush fixture");
    Check(tasks.StopTimers(nullptr) == Ok && audit.Stopped == std::vector<int>{50}, "StopTimers targets primary roots only");
    Check(tasks.FlushImmediately() == Ok && tasks.Active() && !tasks.Secondary(Secondary::Attack), "immediate flush retains refusing task");
    Check(tasks.Flush() == Ok && !tasks.Active(), "plain flush destroys regardless of abort refusal");
    Check(tasks.SetPrimary(Primary::Primary, std::make_unique<Chain>(audit, 0)) == Ok && !tasks.Active(), "empty complex tree deleted at installation");
    Check(tasks.SetPrimary(Primary::Default, std::make_unique<Installing>(audit, tasks)) == Ok && tasks.Manage() == Ok && tasks.Active()->Type() == 81, "source new primary waits without corrupting current callback");
    audit.Destroyed.clear();
    Check(tasks.Flush() == Ok && audit.Destroyed == std::vector<int>{81, 80}, "source primary destruction order");
}
void NestedTrees() {
    for (const int children : {0, 2}) {
        Audit audit;
        NativeSourceTaskManager tasks;
        Check(tasks.SetPrimary(Primary::Primary, std::make_unique<Nested>(audit, children)) == Ok && tasks.Active(), "nested first-task creation");
        Check(tasks.Manage() == Ok && !tasks.Active(), "nested completion ascends to parent successor");
        Check(audit.Processed == (children ? std::vector<int>{1, 2, 60} : std::vector<int>{60}), "empty nested creation and normal nested completion share source successor path");
        Check(audit.Processed == audit.Destroyed, "completed simple children destroyed once in source order");
    }
    Audit audit;
    {
        NativeSourceTaskManager tasks;
        Check(tasks.SetPrimary(Primary::Primary, std::make_unique<FinishingControl>(audit)) == Ok && tasks.Manage() == Ok && !tasks.Active(), "null parent-control replacement finishes root");
        Check(audit.Processed.empty() && audit.Aborted == std::vector<int>{70} && audit.Destroyed == std::vector<int>{70}, "null replacement never processes deleted child");
        Check(tasks.SetPrimary(Primary::Default, std::make_unique<Leaf>(audit, 71, false)) == Ok && tasks.SetSecondary(Secondary::Attack, std::make_unique<Leaf>(audit, 72, false)) == Ok, "destructor-order fixture");
    }
    Check(audit.Destroyed == std::vector<int>{70, 71, 72}, "manager destructor flushes primary before secondary");
}
class JumpLeaf : public NativeSourceSimpleTask {
public:
    explicit JumpLeaf(NativeSourceAnimClump& clump) : Jump(clump) { Check(Jump.Begin({}) == NativeSourceJumpStatus::Ok, "slot jump initialized from clip owner"); }
    int Type() const override { return 210; }
    bool ProcessPed() override { return Jump.State().LaunchFinished; }
    bool MakeAbortable(NativeSourceAbortPriority priority, const NativeSourceTaskEvent*) override {
        bool accepted = false;
        return Jump.Abort(Jump.State().Generation, priority, NativeSourceAbortEvent::None, false, accepted) == NativeSourceJumpStatus::Ok && accepted;
    }
    NativeSourceJump Jump;
};
void ClumpLifetime() {
    NativeSourceAnimClump clump;
    NativeSourceAnimClip clip;
    clip.Key = {0, 116}; clip.Duration = 0.2f; clip.Sequences = 1; clip.Partial = clip.FinishAutoRemove = true;
    Check(clump.LoadClips({clip}) == NativeSourceClumpStatus::Ok, "slot fixture synthetic clip");
    NativeSourceTaskManager tasks;
    auto jump = std::make_unique<JumpLeaf>(clump);
    auto* observe = jump.get();
    Check(tasks.SetPrimary(Primary::Primary, std::move(jump)) == Ok && tasks.Manage() == Ok && tasks.Active(), "task slot waits for clump callback");
    std::vector<NativeSourceClumpEvent> events;
    Check(clump.Update(0.2f, events) == NativeSourceClumpStatus::Ok && observe->Jump.ObserveAnimation(observe->Jump.State().Generation) == NativeSourceJumpStatus::Ok, "deliver callback before managing tasks");
    Check(tasks.Manage() == Ok && !tasks.Active() && clump.Find(116), "slot deletes completed task but not its fading association");
    Check(clump.Update(0.25f, events) == NativeSourceClumpStatus::Ok && !clump.Find(116), "clump owns final retirement");
    Check(tasks.SetPrimary(Primary::Primary, std::make_unique<JumpLeaf>(clump)) == Ok && tasks.Flush() == Ok &&
        clump.Find(116) && !clump.Find(116)->State.FinishToken, "forced slot teardown detaches task callback only");
}
}
int main() {
    Scheduling(); ControlAndTeardown(); NestedTrees(); ClumpLifetime();
    std::printf("source-task-manager-ok checks=%zu owned-slot-flow no-full-ped-host-claim\n", s_Checks);
}
