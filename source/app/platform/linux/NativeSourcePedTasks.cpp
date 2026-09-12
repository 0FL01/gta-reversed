#include "NativeSourcePedTasks.h"

namespace {
using Frontier = NativeSourcePedFrontier;
using Phase = NativeSourceJumpPhase;
// eTaskType.h: TASK_SIMPLE_WAIT_FOR_PIZZA=239, followed by these three.
constexpr int AirAndLandType = 240, InAirType = 241, LandType = 242;
Frontier FromJump(NativeSourceJumpStatus status) {
    switch (status) {
    case NativeSourceJumpStatus::Ok: return Frontier::None;
    case NativeSourceJumpStatus::PendingWorld: return Frontier::PendingWorld;
    case NativeSourceJumpStatus::Unsupported: return Frontier::UnsupportedWorld;
    case NativeSourceJumpStatus::WorldError: return Frontier::WorldError;
    case NativeSourceJumpStatus::Stale: return Frontier::StaleAnimation;
    default: return Frontier::InvalidInput;
    }
}
class JumpTask;
class JumpLeaf final : public NativeSourceSimpleTask {
public:
    JumpLeaf(JumpTask& owner, int type);
    ~JumpLeaf() override;
    int Type() const override { return m_Type; }
    bool ProcessPed() override;
    bool MakeAbortable(NativeSourceAbortPriority priority, const NativeSourceTaskEvent* event) override;
    bool ObserveAnimationUpdate() override;
    void AbortAirAnimation();
private:
    JumpTask& m_Owner;
    int m_Type;
    bool m_Started = false;
    bool m_AnimationFault = false;
    NativeSourceAnimClump& m_Clump;
    NativeSourceAnimHandle m_AirAnimation;
    std::uint64_t m_AirToken = 0, m_LastUpdate = 0;
};
class AirAndLand final : public NativeSourceComplexTask {
public:
    explicit AirAndLand(JumpTask& owner) : m_Owner(owner) {}
    int Type() const override { return AirAndLandType; }
    NativeSourceTaskPtr CreateFirstSubTask() override { return std::make_unique<JumpLeaf>(m_Owner, InAirType); }
    NativeSourceTaskPtr CreateNextSubTask() override {
        return Child()->Type() == InAirType ? std::make_unique<JumpLeaf>(m_Owner, LandType) : nullptr;
    }
private:
    JumpTask& m_Owner;
};
class JumpTask final : public NativeSourceComplexTask {
public:
    JumpTask(NativeSourcePedTaskContext& context, NativeSourceAnimClump& clump) : Context(context), Clump(clump), Jump(clump) {}
    int Type() const override { return 211; }
    NativeSourceTaskPtr CreateFirstSubTask() override { return std::make_unique<JumpLeaf>(*this, 210); }
    NativeSourceTaskPtr CreateNextSubTask() override {
        if (Child()->Type() == 210) {
            if (Jump.State().Phase == Phase::HitHead) return std::make_unique<JumpLeaf>(*this, 500);
            if (Jump.State().Phase == Phase::InAir) return std::make_unique<AirAndLand>(*this);
        }
        return nullptr;
    }
    bool ObserveAnimationUpdate() override {
        if (!Started) return true;
        const auto status = Jump.ObserveAnimation(Jump.State().Generation);
        Context.Jump = Jump.State();
        if (status != NativeSourceJumpStatus::Ok) Context.Frontier = FromJump(status);
        return status == NativeSourceJumpStatus::Ok;
    }
    bool MakeAbortable(NativeSourceAbortPriority priority, const NativeSourceTaskEvent* event) override {
        const auto kind = event ? event->JumpAbort : NativeSourceAbortEvent::None;
        if (!Started) {
            const bool accepted = priority == NativeSourceAbortPriority::Immediate ||
                (priority == NativeSourceAbortPriority::Urgent && (kind == NativeSourceAbortEvent::Death || kind == NativeSourceAbortEvent::FatalFallDamage));
            if (accepted) { Context.InAir = Context.Jump.InAir = Context.Jump.Landing = false; }
            return accepted;
        }
        bool accepted = false;
        const auto status = Jump.Abort(Jump.State().Generation, priority, kind, Context.Drowning, accepted);
        const bool parentBypass = priority == NativeSourceAbortPriority::Urgent &&
            (kind == NativeSourceAbortEvent::Death || kind == NativeSourceAbortEvent::FatalFallDamage);
        if (accepted && !parentBypass) {
            auto* leaf = NativeSourceTaskManager::Last(this);
            if (leaf && leaf->Type() == InAirType) static_cast<JumpLeaf*>(leaf)->AbortAirAnimation();
        }
        Context.Frontier = FromJump(status);
        Context.Jump = Jump.State();
        Context.InAir = Context.Jump.InAir;
        return status == NativeSourceJumpStatus::Ok && accepted;
    }
    NativeSourcePedTaskContext& Context;
    NativeSourceAnimClump& Clump;
    NativeSourceJump Jump;
    bool Started = false;
};

JumpLeaf::JumpLeaf(JumpTask& owner, int type) : m_Owner(owner), m_Type(type), m_Clump(owner.Clump) {
    if (type == InAirType) m_AirToken = m_Clump.NewCallbackToken();
}
JumpLeaf::~JumpLeaf() {
    // Do not access m_Owner here: the derived root's members may already be
    // destroyed when its base complex-task destructor destroys the children.
    if (const auto* a = m_Clump.Get(m_AirAnimation); a && a->State.DeleteToken == m_AirToken)
        m_Clump.BindDelete(m_AirAnimation, 0);
}
void JumpLeaf::AbortAirAnimation() {
    if (const auto* a = m_Clump.Get(m_AirAnimation); a && a->State.DeleteToken == m_AirToken) {
        m_Clump.SetBlend(m_AirAnimation, a->State.BlendAmount, -8);
        m_Clump.SetAutoRemove(m_AirAnimation, true, a->State.FinishAutoRemove);
        m_Clump.BindDelete(m_AirAnimation, 0);
    }
    m_AirAnimation = {};
}
bool JumpLeaf::ObserveAnimationUpdate() {
    if (!m_AirAnimation.Serial) return true;
    const auto sequence = m_Clump.UpdateSequence();
    if (sequence != m_LastUpdate && sequence != m_LastUpdate + 1) {
        m_Owner.Context.Frontier = Frontier::StaleAnimation;
        return false;
    }
    for (const auto& event : m_Clump.LastEvents()) {
        if (event.Handle == m_AirAnimation && event.Event.DeleteToken == m_AirToken) m_AirAnimation = {};
    }
    m_LastUpdate = sequence;
    if (!m_AirAnimation.Serial) return true;
    const auto* a = m_Clump.Get(m_AirAnimation);
    if (a && a->State.DeleteToken == m_AirToken) return true;
    m_Owner.Context.Frontier = Frontier::StaleAnimation;
    return false;
}
bool JumpLeaf::MakeAbortable(NativeSourceAbortPriority priority, const NativeSourceTaskEvent* event) {
    return m_Owner.MakeAbortable(priority, event);
}
bool JumpLeaf::ProcessPed() {
    auto& context = m_Owner.Context;
    auto& jump = m_Owner.Jump;
    if (m_AnimationFault) { context.Frontier = Frontier::InvalidInput; return false; }
    auto status = NativeSourceJumpStatus::Ok;
    bool complete = false;
    bool animationOperation = false;
    if (m_Type == 210) {
        if (!m_Started) {
            animationOperation = true;
            auto start = context.JumpStart;
            start.StatModifier = context.CurrentStatModifier;
            status = jump.Begin(start);
            if (status == NativeSourceJumpStatus::Ok) {
                m_Started = m_Owner.Started = true;
                complete = jump.State().Phase == Phase::Finished;
            }
        } else if (jump.State().LaunchFinished) {
            if (context.JumpWorld.Status == NativeSourceJumpWorldStatus::Ready && context.JumpWorld.HasClimbEntity) {
                context.Frontier = Frontier::UnsupportedClimb;
                return false;
            }
            auto world = context.JumpWorld;
            world.StatModifier = context.CurrentStatModifier;
            const int glideId = world.ClimbJump ? 128 : 118;
            if (world.Status == NativeSourceJumpWorldStatus::Ready && !world.JumpBlocked && !m_Clump.Clip({0, glideId})) {
                context.Frontier = Frontier::MissingClip;
                return false; // no launch force committed without its required animation
            }
            status = jump.ResolveWorld(jump.State().Generation, world);
            complete = status == NativeSourceJumpStatus::Ok;
            if (complete && jump.State().Phase == Phase::InAir) {
                NativeSourceAnimHandle glide;
                m_AnimationFault = true;
                context.Jump = jump.State();
                context.InAir = context.Jump.InAir;
                if (m_Clump.Blend({0, glideId}, 8, glide) != NativeSourceClumpStatus::Ok) {
                    // Launch state is already committed. Retain that prefix
                    // and report a terminal task fault; never launch twice or
                    // pretend that a failed association mutation rolled back.
                    context.Frontier = Frontier::InvalidInput;
                    return false;
                }
                m_AnimationFault = false;
                if (world.ClimbJump) m_Clump.SetAutoRemove(glide, m_Clump.Get(glide)->State.BlendAutoRemove, true);
            }
        }
    } else if (m_Type == InAirType) {
        if (!m_AirAnimation.Serial) {
            // TaskSimpleInAir::ProcessPed using-jump-glide branch. Preserve its
            // lookup-before-Blend quirk: newly created glide is found next call.
            const auto* found = m_Clump.Find(118);
            if (!found) found = m_Clump.Find(128);
            const auto previous = found ? found->Handle : NativeSourceAnimHandle{};
            if (!found || (found->State.BlendAmount < 1 && found->State.BlendDelta <= 0)) {
                NativeSourceAnimHandle created;
                if (m_Clump.Blend({0, 118}, 4, created) != NativeSourceClumpStatus::Ok) {
                    context.Frontier = Frontier::MissingClip;
                    return false;
                }
            }
            if (previous.Serial) {
                m_AirAnimation = previous;
                m_Clump.BindDelete(previous, m_AirToken);
                m_LastUpdate = m_Clump.UpdateSequence();
            }
        }
        switch (context.AirStatus) {
        case NativeSourceJumpWorldStatus::Ready: complete = context.Landed; break;
        case NativeSourceJumpWorldStatus::Pending: status = NativeSourceJumpStatus::PendingWorld; break;
        case NativeSourceJumpWorldStatus::Error: status = NativeSourceJumpStatus::WorldError; break;
        default: status = NativeSourceJumpStatus::Unsupported; break;
        }
    } else if (m_Type == LandType) {
        if (!m_Started) {
            animationOperation = true;
            status = jump.BeginLanding(jump.State().Generation, 0, context.MoveBlend.Ratio,
                context.MoveSample.LeftRight != 0 || context.MoveSample.UpDown != 0,
                context.SprintHeld, context.SprintMoveState, context.CurrentStatModifier);
            m_Started = status == NativeSourceJumpStatus::Ok;
        } else if (jump.State().LandFinished) {
            status = jump.CompleteLanding(jump.State().Generation);
            complete = status == NativeSourceJumpStatus::Ok;
        }
    } else if (m_Type == 500) {
        if (!m_Started) {
            animationOperation = true;
            status = jump.BeginHitHeadAnimation(jump.State().Generation, 0);
            m_Started = status == NativeSourceJumpStatus::Ok;
        } else if (jump.State().HitHeadFinished) {
            status = jump.CompleteHitHead(jump.State().Generation);
            complete = status == NativeSourceJumpStatus::Ok;
        }
    }
    context.Frontier = animationOperation && status == NativeSourceJumpStatus::Unsupported ? Frontier::UnsupportedControl : FromJump(status);
    context.Jump = jump.State();
    context.InAir = context.Jump.InAir;
    return complete;
}

class OnFootTask final : public NativeSourceSimpleTask {
public:
    OnFootTask(NativeSourcePedTaskContext& context, NativeSourceAnimClump& clump, NativeSourceTaskManager& tasks)
        : m_Context(context), m_Clump(clump), m_Tasks(tasks) {}
    int Type() const override { return 0; }
    bool ProcessPed() override {
        auto& context = m_Context;
        context.Frontier = Frontier::None;
        if (!context.HasPad) return false;
        if (context.OtherControlMode) { context.Frontier = Frontier::UnsupportedControl; return false; }
        auto movement = context.MoveBlend;
        if (NativeSourceUpdateMoveBlend(context.MoveSample, movement) != NativePedControlStatus::Ok) {
            context.Frontier = Frontier::InvalidInput;
            return false;
        }
        auto input = context.Locomotion;
        input.MoveRatio = movement.Ratio;
        const auto result = NativeSourceProcessWalkRun(m_Clump, input, context.Movement);
        if (result != NativeSourceWalkRunStatus::Ok) {
            context.Frontier = result == NativeSourceWalkRunStatus::MissingClip ? Frontier::MissingClip :
                result == NativeSourceWalkRunStatus::InvalidInput ? Frontier::InvalidInput : Frontier::UnsupportedControl;
            return false;
        }
        context.MoveBlend = movement;
        if (context.Movement.ResetWalkAnimationsConsumed) context.Locomotion.ResetWalkAnimations = false;
        // TaskSimplePlayerOnFoot::PlayerControlZelda jump request after move
        // animation selection; installed in PRIMARY, not a second tick loop.
        if (!context.InAir && !context.HeavyWeapon && context.JumpJustDown && !context.Targeting &&
            !context.MoveSample.Attached && !context.CameraPreventsJump) {
            if (const auto* active = m_Tasks.Active(); active && active->Type() != 211) {
                if (m_Tasks.SetPrimary(NativePlayerPrimarySlot::Primary, std::make_unique<JumpTask>(context, m_Clump)) != NativeSourceTaskStatus::Ok)
                    context.Frontier = Frontier::SlotBusy;
            }
        }
        return false; // source PlayerOnFoot task remains installed
    }
    bool MakeAbortable(NativeSourceAbortPriority priority, const NativeSourceTaskEvent* event) override {
        // Target/first-person weapon cleanup belongs to the camera/weapon
        // owners. Do not silently accept an event requiring that side effect.
        if (event && (m_Context.Targeting || m_Context.CameraPreventsJump)) {
            m_Context.Frontier = Frontier::UnsupportedControl;
            return false;
        }
        if (priority == NativeSourceAbortPriority::Immediate) {
            NativeSourceAnimHandle idle;
            if (m_Clump.Blend({m_Context.Locomotion.Group, 3}, 1000, idle) != NativeSourceClumpStatus::Ok) {
                m_Context.Frontier = Frontier::MissingClip;
                return false;
            }
            m_Context.MoveBlend.Ratio = 0;
            return true;
        }
        if (priority == NativeSourceAbortPriority::Urgent) {
            auto* attack = m_Tasks.Secondary(NativePlayerSecondarySlot::Attack);
            if (!attack) return true;
            if (event) {
                if (event->Priority < 61) return false; // source early priority gate
                m_Context.Frontier = Frontier::UnsupportedControl; // general damage/projectile handling not adopted
                return false;
            }
            return attack->MakeAbortable(priority, nullptr);
        }
        return false;
    }
private:
    NativeSourcePedTaskContext& m_Context;
    NativeSourceAnimClump& m_Clump;
    NativeSourceTaskManager& m_Tasks;
};
}

NativeSourceTaskPtr NativeSourceMakePlayerOnFootTask(NativeSourcePedTaskContext& context,
    NativeSourceAnimClump& clump, NativeSourceTaskManager& manager) {
    return std::make_unique<OnFootTask>(context, clump, manager);
}
NativeSourceTaskPtr NativeSourceMakeJumpTask(NativeSourcePedTaskContext& context, NativeSourceAnimClump& clump) {
    return std::make_unique<JumpTask>(context, clump);
}
