#include "NativePedHierarchy.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
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

NativePedHierarchyStatus NativePlanPedBindPositions(const NativePedBindPositionInput& input,
    std::vector<std::array<float, 3>>& out) {
    using S = NativePedHierarchyStatus;
    if (!input.HierarchyKnown) return S::UnknownHierarchy;
    if (!input.SkinKnown) return S::UnknownSkin;
    if (input.Bones.empty() || input.Bones.size() > 64) return S::InvalidInput;
    std::vector<std::array<float, 3>> candidate(input.Bones.size());
    std::array<std::size_t, 32> stack;
    std::size_t parent = 0, depth = 0;
    for (std::size_t i = 1; i < input.Bones.size(); ++i) {
        const auto& bone = input.Bones[i];
        const auto& ancestor = input.Bones[parent];
        if (!bone.InverseKnown || !ancestor.InverseKnown) return S::UnknownInverseBind;
        if (!Finite(bone.InverseBind) || !Finite(ancestor.InverseBind)) return S::InvalidInput;
        const auto inverse = NativeInvertPedMatrix(bone.InverseBind);
        if (!Finite(inverse)) return S::InvalidInput;
        candidate[i] = NativeTransformPedPoint(inverse.Value.Pos, ancestor.InverseBind);
        for (float value : candidate[i]) if (!std::isfinite(value)) return S::InvalidInput;
        if (bone.Flags & 2U) {
            // Original stack slot zero is uninitialized: PUSH preincrements.
            if (depth + 1 >= stack.size()) return S::InvalidInput;
            stack[++depth] = parent;
        }
        if (bone.Flags & 1U) {
            if (!depth) {
                if (i + 1 != input.Bones.size()) return S::InvalidInput;
            } else {
                parent = stack[depth--];
            }
        } else {
            parent = i;
        }
    }
    out = std::move(candidate);
    return S::Planned;
}

NativePedHierarchyStatus NativePlanPedBlendInitialization(const NativePedBlendInitInput& input,
    std::vector<NativePedBlendFrameBinding>& out) {
    using S = NativePedHierarchyStatus;
    std::vector<std::array<float, 3>> positions;
    const auto status = NativePlanPedBindPositions(input.Bind, positions);
    if (status != S::Planned) return status;
    if (!input.InterpolatorKnown) return S::UnknownInterpolator;
    if (input.Tags.size() != positions.size() || input.InterpolationStride < 28 ||
        input.InterpolationStride > 256) return S::InvalidInput;
    std::vector<NativePedBlendFrameBinding> candidate(positions.size());
    for (std::size_t i = 0; i < positions.size(); ++i) {
        candidate[i] = {static_cast<std::uint8_t>(i == 0 ? 8 : 0), positions[i], input.Tags[i],
            static_cast<std::uint32_t>(i), static_cast<std::uint32_t>(i) * input.InterpolationStride};
    }
    out = std::move(candidate);
    return S::Planned;
}

void NativeResetPedBlendInterpolationFrame(NativePedInterpolationFrame& out) {
    out = {true, {0, 0, 0, 1}, {0, 0, 0}};
}

namespace {
NativePedHierarchyStatus ApplyFrame(const NativePedInterpolationFrame& input,
    bool blend, NativePedHitMatrix& out) {
    using S = NativePedHierarchyStatus;
    if (!input.Known) return S::UnknownAppliedPose;
    for (float value : input.Quaternion) if (!std::isfinite(value)) return S::InvalidInput;
    for (float value : input.Translation) if (!std::isfinite(value)) return S::InvalidInput;
    static_assert(std::numeric_limits<long double>::digits == 64);
    const auto& [xf, yf, zf, wf] = input.Quaternion;
    const long double x = xf, y = yf, z = zf, w = wf;
    // RW's callback/inline paths retain w*y and w*z; GTA's registered callback
    // spills all products. The two applications must not share a guessed pose.
    const float xx = x * x, yy = y * y, zz = z * z;
    const float zy = z * y, zx = z * x, xy = y * x, wx = w * x;
    const long double wy = blend ? static_cast<float>(w * y) : w * y;
    const long double wz = blend ? static_cast<float>(w * z) : w * z;
    NativePedHitMatrix candidate;
    candidate.Flags = 3;
    candidate.Value.Right = {static_cast<float>(1.0L - (static_cast<long double>(zz) + yy) * 2.0L),
        static_cast<float>((static_cast<long double>(xy) + wz) * 2.0L),
        static_cast<float>((static_cast<long double>(zx) - wy) * 2.0L)};
    candidate.Value.Up = {static_cast<float>((static_cast<long double>(xy) - wz) * 2.0L),
        static_cast<float>(1.0L - (static_cast<long double>(zz) + xx) * 2.0L),
        static_cast<float>((static_cast<long double>(wx) + zy) * 2.0L)};
    candidate.Value.At = {static_cast<float>((wy + zx) * 2.0L),
        static_cast<float>((static_cast<long double>(zy) - wx) * 2.0L),
        static_cast<float>(1.0L - (static_cast<long double>(yy) + xx) * 2.0L)};
    candidate.Value.Pos = input.Translation;
    if (!Finite(candidate)) return S::InvalidInput;
    out = candidate;
    return S::Planned;
}
}

NativePedHierarchyStatus NativeApplyPedInterpolationFrame(const NativePedInterpolationFrame& input,
    NativePedHitMatrix& out) {
    return ApplyFrame(input, false, out);
}

NativePedHierarchyStatus NativeApplyPedBlendFrame(const NativePedInterpolationFrame& input,
    NativePedHitMatrix& out) {
    return ApplyFrame(input, true, out);
}

NativePedHierarchyStatus NativeObservePedBlendPartialScale(const NativePedBlendProductionInput& input,
    float& out) {
    using S = NativePedHierarchyStatus;
    static_assert(std::numeric_limits<long double>::digits == 64);
    if (!input.FrameKnown) return S::UnknownFrame;
    if (input.Flags & 8U) {
        if (!input.PedPositionKnown) return S::UnknownPedPosition;
        if (input.HasPedPosition) return !input.Compressed && (input.Flags & 16U)
            ? S::Velocity3DRequired : S::Velocity2DRequired;
    }
    if (!input.ContextKnown) return S::UnknownBlendContext;
    if (input.Nodes.empty() || input.Nodes.size() > 11) return S::InvalidInput;
    float partial = 0;
    for (const auto& node : input.Nodes) {
        if (!node.Known) return S::UnknownBlendNode;
        if (!node.Valid || !input.IncludePartial) continue;
        if (!node.PartialKnown) return S::UnknownBlendNode;
        if (node.Partial) {
            if (!node.BlendKnown) return S::UnknownBlendNode;
            if (!std::isfinite(node.BlendAmount)) return S::InvalidInput;
            partial = static_cast<float>(static_cast<long double>(node.BlendAmount) + partial);
        }
    }
    const float candidate = static_cast<float>(1.0L - partial);
    if (!std::isfinite(candidate)) return S::InvalidInput;
    out = candidate;
    return S::Planned;
}

NativePedHierarchyStatus NativePlanPedBlendProduction(const NativePedBlendProductionInput& input,
    NativePedBlendProductionPlan& out) {
    using S = NativePedHierarchyStatus;
    NativePedBlendProductionPlan candidate;
    const auto status = NativeObservePedBlendPartialScale(input, candidate.PartialScale);
    if (status != S::Planned) return status;
    std::array<float, 4> q{};
    std::array<float, 3> t{};
    float blend = 0;
    for (const auto& node : input.Nodes) {
        ++candidate.AdvanceNodeArrays;
        if (!node.Valid) continue;
        if (!node.UpdateKnown) return S::UnknownNodeUpdate;
        if (!std::isfinite(node.UpdatedPartialScale) ||
            std::bit_cast<std::uint32_t>(node.UpdatedPartialScale) !=
            std::bit_cast<std::uint32_t>(candidate.PartialScale)) return S::InvalidInput;
        if (!node.TranslationKnown) return S::UnknownBlendNode;
        for (float value : node.Quaternion) if (!std::isfinite(value)) return S::InvalidInput;
        if (node.HasTranslation) {
            if (!node.BlendKnown) return S::UnknownBlendNode;
            if (!std::isfinite(node.BlendAmount)) return S::InvalidInput;
            for (std::size_t j = 0; j < t.size(); ++j) {
                if (!std::isfinite(node.Translation[j])) return S::InvalidInput;
                t[j] = static_cast<float>(static_cast<long double>(node.Translation[j]) + t[j]);
                if (!std::isfinite(t[j])) return S::InvalidInput;
            }
            blend = static_cast<float>(static_cast<long double>(node.BlendAmount) + blend);
            if (!std::isfinite(blend)) return S::InvalidInput;
        }
        const auto dot = static_cast<float>(((static_cast<long double>(node.Quaternion[0]) * q[0] +
            static_cast<long double>(node.Quaternion[1]) * q[1]) +
            static_cast<long double>(node.Quaternion[2]) * q[2]) +
            static_cast<long double>(node.Quaternion[3]) * q[3]);
        if (!std::isfinite(dot)) return S::InvalidInput;
        for (std::size_t j = 0; j < q.size(); ++j) {
            q[j] = static_cast<float>(dot >= 0 ? static_cast<long double>(node.Quaternion[j]) + q[j]
                : static_cast<long double>(q[j]) - node.Quaternion[j]);
            if (!std::isfinite(q[j])) return S::InvalidInput;
        }
    }
    if (!(input.Flags & 2U)) {
        const auto squared = static_cast<float>(((static_cast<long double>(q[0]) * q[0] +
            static_cast<long double>(q[1]) * q[1]) + static_cast<long double>(q[2]) * q[2]) +
            static_cast<long double>(q[3]) * q[3]);
        if (!std::isfinite(squared)) return S::InvalidInput;
        if (squared == 0) {
            q[3] = 1;
        } else {
            const float magnitude = std::sqrt(static_cast<long double>(squared));
            const float reciprocal = 1.0L / magnitude;
            if (!std::isfinite(reciprocal)) return S::InvalidInput;
            for (float& value : q) value = static_cast<long double>(value) * reciprocal;
        }
        candidate.Quaternion = q;
    }
    if (!(input.Flags & 4U)) {
        for (std::size_t j = 0; j < t.size(); ++j) {
            if (!std::isfinite(input.RestPosition[j])) return S::InvalidInput;
            const float weighted = static_cast<long double>(t[j]) * blend;
            t[j] = static_cast<long double>(input.RestPosition[j]) * (1.0L - blend) + weighted;
            if (!std::isfinite(t[j])) return S::InvalidInput;
        }
        candidate.Translation = t;
    }
    out = std::move(candidate);
    return S::Planned;
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
        auto applied = node.Applied;
        if (node.Interpolation) {
            if (node.Application != NativePedFrameApplication::HAnimDefault &&
                node.Application != NativePedFrameApplication::AnimBlend) return S::InvalidInput;
            const auto status = node.Application == NativePedFrameApplication::AnimBlend
                ? NativeApplyPedBlendFrame(*node.Interpolation, applied)
                : NativeApplyPedInterpolationFrame(*node.Interpolation, applied);
            if (status != S::Planned) return status;
        } else if (!node.AppliedKnown) return S::UnknownAppliedPose;
        if (!node.FrameKnown) return S::UnknownFrame;
        if (!Finite(applied)) return S::InvalidInput;
        const auto current = NativeMultiplyPedMatrices(applied, parent);
        if (!Finite(current)) return S::InvalidInput;
        NativePedHierarchyNodeUpdate update;
        update.Tag = node.Tag;
        update.FramePrivateFlags = node.FramePrivateFlags;
        if (!noMatrices) update.Matrix = current;
        if (node.HasFrame) {
            if (modelling) {
                update.Modelling = applied;
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
