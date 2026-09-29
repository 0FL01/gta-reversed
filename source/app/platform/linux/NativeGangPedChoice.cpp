#include "NativeGangPedChoice.h"

NativeGangPedChoiceStatus NativeChooseGangPedModel(const NativeGangPedChoiceInput& input,
    NativeSourceRngRef rng, NativeGangPedSequence& sequence, std::int32_t& out) {
    if (!input.GroupKnown) return NativeGangPedChoiceStatus::UnknownGroup;
    if (input.ModelOverride != -1) {
        out = input.CurrentGroup[0].Model;
        return NativeGangPedChoiceStatus::Chosen;
    }
    if (input.ZoneZeroCount < 1 || input.ZoneZeroCount > 21)
        return NativeGangPedChoiceStatus::InvalidInput;
    if (rng.Readiness() != NativeSourceRngStatus::Ready)
        return NativeGangPedChoiceStatus::UnknownRng;

    const auto offset = rng.NextRand15();
    if (!offset.Value) return NativeGangPedChoiceStatus::UnknownRng;
    const auto direction = rng.NextRand15();
    if (!direction.Value) return NativeGangPedChoiceStatus::UnknownRng;
    sequence = {input.ZoneZeroCount, *offset.Value % input.ZoneZeroCount,
        ((*direction.Value >> 4u) & 1u) != 0};
    for (int i = 0; i < input.ZoneZeroCount; ++i) {
        const auto slot = sequence.Ascending ?
            (i + sequence.Offset) % sequence.Elements :
            (sequence.Elements - i + sequence.Offset) % sequence.Elements;
        const auto& model = input.CurrentGroup[std::size_t(slot)];
        if (!model.StreamingKnown) return NativeGangPedChoiceStatus::UnknownStreaming;
        if (!model.Loaded) continue;
        out = model.Model;
        return NativeGangPedChoiceStatus::Chosen;
    }
    return NativeGangPedChoiceStatus::NoLoadedModel;
}
