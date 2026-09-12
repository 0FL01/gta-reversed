#pragma once

#include "NativeSourceVehicleLifecycle.h"
#include "SfxDecode.h"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

enum class NativeSourceSliceFeedbackStatus : std::uint8_t {
    Ok, DuplicateIdempotent, NotLoaded, InvalidInput, StaleLifecycle,
    MissingLifecycleEvent, AudioError, Overflow,
};

enum class NativeSourceSliceActionKind : std::uint8_t {
    Spawn, EnterVehicle, DoorOpened, DoorClosed, DriverAttached, VehicleControl,
    ExitVehicle, DriverDetached, DestroyVehicle, WorldEvicted,
};

enum NativeSourceSliceActionFlag : std::uint8_t {
    NativeSourceSliceActionAccelerate = 1,
    NativeSourceSliceActionBrake = 2,
    NativeSourceSliceActionSteer = 4,
    NativeSourceSliceActionEnterExit = 8,
};

struct NativeSourceSliceHudState {
    bool Visible = true;
    float Health = 100.0f, MaxHealth = 100.0f, Armour = 0.0f;
    std::uint8_t MaxArmour = 100;
    std::int32_t Money{}, DisplayMoney{};
    std::uint8_t WantedLevel{}, ActiveWeapon{};
    std::uint64_t PedIdentity{}, VehicleIdentity{};
    NativeSourceVehicleLifecyclePhase Phase = NativeSourceVehicleLifecyclePhase::Empty;
    NativeSourceVehicleLifecycleTask Task = NativeSourceVehicleLifecycleTask::PlayerOnFoot;
    bool InVehicle{};
    float VehicleSpeed{};
    std::uint64_t InputSequence{};
    std::uint32_t TimeMs{};
    bool operator==(const NativeSourceSliceHudState&) const = default;
};

struct NativeSourceSliceActionEvent {
    std::uint64_t Sequence{}, LifecycleSequence{}, InputSequence{};
    NativeSourceSliceActionKind Kind{};
    NativeSourceVehicleLifecyclePhase Phase{};
    NativeSourceVehicleLifecycleTask Task = NativeSourceVehicleLifecycleTask::PlayerOnFoot;
    std::uint8_t Flags{}, PadButtons{};
    std::int16_t MoveX{};
    float GasPedal{}, BrakePedal{}, Steering{}, VehicleSpeed{};
    std::uint32_t TimeMs{};
    bool operator==(const NativeSourceSliceActionEvent&) const = default;
};

struct NativeSourceSliceAudioEvent {
    std::uint64_t Sequence{}, LifecycleSequence{}, InputSequence{};
    std::int16_t EventId{}, BankId{}, BankSlot{}, SoundId{};
    std::int8_t DoorType{};
    float FrequencyVariance{};
    NativeCollisionVector Position{};
    std::shared_ptr<const SfxSingleSoundResult> Clip;
    std::uint32_t TimeMs{};
    bool operator==(const NativeSourceSliceAudioEvent&) const = default;
};

struct NativeSourceSliceFeedbackSnapshot {
    std::uint64_t Epoch{}, Generation{}, LifecycleGeneration{}, LastLifecycleEvent{};
    NativeSourceSliceHudState Hud;
    std::vector<NativeSourceSliceActionEvent> Actions;
    std::vector<NativeSourceSliceAudioEvent> Audio;
    bool PresentationFeedback = false;
    bool operator==(const NativeSourceSliceFeedbackSnapshot&) const = default;
};

// Main-thread value observer for the bounded P3 source lifecycle. It accepts
// monotonically newer publications with zero or one new journal event; event-
// free camera-only publications may be coalesced, but event publication cannot.
// It never samples
// Godot nodes, devices or presentation state. The HUD starts from the actual CPed,
// CPlayerInfo::Clear, CPlayerPedData and CWanted initial values. Door events use
// CAutomobile::OpenDoor + CAEVehicleAudioEntity's model400 NEW-door routing and
// retain exact PCM decoded from the owned install. Full HUD mutation, door
// animation, mixer/spatialization and the other vehicle SFX families stay open.
class NativeSourceSliceFeedback {
public:
    NativeSourceSliceFeedback() = default;
    NativeSourceSliceFeedback(const NativeSourceSliceFeedback&) = delete;
    NativeSourceSliceFeedback& operator=(const NativeSourceSliceFeedback&) = delete;

    // Caller must hold the existing parser/OS-file ownership boundary while
    // this sets the shared read-only asset root and decodes bank138 sounds.
    NativeSourceSliceFeedbackStatus Initialize(const char* gameDir,
        std::shared_ptr<const NativeSourceVehicleLifecycleSnapshot>, std::string& error);
    NativeSourceSliceFeedbackStatus Observe(
        std::shared_ptr<const NativeSourceVehicleLifecycleSnapshot>, std::string& error);

    std::shared_ptr<const NativeSourceSliceFeedbackSnapshot> LastCommitted() const noexcept { return m_Published; }
    std::span<const NativeSourceSliceActionEvent> Actions() const noexcept { return m_Actions; }
    std::span<const NativeSourceSliceAudioEvent> Audio() const noexcept { return m_Audio; }

private:
    std::shared_ptr<const SfxSingleSoundResult> m_DoorOpen;
    std::shared_ptr<const SfxSingleSoundResult> m_DoorClose;
    std::shared_ptr<const NativeSourceVehicleLifecycleSnapshot> m_Lifecycle;
    std::shared_ptr<const NativeSourceSliceFeedbackSnapshot> m_Published;
    std::vector<NativeSourceSliceActionEvent> m_Actions;
    std::vector<NativeSourceSliceAudioEvent> m_Audio;
};
