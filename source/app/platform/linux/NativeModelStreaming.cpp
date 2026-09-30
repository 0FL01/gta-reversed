#include "NativeModelStreaming.h"

#include <cassert>

namespace {
constexpr std::uint32_t GameRequired = 2, MissionRequired = 4, Keep = 8, Priority = 16;
constexpr std::int32_t TxdBase = 20000, ColBase = 25000, AnimationBase = 25575;
constexpr std::int32_t ResourceEnd = 26312;
bool Valid(const NativeModelStreamObservation& o) {
    return o.Model >= 0 && o.Model < ResourceEnd && o.LoadState <= 4;
}
void Add(NativeModelStreamPlan& p, NativeModelStreamEffectKind kind,
    std::int32_t argument = 0, std::uint32_t flags = 0) {
    assert(p.Count < p.Effects.size());
    p.Effects[p.Count++] = {kind, argument, flags};
}
void Flags(NativeModelStreamPlan& p, std::uint32_t flags) {
    Add(p, NativeModelStreamEffectKind::WriteFlags, std::uint8_t(flags));
}
}

NativeModelStreamStatus NativePlanModelRequest(const NativeModelStreamObservation& o,
    std::uint32_t flags, NativeModelStreamPlan& out) {
    if (!o.Known) return NativeModelStreamStatus::UnknownState;
    if (!Valid(o)) return NativeModelStreamStatus::InvalidInput;
    NativeModelStreamPlan p;
    if (o.LoadState == 2) {
        if ((flags & Priority) && !(o.Flags & Priority)) {
            Add(p, NativeModelStreamEffectKind::IncrementPriorityRequests);
            Flags(p, o.Flags | Priority);
        }
    } else if (o.LoadState != 0) {
        flags &= ~Priority;
    }
    const auto merged = std::uint8_t(o.Flags | flags);
    Flags(p, merged);
    if (o.LoadState == 1) {
        if (!o.ListKnown) return NativeModelStreamStatus::UnknownList;
        if (o.InList) {
            Add(p, NativeModelStreamEffectKind::RemoveFromList);
            if (o.Model < TxdBase) {
                if (!o.ModelTypeKnown) return NativeModelStreamStatus::UnknownModelType;
                if (o.ModelType == 6 || o.ModelType == 7) {
                    out = p;
                    return NativeModelStreamStatus::Planned;
                }
            }
            if (!(merged & (GameRequired | MissionRequired)))
                Add(p, NativeModelStreamEffectKind::AddToLoadedList);
        }
    } else if (o.LoadState == 0) {
        if (o.Model < ColBase) {
            if (!o.DependenciesKnown) return NativeModelStreamStatus::UnknownDependencies;
            if (o.Model < TxdBase) {
                if (o.TxdSlot < 0 || o.TxdSlot >= 5000 ||
                    o.AnimationSlot < -1 || o.AnimationSlot >= 180)
                    return NativeModelStreamStatus::InvalidInput;
                Add(p, NativeModelStreamEffectKind::RequestTxd, TxdBase + o.TxdSlot, flags);
                if (o.AnimationSlot != -1)
                    Add(p, NativeModelStreamEffectKind::RequestAnimation, AnimationBase + o.AnimationSlot, Keep);
            } else {
                if (o.ParentTxdSlot < -1 || o.ParentTxdSlot >= 5000)
                    return NativeModelStreamStatus::InvalidInput;
                if (o.ParentTxdSlot != -1)
                    Add(p, NativeModelStreamEffectKind::RequestTxd, TxdBase + o.ParentTxdSlot, flags);
            }
        }
        Add(p, NativeModelStreamEffectKind::AddToRequestedList);
        Add(p, NativeModelStreamEffectKind::IncrementModelRequests);
        if (flags & Priority) Add(p, NativeModelStreamEffectKind::IncrementPriorityRequests);
        // The original discards previous flags on a first request.
        Flags(p, 0);
        Flags(p, flags);
        Add(p, NativeModelStreamEffectKind::PublishRequestedState);
    }
    out = p;
    return NativeModelStreamStatus::Planned;
}

NativeModelStreamStatus NativePlanModelDeletable(const NativeModelStreamObservation& o,
    NativeModelStreamPlan& out) {
    if (!o.Known) return NativeModelStreamStatus::UnknownState;
    if (!Valid(o)) return NativeModelStreamStatus::InvalidInput;
    NativeModelStreamPlan p;
    const auto flags = std::uint8_t(o.Flags & ~GameRequired);
    Flags(p, flags);
    if (!(flags & MissionRequired)) {
        if (o.LoadState == 1) {
            if (!o.ListKnown) return NativeModelStreamStatus::UnknownList;
            if (!o.InList) Add(p, NativeModelStreamEffectKind::AddToLoadedList);
        } else if (!(flags & Keep)) {
            Add(p, NativeModelStreamEffectKind::RemoveModel, o.Model);
        }
    }
    out = p;
    return NativeModelStreamStatus::Planned;
}
