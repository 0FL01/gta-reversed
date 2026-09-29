#include "NativePedStreaming.h"

#include <cstdio>
#include <cstdlib>
#include <bit>
#include <cmath>
#include <limits>

namespace {
std::size_t s_Checks = 0;
void Check(bool condition, const char* message) {
    ++s_Checks;
    if (!condition) { std::fprintf(stderr, "ped-streaming-fail %s\n", message); std::abort(); }
}

NativePedStreamInput Fixture(int profile, int count, int distribution) {
    NativePedStreamInput input;
    input.ZoneKnown = input.SlotsKnown = true;
    input.RaceMask = profile == 1 ? 0 : profile == 2 ? 4 : 15;
    input.Percentages.fill(6);
    if (distribution == 1) { input.Percentages.fill(0); input.Percentages[17] = 100; }
    if (distribution == 2) { input.Percentages.fill(0); input.Percentages[0] = 1; input.Percentages[1] = 99; }
    for (std::size_t g = 0; g < input.Groups.size(); ++g) {
        auto& group = input.Groups[g];
        group.Known = true;
        group.Count = std::uint16_t(count);
        for (std::size_t j = 0; j < group.Count; ++j)
            group.Models[j] = {std::int32_t(10 + g * 21 + j), true,
                std::uint8_t(profile == 1 ? 1 : (j + g) % 5)};
    }
    if (profile == 3) {
        for (std::size_t j = 0; j < input.Slots.size(); ++j)
            input.Slots[j] = std::int32_t(10 + 17 * 21 + j);
    }
    return input;
}

void Cases() {
    for (std::uint32_t index = 0; index < 64; ++index) {
        // Cover the full rand15 domain, plus exact percentage-boundary seeds.
        // Small consecutive CRT seeds alone would mostly choose group0.
        constexpr std::uint32_t boundarySeeds[]{2708534849u, 2768500289u, 3596154433u, 4028364353u};
        const auto seed = index < 4 ? boundarySeeds[index] : index * 0x9e3779b9u;
        for (int profile = 0; profile < 4; ++profile)
            for (int count : {0, 1, 8, 21})
                for (int distribution = 0; distribution < 3; ++distribution) {
                    const auto input = Fixture(profile, count, distribution);
                    NativePedStreamState state;
                    for (std::size_t g = 0; g < state.NextPedToLoad.size(); ++g)
                        state.NextPedToLoad[g] = std::int32_t((seed + g) % 23);
                    NativeSourceRng rng;
                    Check(rng.SeedOnce(seed) == NativeSourceRngStatus::Ready, "fixture shared seed");
                    NativePedStreamChoice result{777, 88, 99};
                    const auto status = NativePickPedModelToStream(input, rng.Reference(), state, result);
                    const auto provenance = *rng.Inspect().Value;
                    Check(status == NativePedStreamStatus::Selected || status == NativePedStreamStatus::NoSelection,
                        "valid fixture selection or exhausted");
                    if (status == NativePedStreamStatus::Selected)
                        Check(result.Draws == provenance.DrawCount && result.Model > 0, "one draw per attempt");
                    else Check(result.Model == 777 && result.Group == 88 && result.Draws == 99 &&
                        provenance.DrawCount == 10, "exhaustion retains choice and consumes ten draws");
                    std::printf("CASE %u %d %d %d %d %d %u %llu", seed, profile, count, distribution,
                        int(status), result.Model, provenance.State, static_cast<unsigned long long>(provenance.DrawCount));
                    for (const auto cursor : state.NextPedToLoad) std::printf(" %d", cursor);
                    std::printf("\n");
                }
    }
}

void Guards() {
    auto input = Fixture(0, 8, 1);
    NativeSourceRng rng;
    Check(rng.SeedOnce(0) == NativeSourceRngStatus::Ready, "guard seed");
    NativePedStreamState state;
    NativePedStreamChoice choice{777, 88, 99};
    input.ZoneKnown = false;
    Check(NativePickPedModelToStream(input, rng.Reference(), state, choice) ==
        NativePedStreamStatus::UnknownZone && !rng.Inspect().Value->DrawCount, "unknown zone consumes no RNG");
    input.ZoneKnown = true;
    input.SlotsKnown = false;
    Check(NativePickPedModelToStream(input, rng.Reference(), state, choice) ==
        NativePedStreamStatus::UnknownSlots && !rng.Inspect().Value->DrawCount, "unknown slots not empty");
    input.SlotsKnown = true;
    Check(NativePickPedModelToStream(input, {}, state, choice) == NativePedStreamStatus::UnknownRng,
        "no independent RNG fallback");
    input.Groups[17].Known = false;
    Check(NativePickPedModelToStream(input, rng.Reference(), state, choice) == NativePedStreamStatus::UnknownGroup &&
        rng.Inspect().Value->DrawCount == 1 && choice.Model == 777, "group unavailable after source draw");
    input.Groups[17].Known = true;
    input.Groups[17].Models[1].RaceKnown = false;
    Check(NativePickPedModelToStream(input, rng.Reference(), state, choice) == NativePedStreamStatus::UnknownModel &&
        state.NextPedToLoad[17] == 1 && rng.Inspect().Value->DrawCount == 2 && choice.Model == 777,
        "race unavailable retains consumed cursor/draw but not a selected model");
    input.Percentages.fill(0);
    Check(NativePickPedModelToStream(input, rng.Reference(), state, choice) == NativePedStreamStatus::InvalidDistribution &&
        rng.Inspect().Value->DrawCount == 3 && choice.Model == 777, "malformed distribution cannot overrun group array");
    input.Percentages[17] = 100;
    input.Groups[17].Count = 22;
    Check(NativePickPedModelToStream(input, rng.Reference(), state, choice) == NativePedStreamStatus::InvalidInput,
        "source fixed group capacity");
    input.Groups[17].Count = 8;
    state.NextPedToLoad[17] = -1;
    Check(NativePickPedModelToStream(input, rng.Reference(), state, choice) == NativePedStreamStatus::InvalidInput,
        "invalid cursor cannot become a source slot");
    for (const auto seed : {2768500289u, 3596154433u, 4028364353u}) {
        NativeSourceRng boundary;
        Check(boundary.SeedOnce(seed) == NativeSourceRngStatus::Ready, "percentage boundary seed");
        auto fixture = Fixture(0, 1, 2);
        NativePedStreamState cursors;
        Check(NativePickPedModelToStream(fixture, boundary.Reference(), cursors, choice) == NativePedStreamStatus::Selected &&
            choice.Group == (seed == 2768500289u ? 0 : 1) && choice.Draws == 1,
            "strict boundary and rand15 maximum never spill beyond authored group percentages");
    }
}

void SlotPlans() {
    for (std::uint32_t index = 0; index < 256; ++index) {
        NativePedRequestedSlots slots;
        slots.Known = true;
        std::array<std::int32_t, 8> requests;
        for (std::size_t i = 0; i < slots.Models.size(); ++i) {
            slots.Models[i] = (index & (1u << i)) ? std::int32_t(10 + i) : -1;
            slots.Count += slots.Models[i] >= 0;
            switch ((index + i) % 5) {
            case 0: requests[i] = -1; break;
            case 1: requests[i] = -2; break;
            case 2: requests[i] = slots.Models[i] >= 0 ? slots.Models[i] : 10; break;
            case 3: requests[i] = 10; break; // intentional duplicate
            default: requests[i] = -3; break; // source also preserves this
            }
        }
        const auto observed = slots;
        NativePedSlotPlan plan;
        Check(NativePlanPedSlotRequests(slots, requests, plan) == NativePedStreamStatus::PlannedSlots,
            "all slot occupancy patterns and source request sentinels");
        Check(slots == observed, "planning cannot publish requested or loaded identities");
        std::printf("SLOTS %u %u %u", index, plan.Next.Count, plan.EffectCount);
        for (auto model : plan.Next.Models) std::printf(" %d", model);
        for (std::size_t i = 0; i < plan.EffectCount; ++i)
            std::printf(" %d %u %d", int(plan.Effects[i].Kind), unsigned(plan.Effects[i].Slot), plan.Effects[i].Model);
        std::printf("\n");
    }
    NativePedRequestedSlots unknown;
    NativePedSlotPlan result;
    result.Next.Count = 77;
    std::array<std::int32_t, 8> requests{};
    Check(NativePlanPedSlotRequests(unknown, requests, result) == NativePedStreamStatus::UnknownSlots &&
        result.Next.Count == 77, "unknown requested roster is not empty and preserves output");
    unknown.Known = true;
    unknown.Count = 8;
    Check(NativePlanPedSlotRequests(unknown, requests, result) == NativePedStreamStatus::InvalidInput &&
        result.Next.Count == 77, "inconsistent source request count rejected atomically");
    unknown.Count = 0;
    requests[7] = 20000;
    Check(NativePlanPedSlotRequests(unknown, requests, result) == NativePedStreamStatus::InvalidInput &&
        result.Next.Count == 77, "invalid later request cannot partially retire prior slots");
}

void ZonePhases() {
    for (std::uint32_t index = 0; index < 256; ++index) {
        for (std::uint32_t mode = 0; mode < 8; ++mode) {
            const auto seed = index * 0x9e3779b9u + mode;
            constexpr int counts[]{0, 1, 8, 21};
            const auto groupCount = counts[index % 4];
            NativePedZoneStreamInput input;
            input.ZoneKnown = input.CheatKnown = true;
            input.HasZone = mode != 4;
            input.ZoneStreamingCheat = mode == 5;
            input.PopulationType = 0xa0; // Original uses low five PopType bits.
            input.Selection = Fixture(0, groupCount, 1);
            NativePedZoneStreamState state;
            state.Slots.Known = true;
            state.CurrentZoneType = mode == 0 ? -1 : 0;
            state.TimeBeforeNextLoad = mode == 1 ? 0 : mode == 6 ? 299 : -1;
            for (std::size_t i = 0; i < state.Slots.Models.size(); ++i) {
                const auto model = std::int32_t((index & 1u ? 367 : 10) + i);
                if (index & (1u << i)) {
                    state.Slots.Models[i] = model;
                    ++state.Slots.Count;
                }
                input.References[i] = {true, state.Slots.Models[i],
                    std::uint16_t(mode == 3 || (mode == 7 && i < 3) ? 1 : 0)};
            }
            NativeSourceRng rng;
            Check(rng.SeedOnce(seed) == NativeSourceRngStatus::Ready, "zone-phase seed");
            NativePedZoneStreamEffects effects;
            Check(NativeAdvancePedZoneRequests(input, rng.Reference(), state, effects) ==
                NativePedStreamStatus::ZonePhaseComplete, "qualified civilian phase before gang tail");
            const auto provenance = *rng.Inspect().Value;
            std::printf("ZONE %u %u %u %u %d %d %u %llu %u", index, mode, seed, state.Slots.Count,
                state.CurrentZoneType, state.TimeBeforeNextLoad, provenance.State,
                static_cast<unsigned long long>(provenance.DrawCount), effects.Count);
            for (const auto model : state.Slots.Models) std::printf(" %d", model);
            for (const auto cursor : state.Selection.NextPedToLoad) std::printf(" %d", cursor);
            for (std::size_t i = 0; i < effects.Count; ++i)
                std::printf(" %d %d", int(effects.Effects[i].Kind), effects.Effects[i].Model);
            std::printf("\n");
        }
    }
    NativePedZoneStreamInput input;
    NativePedZoneStreamState state;
    const auto initial = state;
    NativePedZoneStreamEffects effects;
    Check(NativeAdvancePedZoneRequests(input, {}, state, effects) == NativePedStreamStatus::UnknownZone &&
        state == initial && !effects.Count, "unknown zone is not an absent zone");
    input.ZoneKnown = true;
    Check(NativeAdvancePedZoneRequests(input, {}, state, effects) == NativePedStreamStatus::ZonePhaseComplete &&
        state == initial, "absent zone short circuits unknown cheat and slot state");
    input.HasZone = true;
    Check(NativeAdvancePedZoneRequests(input, {}, state, effects) == NativePedStreamStatus::UnknownCheat &&
        state == initial, "unknown cheat not disabled");
    input.CheatKnown = input.ZoneStreamingCheat = true;
    Check(NativeAdvancePedZoneRequests(input, {}, state, effects) == NativePedStreamStatus::ZonePhaseComplete &&
        state == initial, "cheat early guard before slot observations");
    input.ZoneStreamingCheat = false;
    Check(NativeAdvancePedZoneRequests(input, {}, state, effects) == NativePedStreamStatus::UnknownSlots &&
        state == initial, "requested slots must be owned");
    state.Slots.Known = true;
    state.CurrentZoneType = 0;
    Check(NativeAdvancePedZoneRequests(input, {}, state, effects) == NativePedStreamStatus::ZonePhaseComplete &&
        state.TimeBeforeNextLoad == -1, "initial same-zone timer zero decrements without RNG");
    state.Slots.Models[0] = 7;
    state.Slots.Count = 1;
    const auto occupied = state;
    Check(NativeAdvancePedZoneRequests(input, {}, state, effects) == NativePedStreamStatus::UnknownReferences &&
        state == occupied && !effects.Count, "unknown refcount cannot authorize slot replacement");
    input.References[0] = {true, 8, 0};
    Check(NativeAdvancePedZoneRequests(input, {}, state, effects) == NativePedStreamStatus::UnknownReferences &&
        state == occupied, "stale identity refcount cannot authorize slot replacement");
    input.References[0] = {true, 7, 0};
    input.Selection = Fixture(0, 8, 1);
    Check(NativeAdvancePedZoneRequests(input, {}, state, effects) == NativePedStreamStatus::UnknownRng &&
        state == occupied && !effects.Count, "source picker cannot invent RNG");
    input.PopulationType = 1;
    input.Selection.Groups[17].Known = false;
    NativeSourceRng rng;
    Check(rng.SeedOnce(0) == NativeSourceRngStatus::Ready, "partial phase seed");
    Check(NativeAdvancePedZoneRequests(input, rng.Reference(), state, effects) == NativePedStreamStatus::UnknownGroup &&
        state.CurrentZoneType == 1 && state.Slots.Count == 0 && state.Slots.Models[0] == -1 &&
        state.TimeBeforeNextLoad == -1 && effects.Count == 1 && effects.Effects[0].Model == 7 &&
        rng.Inspect().Value->DrawCount == 1, "unavailable selection retains ordered retirement/RNG prefix, not fake completion");
    input.Selection.Groups[17].Known = true;
    input.Selection.Groups[17].Models[2].RaceKnown = false;
    state = occupied;
    NativeSourceRng partial;
    Check(partial.SeedOnce(0) == NativeSourceRngStatus::Ready, "partial accepted request seed");
    Check(NativeAdvancePedZoneRequests(input, partial.Reference(), state, effects) == NativePedStreamStatus::UnknownModel &&
        state.CurrentZoneType == 1 && state.Slots.Count == 1 && state.Slots.Models[0] == 368 &&
        state.Selection.NextPedToLoad[17] == 2 && state.TimeBeforeNextLoad == -1 && effects.Count == 3 &&
        effects.Effects[1] == NativePedZoneEffect{NativePedZoneEffectKind::RequestKeepAndGameRequired, 368} &&
        effects.Effects[2] == NativePedZoneEffect{NativePedZoneEffectKind::ClearGameRequired, 368} &&
        partial.Inspect().Value->DrawCount == 2, "later unavailable race retains accepted request and all consumed prefixes");
    state = occupied;
    state.Slots.Count = 2;
    const auto invalid = state;
    Check(NativeAdvancePedZoneRequests(input, {}, state, effects) == NativePedStreamStatus::InvalidInput &&
        state == invalid && !effects.Count, "inconsistent requested count cannot emit retirement or selection effects");
}

NativePedGangStreamInput GangFixture(std::uint16_t mask, std::uint16_t count) {
    NativePedGangStreamInput input;
    input.ZoneKnown = input.HasZone = input.CheatKnown = input.RequestedGangsKnown = true;
    input.RequestedGangs = mask;
    for (std::size_t gang = 0; gang < input.Groups.size(); ++gang) {
        auto& group = input.Groups[gang];
        group.Known = true;
        group.Count = count;
        for (std::size_t slot = 0; slot < count; ++slot)
            group.Models[slot].Model = std::int32_t(10 + gang * 21 + slot);
    }
    return input;
}

void GangCase(std::uint16_t mask, std::uint16_t count, std::int32_t current,
    std::int32_t timer, int mode) {
    auto input = GangFixture(mask, count);
    input.HasZone = mode != 1;
    input.ZoneStreamingCheat = mode == 2;
    NativePedGangStreamState state{timer, current};
    NativePedGangStreamEffects effects;
    Check(NativeAdvancePedGangRequests(input, state, effects) == NativePedStreamStatus::GangPhaseComplete,
        "source gang phase with explicit observations completes its intents");
    const bool active = mode == 0 && timer < 0;
    Check(state.CurrentMember == (active ? (current + 1) % 21 : current) &&
        state.TimeBeforeNextLoad == (mode != 0 ? timer : active ? 550 : timer - 1),
        "independent source timer/member and skipped guards");
    std::printf("GANG %d %u %u %d %d %d %d %u", mode, unsigned(mask), unsigned(count),
        current, timer, state.CurrentMember, state.TimeBeforeNextLoad, unsigned(effects.Count));
    for (std::size_t i = 0; i < effects.Count; ++i)
        std::printf(" %d %d", int(effects.Effects[i].Kind), effects.Effects[i].Model);
    std::printf("\n");
}

void GangPhases() {
    for (std::uint16_t mask = 0; mask < 1024; ++mask)
        for (const auto count : {1, 2, 8, 21}) GangCase(mask, std::uint16_t(count), mask % 21, -1, 0);
    for (std::int32_t current = 0; current < 21; ++current) {
        for (const auto count : {1, 2, 3, 4, 7, 8, 21})
            GangCase(0x83ff, std::uint16_t(count), current, -32768, 0);
        GangCase(1023, 8, current, 0, 0);
        GangCase(1023, 8, current, 2147483647, 0);
        GangCase(1023, 8, current, -1, 1);
        GangCase(1023, 8, current, -1, 2);
    }
    GangCase(0x8000, 8, 20, -1, 0);

    auto input = GangFixture(3, 8);
    NativePedGangStreamState state{-1, 0};
    const auto before = state;
    NativePedGangStreamEffects effects;
    effects.Count = 1;
    input.ZoneKnown = false;
    Check(NativeAdvancePedGangRequests(input, state, effects) == NativePedStreamStatus::UnknownZone &&
        state == before && !effects.Count, "unknown source zone is not an absent zone");
    input.ZoneKnown = true;
    input.HasZone = false;
    input.CheatKnown = false;
    Check(NativeAdvancePedGangRequests(input, state, effects) == NativePedStreamStatus::GangPhaseComplete &&
        state == before, "actual no-zone guard precedes cheat knowledge and gang timer");
    input.HasZone = true;
    Check(NativeAdvancePedGangRequests(input, state, effects) == NativePedStreamStatus::UnknownCheat &&
        state == before, "unknown streaming cheat is not a negative observation");
    input.CheatKnown = true;
    input.RequestedGangsKnown = false;
    Check(NativeAdvancePedGangRequests(input, state, effects) == NativePedStreamStatus::UnknownRequestedGangs &&
        state == before && !effects.Count, "missing requested bitfield cannot mean no gangs");
    state.TimeBeforeNextLoad = 0;
    Check(NativeAdvancePedGangRequests(input, state, effects) == NativePedStreamStatus::GangPhaseComplete &&
        state.TimeBeforeNextLoad == -1 && !effects.Count, "waiting timer needs no gang observations");
    input.RequestedGangsKnown = true;
    input.Groups[1].Known = false;
    Check(NativeAdvancePedGangRequests(input, state, effects) == NativePedStreamStatus::UnknownGroup &&
        state.CurrentMember == 1 && state.TimeBeforeNextLoad == 550 && effects.Count == 2 &&
        effects.Effects[0] == NativePedGangEffect{NativePedGangEffectKind::MakeModelAndTxdDeletable, 10} &&
        effects.Effects[1] == NativePedGangEffect{NativePedGangEffectKind::RequestGameRequired, 12},
        "later unknown gang retains original state and ordered intent prefix");
    state = before;
    input.Groups[0].Models[2].Model = -1;
    Check(NativeAdvancePedGangRequests(input, state, effects) == NativePedStreamStatus::UnknownModel &&
        state.CurrentMember == 1 && effects.Count == 1 && effects.Effects[0].Model == 10,
        "missing requested model retains deletion prefix but never authorizes a substitute");
    state = before;
    input.Groups[0].Count = 0;
    Check(NativeAdvancePedGangRequests(input, state, effects) == NativePedStreamStatus::InvalidInput &&
        state.TimeBeforeNextLoad == 550 && !effects.Count, "active empty group cannot perform source modulo zero");
    state = before;
    input.Groups[0].Count = 22;
    Check(NativeAdvancePedGangRequests(input, state, effects) == NativePedStreamStatus::InvalidInput &&
        !effects.Count, "fixed source gang group capacity cannot be exceeded");
    state = {-1, 21};
    const auto invalid = state;
    Check(NativeAdvancePedGangRequests(input, state, effects) == NativePedStreamStatus::InvalidInput &&
        state == invalid && !effects.Count, "invalid source member cursor cannot wrap into fabricated authority");
}

NativePedGangMaskInput MaskFixture(std::uint16_t base, int profile, int pedCount, int carCount,
    std::int32_t current) {
    NativePedGangMaskInput input;
    input.ZoneKnown = input.DemandKnown = input.MemberKnown = true;
    input.HasZone = profile != 4;
    input.Wanted = profile == 5 ? std::uint16_t(base | 0x2ff) : base;
    input.CurrentMember = current;
    input.PedGroups = GangFixture(0, std::uint16_t(pedCount)).Groups;
    for (std::size_t gang = 0; gang < 10; ++gang) {
        input.LoadedCars[gang] = {true, std::uint16_t(profile == 2 ? 23 : (profile == 1 && gang % 3 == 0))};
        auto& group = input.CarGroups[gang];
        group.Known = true;
        group.Count = std::uint16_t(carCount);
        for (std::size_t slot = 0; slot < group.Count; ++slot)
            group.Models[slot] = {std::int32_t(500 + gang * 23 + slot), true,
                profile == 3 || (profile == 1 && (gang + slot) % 2 == 0)};
    }
    return input;
}

void MaskCase(std::uint16_t base, int profile, int pedCount, int carCount, std::int32_t current) {
    const auto input = MaskFixture(base, profile, pedCount, carCount, current);
    NativePedGangMaskState state{true,
        std::uint16_t(profile == 1 ? (~base & 1023) : profile == 2 ? base : profile == 3 ? 0x83ff : 0),
        std::uint16_t(profile == 1 ? base >> 1 : profile == 3 ? 0x8000 : 0)};
    const auto before = state;
    const auto seed = std::uint32_t(base) * 0x9e3779b9u + std::uint32_t(profile * 21 + current);
    NativeSourceRng rng;
    Check(rng.SeedOnce(seed) == NativeSourceRngStatus::Ready, "mask fixture uses one borrowed CRT stream");
    NativePedGangMaskEffects effects;
    Check(NativeAdvancePedGangMask(input, rng.Reference(), state, effects) ==
        NativePedStreamStatus::GangMaskPhaseComplete, "qualified gang mask phase completes");
    if (input.HasZone)
        Check(state.RequestedCars == input.Wanted && (state.RequestedPeds & 1023) == input.Wanted,
            "request bits track source intents, not asset readiness");
    else Check(state == before && !effects.Count && !rng.Inspect().Value->DrawCount, "absent zone leaves all masks and RNG");
    const auto provenance = *rng.Inspect().Value;
    std::printf("MASK %u %d %d %d %d %u %u %u %u %u %llu %u", unsigned(base), profile, pedCount,
        carCount, current, seed, unsigned(before.RequestedPeds), unsigned(before.RequestedCars),
        unsigned(state.RequestedPeds), unsigned(state.RequestedCars),
        static_cast<unsigned long long>(provenance.DrawCount), provenance.State);
    std::printf(" %u", unsigned(effects.Count));
    for (std::size_t i = 0; i < effects.Count; ++i)
        std::printf(" %d %d", int(effects.Effects[i].Kind), effects.Effects[i].Model);
    std::printf("\n");
}

void GangMasks() {
    constexpr int counts[]{1, 2, 8, 21};
    constexpr int carCounts[]{1, 4, 23};
    for (std::uint16_t mask = 0; mask < 1024; ++mask)
        for (int profile = 0; profile < 6; ++profile)
            MaskCase(mask, profile, counts[mask % 4], carCounts[mask % 3], mask % 21);
    for (std::int32_t current = 0; current < 21; ++current)
        for (const auto count : counts)
            for (int profile = 0; profile < 4; ++profile) MaskCase(0x155, profile, count, 23, current);
    MaskCase(0, 3, 21, 23, 20); // All 210 retirement intents, original car retirement is empty.

    auto input = MaskFixture(1, 0, 2, 1, 0);
    NativePedGangMaskState state{true, 0, 0};
    NativePedGangMaskEffects effects;
    NativeSourceRng rng;
    Check(rng.SeedOnce(0) == NativeSourceRngStatus::Ready, "mask guard shared seed");
    input.ZoneKnown = false;
    Check(NativeAdvancePedGangMask(input, rng.Reference(), state, effects) == NativePedStreamStatus::UnknownZone &&
        !effects.Count && !state.RequestedPeds, "unknown zone not verified absent zone");
    input.ZoneKnown = true;
    input.DemandKnown = false;
    Check(NativeAdvancePedGangMask(input, rng.Reference(), state, effects) == NativePedStreamStatus::UnknownGangDemand &&
        !effects.Count && !rng.Inspect().Value->DrawCount, "unknown war policy not a default no-attack mask");
    input.HasZone = false;
    state.Known = false;
    Check(NativeAdvancePedGangMask(input, {}, state, effects) == NativePedStreamStatus::GangMaskPhaseComplete,
        "known no-zone precedes demand and requested-mask authority");
    input.HasZone = input.DemandKnown = true;
    Check(NativeAdvancePedGangMask(input, {}, state, effects) == NativePedStreamStatus::UnknownRequestedGangs,
        "unknown request masks never default to empty");
    state.Known = true;
    input.MemberKnown = false;
    Check(NativeAdvancePedGangMask(input, rng.Reference(), state, effects) == NativePedStreamStatus::UnknownGangMember &&
        !effects.Count && !state.RequestedPeds, "unknown source member never becomes zero");
    input.MemberKnown = true;
    input.CurrentMember = 21;
    Check(NativeAdvancePedGangMask(input, rng.Reference(), state, effects) == NativePedStreamStatus::InvalidInput &&
        !effects.Count && !state.RequestedPeds, "invalid member cannot advance a requested mask");
    input.CurrentMember = 0;
    input.LoadedCars[0].Known = false;
    Check(NativeAdvancePedGangMask(input, rng.Reference(), state, effects) == NativePedStreamStatus::UnknownGangCars &&
        state.RequestedPeds == 1 && !state.RequestedCars && effects.Count == 2,
        "ped requests and bit publication precede unknown gang-car count");
    state = {true, 0, 0};
    input.LoadedCars[0].Known = true;
    input.CarGroups[0].Models[0].StreamingKnown = false;
    Check(NativeAdvancePedGangMask(input, rng.Reference(), state, effects) == NativePedStreamStatus::UnknownStreaming &&
        state.RequestedPeds == 1 && !state.RequestedCars && effects.Count == 2 && rng.Inspect().Value->DrawCount == 1,
        "unknown selected car preserves consumed draw and ped prefix, not fake unready or loaded");
    state = {true, 0, 0};
    input.LoadedCars[0].Count = 1;
    Check(NativeAdvancePedGangMask(input, {}, state, effects) == NativePedStreamStatus::GangMaskPhaseComplete &&
        effects.Count == 2 && state.RequestedCars == 1,
        "original nonempty car-count guard skips unknown model/RNG without changing readiness");
    state = {true, 1, 1};
    input.LoadedCars[0].Known = false;
    input.PedGroups[0].Known = false;
    Check(NativeAdvancePedGangMask(input, {}, state, effects) == NativePedStreamStatus::GangMaskPhaseComplete && !effects.Count,
        "identical request masks early-return before groups and loaded-car observations");
    state = {true, 0, 0};
    input = MaskFixture(1, 0, 2, 1, 0);
    input.CarGroups[0].Count = 0;
    Check(NativeAdvancePedGangMask(input, rng.Reference(), state, effects) == NativePedStreamStatus::InvalidInput &&
        state.RequestedPeds == 1 && !state.RequestedCars && rng.Inspect().Value->DrawCount == 2,
        "empty car group rejects original modulo-zero after the consumed rand prefix");
    input = MaskFixture(3, 0, 1, 1, 0);
    state = {true, 0, 0};
    input.PedGroups[1].Known = false;
    Check(NativeAdvancePedGangMask(input, rng.Reference(), state, effects) == NativePedStreamStatus::UnknownGroup &&
        state.RequestedPeds == 1 && !state.RequestedCars && effects.Count == 3 &&
        effects.Effects[0].Model == effects.Effects[1].Model,
        "same-model double request is not deduplicated; later unknown preserves interleaved car intent");
    input.Wanted = 1024;
    Check(NativeAdvancePedGangMask(input, rng.Reference(), state, effects) == NativePedStreamStatus::InvalidInput &&
        !effects.Count, "unqualified demand bits cannot refer to non-gang groups");
    input = MaskFixture(1, 0, 2, 1, 0);
    state = {true, 0, 0};
    Check(NativeAdvancePedGangMask(input, {}, state, effects) == NativePedStreamStatus::UnknownRng &&
        state.RequestedPeds == 1 && effects.Count == 2 && !state.RequestedCars,
        "missing RNG preserves authentic preceding requests, not a seeded substitute");
    input.PedGroups[0].Count = 22;
    state = {true, 0, 0};
    Check(NativeAdvancePedGangMask(input, {}, state, effects) == NativePedStreamStatus::InvalidInput && !effects.Count,
        "oversized ped group never overruns its source allocation");
    input = MaskFixture(1, 0, 2, 1, 0);
    input.CarGroups[0].Count = 24;
    Check(NativeAdvancePedGangMask(input, rng.Reference(), state, effects) == NativePedStreamStatus::InvalidInput &&
        effects.Count == 2 && state.RequestedPeds == 1, "oversized car group preserves ped prefix without a draw");
    input.LoadedCars[0].Count = 24;
    Check(NativeAdvancePedGangMask(input, rng.Reference(), state, effects) == NativePedStreamStatus::InvalidInput &&
        !effects.Count, "loaded gang group count cannot exceed 23 members");
}

void DemandCase(std::uint16_t mask, bool cheat, std::int32_t attackState, std::int32_t gang,
    std::array<float, 2> player, std::array<float, 2> attack, bool hasZone = true) {
    NativePedGangDemandInput input;
    input.ZoneKnown = true;
    input.HasZone = hasZone;
    input.CheatKnown = true;
    input.StreetsCheat = cheat;
    for (std::size_t i = 0; i < input.GangStrength.size(); ++i)
        input.GangStrength[i] = mask & (1u << i) ? std::uint8_t(1 + (mask + i) % 255) : 0;
    input.War = {true, attackState, true, true, attack, player, true, gang};
    NativePedGangDemand out{true, 0x3ff};
    Check(NativeObservePedGangDemand(input, out) == NativePedStreamStatus::QualifiedGangDemand,
        "source-qualified demand, not assumed no attack");
    Check(out.HasZone == hasZone && !(out.Wanted & ~0x3ffu), "bounded qualified source mask");
    std::printf("DEMAND %u %d %d %d %d %u %u %u %u %u\n", unsigned(mask), int(cheat),
        attackState, gang, int(hasZone), std::bit_cast<std::uint32_t>(player[0]),
        std::bit_cast<std::uint32_t>(player[1]), std::bit_cast<std::uint32_t>(attack[0]),
        std::bit_cast<std::uint32_t>(attack[1]), unsigned(out.Wanted));
}

void GangDemands() {
    for (std::uint16_t mask = 0; mask < 1024; ++mask) {
        DemandCase(mask, false, 0, mask % 10, {0, 0}, {0, 0});
        DemandCase(mask, false, 1, mask % 10, {std::nextafter(150.0f, 0.0f), 0}, {0, 0});
        DemandCase(mask, false, 2, mask % 10, {150, 0}, {0, 0});
        DemandCase(mask, true, 2, mask % 10, {90, std::nextafter(120.0f, 0.0f)}, {0, 0});
        DemandCase(mask, true, 1, mask % 10, {0, 0}, {0, 0});
        DemandCase(mask, true, 2, mask % 10, {0, 0}, {0, 0}, false);
    }
    for (std::int32_t gang = 0; gang < 10; ++gang)
        for (const float offset : {0.0f, 10000.0f, -10000.0f, 10000000.0f})
            for (const float x : {std::nextafter(150.0f, 0.0f), 150.0f, std::nextafter(150.0f, 200.0f)})
                for (const float y : {0.0f, 0.01f, 0.1f, 1.0f})
                    DemandCase(0x155, false, 1, gang, {offset + x, offset + y}, {offset, offset});
    DemandCase(0, false, 2, 9, {std::numeric_limits<float>::max(), 0},
        {-std::numeric_limits<float>::max(), 0});

    NativePedGangDemandInput input;
    NativePedGangDemand out{true, 777}, before = out;
    Check(NativeObservePedGangDemand(input, out) == NativePedStreamStatus::UnknownZone && out == before,
        "unavailable zone retains output");
    input.ZoneKnown = true;
    Check(NativeObservePedGangDemand(input, out) == NativePedStreamStatus::QualifiedGangDemand &&
        out == NativePedGangDemand{}, "verified absent zone needs no cheat or war observation");
    input.HasZone = true;
    out = before;
    Check(NativeObservePedGangDemand(input, out) == NativePedStreamStatus::UnknownCheat && out == before,
        "unknown streets cheat not false");
    input.CheatKnown = true;
    Check(NativeObservePedGangDemand(input, out) == NativePedStreamStatus::UnknownGangWar && out == before,
        "unowned gang war not a no-attack default");
    input.War.StateKnown = true;
    Check(NativeObservePedGangDemand(input, out) == NativePedStreamStatus::QualifiedGangDemand && !out.Wanted,
        "observed no attack needs no player, point or gang");
    input.War.AttackState = 1;
    out = before;
    Check(NativeObservePedGangDemand(input, out) == NativePedStreamStatus::UnknownPlayerPosition && out == before,
        "active attack requires real player position");
    input.War.PlayerPositionKnown = true;
    Check(NativeObservePedGangDemand(input, out) == NativePedStreamStatus::UnknownAttackPosition && out == before,
        "active attack requires real point of attack");
    input.War.AttackPositionKnown = true;
    Check(NativeObservePedGangDemand(input, out) == NativePedStreamStatus::UnknownGangMember && out == before,
        "near attack cannot assume gang zero");
    input.War.PlayerPosition[0] = 150;
    Check(NativeObservePedGangDemand(input, out) == NativePedStreamStatus::QualifiedGangDemand && !out.Wanted,
        "strict boundary excludes attack before gang observation");
    input.War.PlayerPosition[0] = 0;
    input.War.GangKnown = true;
    input.War.Gang = 10;
    out = before;
    Check(NativeObservePedGangDemand(input, out) == NativePedStreamStatus::InvalidInput && out == before,
        "invalid near gang cannot create out-of-domain bits");
    input.War.Gang = 9;
    input.War.AttackState = 3;
    Check(NativeObservePedGangDemand(input, out) == NativePedStreamStatus::InvalidInput && out == before,
        "unknown enum is not a guessed attack state");
    input.War.AttackState = 2;
    input.War.PlayerPosition[0] = std::numeric_limits<float>::quiet_NaN();
    Check(NativeObservePedGangDemand(input, out) == NativePedStreamStatus::InvalidInput && out == before,
        "invalid position retains mask");
}
}

int main() {
    Guards();
    Cases();
    SlotPlans();
    ZonePhases();
    GangPhases();
    GangMasks();
    GangDemands();
    std::printf("native-ped-streaming-ok checks=%zu cases=3072 slot-plans=256 zone-phases=2048 gang-phases=4328 gang-masks=6481 gang-demands=6625 attempts=10 slots=requested-only gang-state=requested-fixtures census=incomplete\n", s_Checks);
}
