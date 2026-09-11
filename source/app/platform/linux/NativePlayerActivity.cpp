#include "app/platform/linux/NativePlayerActivity.h"

#include <algorithm>

namespace {
constexpr std::size_t Index(NativePlayerPrimarySlot slot) {
    return static_cast<std::size_t>(slot);
}

constexpr std::size_t Index(NativePlayerSecondarySlot slot) {
    return static_cast<std::size_t>(slot);
}

NativeMissionStartDecision Denied(NativeMissionStartReason reason) {
    return {NativeMissionStartOutcome::Denied, reason};
}

NativeMissionStartDecision Unsupported() {
    return {NativeMissionStartOutcome::Unsupported, NativeMissionStartReason::ActivityUnsupported};
}

bool HasTask(const NativePlayerTaskChain& chain, NativePlayerTaskType task) {
    return std::find(chain.begin(), chain.end(), task) != chain.end();
}
} // namespace

NativeMissionStartDecision NativePlayerCanStartMission(const NativePlayerActivitySnapshot& snapshot) {
    if (snapshot.Authority != NativePlayerActivityAuthority::SourceBacked) {
        return Unsupported();
    }

    // CPlayerPed::CanPlayerStartMission, 0x609590: preserve source gate order.
    if (!snapshot.GamePlaying) {
        return Denied(NativeMissionStartReason::GameNotPlaying);
    }
    if (snapshot.CoopGame) {
        return Denied(NativeMissionStartReason::CoopGame);
    }

    if (snapshot.PedState == NativePlayerPedState::Unsupported) {
        return Unsupported();
    }

    const bool pedInControl = !snapshot.Landing && !snapshot.InAir && snapshot.Alive &&
                              snapshot.PedState != NativePlayerPedState::Arrested;
    if (!pedInControl && snapshot.PedState != NativePlayerPedState::Driving) {
        return Denied(NativeMissionStartReason::PedNotInControlOrDriving);
    }

    if (!snapshot.PrimaryTasks[Index(NativePlayerPrimarySlot::PhysicalResponse)].empty()) {
        return Denied(NativeMissionStartReason::PhysicalResponseTask);
    }
    if (!snapshot.PrimaryTasks[Index(NativePlayerPrimarySlot::EventResponseNonTemp)].empty()) {
        return Denied(NativeMissionStartReason::EventResponseNonTempTask);
    }

    const auto& primary = snapshot.PrimaryTasks[Index(NativePlayerPrimarySlot::Primary)];
    if (!primary.empty()) {
        if (primary.front() == NativePlayerTaskType::Unsupported) {
            return Unsupported();
        }
        if (primary.front() != NativePlayerTaskType::CarDrive) {
            return Denied(NativeMissionStartReason::PrimaryTaskNotCarDrive);
        }
    }

    if (!snapshot.SecondaryTasks[Index(NativePlayerSecondarySlot::Attack)].empty()) {
        return Denied(NativeMissionStartReason::AttackTask);
    }
    if (!snapshot.Alive) {
        return Denied(NativeMissionStartReason::PlayerNotAlive);
    }

    if (std::find(snapshot.TypedEvents.begin(), snapshot.TypedEvents.end(), NativePlayerEventType::ScriptCommand) !=
        snapshot.TypedEvents.end()) {
        return Denied(NativeMissionStartReason::ScriptCommandEvent);
    }
    if (std::find(snapshot.TypedEvents.begin(), snapshot.TypedEvents.end(), NativePlayerEventType::Unsupported) !=
        snapshot.TypedEvents.end()) {
        return Unsupported();
    }
    return {NativeMissionStartOutcome::Allowed, NativeMissionStartReason::None};
}

bool NativePlayerPickupBusy(const NativePlayerActivitySnapshot& snapshot) {
    if (snapshot.Authority != NativePlayerActivityAuthority::SourceBacked) {
        return true;
    }
    // CPedIntelligence::FindTaskByType checks these four primary chains only,
    // in this non-slot order, for both pickup blockers.
    constexpr std::array slots{
        NativePlayerPrimarySlot::Default,
        NativePlayerPrimarySlot::Primary,
        NativePlayerPrimarySlot::EventResponseTemp,
        NativePlayerPrimarySlot::EventResponseNonTemp,
    };
    for (const auto slot : slots) {
        const auto& chain = snapshot.PrimaryTasks[Index(slot)];
        if (HasTask(chain, NativePlayerTaskType::EnterCarAsDriver) ||
            HasTask(chain, NativePlayerTaskType::UseMobilePhone)) {
            return true;
        }
    }
    return false;
}

bool NativePlayerWantsUnarmedPickup(const NativePlayerActivitySnapshot& snapshot) {
    if (snapshot.Authority != NativePlayerActivityAuthority::SourceBacked) {
        return false;
    }
    constexpr std::size_t unarmedSlot = 0;
    const auto weapon = snapshot.WeaponSlots[unarmedSlot].Type;
    if (weapon == NativePlayerWeaponType::Unarmed) {
        return true;
    }
    if (weapon == NativePlayerWeaponType::Unsupported) {
        return false;
    }

    // GetTaskJetPack is the simplest task in the first occupied primary slot.
    for (const auto& chain : snapshot.PrimaryTasks) {
        if (chain.empty()) {
            continue;
        }
        if (chain.back() == NativePlayerTaskType::JetPack) {
            return false;
        }
        if (chain.back() == NativePlayerTaskType::Unsupported) {
            return false;
        }
        break;
    }

    if (snapshot.ActiveWeaponSlot == unarmedSlot) {
        if (snapshot.PedState == NativePlayerPedState::Attack ||
            snapshot.PedState == NativePlayerPedState::AimGun ||
            snapshot.PedState == NativePlayerPedState::Unsupported) {
            return false;
        }
    }
    return true;
}
