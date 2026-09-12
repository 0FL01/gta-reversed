#include "NativeSourceSliceFeedback.h"

#include <algorithm>
#include <cmath>
#include <exception>
#include <limits>
#include <ranges>
#include <stdexcept>

using int32 = int32_t;
using int64 = int64_t;
using uint32 = uint32_t;
using uint64 = uint64_t;
#ifndef __stdcall
#define __stdcall
#endif
#include "oswrapper/oswrapper.h"

namespace {
using Status = NativeSourceSliceFeedbackStatus;
constexpr std::uint8_t Square = 1, Cross = 2, Triangle = 4;
constexpr std::int16_t VehicleDoorOpenEvent = 80;
constexpr std::int16_t VehicleDoorCloseEvent = 86;
constexpr std::int16_t VehicleGeneralBank = 138;
constexpr std::int16_t VehicleGeneralSlot = 19;
constexpr std::int16_t NewDoorOpenSound = 40;
constexpr std::int16_t NewDoorCloseSound = 33;
constexpr std::int8_t NewDoorType = 2;

bool Finite(const NativeCollisionVector& value) {
    return std::ranges::all_of(value, [](float component) { return std::isfinite(component); });
}

bool ValidateLifecycle(const NativeSourceVehicleLifecycleSnapshot& state) {
    if (!state.Epoch || !state.Generation || !state.WorldGeneration || !state.PedIdentity ||
        !state.VehicleIdentity || !state.WorldCollision || state.Events.empty() ||
        (state.InputSequence && state.Events.back().InputSequence > state.InputSequence)) return false;
    for (std::size_t i = 0; i < state.Events.size(); ++i) {
        const auto& event = state.Events[i];
        if (event.Sequence != i + 1 || event.PedIdentity != state.PedIdentity ||
            event.VehicleIdentity != state.VehicleIdentity || !event.WorldGeneration) return false;
    }
    if (state.Phase == NativeSourceVehicleLifecyclePhase::Driving ||
        state.Phase == NativeSourceVehicleLifecyclePhase::Exiting) {
        if (!state.InVehicle || state.PedInWorld || state.PedUsesCollision || !state.Automobile) return false;
    } else if (state.Phase == NativeSourceVehicleLifecyclePhase::OnFoot) {
        if (state.InVehicle || !state.PedInWorld || !state.PedUsesCollision || !state.Automobile) return false;
    } else if (state.Phase == NativeSourceVehicleLifecyclePhase::Entering) {
        if (state.InVehicle || !state.PedInWorld || !state.PedUsesCollision || !state.Automobile) return false;
    } else if (state.Phase != NativeSourceVehicleLifecyclePhase::Destroyed) {
        return false;
    }
    return !state.Automobile || (state.Automobile->ModelId == 400 &&
        state.Automobile->ModelName == "landstal" && std::isfinite(state.Automobile->ForwardSpeed) &&
        Finite(state.Automobile->Matrix.Position));
}

NativeSourceSliceActionKind ActionKind(NativeSourceVehicleLifecycleEventKind kind) {
    switch (kind) {
    case NativeSourceVehicleLifecycleEventKind::Spawn: return NativeSourceSliceActionKind::Spawn;
    case NativeSourceVehicleLifecycleEventKind::EnterRequested: return NativeSourceSliceActionKind::EnterVehicle;
    case NativeSourceVehicleLifecycleEventKind::DoorOpened: return NativeSourceSliceActionKind::DoorOpened;
    case NativeSourceVehicleLifecycleEventKind::DoorClosed: return NativeSourceSliceActionKind::DoorClosed;
    case NativeSourceVehicleLifecycleEventKind::DriverAttached: return NativeSourceSliceActionKind::DriverAttached;
    case NativeSourceVehicleLifecycleEventKind::Drive: return NativeSourceSliceActionKind::VehicleControl;
    case NativeSourceVehicleLifecycleEventKind::ExitRequested: return NativeSourceSliceActionKind::ExitVehicle;
    case NativeSourceVehicleLifecycleEventKind::DriverDetached: return NativeSourceSliceActionKind::DriverDetached;
    case NativeSourceVehicleLifecycleEventKind::Destroyed: return NativeSourceSliceActionKind::DestroyVehicle;
    case NativeSourceVehicleLifecycleEventKind::WorldEvicted: return NativeSourceSliceActionKind::WorldEvicted;
    }
    return NativeSourceSliceActionKind::Spawn;
}

bool IsOpenEvent(NativeSourceVehicleLifecycleEventKind kind) {
    return kind == NativeSourceVehicleLifecycleEventKind::DoorOpened;
}
bool IsCloseEvent(NativeSourceVehicleLifecycleEventKind kind) {
    return kind == NativeSourceVehicleLifecycleEventKind::DoorClosed;
}

NativeSourceSliceHudState Hud(const NativeSourceVehicleLifecycleSnapshot& state) {
    NativeSourceSliceHudState hud;
    hud.PedIdentity = state.PedIdentity; hud.VehicleIdentity = state.VehicleIdentity;
    hud.Phase = state.Phase; hud.Task = state.Task; hud.InVehicle = state.InVehicle;
    hud.VehicleSpeed = state.Automobile ? state.Automobile->ForwardSpeed : 0.0f;
    hud.InputSequence = state.InputSequence; hud.TimeMs = state.TimeMs;
    return hud;
}
}

NativeSourceSliceFeedbackStatus NativeSourceSliceFeedback::Initialize(const char* gameDir,
    std::shared_ptr<const NativeSourceVehicleLifecycleSnapshot> lifecycle, std::string& error) {
    if (m_Published || !gameDir || !*gameDir || !lifecycle || !ValidateLifecycle(*lifecycle) ||
        lifecycle->Generation != 1 || lifecycle->Events.size() != 1 ||
        lifecycle->Events[0].Kind != NativeSourceVehicleLifecycleEventKind::Spawn) {
        error = "invalid source slice feedback initialization";
        return Status::InvalidInput;
    }
    SfxSingleSoundResult open, close;
    OS_SetFilePathOffset(gameDir);
    if (!SfxDecode_Sound(VehicleGeneralBank, NewDoorOpenSound, open, error) ||
        !SfxDecode_Sound(VehicleGeneralBank, NewDoorCloseSound, close, error)) {
        if (error.empty()) error = "source model400 door PCM unavailable";
        return Status::AudioError;
    }
    try {
        auto openOwner = std::make_shared<const SfxSingleSoundResult>(std::move(open));
        auto closeOwner = std::make_shared<const SfxSingleSoundResult>(std::move(close));
        std::vector<NativeSourceSliceActionEvent> actions;
        actions.push_back({1, 1, 0, NativeSourceSliceActionKind::Spawn,
            lifecycle->Phase, lifecycle->Task, 0, 0, 0, 0, 0, 0, 0, lifecycle->TimeMs});
        NativeSourceSliceFeedbackSnapshot next;
        next.Epoch = lifecycle->Epoch; next.Generation = 1;
        next.LifecycleGeneration = lifecycle->Generation; next.LastLifecycleEvent = 1;
        next.Hud = Hud(*lifecycle); next.Actions = actions;
        auto published = std::make_shared<const NativeSourceSliceFeedbackSnapshot>(std::move(next));
        m_DoorOpen = std::move(openOwner); m_DoorClose = std::move(closeOwner);
        m_Lifecycle = std::move(lifecycle); m_Actions = std::move(actions);
        m_Audio.clear(); m_Published = std::move(published);
        error.clear();
        return Status::Ok;
    } catch (const std::exception& exception) {
        error = exception.what();
        return Status::Overflow;
    }
}

NativeSourceSliceFeedbackStatus NativeSourceSliceFeedback::Observe(
    std::shared_ptr<const NativeSourceVehicleLifecycleSnapshot> lifecycle, std::string& error) {
    if (!m_Published || !m_Lifecycle) { error = "source slice feedback not loaded"; return Status::NotLoaded; }
    if (lifecycle && lifecycle->Generation == m_Published->LifecycleGeneration &&
        *lifecycle == *m_Lifecycle) { error.clear(); return Status::DuplicateIdempotent; }
    if (!lifecycle || !ValidateLifecycle(*lifecycle) || lifecycle->Epoch != m_Published->Epoch ||
        lifecycle->Generation <= m_Published->LifecycleGeneration ||
        lifecycle->TimeMs < m_Lifecycle->TimeMs || lifecycle->InputSequence < m_Lifecycle->InputSequence) {
        error = "stale or skipped source lifecycle publication";
        return Status::StaleLifecycle;
    }
    if (lifecycle->Events.size() < m_Lifecycle->Events.size() ||
        lifecycle->Events.size() > m_Lifecycle->Events.size() + 1 ||
        !std::equal(m_Lifecycle->Events.begin(), m_Lifecycle->Events.end(), lifecycle->Events.begin())) {
        error = "source lifecycle event prefix is missing or changed";
        return Status::MissingLifecycleEvent;
    }
    try {
        auto actions = m_Actions;
        auto audio = m_Audio;
        for (std::size_t i = m_Lifecycle->Events.size(); i < lifecycle->Events.size(); ++i) {
            const auto& source = lifecycle->Events[i];
            NativeSourceSliceActionEvent action;
            action.Sequence = actions.size() + 1; action.LifecycleSequence = source.Sequence;
            action.InputSequence = source.InputSequence; action.Kind = ActionKind(source.Kind);
            action.Phase = source.Phase; action.Task = source.Task;
            action.PadButtons = lifecycle->PadButtons; action.TimeMs = source.TimeMs;
            if (source.Kind == NativeSourceVehicleLifecycleEventKind::EnterRequested ||
                source.Kind == NativeSourceVehicleLifecycleEventKind::ExitRequested) {
                if (!(lifecycle->PadPressed & Triangle))
                    throw std::runtime_error("vehicle lifecycle action lost Triangle edge");
                action.Flags |= NativeSourceSliceActionEnterExit;
            }
            if (source.Kind == NativeSourceVehicleLifecycleEventKind::Drive && lifecycle->Automobile) {
                if (lifecycle->PadButtons & Cross) action.Flags |= NativeSourceSliceActionAccelerate;
                if (lifecycle->PadButtons & Square) action.Flags |= NativeSourceSliceActionBrake;
                if (lifecycle->PadMoveX != 0) action.Flags |= NativeSourceSliceActionSteer;
                action.MoveX = lifecycle->PadMoveX;
                action.GasPedal = lifecycle->Automobile->GasPedal;
                action.BrakePedal = lifecycle->Automobile->BrakePedal;
                action.Steering = lifecycle->Automobile->SteerAngle;
                action.VehicleSpeed = lifecycle->Automobile->ForwardSpeed;
            }
            actions.push_back(action);
            if (IsOpenEvent(source.Kind) || IsCloseEvent(source.Kind)) {
                if (!lifecycle->Automobile) throw std::runtime_error("door event has no source automobile");
                NativeSourceSliceAudioEvent sound;
                sound.Sequence = audio.size() + 1; sound.LifecycleSequence = source.Sequence;
                sound.InputSequence = source.InputSequence; sound.EventId = IsOpenEvent(source.Kind)
                    ? VehicleDoorOpenEvent : VehicleDoorCloseEvent;
                sound.BankId = VehicleGeneralBank; sound.BankSlot = VehicleGeneralSlot;
                sound.SoundId = IsOpenEvent(source.Kind) ? NewDoorOpenSound : NewDoorCloseSound;
                sound.DoorType = NewDoorType; sound.FrequencyVariance = 0.02f;
                sound.Position = lifecycle->Automobile->Matrix.Position;
                sound.Clip = IsOpenEvent(source.Kind) ? m_DoorOpen : m_DoorClose;
                sound.TimeMs = source.TimeMs;
                audio.push_back(std::move(sound));
            }
        }
        NativeSourceSliceFeedbackSnapshot next;
        next.Epoch = m_Published->Epoch; next.Generation = m_Published->Generation + 1;
        next.LifecycleGeneration = lifecycle->Generation;
        next.LastLifecycleEvent = lifecycle->Events.size(); next.Hud = Hud(*lifecycle);
        next.Actions = actions; next.Audio = audio;
        auto published = std::make_shared<const NativeSourceSliceFeedbackSnapshot>(std::move(next));
        m_Lifecycle = std::move(lifecycle); m_Actions = std::move(actions);
        m_Audio = std::move(audio); m_Published = std::move(published);
        error.clear();
        return Status::Ok;
    } catch (const std::exception& exception) {
        error = exception.what();
        return Status::Overflow;
    }
}
