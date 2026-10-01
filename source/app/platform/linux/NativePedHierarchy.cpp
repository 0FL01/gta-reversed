#include "NativePedHierarchy.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace {
bool Finite(const NativePedHitMatrix& matrix) {
    const auto& [right, up, at, pos] = matrix.Value;
    const auto finite = [](const auto& row) {
        return std::all_of(row.begin(), row.end(), [](float value) { return std::isfinite(value); });
    };
    return finite(right) && finite(up) && finite(at) && finite(pos);
}
NativePedHitMatrix Identity() {
    return {{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {0, 0, 0}}, 0x20003U};
}
}

NativePedHierarchyStatus NativePlanPedHierarchyUpdate(const NativePedHierarchyInput& input,
    NativePedHierarchyPlan& out) {
    using S = NativePedHierarchyStatus;
    if (!input.HierarchyKnown) return S::UnknownHierarchy;
    if (input.Nodes.empty() || input.Nodes.size() > 256) return S::InvalidInput;
    const bool noMatrices = (input.Flags & 2U) != 0;
    const bool local = !noMatrices && (input.Flags & 0x4000U) != 0;
    const bool modelling = (input.Flags & 0x1000U) != 0;
    const bool ltms = (input.Flags & 0x2000U) != 0;
    const bool sub = !noMatrices && (input.Flags & 1U) != 0;
    if (sub && !input.SubParentKnown) return S::UnknownParent;
    if (sub && (input.ParentIndex < -1 || input.ParentIndex >= 256)) return S::InvalidInput;
    const bool selectedSub = sub && input.ParentIndex != -1;
    const bool readsRoot = noMatrices || (!selectedSub && !local) || (local && ltms);
    auto root = Identity();
    if (readsRoot) {
        if (!input.ParentKnown) return S::UnknownParent;
        if (input.HasParent) root = input.ParentWorld;
        if (!Finite(root)) return S::InvalidInput;
    }
    auto parent = local ? Identity() : root;
    if (selectedSub) {
        if (!Finite(input.SubParent)) return S::InvalidInput;
        parent = input.SubParent;
    }
    NativePedHierarchyPlan candidate;
    if (ltms) {
        if (!input.RootFrameKnown) return S::UnknownRootFrame;
        if (!input.HasRootFrame) return S::InvalidInput;
        candidate.RootPrivateFlags = input.RootPrivateFlags;
        if ((input.RootPrivateFlags & 3U) == 0) {
            candidate.EnqueueRootDirty = true;
            candidate.RootPrivateFlags |= 2U;
        }
    }
    // Original normal branch stores pointers and the NOMATRICES branch stores
    // full matrices. Both carry the same parent value; malformed stack use is
    // rejected before any source state is published.
    std::array<NativePedHitMatrix, 32> stack;
    std::size_t depth = 0;
    candidate.Nodes.reserve(input.Nodes.size());
    for (const auto& node : input.Nodes) {
        if (!node.AppliedKnown) return S::UnknownAppliedPose;
        if (!node.FrameKnown) return S::UnknownFrame;
        if (!Finite(node.Applied)) return S::InvalidInput;
        const auto current = NativeMultiplyPedMatrices(node.Applied, parent);
        if (!Finite(current)) return S::InvalidInput;
        NativePedHierarchyNodeUpdate update;
        update.Tag = node.Tag;
        update.FramePrivateFlags = node.FramePrivateFlags;
        if (!noMatrices) update.Matrix = current;
        if (node.HasFrame) {
            if (modelling) {
                update.Modelling = node.Applied;
                update.UpdateObjects = !ltms;
            }
            if (ltms) {
                const auto world = local ? NativeMultiplyPedMatrices(current, root) : current;
                if (!Finite(world)) return S::InvalidInput;
                update.Ltm = world;
                update.FramePrivateFlags = (node.FramePrivateFlags & ~4U) | 8U;
            }
        }
        candidate.Nodes.push_back(update);
        switch (node.Flags & 3U) {
        case 0: parent = current; break;
        case 1:
            if (depth == 0) {
                // Shipped trees terminate with a POP whose restored parent is
                // never consumed. Original scratch reads have no observable
                // effect then; do not fabricate a usable parent for a later node.
                if (candidate.Nodes.size() != input.Nodes.size()) return S::InvalidInput;
            } else parent = stack[--depth];
            break;
        case 2:
            if (depth == stack.size()) return S::InvalidInput;
            stack[depth++] = parent;
            parent = current;
            break;
        case 3: break; // Both bits: source switch leaves parent/stack unchanged.
        }
    }
    out = std::move(candidate);
    return S::Planned;
}
