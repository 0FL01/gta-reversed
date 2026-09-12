#include "NativeSourceJump.h"
#include <cmath>
#include <limits>

namespace {
constexpr auto Ok = NativeSourceJumpStatus::Ok;
bool Valid(const NativeSourceMoveAssociation& a) {
    return !a.Present || (std::isfinite(a.Blend) && a.Blend >= 0 && a.Blend <= 1 &&
        std::isfinite(a.Time) && std::isfinite(a.Total) && a.Total > 0);
}
}

NativeSourceJumpStatus NativeSourceJump::Begin(const NativeSourceJumpStart& start) {
    if (!Valid(start.Walk) || !Valid(start.Run) || !Valid(start.Sprint) ||
        !std::isfinite(start.LaunchDuration) || start.LaunchDuration <= 0 ||
        !std::isfinite(start.StatModifier) || start.StatModifier < 0 ||
        m_State.Generation == std::numeric_limits<std::uint64_t>::max() ||
        (m_State.Phase != NativeSourceJumpPhase::Idle && m_State.Phase != NativeSourceJumpPhase::Finished && m_State.Phase != NativeSourceJumpPhase::Aborted))
        return NativeSourceJumpStatus::InvalidInput;
    NativeSourceJumpState next;
    next.Generation = m_State.Generation + 1;
    next.Phase = NativeSourceJumpPhase::Finished;
    next.SlopePitchRequested = !start.ExistingLaunch && (start.SteepSlopeAgainstForward || (start.Player && !start.DirectionAllowed));
    if (!start.ExistingLaunch && !start.SteepSlopeAgainstForward && (!start.Player || start.DirectionAllowed)) {
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
    }
    m_Start = start;
    m_State = next;
    return Ok;
}

NativeSourceJumpStatus NativeSourceJump::AdvanceAnimation(std::uint64_t generation, float seconds) {
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

NativeSourceJumpStatus NativeSourceJump::ResolveWorld(std::uint64_t generation, const NativeSourceJumpWorld& world) {
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
    if (m_State.Phase != NativeSourceJumpPhase::InAir || !std::isfinite(duration) || duration <= 0 || !std::isfinite(moveRatio) ||
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
    if (!std::isfinite(next.Animation.Speed)) return NativeSourceJumpStatus::InvalidInput;
    m_State = next;
    return Ok;
}

NativeSourceJumpStatus NativeSourceJump::CompleteLanding(std::uint64_t generation) {
    if (generation != m_State.Generation) return NativeSourceJumpStatus::Stale;
    if (m_State.Phase != NativeSourceJumpPhase::Land || !m_State.LandFinished) return NativeSourceJumpStatus::InvalidInput;
    m_State.Landing = m_State.InAir = false;
    m_State.ResetLocomotionTime = true;
    m_State.Phase = NativeSourceJumpPhase::Finished;
    return Ok;
}

NativeSourceJumpStatus NativeSourceJump::BeginHitHeadAnimation(std::uint64_t generation, float duration) {
    if (generation != m_State.Generation) return NativeSourceJumpStatus::Stale;
    if (m_State.Phase != NativeSourceJumpPhase::HitHead || m_State.HasAnimation || m_State.HitHeadFinished ||
        !std::isfinite(duration) || duration <= 0) return NativeSourceJumpStatus::InvalidInput;
    m_State.Animation = {};
    m_State.Animation.TotalTime = duration;
    m_State.Animation.FinishToken = generation;
    m_State.Animation.FinishAutoRemove = true; // AnimAssocDescriptions: HIT_WALL
    m_State.Animation.BlendDelta = 8;
    m_State.HasAnimation = true;
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
    m_State = next;
    accepted = abort;
    return Ok;
}
