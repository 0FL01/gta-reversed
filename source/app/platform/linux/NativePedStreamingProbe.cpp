#include "NativePedStreaming.h"

#include <cstdio>
#include <cstdlib>

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
}

int main() {
    Guards();
    Cases();
    SlotPlans();
    std::printf("native-ped-streaming-ok checks=%zu cases=3072 slot-plans=256 attempts=10 slots=requested-only census=incomplete\n", s_Checks);
}
