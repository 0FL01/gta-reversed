#include "NativePedStreaming.h"
#include "NativeCarGeneratorPopulation.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <limits>

void NativeInitializePedGangWar(NativePedGangWarControlState& state) {
    state.Known = true;
    state.OffensiveState = 0;
    state.SpecificZoneCount = 0;
    state.Provocation = 0.0f;
    state.Active = false;
    state.PlayerOnMission = false;
    state.War.StateKnown = true;
    state.War.AttackState = 0;
}

void NativeResetPedStreamingRequests(NativePedZoneStreamState& state, std::uint16_t& requestedGangPeds) {
    state.Slots.Known = true;
    state.Slots.Models.fill(-1);
    state.Slots.Count = 0;
    state.Selection.NextPedToLoad.fill(0);
    state.CurrentZoneType = -1;
    requestedGangPeds = 0;
}

NativePedStreamStatus NativePlanPedGangWarUpdate(const NativePedGangWarControlState& state,
    const NativePedGangWarUpdateInput& input, NativePedGangWarUpdatePlan& out) {
    if (!state.Known) return NativePedStreamStatus::UnknownGangWar;
    if (!input.MissionKnown) return NativePedStreamStatus::UnknownMission;
    if (state.SpecificZoneCount < 0 || state.SpecificZoneCount > 6) return NativePedStreamStatus::InvalidInput;
    NativePedGangWarUpdatePlan candidate;
    candidate.PlayerOnMission = input.PlayerOnMission;
    if (input.PlayerOnMission && !state.PlayerOnMission && !state.SpecificZoneCount)
        candidate.Effects[candidate.Count++] = NativePedGangWarUpdateEffectKind::EndGangWarForMission;
    candidate.Effects[candidate.Count++] = NativePedGangWarUpdateEffectKind::PublishMissionState;
    if (!input.CutsceneKnown) return NativePedStreamStatus::UnknownCutscene;
    if (!input.CutsceneProcessing) {
        if (!input.FrameCounterKnown) return NativePedStreamStatus::UnknownFrameCounter;
        if (std::uint8_t(input.FrameCounter) == 56)
            candidate.Effects[candidate.Count++] = NativePedGangWarUpdateEffectKind::UpdateTerritoryPercentage;
        if (state.Active) {
            if (!input.CoopKnown) return NativePedStreamStatus::UnknownCoop;
            if (!input.Coop)
                candidate.Effects[candidate.Count++] = NativePedGangWarUpdateEffectKind::ActiveControllerUpdate;
        }
    }
    out = candidate;
    return NativePedStreamStatus::PlannedGangWarUpdate;
}

NativePedStreamStatus NativeObservePedGangDemand(const NativePedGangDemandInput& input,
    NativePedGangDemand& out) {
    if (!input.ZoneKnown) return NativePedStreamStatus::UnknownZone;
    NativePedGangDemand candidate{input.HasZone, 0};
    if (!input.HasZone) {
        out = candidate;
        return NativePedStreamStatus::QualifiedGangDemand;
    }
    if (!input.CheatKnown) return NativePedStreamStatus::UnknownCheat;
    for (std::size_t gang = 0; gang < input.GangStrength.size(); ++gang)
        if (input.GangStrength[gang]) candidate.Wanted |= std::uint16_t(1u << gang);
    if (input.StreetsCheat) candidate.Wanted |= 0xffu;
    const auto& war = input.War;
    if (!war.StateKnown) return NativePedStreamStatus::UnknownGangWar;
    if (war.AttackState < 0 || war.AttackState > 2) return NativePedStreamStatus::InvalidInput;
    if (war.AttackState) {
        if (!war.PlayerPositionKnown) return NativePedStreamStatus::UnknownPlayerPosition;
        if (!war.AttackPositionKnown) return NativePedStreamStatus::UnknownAttackPosition;
        for (std::size_t i = 0; i < 2; ++i)
            if (!std::isfinite(war.PlayerPosition[i]) || !std::isfinite(war.AttackPosition[i]))
                return NativePedStreamStatus::InvalidInput;
        // Original x87 subtract/spill, extended products/add then one float
        // spill, sqrt/spill. Keep the extended add, including unequal exponents.
        const float dx = float(static_cast<long double>(war.PlayerPosition[0]) - war.AttackPosition[0]);
        const float dy = float(static_cast<long double>(war.PlayerPosition[1]) - war.AttackPosition[1]);
        const float squared = float(static_cast<long double>(dx) * dx + static_cast<long double>(dy) * dy);
        const float distance = float(std::sqrt(double(squared)));
        if (distance < 150.0f) {
            if (!war.GangKnown) return NativePedStreamStatus::UnknownGangMember;
            if (war.Gang < 0 || war.Gang >= 10) return NativePedStreamStatus::InvalidInput;
            candidate.Wanted |= std::uint16_t(1u << war.Gang);
        }
    }
    out = candidate;
    return NativePedStreamStatus::QualifiedGangDemand;
}

NativePedStreamStatus NativeQualifyPedGangDemand(const NativePopulationCycleObservation& cycle,
    bool cheatKnown, bool streetsCheat, const NativePedGangWarObservation& war,
    NativePedGangMaskInput& out) {
    NativePedGangDemandInput input;
    input.ZoneKnown = cycle.Known;
    input.HasZone = cycle.Known;
    input.GangStrength = cycle.Zone.GangStrength;
    input.CheatKnown = cheatKnown;
    input.StreetsCheat = streetsCheat;
    input.War = war;
    NativePedGangDemand demand;
    const auto status = NativeObservePedGangDemand(input, demand);
    if (status != NativePedStreamStatus::QualifiedGangDemand) return status;
    out.ZoneKnown = true;
    out.HasZone = demand.HasZone;
    out.DemandKnown = true;
    out.Wanted = demand.Wanted;
    return status;
}

NativePedStreamStatus NativePlanPedSlotRequests(const NativePedRequestedSlots& slots,
    const std::array<std::int32_t, 8>& requested, NativePedSlotPlan& out) {
    if (!slots.Known) return NativePedStreamStatus::UnknownSlots;
    std::uint32_t count = 0;
    for (std::size_t i = 0; i < slots.Models.size(); ++i) {
        if (slots.Models[i] < -1 || slots.Models[i] >= 20000 || requested[i] >= 20000)
            return NativePedStreamStatus::InvalidInput;
        count += slots.Models[i] >= 0;
    }
    if (count != slots.Count) return NativePedStreamStatus::InvalidInput;
    NativePedSlotPlan candidate;
    candidate.Next = slots;
    for (std::size_t i = 0; i < requested.size(); ++i) {
        const auto request = requested[i];
        if (request < 0 && request != -2) continue;
        auto& old = candidate.Next.Models[i];
        if (old >= 0) {
            candidate.Effects[candidate.EffectCount++] = {
                NativePedSlotEffectKind::MakeModelAndTxdDeletable, std::uint8_t(i), old};
            old = -1;
            --candidate.Next.Count;
        }
        if (request >= 0) {
            candidate.Effects[candidate.EffectCount++] = {
                NativePedSlotEffectKind::RequestKeepInMemory, std::uint8_t(i), request};
            old = request;
            ++candidate.Next.Count;
        }
    }
    out = candidate;
    return NativePedStreamStatus::PlannedSlots;
}

namespace {
template<std::size_t N>
NativePedStreamStatus QualifyGroups(const NativePedModelMetadata& metadata,
    std::uint32_t first, std::uint32_t worldZone, std::array<NativePedStreamGroup, N>& out) {
    assert(first + N <= NativePedGroupTranslation.size());
    if (worldZone >= 3) return NativePedStreamStatus::InvalidInput;
    if (metadata.Models().empty()) return NativePedStreamStatus::UnknownModel;
    std::array<NativePedStreamGroup, N> candidate{};
    for (std::size_t i = 0; i < candidate.size(); ++i) {
        const auto& source = metadata.Groups()[NativePedGroupTranslation[first + i][worldZone]];
        if (source.Count > 21) return NativePedStreamStatus::InvalidInput;
        auto& group = candidate[i];
        group.Known = true;
        group.Count = source.Count;
        for (std::size_t j = 0; j < group.Count; ++j) {
            const auto* model = metadata.Find(source.Models[j]);
            if (!model) return NativePedStreamStatus::UnknownModel;
            if (model->Race < 0 || model->Race > 4) return NativePedStreamStatus::InvalidInput;
            group.Models[j] = {source.Models[j], true, std::uint8_t(model->Race)};
        }
    }
    out = candidate;
    return NativePedStreamStatus::QualifiedGroups;
}
} // namespace

NativePedStreamStatus NativeQualifyPedStreamingGroups(const NativePedModelMetadata& metadata,
    std::uint32_t worldZone, std::array<NativePedStreamGroup, 18>& out) {
    return QualifyGroups(metadata, 0, worldZone, out);
}

NativePedStreamStatus NativeQualifyPedGangGroups(const NativePedModelMetadata& metadata,
    std::array<NativePedStreamGroup, 10>& out) {
    return QualifyGroups(metadata, 18, 0, out);
}

NativePedStreamStatus NativeQualifyPedGangCarGroups(const NativeCarGeneratorPopulation& population,
    std::array<NativePedGangCarGroup, 10>& out) {
    std::array<NativePedGangCarGroup, 10> candidate{};
    for (std::size_t gang = 0; gang < candidate.size(); ++gang) {
        std::span<const std::int32_t> models;
        if (!population.ObserveGroupModels(std::uint32_t(18 + gang), models))
            return NativePedStreamStatus::UnknownGroup;
        if (models.size() > candidate[gang].Models.size()) return NativePedStreamStatus::InvalidInput;
        candidate[gang].Known = true;
        candidate[gang].Count = std::uint16_t(models.size());
        for (std::size_t i = 0; i < models.size(); ++i) {
            if (models[i] <= 0 || models[i] >= 20000) return NativePedStreamStatus::UnknownModel;
            candidate[gang].Models[i].Model = models[i];
        }
    }
    out = candidate;
    return NativePedStreamStatus::QualifiedGroups;
}

NativePedStreamStatus NativeAdvancePedGangMask(const NativePedGangMaskInput& input,
    NativeSourceRngRef rng, NativePedGangMaskState& state, NativePedGangMaskEffects& out) {
    out = {};
    if (!input.ZoneKnown) return NativePedStreamStatus::UnknownZone;
    if (!input.HasZone) return NativePedStreamStatus::GangMaskPhaseComplete;
    if (!input.DemandKnown) return NativePedStreamStatus::UnknownGangDemand;
    if (input.Wanted & ~0x3ffu) return NativePedStreamStatus::InvalidInput;
    if (!state.Known) return NativePedStreamStatus::UnknownRequestedGangs;
    if (input.Wanted == state.RequestedPeds && input.Wanted == state.RequestedCars)
        return NativePedStreamStatus::GangMaskPhaseComplete;
    const auto emit = [&](NativePedGangMaskEffectKind kind, std::int32_t model) {
        assert(out.Count < out.Effects.size());
        out.Effects[out.Count++] = {kind, model};
    };
    for (std::size_t gang = 0; gang < input.PedGroups.size(); ++gang) {
        const auto bit = std::uint16_t(1u << gang);
        const bool wanted = (input.Wanted & bit) != 0;
        const bool requested = (state.RequestedPeds & bit) != 0;
        if (wanted != requested) {
            const auto& group = input.PedGroups[gang];
            if (!group.Known) return NativePedStreamStatus::UnknownGroup;
            if (group.Count > group.Models.size()) return NativePedStreamStatus::InvalidInput;
            if (wanted) {
                if (!input.MemberKnown) return NativePedStreamStatus::UnknownGangMember;
                if (input.CurrentMember < 0 || input.CurrentMember > 20 || !group.Count)
                    return NativePedStreamStatus::InvalidInput;
                for (std::int32_t offset = 0; offset < 2; ++offset) {
                    const auto model = group.Models[std::size_t((input.CurrentMember + offset) % group.Count)].Model;
                    if (model <= 0 || model >= 20000) return NativePedStreamStatus::UnknownModel;
                    emit(NativePedGangMaskEffectKind::RequestKeepInMemory, model);
                }
                state.RequestedPeds |= bit;
            } else {
                for (std::size_t slot = 0; slot < group.Count; ++slot) {
                    const auto model = group.Models[slot].Model;
                    if (model <= 0 || model >= 20000) return NativePedStreamStatus::UnknownModel;
                    emit(NativePedGangMaskEffectKind::MakeModelAndTxdDeletable, model);
                }
                state.RequestedPeds &= std::uint16_t(~bit);
            }
        }
        const auto& cars = input.LoadedCars[gang];
        if (!cars.Known) return NativePedStreamStatus::UnknownGangCars;
        if (cars.Count > 23) return NativePedStreamStatus::InvalidInput;
        // Literal original branch, NOT the tempting >=1 correction. With a
        // stable empty snapshot the retirement branch copies zero members.
        if (cars.Count || !wanted || (state.RequestedCars & bit)) continue;
        const auto& group = input.CarGroups[gang];
        if (!group.Known) return NativePedStreamStatus::UnknownGroup;
        if (group.Count > group.Models.size()) return NativePedStreamStatus::InvalidInput;
        const auto draw = rng.NextRand15();
        if (!draw.Value) return NativePedStreamStatus::UnknownRng;
        if (!group.Count) return NativePedStreamStatus::InvalidInput; // Original divides by zero after rand.
        const auto& model = group.Models[*draw.Value % group.Count];
        if (model.Model <= 0 || model.Model >= 20000) return NativePedStreamStatus::UnknownModel;
        if (!model.StreamingKnown) return NativePedStreamStatus::UnknownStreaming;
        if (!model.Loaded) emit(NativePedGangMaskEffectKind::RequestKeepInMemory, model.Model);
    }
    state.RequestedCars = input.Wanted;
    return NativePedStreamStatus::GangMaskPhaseComplete;
}

NativePedStreamStatus NativePickPedModelToStream(const NativePedStreamInput& input,
    NativeSourceRngRef rng, NativePedStreamState& state, NativePedStreamChoice& out) {
    if (!input.ZoneKnown) return NativePedStreamStatus::UnknownZone;
    if (!input.SlotsKnown) return NativePedStreamStatus::UnknownSlots;
    if (rng.Readiness() != NativeSourceRngStatus::Ready) return NativePedStreamStatus::UnknownRng;
    for (const auto model : input.Slots)
        if (model < -1 || model >= 20000) return NativePedStreamStatus::InvalidInput;
    NativePedStreamChoice candidate;
    for (std::uint8_t trial = 0; trial < 10; ++trial) {
        const auto draw = rng.NextRand15();
        if (!draw.Value) return NativePedStreamStatus::UnknownRng;
        ++candidate.Draws;
        // Original: rand * double(1/32768), float spill, double(100), trunc.
        // It is neither rand%100 nor the refactor's inclusive >= boundary.
        const float fraction = float(double(*draw.Value) * (1.0 / 32768.0));
        auto percentage = std::int32_t(double(fraction) * 100.0);
        std::size_t selected = 0;
        for (; selected < input.Percentages.size(); ++selected) {
            if (percentage < input.Percentages[selected]) break;
            percentage -= input.Percentages[selected];
        }
        if (selected == input.Percentages.size()) return NativePedStreamStatus::InvalidDistribution;
        const auto& group = input.Groups[selected];
        if (!group.Known) return NativePedStreamStatus::UnknownGroup;
        if (group.Count > group.Models.size()) return NativePedStreamStatus::InvalidInput;
        auto& cursor = state.NextPedToLoad[selected];
        if (cursor < 0 || cursor == std::numeric_limits<std::int32_t>::max())
            return NativePedStreamStatus::InvalidInput;
        for (std::size_t visited = 0; visited < group.Count; ++visited) {
            cursor = (cursor + 1) % group.Count;
            const auto& model = group.Models[std::size_t(cursor)];
            if (model.Model <= 0 || model.Model >= 20000 || model.Race > 4)
                return NativePedStreamStatus::InvalidInput;
            // The original reads race even for a duplicate request slot.
            if (!model.RaceKnown) return NativePedStreamStatus::UnknownModel;
            const bool duplicate = std::ranges::find(input.Slots, model.Model) != input.Slots.end();
            const bool raceAllowed = model.Race == 0 ||
                ((input.RaceMask & 15u) & (1u << (model.Race - 1))) != 0;
            if (duplicate || !raceAllowed) continue;
            candidate.Model = model.Model;
            candidate.Group = std::int32_t(selected);
            out = candidate;
            return NativePedStreamStatus::Selected;
        }
    }
    return NativePedStreamStatus::NoSelection;
}

NativePedStreamStatus NativeQualifyPedCycleSelection(const NativePedModelMetadata& metadata,
    std::uint32_t worldZone, const NativePopulationCycleObservation& observed, NativePedStreamInput& out) {
    if (!observed.Known) return NativePedStreamStatus::UnknownZone;
    if (observed.Zone.PopulationType >= 20 || observed.RowIndex >= 480 ||
        observed.RowIndex / 24 != observed.Zone.PopulationType)
        return NativePedStreamStatus::InvalidInput;
    NativePedStreamInput candidate;
    const auto status = NativeQualifyPedStreamingGroups(metadata, worldZone, candidate.Groups);
    if (status != NativePedStreamStatus::QualifiedGroups) return status;
    candidate.ZoneKnown = true;
    candidate.RaceMask = observed.Zone.Races & 15u;
    std::copy_n(observed.Row.begin() + 6, candidate.Percentages.size(), candidate.Percentages.begin());
    out = candidate;
    return NativePedStreamStatus::QualifiedCycleInput;
}

NativePedStreamStatus NativeAdvancePedZoneRequests(const NativePedZoneStreamInput& input,
    NativeSourceRngRef rng, NativePedZoneStreamState& state, NativePedZoneStreamEffects& out) {
    out = {};
    if (!input.ZoneKnown) return NativePedStreamStatus::UnknownZone;
    if (!input.HasZone) return NativePedStreamStatus::ZonePhaseComplete;
    if (!input.CheatKnown) return NativePedStreamStatus::UnknownCheat;
    if (input.ZoneStreamingCheat) return NativePedStreamStatus::ZonePhaseComplete;
    if (!state.Slots.Known) return NativePedStreamStatus::UnknownSlots;
    if (state.CurrentZoneType < -1 || state.CurrentZoneType > 31)
        return NativePedStreamStatus::InvalidInput;
    std::uint32_t count = 0;
    for (const auto model : state.Slots.Models) {
        if (model < -1 || model >= 20000) return NativePedStreamStatus::InvalidInput;
        count += model >= 0;
    }
    if (count != state.Slots.Count) return NativePedStreamStatus::InvalidInput;
    const auto emit = [&](NativePedZoneEffectKind kind, std::int32_t model) {
        assert(out.Count < out.Effects.size());
        out.Effects[out.Count++] = {kind, model};
    };
    const auto request = [&](std::int32_t model) {
        emit(NativePedZoneEffectKind::RequestKeepAndGameRequired, model);
        emit(NativePedZoneEffectKind::ClearGameRequired, model);
    };
    const auto choose = [&](NativePedStreamChoice& choice) {
        auto selection = input.Selection;
        selection.SlotsKnown = state.Slots.Known;
        selection.Slots = state.Slots.Models;
        return NativePickPedModelToStream(selection, rng, state.Selection, choice);
    };
    const auto populationType = std::int32_t(input.PopulationType & 31u);
    if (populationType == state.CurrentZoneType) {
        if (state.TimeBeforeNextLoad >= 0) {
            --state.TimeBeforeNextLoad;
            return NativePedStreamStatus::ZonePhaseComplete;
        }
        std::size_t slot = 0;
        for (; slot < state.Slots.Models.size(); ++slot) {
            const auto model = state.Slots.Models[slot];
            if (model == -1) break;
            const auto& references = input.References[slot];
            if (!references.Known || references.Model != model)
                return NativePedStreamStatus::UnknownReferences;
            if (references.Count == 0) break;
        }
        if (slot == state.Slots.Models.size()) return NativePedStreamStatus::ZonePhaseComplete;
        NativePedStreamChoice choice;
        const auto status = choose(choice);
        if (status == NativePedStreamStatus::NoSelection) return NativePedStreamStatus::ZonePhaseComplete;
        if (status != NativePedStreamStatus::Selected) return status;
        if (choice.Model == state.Slots.Models[slot]) return NativePedStreamStatus::ZonePhaseComplete;
        request(choice.Model);
        if (state.Slots.Count == 8) {
            emit(NativePedZoneEffectKind::MakeModelAndTxdDeletable, state.Slots.Models[slot]);
            state.Slots.Models[slot] = -1;
        } else {
            ++state.Slots.Count;
        }
        const auto freeSlot = std::ranges::find(state.Slots.Models, -1);
        assert(freeSlot != state.Slots.Models.end());
        *freeSlot = choice.Model;
        state.TimeBeforeNextLoad = 300;
        return NativePedStreamStatus::ZonePhaseComplete;
    }
    const auto toLoad = std::max(state.Slots.Count, 4u);
    for (auto& model : state.Slots.Models) {
        if (model < 0) continue;
        emit(NativePedZoneEffectKind::MakeModelAndTxdDeletable, model);
        model = -1;
    }
    state.Slots.Count = 0;
    state.CurrentZoneType = populationType;
    for (std::uint32_t i = 0; i < toLoad; ++i) {
        NativePedStreamChoice choice;
        const auto status = choose(choice);
        if (status == NativePedStreamStatus::NoSelection) continue;
        if (status != NativePedStreamStatus::Selected) return status;
        request(choice.Model);
        state.Slots.Models[i] = choice.Model;
        ++state.Slots.Count;
    }
    // Original zone-change branch enters the common decrement with eax=300.
    // A successful same-zone replacement instead writes 300 and bypasses it.
    state.TimeBeforeNextLoad = 299;
    return NativePedStreamStatus::ZonePhaseComplete;
}

NativePedStreamStatus NativeAdvancePedGangRequests(const NativePedGangStreamInput& input,
    NativePedGangStreamState& state, NativePedGangStreamEffects& out) {
    out = {};
    if (!input.ZoneKnown) return NativePedStreamStatus::UnknownZone;
    if (!input.HasZone) return NativePedStreamStatus::GangPhaseComplete;
    if (!input.CheatKnown) return NativePedStreamStatus::UnknownCheat;
    if (input.ZoneStreamingCheat) return NativePedStreamStatus::GangPhaseComplete;
    if (state.TimeBeforeNextLoad >= 0) {
        --state.TimeBeforeNextLoad;
        return NativePedStreamStatus::GangPhaseComplete;
    }
    if (state.CurrentMember < 0 || state.CurrentMember >= 21) return NativePedStreamStatus::InvalidInput;
    if (!input.RequestedGangsKnown) return NativePedStreamStatus::UnknownRequestedGangs;
    const auto previous = state.CurrentMember;
    const auto previousNext = previous + 1;
    state.CurrentMember = previousNext % 21;
    state.TimeBeforeNextLoad = 550;
    for (std::size_t gang = 0; gang < input.Groups.size(); ++gang) {
        if (!(input.RequestedGangs & (1u << gang))) continue;
        const auto& group = input.Groups[gang];
        if (!group.Known) return NativePedStreamStatus::UnknownGroup;
        if (group.Count == 0 || group.Count > 21) return NativePedStreamStatus::InvalidInput;
        const auto oldA = previous % group.Count;
        const auto oldB = previousNext % group.Count;
        const auto newA = state.CurrentMember % group.Count;
        const auto newB = (state.CurrentMember + 1) % group.Count;
        for (std::int32_t slot = 0; slot < group.Count; ++slot) {
            const bool oldMember = slot == oldA || slot == oldB;
            const bool newMember = slot == newA || slot == newB;
            if (oldMember == newMember) continue;
            const auto model = group.Models[std::size_t(slot)].Model;
            if (model <= 0 || model >= 20000) return NativePedStreamStatus::UnknownModel;
            assert(out.Count < out.Effects.size());
            out.Effects[out.Count++] = {newMember ? NativePedGangEffectKind::RequestGameRequired :
                NativePedGangEffectKind::MakeModelAndTxdDeletable, model};
        }
    }
    return NativePedStreamStatus::GangPhaseComplete;
}
