// Source gang-model selection from caller-qualified groups and streaming state.
#pragma once

#include "NativeSourceRng.h"

#include <array>
#include <cstdint>

enum class NativeGangPedChoiceStatus {
    Chosen, NoLoadedModel, InvalidInput, UnknownGroup, UnknownStreaming, UnknownRng
};

struct NativeGangPedModel {
    std::uint16_t Model = 0;
    bool StreamingKnown = false;
    bool Loaded = false;
};

struct NativeGangPedChoiceInput {
    bool GroupKnown = false;
    // Original uses zone0's count but the current world zone's 21-slot row.
    std::int32_t ZoneZeroCount = 0;
    std::int8_t ModelOverride = -1;
    std::array<NativeGangPedModel, 21> CurrentGroup{};
};

struct NativeGangPedSequence {
    std::int32_t Elements = 0;
    std::int32_t Offset = 0;
    bool Ascending = false;
    bool operator==(const NativeGangPedSequence&) const = default;
};

// Sequence is shared source CCarCtrl scratch state, not a private/forked RNG.
// Forced overrides use the row's FIRST model, not the override byte as index,
// and do not consult loaded state or consume RNG. Ordinary selection consumes
// exactly two draws even when all models are known unloaded. Empty zone0 count
// rejects before original modulo-zero would execute; unknown is never unloaded.
// Failed selections retain out but preserve any real consumed RNG/scratch prefix.
NativeGangPedChoiceStatus NativeChooseGangPedModel(const NativeGangPedChoiceInput&,
    NativeSourceRngRef, NativeGangPedSequence&, std::int32_t& out);
