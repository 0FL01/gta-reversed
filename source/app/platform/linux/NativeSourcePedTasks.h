#pragma once
#include "NativeSourceTaskManager.h"
#include "NativeSourceWalkRun.h"

enum class NativeSourcePedFrontier {
    None, UnsupportedControl, MissingClip, InvalidInput, UnsupportedWorld,
    PendingWorld, WorldError, UnsupportedClimb, StaleAnimation, SlotBusy,
};

// Owned observations for the ordinary on-foot/basic-jump slice. World/contact
// decisions are supplied by the physical owner; default Unsupported is NOT a
// synthetic floor. These task bodies never integrate position or advance time.
struct NativeSourcePedTaskContext {
    NativeSourceMoveSample MoveSample;
    NativeSourceMoveBlend MoveBlend;
    NativeSourceWalkRunInput Locomotion;
    NativeSourceWalkRunResult Movement;
    NativeSourceJumpStart JumpStart;
    NativeSourceJumpWorld JumpWorld;
    NativeSourceJumpWorldStatus AirStatus = NativeSourceJumpWorldStatus::Unsupported;
    bool Landed = false;
    bool HasPad = true, OtherControlMode = false;
    bool JumpJustDown = false, InAir = false, HeavyWeapon = false, Targeting = false;
    bool CameraPreventsJump = false, Drowning = false;
    bool SprintHeld = false, SprintMoveState = false;
    float CurrentStatModifier = 1;
    NativeSourcePedFrontier Frontier = NativeSourcePedFrontier::None;
    NativeSourceJumpState Jump;
};

// Source types: PlayerOnFoot=0, SimpleJump=210, ComplexJump=211,
// InAirAndLand=240, InAir=241, Land=242, HitHead=500 (Enums/eTaskType.h).
// Context, clump and manager outlive the installed tasks. After each clump
// Update call manager.NotifyAnimations BEFORE Manage, including inactive slots.
// Ordinary animation/task behavior only: weapon/duck/fight, sprint/exhaustion,
// climb, falling/collapse, physical contacts and effect consumption remain
// explicit separate owners, not silently completed by these factories.
NativeSourceTaskPtr NativeSourceMakePlayerOnFootTask(NativeSourcePedTaskContext& context,
    NativeSourceAnimClump& clump, NativeSourceTaskManager& manager);
NativeSourceTaskPtr NativeSourceMakeJumpTask(NativeSourcePedTaskContext& context, NativeSourceAnimClump& clump);
