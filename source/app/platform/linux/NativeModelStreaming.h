// Source RequestModel/SetModelIsDeletable plans, not an asset or list owner.
#pragma once

#include <array>
#include <cstdint>

enum class NativeModelStreamStatus {
    Planned, UnknownState, UnknownList, UnknownModelType, UnknownDependencies, InvalidInput
};

struct NativeModelStreamObservation {
    bool Known = false;
    std::int32_t Model = -1;
    std::uint8_t LoadState = 0, Flags = 0;
    bool ListKnown = false, InList = false;
    bool ModelTypeKnown = false;
    // Actual CBaseModelInfo type: vehicle=6, ped=7. Never inferred from names.
    std::uint8_t ModelType = 0;
    bool DependenciesKnown = false;
    // DFF: TXD slot and optional animation slot. TXD: optional parent slot.
    std::int32_t TxdSlot = -1, AnimationSlot = -1, ParentTxdSlot = -1;
};

enum class NativeModelStreamEffectKind {
    WriteFlags, IncrementPriorityRequests, RemoveFromList, AddToLoadedList,
    RequestTxd, RequestAnimation, AddToRequestedList, IncrementModelRequests,
    PublishRequestedState, RemoveModel
};
struct NativeModelStreamEffect {
    NativeModelStreamEffectKind Kind{};
    // Resource ID for dependency/removal; byte value for WriteFlags.
    std::int32_t Argument = 0;
    std::uint32_t Flags = 0;
    bool operator==(const NativeModelStreamEffect&) const = default;
};
struct NativeModelStreamPlan {
    std::array<NativeModelStreamEffect, 10> Effects{};
    std::uint8_t Count = 0;
    bool operator==(const NativeModelStreamPlan&) const = default;
};

// Atomic planning only. Fulfill every effect in order; recursive dependencies
// precede publishing REQUESTED. Request/KEEP and list membership NEVER prove
// parser completion, render readiness, references, or population completeness.
// Unknown facts preserve out; no effects or source counters are changed here.
NativeModelStreamStatus NativePlanModelRequest(const NativeModelStreamObservation&,
    std::uint32_t flags, NativeModelStreamPlan& out);
// Original non-mission SetModelIsDeletable. RemoveModel is an external effect,
// not a fabricated unloaded state. The mission-required guard stays intact.
NativeModelStreamStatus NativePlanModelDeletable(const NativeModelStreamObservation&,
    NativeModelStreamPlan& out);
