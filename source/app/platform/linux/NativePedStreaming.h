// Original PickPedMIToStreamInForCurrentZone request selection. Not a loader.
#pragma once

#include "NativePedModelMetadata.h"
#include "NativeSourceRng.h"

struct NativePopulationCycleObservation;

struct NativePedStreamModel {
    std::int32_t Model = -1;
    bool RaceKnown = false;
    std::uint8_t Race = 0;
};

struct NativePedStreamGroup {
    bool Known = false;
    std::uint16_t Count = 0;
    std::array<NativePedStreamModel, 21> Models{};
};

struct NativePedStreamInput {
    bool ZoneKnown = false;
    std::uint8_t RaceMask = 0;
    // Source rescaled row; never normalize at selection time.
    std::array<std::uint8_t, 18> Percentages{};
    std::array<NativePedStreamGroup, 18> Groups{};
    // ms_pedsLoaded contains REQUESTED identities, not only loaded assets.
    bool SlotsKnown = false;
    std::array<std::int32_t, 8> Slots{-1, -1, -1, -1, -1, -1, -1, -1};
};

struct NativePedStreamState {
    std::array<std::int32_t, 18> NextPedToLoad{};
    bool operator==(const NativePedStreamState&) const = default;
};

struct NativePedStreamChoice {
    std::int32_t Model = -1, Group = -1;
    std::uint8_t Draws = 0;
};

enum class NativePedStreamStatus {
    Selected, NoSelection, UnknownZone, UnknownSlots, UnknownGroup,
    UnknownModel, UnknownRng, InvalidInput, InvalidDistribution, QualifiedGroups, PlannedSlots,
    ZonePhaseComplete, UnknownCheat, UnknownReferences, QualifiedCycleInput
};

struct NativePedRequestedSlots {
    bool Known = false;
    std::array<std::int32_t, 8> Models{-1, -1, -1, -1, -1, -1, -1, -1};
    std::uint32_t Count = 0;
    bool operator==(const NativePedRequestedSlots&) const = default;
};

enum class NativePedSlotEffectKind { MakeModelAndTxdDeletable, RequestKeepInMemory };
struct NativePedSlotEffect {
    NativePedSlotEffectKind Kind{};
    std::uint8_t Slot = 0;
    std::int32_t Model = -1;
    bool operator==(const NativePedSlotEffect&) const = default;
};

struct NativePedSlotPlan {
    NativePedRequestedSlots Next;
    std::array<NativePedSlotEffect, 16> Effects{};
    std::uint8_t EffectCount = 0;
};

// Source StreamPedsIntoRandomSlots effects and final requested identities.
// -2 retires an occupied slot; other negative inputs preserve it. Same-model
// replacement and duplicate requests are intentionally not deduplicated.
// Pure plan: caller must execute effects in order before publishing Next.
// Deletable does not mean unloaded, Request does not mean parser-completed.
NativePedStreamStatus NativePlanPedSlotRequests(const NativePedRequestedSlots&,
    const std::array<std::int32_t, 8>& requested, NativePedSlotPlan& out);

// Source compiled translation table, independently verified against the owned
// retail data table. Numeric ordinals are data/enum identities, not PE code.
inline constexpr std::array<std::array<std::uint8_t, 3>, 33> NativePedGroupTranslation{{
    {0,1,2}, {3,4,5}, {6,7,8}, {9,9,9}, {10,10,10}, {11,12,13},
    {14,15,16}, {17,18,19}, {20,21,22}, {23,24,25}, {26,27,28}, {29,29,29},
    {30,31,32}, {33,34,35}, {36,37,38}, {39,39,39}, {40,40,40}, {41,41,41},
    {42,42,42}, {43,43,43}, {44,44,44}, {45,45,45}, {46,46,46}, {47,47,47},
    {48,48,48}, {49,49,49}, {50,50,50}, {51,51,51}, {52,52,52}, {53,53,53},
    {54,54,54}, {55,55,55}, {56,56,56}
}};

// Memory-only actual metadata adapter; world zone must come from its owner.
NativePedStreamStatus NativeQualifyPedStreamingGroups(const NativePedModelMetadata&,
    std::uint32_t worldZone, std::array<NativePedStreamGroup, 18>& out);

// Bind the shared cycle reader's actual row and live race setting to actual
// translated ped groups. Requested slots, references and world-region authority
// remain separate observations; success never sets SlotsKnown or implies loaded.
NativePedStreamStatus NativeQualifyPedCycleSelection(const NativePedModelMetadata&,
    std::uint32_t worldZone, const NativePopulationCycleObservation&, NativePedStreamInput& out);

// Exact ten attempts, strict percentage boundary, pre-increment cursor and all
// eight requested-slot duplicate exclusions. Cursors/RNG consumed before an
// unavailable observation stay consumed; non-Selected leaves choice unchanged.
// No model/actor/pool/world effects or census completeness are implied.
NativePedStreamStatus NativePickPedModelToStream(const NativePedStreamInput&,
    NativeSourceRngRef, NativePedStreamState&, NativePedStreamChoice& out);

struct NativePedSlotReferences {
    bool Known = false;
    std::int32_t Model = -1;
    std::uint16_t Count = 0;
};

struct NativePedZoneStreamInput {
    bool ZoneKnown = false, HasZone = false;
    bool CheatKnown = false, ZoneStreamingCheat = false;
    std::uint8_t PopulationType = 0;
    NativePedStreamInput Selection;
    std::array<NativePedSlotReferences, 8> References{};
};

struct NativePedZoneStreamState {
    NativePedRequestedSlots Slots;
    NativePedStreamState Selection;
    std::int32_t CurrentZoneType = -1;
    std::int32_t TimeBeforeNextLoad = 0;
    bool operator==(const NativePedZoneStreamState&) const = default;
};

enum class NativePedZoneEffectKind {
    MakeModelAndTxdDeletable, RequestKeepAndGameRequired, ClearGameRequired
};
struct NativePedZoneEffect {
    NativePedZoneEffectKind Kind{};
    std::int32_t Model = -1;
    bool operator==(const NativePedZoneEffect&) const = default;
};
struct NativePedZoneStreamEffects {
    std::array<NativePedZoneEffect, 24> Effects{};
    std::uint8_t Count = 0;
};

// ONLY the ordinary civilian phase of StreamZoneModels, before the gang timer.
// State is the source requested-slot ledger, never a parser-completed roster.
// Effects are ordered intents; a consumer must fulfill them before observing
// the advanced ledger. On unavailable selection, retain the consumed state/RNG
// and emitted effect prefix; do NOT retry this phase as a new source call.
// Early guard failures leave state unchanged. No asset, actor or census claims.
NativePedStreamStatus NativeAdvancePedZoneRequests(const NativePedZoneStreamInput&,
    NativeSourceRngRef, NativePedZoneStreamState&, NativePedZoneStreamEffects& out);
