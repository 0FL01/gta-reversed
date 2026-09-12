#pragma once
#include "NativeSourcePedControl.h"

enum class NativeSourceJumpPhase { Idle, Launch, AwaitWorld, InAir, Land, HitHead, Climb, Finished, Aborted };
enum class NativeSourceJumpStatus { Ok, PendingWorld, InvalidInput, Stale, Unsupported, WorldError };
enum class NativeSourceJumpWorldStatus { Unsupported, Pending, Ready, Error };
enum class NativeSourceJumpAirAnimation { None, JumpGlide, ClimbJump };
enum class NativeSourceAbortPriority { Leisure, Urgent, Immediate };
// FatalFallDamage means the source conjunction: WEAPON_FALL, health zero,
// and AddToEventGroup. ScriptCommand71 includes the exact event priority.
enum class NativeSourceAbortEvent { None, Death, FatalFallDamage, ScriptCommand71, StuckInAir, Other };

struct NativeSourceMoveAssociation {
    bool Present = false;
    float Blend = 0, Time = 0, Total = 1;
};
struct NativeSourceJumpStart {
    NativeSourceMoveAssociation Walk, Run, Sprint;
    float LaunchDuration = 0, StatModifier = 1; // no invented/default clip duration
    bool ExistingLaunch = false, SteepSlopeAgainstForward = false, DirectionAllowed = true;
    bool Player = true, HasPlayerData = true, HighJump = false;
    bool ForceClimbDisabled = false;
};
struct NativeSourceJumpWorld {
    // Source collision/climb predicates must be supplied by their real owner.
    // Missing authority stays Unsupported; pending work and errors are distinct.
    NativeSourceJumpWorldStatus Status = NativeSourceJumpWorldStatus::Unsupported;
    bool HasClimbEntity = false, JumpBlocked = false, ClimbJump = false;
    bool StandingOnEntity = false;
    float MoveX = 0, MoveY = 0, StandingMoveX = 0, StandingMoveY = 0, Rotation = 0;
    // Launch reads these AFTER its animation callback. They may differ from
    // the associations/stat modifier used by StartLaunchAnim.
    NativeSourceMoveAssociation Run, Sprint;
    float StatModifier = 1;
    bool MegaJump = false;
};
struct NativeSourceJumpState {
    std::uint64_t Generation = 0;
    NativeSourceJumpPhase Phase = NativeSourceJumpPhase::Idle;
    NativeSourceAnimAssociation Animation;
    bool HasAnimation = false, InAir = false, Landing = false;
    bool LaunchStarted = false, LaunchFinished = false, LandFinished = false, RightFoot = false, LeftFoot = false;
    bool HitHeadFinished = false;
    bool ResetLocomotionTime = false, QuietBlockedSound = false;
    // Symbolic source animation choice, not a manufactured time percentage.
    bool RightLaunch = false, RunningLand = false;
    bool SlopePitchRequested = false, CopyCurrentRotationToAim = false, ClearStanding = false;
    NativeSourceJumpAirAnimation AirAnimation = NativeSourceJumpAirAnimation::None;
    bool AirAnimationFinishAutoRemove = false;
    // Last one-shot ResolveWorld launch observation, NOT a per-tick command,
    // integrated speed or replacement physics. ResolveWorld cannot repeat.
    float UpwardForce = 0, MoveX = 0, MoveY = 0;
    bool WriteHorizontalSpeed = false;
    bool operator==(const NativeSourceJumpState&) const = default;
};

// Owned basic jump/land task flow from TaskSimpleJump, TaskComplexJump,
// TaskComplexInAirAndLand and TaskSimpleLand. World collision, climb task body,
// falling/collapse, clump association lifetime, blood-shadow effects and physical
// integration remain separate owners. This is not a complete CPed/task manager.
// Generation is per task object; the enclosing owner must qualify entity/scene
// identity before calling it. No callback pointers or asynchronous jobs escape.
class NativeSourceJump {
public:
    NativeSourceJump() = default;
    NativeSourceJump(const NativeSourceJump&) = delete;
    NativeSourceJump& operator=(const NativeSourceJump&) = delete;
    NativeSourceJumpStatus Begin(const NativeSourceJumpStart& start);
    NativeSourceJumpStatus AdvanceAnimation(std::uint64_t generation, float seconds);
    NativeSourceJumpStatus ResolveWorld(std::uint64_t generation, const NativeSourceJumpWorld& world);
    NativeSourceJumpStatus BeginLanding(std::uint64_t generation, float duration, float moveRatio,
        bool padMoving, bool sprintHeld, bool sprintMoveState, float currentStatModifier);
    NativeSourceJumpStatus CompleteLanding(std::uint64_t generation);
    NativeSourceJumpStatus BeginHitHeadAnimation(std::uint64_t generation, float duration);
    NativeSourceJumpStatus CompleteHitHead(std::uint64_t generation);
    NativeSourceJumpStatus Abort(std::uint64_t generation, NativeSourceAbortPriority priority,
        NativeSourceAbortEvent event, bool drowning, bool& accepted);
    const NativeSourceJumpState& State() const { return m_State; }

private:
    NativeSourceJumpState m_State;
    NativeSourceJumpStart m_Start;
};
