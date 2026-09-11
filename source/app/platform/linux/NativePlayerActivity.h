// Owned native copy of the player state used by source task/event predicates.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

enum class NativePlayerActivityAuthority : std::uint8_t {
    // No claim about task/event eligibility can be made.
    Unsupported,
    // Complete only for the explicit native projection represented below;
    // this is not a general CTaskManager/CPedIntelligence implementation.
    SourceBacked,
};

enum class NativePlayerPrimarySlot : std::uint8_t {
    PhysicalResponse,
    EventResponseTemp,
    EventResponseNonTemp,
    Primary,
    Default,
    Count,
};

enum class NativePlayerSecondarySlot : std::uint8_t {
    Attack,
    Duck,
    Say,
    Facial,
    PartialAnim,
    IK,
    Count,
};

enum class NativePlayerTaskType : std::uint8_t {
    PlayerOnFoot,
    EnterCarAsDriver,
    CarDrive,
    UseMobilePhone,
    Jump,
    SimpleJump,
    InAirAndLand,
    InAir,
    Land,
    JetPack,
    Facial,
    Unsupported,
};

enum class NativePlayerEventType : std::uint8_t {
    ScriptCommand,
    InAir,
    Other,
    Unsupported,
};

enum class NativePlayerPedState : std::uint8_t {
    Idle,
    Driving,
    Attack,
    AimGun,
    Arrested,
    Die,
    Dead,
    Other,
    Unsupported,
};

enum class NativePlayerWeaponType : std::uint8_t {
    Unarmed,
    Other,
    Unsupported,
};

enum class NativePlayerWeaponState : std::uint8_t {
    Ready,
    Other,
    Unsupported,
};

struct NativePlayerWeaponSlot {
    NativePlayerWeaponType Type = NativePlayerWeaponType::Unsupported;
    NativePlayerWeaponState State = NativePlayerWeaponState::Unsupported;
    std::uint32_t AmmoInClip = 0;
    std::uint32_t TotalAmmo = 0;
};

// Root first, followed by each active GetSubTask() link.
using NativePlayerTaskChain = std::vector<NativePlayerTaskType>;

struct NativePlayerActivitySnapshot {
    static constexpr std::size_t PrimarySlotCount = static_cast<std::size_t>(NativePlayerPrimarySlot::Count);
    static constexpr std::size_t SecondarySlotCount = static_cast<std::size_t>(NativePlayerSecondarySlot::Count);
    static constexpr std::size_t WeaponSlotCount = 13;

    NativePlayerActivityAuthority Authority = NativePlayerActivityAuthority::Unsupported;
    // Monotonic for this gameplay owner, including Spawn() resets where
    // RealtimeGameplayState::Ticks returns to zero.
    std::uint64_t Revision = 0;
    // Known native eligibility projection only: 0053 PlayerOnFoot/Facial plus
    // the controller transitions represented by NativePlayerTaskType.
    std::array<NativePlayerTaskChain, PrimarySlotCount> PrimaryTasks;
    std::array<NativePlayerTaskChain, SecondarySlotCount> SecondaryTasks;
    std::vector<NativePlayerEventType> TypedEvents;
    NativePlayerPedState PedState = NativePlayerPedState::Unsupported;
    bool InAir = false;
    bool Landing = false;
    bool Alive = false; // CPed::IsAlive: false only for PEDSTATE_DIE/DEAD.
    bool GamePlaying = false;
    bool CoopGame = false;
    std::array<NativePlayerWeaponSlot, WeaponSlotCount> WeaponSlots;
    std::uint8_t ActiveWeaponSlot = 0;
};

enum class NativeMissionStartOutcome : std::uint8_t {
    Allowed,
    Denied,
    Unsupported,
};

enum class NativeMissionStartReason : std::uint8_t {
    None,
    ActivityUnsupported,
    GameNotPlaying,
    CoopGame,
    PedNotInControlOrDriving,
    PhysicalResponseTask,
    EventResponseNonTempTask,
    PrimaryTaskNotCarDrive,
    AttackTask,
    PlayerNotAlive,
    ScriptCommandEvent,
};

struct NativeMissionStartDecision {
    NativeMissionStartOutcome Outcome = NativeMissionStartOutcome::Unsupported;
    NativeMissionStartReason Reason = NativeMissionStartReason::ActivityUnsupported;
};

NativeMissionStartDecision NativePlayerCanStartMission(const NativePlayerActivitySnapshot& snapshot);
// Unsupported authority is conservative (busy=true, wants=false). A
// SourceBacked snapshot must not hide an unknown command in a known chain;
// primary-root Unsupported remains available for mission's typed result.
bool NativePlayerPickupBusy(const NativePlayerActivitySnapshot& snapshot);
bool NativePlayerWantsUnarmedPickup(const NativePlayerActivitySnapshot& snapshot);
