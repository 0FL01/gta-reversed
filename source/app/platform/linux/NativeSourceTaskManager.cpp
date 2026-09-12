// Source algorithm: game_sa/Tasks/TaskManager.cpp and TaskComplex.h.
// Original Plugin-SDK contributors: https://github.com/DK22Pac/plugin-sdk
#include "NativeSourceTaskManager.h"
#include <cassert>
#include <utility>

namespace {
constexpr auto Ok = NativeSourceTaskStatus::Ok;
struct FlagGuard {
    bool& Flag;
    explicit FlagGuard(bool& flag) : Flag(flag) { Flag = true; }
    ~FlagGuard() { Flag = false; }
};
struct SlotGuard {
    NativeSourceTaskPtr*& Current;
    SlotGuard(NativeSourceTaskPtr*& current, NativeSourceTaskPtr& slot) : Current(current) { Current = &slot; }
    ~SlotGuard() { Current = nullptr; }
};
NativeSourceTask* Find(NativeSourceTask* task, std::int32_t type) {
    for (; task; task = task->Child()) if (task->Type() == type) return task;
    return nullptr;
}
}

bool NativeSourceComplexTask::MakeAbortable(NativeSourceAbortPriority priority, const NativeSourceTaskEvent* event) {
    assert(m_Child);
    return m_Child->MakeAbortable(priority, event);
}
void NativeSourceComplexTask::SetChild(NativeSourceTaskPtr child) {
    // Match source SetSubTask: old destruction precedes assigning new parent.
    auto old = std::exchange(m_Child, std::move(child));
    old.reset();
    if (m_Child) m_Child->m_Parent = this;
}

NativeSourceTask* NativeSourceTaskManager::Last(NativeSourceTask* task) {
    auto* last = task;
    for (; task; task = task->Child()) last = task;
    return last;
}
NativeSourceTaskManager::~NativeSourceTaskManager() {
    assert(!m_Managing && !m_Changing);
    Flush(); // source destructor order: primary roots, then secondary roots
}
void NativeSourceTaskManager::SetNextSubTask(NativeSourceComplexTask* task) {
    for (; task; task = task->Parent()) {
        if (auto next = task->CreateNextSubTask()) {
            task->SetChild(std::move(next));
            AddSubTasks(task->Child());
            return;
        }
        task->SetChild(nullptr);
    }
}
void NativeSourceTaskManager::AddSubTasks(NativeSourceTask* task) {
    if (!task) return;
    while (!task->IsSimple()) {
        auto* complex = static_cast<NativeSourceComplexTask*>(task);
        if (auto child = complex->CreateFirstSubTask()) {
            complex->SetChild(std::move(child));
            task = complex->Child();
        } else {
            if (auto* parent = task->Parent()) SetNextSubTask(parent);
            break;
        }
    }
}
void NativeSourceTaskManager::ParentsControlChildren(NativeSourceTask* task) {
    while (task && !task->IsSimple()) {
        auto* complex = static_cast<NativeSourceComplexTask*>(task);
        auto control = complex->ControlSubTask();
        if (control.Replace) {
            auto* previous = complex->Child();
            assert(previous);
            previous->MakeAbortable(NativeSourceAbortPriority::Urgent, nullptr);
            // Source replaces regardless of the child's abort response.
            complex->SetChild(std::move(control.Replacement));
            AddSubTasks(complex->Child());
            return;
        }
        task = task->Child();
    }
}
NativeSourceTaskManager::ProcessResult NativeSourceTaskManager::ProcessTree(NativeSourceTask* root, bool primary) {
    ParentsControlChildren(root);
    auto* leaf = Last(root);
    if (!leaf->IsSimple()) {
        if (!primary) return ProcessResult::Finished;
        SetNextSubTask(leaf->Parent());
        leaf = Last(root);
        if (!leaf->IsSimple()) return ProcessResult::Finished;
    }
    if (!static_cast<NativeSourceSimpleTask*>(leaf)->ProcessPed()) return ProcessResult::Success;
    SetNextSubTask(leaf->Parent());
    return root->Child() ? ProcessResult::Advanced : ProcessResult::Finished;
}

NativeSourceTaskStatus NativeSourceTaskManager::Change(NativeSourceTaskPtr& slot, NativeSourceTaskPtr&& task) {
    if (m_Changing || m_Processing == &slot) return NativeSourceTaskStatus::Busy;
    FlagGuard guard(m_Changing);
    slot.reset();
    slot = std::move(task);
    AddSubTasks(slot.get());
    if (slot && !Last(slot.get())->IsSimple()) slot.reset();
    return Ok;
}
NativeSourceTaskStatus NativeSourceTaskManager::SetPrimary(NativePlayerPrimarySlot slot, NativeSourceTaskPtr&& task) {
    const auto index = static_cast<std::size_t>(slot);
    return index < m_Primary.size() ? Change(m_Primary[index], std::move(task)) : NativeSourceTaskStatus::InvalidSlot;
}
NativeSourceTaskStatus NativeSourceTaskManager::SetSecondary(NativePlayerSecondarySlot slot, NativeSourceTaskPtr&& task) {
    const auto index = static_cast<std::size_t>(slot);
    return index < m_Secondary.size() ? Change(m_Secondary[index], std::move(task)) : NativeSourceTaskStatus::InvalidSlot;
}
NativeSourceTask* NativeSourceTaskManager::Primary(NativePlayerPrimarySlot slot) const {
    const auto index = static_cast<std::size_t>(slot);
    return index < m_Primary.size() ? m_Primary[index].get() : nullptr;
}
NativeSourceTask* NativeSourceTaskManager::Secondary(NativePlayerSecondarySlot slot) const {
    const auto index = static_cast<std::size_t>(slot);
    return index < m_Secondary.size() ? m_Secondary[index].get() : nullptr;
}
NativeSourceTask* NativeSourceTaskManager::Active() const {
    for (const auto& task : m_Primary) if (task) return task.get();
    return nullptr;
}
NativeSourceTask* NativeSourceTaskManager::FindActive(std::int32_t type) const {
    if (auto* task = Find(Active(), type)) return task;
    // Match the existing reversed source's documented first-match bugfix.
    for (const auto& task : m_Secondary) if (auto* found = Find(task.get(), type)) return found;
    return nullptr;
}

NativeSourceTaskStatus NativeSourceTaskManager::Manage() {
    if (m_Managing || m_Changing) return NativeSourceTaskStatus::Busy;
    FlagGuard guard(m_Managing);
    for (auto& task : m_Primary) {
        if (!task) continue;
        SlotGuard slot(m_Processing, task);
        if (!Last(task.get())->IsSimple()) { task.reset(); return Ok; }
        // Original source ceiling: ten advanced primary sub-tasks per call.
        for (std::size_t i = 0; i < 10; ++i) {
            const auto result = ProcessTree(task.get(), true);
            if (result == ProcessResult::Finished) task.reset();
            if (result != ProcessResult::Advanced) break;
        }
        break; // lower-priority primaries do NOT run after this root finishes
    }
    for (auto& task : m_Secondary) {
        if (!task) continue;
        SlotGuard slot(m_Processing, task);
        ProcessResult result;
        do { result = ProcessTree(task.get(), false); } while (result == ProcessResult::Advanced);
        if (result == ProcessResult::Finished) task.reset();
    }
    return Ok;
}
NativeSourceTaskStatus NativeSourceTaskManager::Flush() {
    if (m_Managing || m_Changing) return NativeSourceTaskStatus::Busy;
    FlagGuard guard(m_Changing);
    for (auto& task : m_Primary) task.reset();
    for (auto& task : m_Secondary) task.reset();
    return Ok;
}
NativeSourceTaskStatus NativeSourceTaskManager::FlushImmediately() {
    if (m_Managing || m_Changing) return NativeSourceTaskStatus::Busy;
    FlagGuard guard(m_Changing);
    for (auto& task : m_Primary) if (task && task->MakeAbortable(NativeSourceAbortPriority::Immediate, nullptr)) task.reset();
    for (auto& task : m_Secondary) if (task && task->MakeAbortable(NativeSourceAbortPriority::Immediate, nullptr)) task.reset();
    return Ok;
}
NativeSourceTaskStatus NativeSourceTaskManager::ClearTaskEventResponse() {
    if (m_Managing || m_Changing) return NativeSourceTaskStatus::Busy;
    FlagGuard guard(m_Changing);
    for (auto slot : {NativePlayerPrimarySlot::EventResponseTemp, NativePlayerPrimarySlot::EventResponseNonTemp}) {
        if (auto& task = m_Primary[static_cast<std::size_t>(slot)]) { task.reset(); break; }
    }
    return Ok;
}
NativeSourceTaskStatus NativeSourceTaskManager::StopTimers(const NativeSourceTaskEvent* event) {
    if (m_Managing || m_Changing) return NativeSourceTaskStatus::Busy;
    FlagGuard guard(m_Changing);
    for (const auto& task : m_Primary) if (task) task->StopTimer(event);
    return Ok;
}
