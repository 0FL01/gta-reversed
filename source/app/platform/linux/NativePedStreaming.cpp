#include "NativePedStreaming.h"

#include <algorithm>
#include <limits>

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

NativePedStreamStatus NativeQualifyPedStreamingGroups(const NativePedModelMetadata& metadata,
    std::uint32_t worldZone, std::array<NativePedStreamGroup, 18>& out) {
    if (worldZone >= 3) return NativePedStreamStatus::InvalidInput;
    if (metadata.Models().empty()) return NativePedStreamStatus::UnknownModel;
    std::array<NativePedStreamGroup, 18> candidate{};
    for (std::size_t i = 0; i < candidate.size(); ++i) {
        const auto& source = metadata.Groups()[NativePedGroupTranslation[i][worldZone]];
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
