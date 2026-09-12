// Owned source task-tree/slot control. No CPed global, original addresses or
// presentation pointers. Task bodies capture their explicit gameplay owner.
#pragma once
#include "NativePlayerActivity.h"
#include "NativeSourceJump.h"
#include <array>
#include <memory>

class NativeSourceTask;
class NativeSourceComplexTask;
using NativeSourceTaskPtr = std::unique_ptr<NativeSourceTask>;

struct NativeSourceTaskEvent {
    std::int32_t Type = 0, Priority = 0;
    // Qualification belongs to the source event owner. FatalFallDamage includes
    // WEAPON_FALL + health zero + AddToEventGroup; ScriptCommand71 includes its
    // exact priority. Unclassified events are Other, never inferred from text.
    NativeSourceAbortEvent JumpAbort = NativeSourceAbortEvent::Other;
};
class NativeSourceTask {
public:
    virtual ~NativeSourceTask() = default;
    NativeSourceTask(const NativeSourceTask&) = delete;
    NativeSourceTask& operator=(const NativeSourceTask&) = delete;
    virtual bool IsSimple() const = 0;
    virtual std::int32_t Type() const = 0;
    virtual bool MakeAbortable(NativeSourceAbortPriority priority, const NativeSourceTaskEvent* event) = 0;
    virtual void StopTimer(const NativeSourceTaskEvent*) {}
    // Owned replacement for clump callbacks reaching tasks independently of
    // which primary slot currently runs. Must not create/destroy task roots.
    virtual bool ObserveAnimationUpdate() { return true; }
    virtual NativeSourceTask* Child() const = 0;
    NativeSourceComplexTask* Parent() const { return m_Parent; }
protected:
    NativeSourceTask() = default;
private:
    friend class NativeSourceComplexTask;
    NativeSourceComplexTask* m_Parent = nullptr; // nonowning internal link
};
class NativeSourceSimpleTask : public NativeSourceTask {
public:
    bool IsSimple() const final { return true; }
    NativeSourceTask* Child() const final { return nullptr; }
    virtual bool ProcessPed() = 0;
};
struct NativeSourceTaskControl {
    bool Replace = false;
    NativeSourceTaskPtr Replacement;
};
class NativeSourceComplexTask : public NativeSourceTask {
public:
    bool IsSimple() const final { return false; }
    NativeSourceTask* Child() const final { return m_Child.get(); }
    bool MakeAbortable(NativeSourceAbortPriority priority, const NativeSourceTaskEvent* event) override;
    virtual NativeSourceTaskPtr CreateFirstSubTask() = 0;
    virtual NativeSourceTaskPtr CreateNextSubTask() = 0;
    // Keep = original same pointer; Replace with nullptr = original null child.
    virtual NativeSourceTaskControl ControlSubTask() { return {}; }
private:
    friend class NativeSourceTaskManager;
    void SetChild(NativeSourceTaskPtr child);
    NativeSourceTaskPtr m_Child;
};

enum class NativeSourceTaskStatus { Ok, InvalidSlot, Busy, AnimationNotificationRejected };
// Single owner/thread. Manage may install tasks into OTHER slots (e.g. default
// on-foot installs primary jump), but recursive management or destruction of
// the currently executing root is rejected. Lifecycle callbacks during slot
// replacement cannot recursively replace roots. No rollback of task-body
// effects is claimed on an exception; guards only restore call eligibility.
class NativeSourceTaskManager {
public:
    NativeSourceTaskManager() = default;
    ~NativeSourceTaskManager();
    NativeSourceTaskManager(const NativeSourceTaskManager&) = delete;
    NativeSourceTaskManager& operator=(const NativeSourceTaskManager&) = delete;
    NativeSourceTaskStatus SetPrimary(NativePlayerPrimarySlot slot, NativeSourceTaskPtr&& task);
    NativeSourceTaskStatus SetSecondary(NativePlayerSecondarySlot slot, NativeSourceTaskPtr&& task);
    NativeSourceTaskStatus Manage();
    NativeSourceTaskStatus NotifyAnimations();
    NativeSourceTaskStatus Flush();
    NativeSourceTaskStatus FlushImmediately();
    NativeSourceTaskStatus ClearTaskEventResponse();
    NativeSourceTaskStatus StopTimers(const NativeSourceTaskEvent* event);
    NativeSourceTask* Primary(NativePlayerPrimarySlot slot) const;
    NativeSourceTask* Secondary(NativePlayerSecondarySlot slot) const;
    NativeSourceTask* Active() const;
    NativeSourceTask* FindActive(std::int32_t type) const;
    static NativeSourceTask* Last(NativeSourceTask* task);
private:
    static void AddSubTasks(NativeSourceTask* task);
    static void SetNextSubTask(NativeSourceComplexTask* task);
    static void ParentsControlChildren(NativeSourceTask* task);
    enum class ProcessResult { Finished, Success, Advanced };
    static ProcessResult ProcessTree(NativeSourceTask* root, bool primary);
    NativeSourceTaskStatus Change(NativeSourceTaskPtr& slot, NativeSourceTaskPtr&& task);
    std::array<NativeSourceTaskPtr, static_cast<std::size_t>(NativePlayerPrimarySlot::Count)> m_Primary;
    std::array<NativeSourceTaskPtr, static_cast<std::size_t>(NativePlayerSecondarySlot::Count)> m_Secondary;
    bool m_Managing = false, m_Changing = false;
    NativeSourceTaskPtr* m_Processing = nullptr;
};
