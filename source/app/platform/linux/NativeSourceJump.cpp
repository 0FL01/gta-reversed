#include "NativeSourceJump.h"
#include <cmath>
#include <limits>
#include <atomic>
#include <stdexcept>

namespace {
constexpr auto Ok = NativeSourceJumpStatus::Ok;
std::atomic<std::uint64_t> s_NextCallback{1};
std::uint64_t AllocateCallback() {
    auto value = s_NextCallback.load(std::memory_order_relaxed);
    while (value != std::numeric_limits<std::uint64_t>::max()) {
        if (s_NextCallback.compare_exchange_weak(value, value + 1, std::memory_order_relaxed)) return value;
    }
    throw std::overflow_error("source jump callback identity exhausted");
}
NativeSourceMoveAssociation Movement(const NativeSourceAnimClump& clump, int id) {
    const auto* a = clump.Find(id);
    return a ? NativeSourceMoveAssociation{true, a->State.BlendAmount, a->State.CurrentTime, a->State.TotalTime} : NativeSourceMoveAssociation{};
}
bool Valid(const NativeSourceMoveAssociation& a) {
    return !a.Present || (std::isfinite(a.Blend) && a.Blend >= 0 && a.Blend <= 1 &&
        std::isfinite(a.Time) && std::isfinite(a.Total) && a.Total > 0);
}
}

NativeSourceJump::NativeSourceJump(NativeSourceAnimClump& clump) : m_Clump(&clump), m_CallbackToken(AllocateCallback()) {}
NativeSourceJump::~NativeSourceJump() { DetachAnimation(); }

void NativeSourceJump::DetachAnimation() {
    if (!m_Clump) return;
    if (const auto* a = m_Clump->Get(m_Association); a && a->State.FinishToken == m_CallbackToken)
        m_Clump->BindFinish(m_Association, 0);
    m_Association = {};
}

NativeSourceJumpStatus NativeSourceJump::AttachAnimation(NativeSourceJumpState& next, std::int32_t animation) {
    if (!m_Clump) return Ok;
    const auto* clip = m_Clump->Clip({0, animation});
    if (!clip || clip->Looped || clip->Synchronised || clip->Facial || !clip->Partial || !clip->FinishAutoRemove)
        return NativeSourceJumpStatus::Unsupported;
    NativeSourceAnimHandle handle;
    if (m_Clump->Blend({0, animation}, next.Animation.BlendDelta, handle) != NativeSourceClumpStatus::Ok)
        return NativeSourceJumpStatus::InvalidInput;
    m_Clump->SetSpeed(handle, next.Animation.Speed);
    m_Clump->BindFinish(handle, m_CallbackToken);
    m_Association = handle;
    m_LastClumpUpdate = m_Clump->UpdateSequence();
    next.Animation = m_Clump->Get(handle)->State; // read-only task projection
    return Ok;
}

NativeSourceJumpStatus NativeSourceJump::Begin(const NativeSourceJumpStart& input) {
    auto start = input;
    if (m_Clump) {
        start.Walk = Movement(*m_Clump, 0);
        start.Run = Movement(*m_Clump, 1);
        start.Sprint = Movement(*m_Clump, 2);
        start.ExistingLaunch = m_Clump->Find(116) || m_Clump->Find(117);
    }
    const bool canLaunch = !start.ExistingLaunch && !start.SteepSlopeAgainstForward && (!start.Player || start.DirectionAllowed);
    if (!Valid(start.Walk) || !Valid(start.Run) || !Valid(start.Sprint) ||
        (!m_Clump && canLaunch && (!std::isfinite(start.LaunchDuration) || start.LaunchDuration <= 0)) ||
        !std::isfinite(start.StatModifier) || start.StatModifier < 0 ||
        m_State.Generation == std::numeric_limits<std::uint64_t>::max() ||
        (m_State.Phase != NativeSourceJumpPhase::Idle && m_State.Phase != NativeSourceJumpPhase::Finished && m_State.Phase != NativeSourceJumpPhase::Aborted))
        return NativeSourceJumpStatus::InvalidInput;
    NativeSourceJumpState next;
    next.Generation = m_State.Generation + 1;
    next.Phase = NativeSourceJumpPhase::Finished;
    next.SlopePitchRequested = !start.ExistingLaunch && (start.SteepSlopeAgainstForward || (start.Player && !start.DirectionAllowed));
    if (canLaunch) {
        // Source StartLaunchAnim selects Sprint, then Run, then Walk. EXACT .3
        // prevents falling back, but does not enter the phase-adjustment branch.
        const auto* move = &start.Sprint;
        if (!move->Present || move->Blend < 0.3f) move = &start.Run;
        if (!move->Present || move->Blend < 0.3f) move = &start.Walk;
        float phase = 0;
        if (move->Present && move->Blend > 0.3f) {
            phase = move->Time / move->Total + 0.367f;
            if (phase > 1) phase -= 1;
        }
        if (!std::isfinite(phase)) return NativeSourceJumpStatus::InvalidInput;
        next.RightLaunch = phase >= 0.5f;
        next.Phase = NativeSourceJumpPhase::Launch;
        next.LaunchStarted = next.HasAnimation = true;
        next.CopyCurrentRotationToAim = true;
        next.Animation.TotalTime = start.LaunchDuration;
        next.Animation.Speed = start.HasPlayerData ? start.StatModifier : 1;
        next.Animation.BlendDelta = 8;
        next.Animation.FinishAutoRemove = true; // AnimAssocDescriptions: launch/launch_R
        next.Animation.FinishToken = next.Generation;
        const auto status = AttachAnimation(next, next.RightLaunch ? 117 : 116);
        if (status != Ok) return status;
    }
    m_Start = start;
    m_State = next;
    return Ok;
}

NativeSourceJumpStatus NativeSourceJump::AdvanceAnimation(std::uint64_t generation, float seconds) {
    if (m_Clump) return NativeSourceJumpStatus::InvalidInput; // never a second clump clock
    if (generation != m_State.Generation) return NativeSourceJumpStatus::Stale;
    if (!m_State.HasAnimation) return NativeSourceJumpStatus::InvalidInput;
    auto next = m_State;
    NativeSourceAnimEvent event;
    // RpAnimBlendClumpUpdateAnimations: blend first, timestep, then time. This
    // launch/land association is unsynchronised; no second clump scheduler.
    if (NativeSourceAnimUpdateBlend(next.Animation, seconds, event) != NativePedControlStatus::Ok)
        return NativeSourceJumpStatus::InvalidInput;
    if (!event.Removed) {
        if (NativeSourceAnimUpdateStep(next.Animation, seconds, 1) != NativePedControlStatus::Ok ||
            NativeSourceAnimUpdateTime(next.Animation, event) != NativePedControlStatus::Ok)
            return NativeSourceJumpStatus::InvalidInput;
    }
    next.RightFoot = next.LeftFoot = false;
    if (event.FinishToken) {
        next.HasAnimation = false; // JumpAnimFinishCB / Land::FinishAnimCB
        if (next.Phase == NativeSourceJumpPhase::Launch) {
            next.LaunchFinished = true;
            next.Phase = NativeSourceJumpPhase::AwaitWorld;
        } else if (next.Phase == NativeSourceJumpPhase::Land) {
            if (next.RunningLand) next.Animation.BlendDelta = -100;
            next.LandFinished = true; // remaining effects occur on ProcessPed
        } else {
            next.HitHeadFinished = true;
        }
    } else if (next.Phase == NativeSourceJumpPhase::Land) {
        // Non-consuming source predicates, seconds not normalized percentages.
        const float now = next.Animation.CurrentTime;
        const float previous = now - next.Animation.TimeStep;
        next.RightFoot = now >= 0.1f && previous < 0.1f;
        next.LeftFoot = now >= 0.2f && previous < 0.2f;
    }
    m_State = next;
    return Ok;
}

NativeSourceJumpStatus NativeSourceJump::ObserveAnimation(std::uint64_t generation) {
    if (generation != m_State.Generation) return NativeSourceJumpStatus::Stale;
    if (!m_Clump) return NativeSourceJumpStatus::InvalidInput;
    const auto sequence = m_Clump->UpdateSequence();
    if (!m_State.HasAnimation) { m_LastClumpUpdate = sequence; return Ok; }
    if (sequence == m_LastClumpUpdate) {
        const auto* a = m_Clump->Get(m_Association);
        return a && a->State.FinishToken == m_CallbackToken ? Ok : NativeSourceJumpStatus::Stale;
    }
    if (sequence != m_LastClumpUpdate + 1) return NativeSourceJumpStatus::Stale; // missed callback delivery is not success
    const NativeSourceClumpEvent* finished = nullptr;
    for (const auto& event : m_Clump->LastEvents())
        if (event.Handle == m_Association && event.Event.FinishToken == m_CallbackToken) finished = &event;
    const auto* association = m_Clump->Get(m_Association);
    if (!finished && (!association || association->State.FinishToken != m_CallbackToken)) return NativeSourceJumpStatus::Stale;
    auto next = m_State;
    next.RightFoot = next.LeftFoot = false;
    next.Animation = finished ? finished->State : association->State;
    if (finished) {
        next.HasAnimation = false;
        if (next.Phase == NativeSourceJumpPhase::Launch) {
            next.LaunchFinished = true;
            next.Phase = NativeSourceJumpPhase::AwaitWorld;
        } else if (next.Phase == NativeSourceJumpPhase::Land) {
            next.LandFinished = true;
            if (next.RunningLand) {
                next.Animation.BlendDelta = -100;
                if (association) m_Clump->SetBlend(m_Association, association->State.BlendAmount, -100);
            }
        } else next.HitHeadFinished = true;
        m_Association = {}; // source callback clears task's reference, not the clump's owner
    } else if (next.Phase == NativeSourceJumpPhase::Land) {
        const auto now = next.Animation.CurrentTime;
        const auto previous = now - next.Animation.TimeStep;
        next.RightFoot = now >= 0.1f && previous < 0.1f;
        next.LeftFoot = now >= 0.2f && previous < 0.2f;
    }
    m_State = next;
    m_LastClumpUpdate = sequence;
    return Ok;
}

NativeSourceJumpStatus NativeSourceJump::ResolveWorld(std::uint64_t generation, const NativeSourceJumpWorld& input) {
    auto world = input;
    if (m_Clump) { world.Run = Movement(*m_Clump, 1); world.Sprint = Movement(*m_Clump, 2); }
    if (generation != m_State.Generation) return NativeSourceJumpStatus::Stale;
    if (m_State.Phase != NativeSourceJumpPhase::AwaitWorld) return NativeSourceJumpStatus::InvalidInput;
    switch (world.Status) {
    case NativeSourceJumpWorldStatus::Unsupported: return NativeSourceJumpStatus::Unsupported;
    case NativeSourceJumpWorldStatus::Pending: return NativeSourceJumpStatus::PendingWorld;
    case NativeSourceJumpWorldStatus::Error: return NativeSourceJumpStatus::WorldError;
    case NativeSourceJumpWorldStatus::Ready: break;
    default: return NativeSourceJumpStatus::InvalidInput;
    }
    if (!std::isfinite(world.MoveX) || !std::isfinite(world.MoveY) || !std::isfinite(world.Rotation) ||
        !std::isfinite(world.StandingMoveX) || !std::isfinite(world.StandingMoveY) || !Valid(world.Run) || !Valid(world.Sprint) ||
        !std::isfinite(world.StatModifier) || world.StatModifier < 0) return NativeSourceJumpStatus::InvalidInput;
    auto next = m_State;
    if (!world.HasClimbEntity && world.JumpBlocked) {
        next.Phase = NativeSourceJumpPhase::HitHead;
        next.Landing = true;
        next.QuietBlockedSound = m_Start.Player;
    } else {
        float horizontal = 0.1f;
        if (world.Sprint.Present) horizontal = 0.17f + (0.22f - 0.17f) * world.Sprint.Blend;
        else if (world.Run.Present) horizontal = 0.1f + (0.17f - 0.1f) * world.Run.Blend;
        next.UpwardForce = m_Start.Player || m_Start.HighJump ? 8.5f : 4.5f;
        if (m_Start.HasPlayerData) { horizontal *= world.StatModifier; next.UpwardForce *= world.StatModifier; }
        if (m_Start.Player && world.MegaJump) next.UpwardForce *= 10;
        next.MoveX = world.MoveX; next.MoveY = world.MoveY;
        if (world.ClimbJump && !world.HasClimbEntity) {
            next.MoveX = next.MoveY = 0;
            next.WriteHorizontalSpeed = true;
        } else if (!world.HasClimbEntity && (world.StandingOnEntity || world.MoveX * world.MoveX + world.MoveY * world.MoveY < horizontal)) {
            // Source compares squared speed against UNSQUARED horizontal speed.
            next.MoveX = -horizontal * std::sin(world.Rotation);
            next.MoveY = horizontal * std::cos(world.Rotation);
            if (world.StandingOnEntity) { next.MoveX += world.StandingMoveX; next.MoveY += world.StandingMoveY; }
            next.WriteHorizontalSpeed = true;
        }
        if (!std::isfinite(next.UpwardForce) || !std::isfinite(next.MoveX) || !std::isfinite(next.MoveY)) return NativeSourceJumpStatus::InvalidInput;
        next.InAir = true;
        next.ClearStanding = true;
        if (!world.HasClimbEntity) {
            next.AirAnimation = world.ClimbJump ? NativeSourceJumpAirAnimation::ClimbJump : NativeSourceJumpAirAnimation::JumpGlide;
            next.AirAnimationFinishAutoRemove = world.ClimbJump;
        }
        next.Phase = world.HasClimbEntity && !m_Start.ForceClimbDisabled ? NativeSourceJumpPhase::Climb : NativeSourceJumpPhase::InAir;
    }
    m_State = next;
    return Ok;
}

NativeSourceJumpStatus NativeSourceJump::BeginLanding(std::uint64_t generation, float duration, float moveRatio,
    bool padMoving, bool sprintHeld, bool sprintMoveState, float currentStatModifier) {
    if (generation != m_State.Generation) return NativeSourceJumpStatus::Stale;
    if (m_State.Phase != NativeSourceJumpPhase::InAir || (!m_Clump && (!std::isfinite(duration) || duration <= 0)) || !std::isfinite(moveRatio) ||
        !std::isfinite(currentStatModifier) || currentStatModifier < 0)
        return NativeSourceJumpStatus::InvalidInput;
    auto next = m_State;
    next.Phase = NativeSourceJumpPhase::Land;
    next.InAir = false; // source InAir::ProcessPed clears this before completion
    next.Landing = next.HasAnimation = true;
    next.RunningLand = m_Start.Player && moveRatio > 1.5f && padMoving;
    next.Animation = {};
    next.Animation.TotalTime = duration;
    next.Animation.FinishToken = next.Generation;
    next.Animation.BlendDelta = 100;
    next.Animation.FinishAutoRemove = true; // AnimAssocDescriptions: jump/fall land
    next.Animation.Speed = m_Start.Player ? currentStatModifier * (sprintHeld && sprintMoveState ? 2.0f : 1.0f) : 1.0f;
    if (m_Clump) {
        const auto* existing = m_Clump->Find(next.RunningLand ? 119 : 122);
        const float previousSpeed = existing ? existing->State.Speed : 1;
        next.Animation.Speed = m_Start.Player ? (sprintHeld && sprintMoveState ? 2 : previousSpeed) * currentStatModifier : previousSpeed;
    }
    if (!std::isfinite(next.Animation.Speed)) return NativeSourceJumpStatus::InvalidInput;
    const auto status = AttachAnimation(next, next.RunningLand ? 119 : 122);
    if (status != Ok) return status;
    m_State = next;
    return Ok;
}

NativeSourceJumpStatus NativeSourceJump::CompleteLanding(std::uint64_t generation) {
    if (generation != m_State.Generation) return NativeSourceJumpStatus::Stale;
    if (m_State.Phase != NativeSourceJumpPhase::Land || !m_State.LandFinished) return NativeSourceJumpStatus::InvalidInput;
    m_State.Landing = m_State.InAir = false;
    m_State.ResetLocomotionTime = true;
    if (m_Clump) {
        for (int id = 0; id <= 2; ++id)
            if (const auto* a = m_Clump->Find(id)) m_Clump->ResetTime(a->Handle);
    }
    m_State.Phase = NativeSourceJumpPhase::Finished;
    return Ok;
}

NativeSourceJumpStatus NativeSourceJump::BeginHitHeadAnimation(std::uint64_t generation, float duration) {
    if (generation != m_State.Generation) return NativeSourceJumpStatus::Stale;
    if (m_State.Phase != NativeSourceJumpPhase::HitHead || m_State.HasAnimation || m_State.HitHeadFinished ||
        (!m_Clump && (!std::isfinite(duration) || duration <= 0))) return NativeSourceJumpStatus::InvalidInput;
    auto next = m_State;
    next.Animation = {};
    next.Animation.TotalTime = duration;
    next.Animation.FinishToken = generation;
    next.Animation.FinishAutoRemove = true; // AnimAssocDescriptions: HIT_WALL
    next.Animation.BlendDelta = 8;
    if (m_Clump) if (const auto* existing = m_Clump->Find(38)) next.Animation.Speed = existing->State.Speed;
    next.HasAnimation = true;
    const auto status = AttachAnimation(next, 38);
    if (status != Ok) return status;
    m_State = next;
    return Ok;
}

NativeSourceJumpStatus NativeSourceJump::CompleteHitHead(std::uint64_t generation) {
    if (generation != m_State.Generation) return NativeSourceJumpStatus::Stale;
    if (m_State.Phase != NativeSourceJumpPhase::HitHead || !m_State.HitHeadFinished) return NativeSourceJumpStatus::InvalidInput;
    m_State.Landing = m_State.InAir = false;
    m_State.Phase = NativeSourceJumpPhase::Finished;
    return Ok;
}

NativeSourceJumpStatus NativeSourceJump::Abort(std::uint64_t generation, NativeSourceAbortPriority priority,
    NativeSourceAbortEvent event, bool drowning, bool& accepted) {
    if (generation != m_State.Generation) return NativeSourceJumpStatus::Stale;
    if (priority != NativeSourceAbortPriority::Leisure && priority != NativeSourceAbortPriority::Urgent && priority != NativeSourceAbortPriority::Immediate)
        return NativeSourceJumpStatus::InvalidInput;
    if (event != NativeSourceAbortEvent::None && event != NativeSourceAbortEvent::Death && event != NativeSourceAbortEvent::FatalFallDamage &&
        event != NativeSourceAbortEvent::ScriptCommand71 && event != NativeSourceAbortEvent::StuckInAir && event != NativeSourceAbortEvent::Other)
        return NativeSourceJumpStatus::InvalidInput;
    if (m_State.Phase == NativeSourceJumpPhase::Idle || m_State.Phase == NativeSourceJumpPhase::Finished || m_State.Phase == NativeSourceJumpPhase::Aborted)
        return NativeSourceJumpStatus::InvalidInput;
    auto next = m_State;
    bool abort = priority == NativeSourceAbortPriority::Urgent &&
        (event == NativeSourceAbortEvent::Death || event == NativeSourceAbortEvent::FatalFallDamage);
    if (m_Clump && m_State.HasAnimation) {
        const auto* a = m_Clump->Get(m_Association);
        if (!a || a->State.FinishToken != m_CallbackToken) return NativeSourceJumpStatus::Stale;
        next.Animation = a->State;
    }
    if (!abort) {
        if (next.Phase == NativeSourceJumpPhase::Launch || next.Phase == NativeSourceJumpPhase::AwaitWorld) {
            if (next.HasAnimation) { next.Animation.BlendAutoRemove = true; next.Animation.BlendDelta = -4; }
            abort = priority == NativeSourceAbortPriority::Immediate;
        } else if (next.Phase == NativeSourceJumpPhase::Land) {
            abort = priority == NativeSourceAbortPriority::Immediate;
            if (abort) { next.Animation.BlendAutoRemove = true; next.Animation.BlendDelta = -1000; }
        } else if (next.Phase == NativeSourceJumpPhase::InAir) {
            abort = priority == NativeSourceAbortPriority::Immediate || (priority == NativeSourceAbortPriority::Urgent &&
                (drowning || event == NativeSourceAbortEvent::ScriptCommand71 || event == NativeSourceAbortEvent::StuckInAir));
        } else if (next.Phase == NativeSourceJumpPhase::HitHead) {
            if (next.HasAnimation) next.Animation.BlendDelta = -4;
            abort = priority == NativeSourceAbortPriority::Urgent || priority == NativeSourceAbortPriority::Immediate;
            if (abort) next.HitHeadFinished = true;
        } else return NativeSourceJumpStatus::Unsupported; // climb child owner not fabricated
    }
    if (abort) {
        next.InAir = next.Landing = next.HasAnimation = false;
        next.RightFoot = next.LeftFoot = false;
        next.Animation.FinishToken = 0; // source task destructor detaches callback
        next.Phase = NativeSourceJumpPhase::Aborted;
    }
    if (m_Clump && m_State.HasAnimation) {
        const auto* a = m_Clump->Get(m_Association);
        if (!a || a->State.FinishToken != m_CallbackToken) return NativeSourceJumpStatus::Stale;
        m_Clump->SetBlend(m_Association, a->State.BlendAmount, next.Animation.BlendDelta);
        m_Clump->SetAutoRemove(m_Association, next.Animation.BlendAutoRemove, next.Animation.FinishAutoRemove);
        if (abort) DetachAnimation();
    }
    m_State = next;
    accepted = abort;
    return Ok;
}
