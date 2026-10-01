#include "NativePedHierarchy.h"

#include <bit>
#include <cstdlib>
#include <iostream>
#include <limits>

namespace {
std::size_t Checks = 0;
void Check(bool value) { ++Checks; if (!value) std::abort(); }
void Emit(const NativePedHitMatrix& matrix) {
    std::cout << ' ' << matrix.Flags;
    for (const auto& row : {matrix.Value.Right, matrix.Value.Up, matrix.Value.At, matrix.Value.Pos})
        for (float value : row) std::cout << ' ' << std::bit_cast<std::uint32_t>(value);
}
NativePedHitMatrix Matrix(std::uint32_t& seed, std::uint32_t flags) {
    NativePedHitMatrix out;
    out.Flags = flags;
    for (auto* row : {&out.Value.Right, &out.Value.Up, &out.Value.At, &out.Value.Pos}) {
        for (auto& value : *row) {
            seed = seed * 1664525U + 1013904223U;
            value = static_cast<float>(static_cast<std::int32_t>(seed >> 8) - 0x800000) / 33554432.0F;
        }
    }
    out.Value.Right[0] += 1.0F; out.Value.Up[1] += 1.0F; out.Value.At[2] += 1.0F;
    return out;
}
void ProductionCases() {
    using S = NativePedHierarchyStatus;
    const auto bits = [](float value) { return std::bit_cast<std::uint32_t>(value); };
    for (std::uint32_t index = 0; index < 64; ++index) {
        for (std::uint32_t mode = 0; mode < 32; ++mode) {
            for (std::size_t count : {1U, 3U, 11U}) {
                std::uint32_t seed = index * 0x9E3779B9U + mode + 1;
                std::vector<NativePedBlendContribution> nodes(count);
                float partial = 0;
                for (std::size_t i = 0; i < count; ++i) {
                    auto& node = nodes[i];
                    node.Known = node.PartialKnown = node.BlendKnown = node.TranslationKnown = node.UpdateKnown = true;
                    node.Valid = (index + i) % 5 != 0;
                    node.Partial = (index + i) % 3 == 0;
                    node.HasTranslation = (index + i) % 3 != 1;
                    seed = seed * 1664525U + 1013904223U;
                    node.BlendAmount = index < 4 ? static_cast<float>((index + i) % 9) / 8.0F
                        : static_cast<float>(seed >> 8) / 16777216.0F;
                    if (index & 1U && node.Valid && node.Partial)
                        partial = static_cast<float>(static_cast<long double>(node.BlendAmount) + partial);
                    for (float& value : node.Quaternion) {
                        seed = seed * 1664525U + 1013904223U;
                        const auto exponent = index < 4 ? 0U : 64U + (seed >> 24) % 107U;
                        value = index == 0 ? 0.0F : std::bit_cast<float>((seed & 0x807FFFFFU) | exponent << 23);
                    }
                    for (float& value : node.Translation) {
                        seed = seed * 1664525U + 1013904223U;
                        value = static_cast<float>(static_cast<std::int32_t>(seed >> 8) - 0x800000) / 65536.0F;
                    }
                }
                const float scale = static_cast<float>(1.0L - partial);
                for (auto& node : nodes) node.UpdatedPartialScale = scale;
                NativePedBlendProductionInput input;
                input.FrameKnown = input.PedPositionKnown = input.ContextKnown = true;
                input.Flags = static_cast<std::uint8_t>((mode % 16) * 2);
                input.RestPosition = {1.25F, -7.0F, index & 1 ? -0.0F : 0.0F};
                input.Compressed = mode >= 16;
                input.IncludePartial = (index & 1U) != 0;
                input.Nodes = nodes;
                float observedScale = 777;
                Check(NativeObservePedBlendPartialScale(input, observedScale) == S::Planned && bits(observedScale) == bits(scale));
                NativePedBlendProductionPlan out;
                Check(NativePlanPedBlendProduction(input, out) == S::Planned);
                Check(out.AdvanceNodeArrays == count && bits(out.PartialScale) == bits(scale));
                Check(out.Quaternion.has_value() == !(input.Flags & 2U) &&
                    out.Translation.has_value() == !(input.Flags & 4U));
                std::cout << "BLEND_PRODUCTION " << index << ' ' << mode << ' ' << count << ' '
                    << static_cast<unsigned>(input.Flags) << ' ' << input.IncludePartial;
                for (float value : input.RestPosition) std::cout << ' ' << bits(value);
                for (const auto& node : nodes) {
                    std::cout << ' ' << node.Valid << ' ' << node.Partial << ' ' << node.HasTranslation
                        << ' ' << bits(node.BlendAmount);
                    for (float value : node.Quaternion) std::cout << ' ' << bits(value);
                    for (float value : node.Translation) std::cout << ' ' << bits(value);
                }
                std::cout << ' ' << bits(out.PartialScale) << ' ' << static_cast<unsigned>(out.AdvanceNodeArrays)
                    << ' ' << out.Quaternion.has_value();
                if (out.Quaternion) for (float value : *out.Quaternion) std::cout << ' ' << bits(value);
                std::cout << ' ' << out.Translation.has_value();
                if (out.Translation) for (float value : *out.Translation) std::cout << ' ' << bits(value);
                std::cout << '\n';
            }
        }
    }
    NativePedBlendContribution node;
    node.Known = node.Valid = node.PartialKnown = node.BlendKnown = node.TranslationKnown = node.UpdateKnown = true;
    node.UpdatedPartialScale = 1;
    NativePedBlendProductionInput input;
    input.Nodes = std::span{&node, 1};
    NativePedBlendProductionPlan out{777, 7, std::array<float, 4>{1, 2, 3, 4}, std::array<float, 3>{5, 6, 7}};
    const auto old = out;
    const auto guard = [&](S expected) { Check(NativePlanPedBlendProduction(input, out) == expected && out == old); };
    guard(S::UnknownFrame);
    input.FrameKnown = true; input.Flags = 8;
    guard(S::UnknownPedPosition);
    input.PedPositionKnown = input.HasPedPosition = true;
    guard(S::Velocity2DRequired);
    input.Flags = 24;
    guard(S::Velocity3DRequired);
    input.Compressed = true;
    guard(S::Velocity2DRequired);
    input.HasPedPosition = false;
    guard(S::UnknownBlendContext);
    input.ContextKnown = true; node.Known = false;
    guard(S::UnknownBlendNode);
    node.Known = true; node.UpdateKnown = false;
    guard(S::UnknownNodeUpdate);
    float observedScale = 777;
    Check(NativeObservePedBlendPartialScale(input, observedScale) == S::Planned && observedScale == 1);
    node.UpdateKnown = true; node.UpdatedPartialScale = 0;
    guard(S::InvalidInput);
    node.UpdatedPartialScale = 1; node.TranslationKnown = false;
    guard(S::UnknownBlendNode);
    node.TranslationKnown = true; node.Quaternion[2] = std::numeric_limits<float>::quiet_NaN();
    guard(S::InvalidInput);
    node.Quaternion[2] = 0; input.IncludePartial = true; node.PartialKnown = false;
    guard(S::UnknownBlendNode);
    node.PartialKnown = true; node.Partial = true; node.BlendKnown = false;
    guard(S::UnknownBlendNode);
    node.BlendKnown = true; input.Nodes = {};
    guard(S::InvalidInput);
    observedScale = 777;
    Check(NativeObservePedBlendPartialScale(input, observedScale) == S::InvalidInput && observedScale == 777);
    input.Nodes = std::span{&node, 1}; input.Flags = 6;
    input.RestPosition[0] = std::numeric_limits<float>::quiet_NaN();
    Check(NativePlanPedBlendProduction(input, out) == S::Planned && !out.Quaternion && !out.Translation);
    input.Flags = 0; input.IncludePartial = false; input.RestPosition = {6, 0, 0};
    node.HasTranslation = true; node.BlendAmount = 0.5F; node.Translation = {4, 0, 0};
    Check(NativePlanPedBlendProduction(input, out) == S::Planned && out.Translation &&
        (*out.Translation)[0] == 5 && out.Quaternion && (*out.Quaternion)[3] == 1);
}
}

int main() {
    using S = NativePedHierarchyStatus;
    ProductionCases();
    std::size_t bindCases = 0;
    for (std::uint32_t index = 0; index < 256; ++index) {
        for (const auto count : {1U, 2U, 8U, 32U, 64U}) {
            std::uint32_t seed = index * 0x9E3779B9U + count;
            std::vector<NativePedBindBone> bones(count);
            constexpr std::array<std::uint32_t, 4> traversal{1, 2, 0, 3};
            for (std::size_t i = 0; i < bones.size(); ++i) {
                auto& bone = bones[i];
                bone.Flags = traversal[i % 4];
                if (i + 1 == bones.size()) bone.Flags = 1;
                bone.InverseKnown = true;
                bone.InverseBind = Matrix(seed, index % 3 == 0 ? 0U : index % 3 == 1 ? 3U : 0x20003U);
                if (index < 8) {
                    bone.InverseBind.Value = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1},
                        {index & 1 ? -0.0F : 0.0F, index & 2 ? -0.0F : 0.0F, index & 4 ? -0.0F : 0.0F}};
                }
            }
            std::vector<std::array<float, 3>> positions;
            Check(NativePlanPedBindPositions({true, true, bones}, positions) == S::Planned);
            Check(positions.size() == count && positions.front() == std::array<float, 3>{});
            std::cout << "BIND_POSITION " << index << ' ' << count;
            for (const auto& bone : bones) {
                std::cout << ' ' << bone.Flags;
                Emit(bone.InverseBind);
            }
            for (const auto& point : positions)
                for (float value : point) std::cout << ' ' << std::bit_cast<std::uint32_t>(value);
            std::cout << '\n';
            ++bindCases;
            std::vector<std::int32_t> tags(count);
            for (std::uint32_t i = 0; i < count; ++i) tags[i] = static_cast<std::int32_t>(index * 100 + i);
            const std::uint32_t stride = index % 3 == 0 ? 28 : index % 3 == 1 ? 36 : 64;
            std::vector<NativePedBlendFrameBinding> bindings;
            Check(NativePlanPedBlendInitialization({{true, true, bones}, true, stride, tags}, bindings) == S::Planned);
            Check(bindings.size() == count && bindings.front().Flags == 8);
            std::cout << "BLEND_INIT " << index << ' ' << count << ' ' << stride;
            for (std::size_t i = 0; i < bones.size(); ++i) {
                std::cout << ' ' << tags[i] << ' ' << bones[i].Flags;
                Emit(bones[i].InverseBind);
            }
            for (const auto& binding : bindings) {
                std::cout << ' ' << static_cast<unsigned>(binding.Flags) << ' ' << binding.Tag
                          << ' ' << binding.KeyFrameByteOffset;
                for (float value : binding.RestPosition) std::cout << ' ' << std::bit_cast<std::uint32_t>(value);
            }
            std::cout << '\n';
        }
    }
    std::array<NativePedBindBone, 3> bindBones;
    for (auto& bone : bindBones) {
        bone.InverseKnown = true;
        bone.InverseBind = {{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {0, 0, 0}}, 3};
    }
    NativePedBindPositionInput bindInput{true, true, bindBones};
    std::vector<std::array<float, 3>> bindOut(1);
    bindOut.front() = {777, 888, 999};
    std::vector<std::array<float, 3>> oldBind(1);
    oldBind.front() = {777, 888, 999};
    bindInput.HierarchyKnown = false;
    Check(NativePlanPedBindPositions(bindInput, bindOut) == S::UnknownHierarchy && bindOut == oldBind);
    bindInput.HierarchyKnown = true; bindInput.SkinKnown = false;
    Check(NativePlanPedBindPositions(bindInput, bindOut) == S::UnknownSkin && bindOut == oldBind);
    bindInput.SkinKnown = true; bindBones.back().InverseKnown = false;
    Check(NativePlanPedBindPositions(bindInput, bindOut) == S::UnknownInverseBind && bindOut == oldBind);
    bindBones.back().InverseKnown = true; bindBones.back().InverseBind.Value.Pos[1] = std::numeric_limits<float>::quiet_NaN();
    Check(NativePlanPedBindPositions(bindInput, bindOut) == S::InvalidInput && bindOut == oldBind);
    bindBones.back().InverseBind.Value.Pos[1] = 0; bindBones[1].Flags = 1;
    Check(NativePlanPedBindPositions(bindInput, bindOut) == S::InvalidInput && bindOut == oldBind);
    bindInput.Bones = {};
    Check(NativePlanPedBindPositions(bindInput, bindOut) == S::InvalidInput && bindOut == oldBind);
    std::vector<NativePedBindBone> deepBind(34, bindBones[0]);
    for (auto& bone : deepBind) bone.Flags = 2;
    bindInput.Bones = deepBind;
    Check(NativePlanPedBindPositions(bindInput, bindOut) == S::InvalidInput && bindOut == oldBind);
    bindBones[0].InverseKnown = false; bindInput.Bones = std::span{bindBones}.first(1);
    Check(NativePlanPedBindPositions(bindInput, bindOut) == S::Planned && bindOut == std::vector<std::array<float, 3>>(1));
    std::array<std::int32_t, 1> initTags{0};
    NativePedBlendInitInput initInput{bindInput, true, 28, initTags};
    std::vector<NativePedBlendFrameBinding> initOut(1);
    initOut.front().Tag = 777;
    const auto oldInit = initOut;
    initInput.InterpolatorKnown = false;
    Check(NativePlanPedBlendInitialization(initInput, initOut) == S::UnknownInterpolator && initOut == oldInit);
    initInput.InterpolatorKnown = true; initInput.InterpolationStride = 27;
    Check(NativePlanPedBlendInitialization(initInput, initOut) == S::InvalidInput && initOut == oldInit);
    initInput.InterpolationStride = 257;
    Check(NativePlanPedBlendInitialization(initInput, initOut) == S::InvalidInput && initOut == oldInit);
    initInput.InterpolationStride = 28; initInput.Tags = {};
    Check(NativePlanPedBlendInitialization(initInput, initOut) == S::InvalidInput && initOut == oldInit);
    NativePedInterpolationFrame resetFrame;
    resetFrame.Quaternion = {777, 888, 999, 111};
    resetFrame.Translation = {222, 333, 444};
    NativeResetPedBlendInterpolationFrame(resetFrame);
    Check(resetFrame.Known && resetFrame.Quaternion == std::array<float, 4>{0, 0, 0, 1} &&
          resetFrame.Translation == std::array<float, 3>{});
    for (std::uint32_t index = 0; index < 8192; ++index) {
        NativePedInterpolationFrame frame;
        frame.Known = true;
        std::uint32_t seed = index * 0x9E3779B9U + 1;
        for (auto& value : frame.Quaternion) {
            seed = seed * 1664525U + 1013904223U;
            const auto exponent = index < 16 ? 0U : (seed >> 24) % 187U;
            value = std::bit_cast<float>((seed & 0x807FFFFFU) | (exponent << 23));
        }
        for (auto& value : frame.Translation) {
            seed = seed * 1664525U + 1013904223U;
            value = std::bit_cast<float>((seed & 0x807FFFFFU) | ((index % 254U) << 23));
        }
        if (index < 8) frame.Quaternion = {index & 1U ? -0.0F : 0.0F, 0, 0, index & 2U ? 1.0F : -1.0F};
        NativePedHitMatrix applied;
        Check(NativeApplyPedInterpolationFrame(frame, applied) == S::Planned);
        std::cout << "INTERPOLATION " << index;
        for (float value : frame.Quaternion) std::cout << ' ' << std::bit_cast<std::uint32_t>(value);
        for (float value : frame.Translation) std::cout << ' ' << std::bit_cast<std::uint32_t>(value);
        Emit(applied);
        std::cout << '\n';
        Check(NativeApplyPedBlendFrame(frame, applied) == S::Planned);
        std::cout << "BLEND_APPLICATION " << index;
        for (float value : frame.Quaternion) std::cout << ' ' << std::bit_cast<std::uint32_t>(value);
        for (float value : frame.Translation) std::cout << ' ' << std::bit_cast<std::uint32_t>(value);
        Emit(applied);
        std::cout << '\n';
    }
    std::size_t cases = 0;
    for (std::uint32_t profile = 0; profile < 192; ++profile) {
        const auto defaultCallback = profile / 64;
        const auto index = profile % 64;
        for (std::uint32_t mode = 0; mode < 32; ++mode) {
            std::uint32_t seed = index * 0x9E3779B9U + mode;
            std::array<NativePedHierarchyNode, 8> nodes;
            constexpr std::array<std::uint32_t, 8> traversal{0, 2, 3, 0, 1, 2, 1, 1};
            for (std::size_t j = 0; j < nodes.size(); ++j) {
                auto& node = nodes[j];
                node.Tag = static_cast<std::int32_t>(j + 1);
                node.Flags = traversal[j];
                node.AppliedKnown = node.FrameKnown = true;
                node.HasFrame = (index & (1U << j)) != 0;
                node.FramePrivateFlags = static_cast<std::uint8_t>(index + j);
                node.Applied = Matrix(seed, (index + j) % 7 == 0 ? 0x20003U : 3U);
                if (defaultCallback) {
                    NativePedInterpolationFrame frame;
                    frame.Known = true;
                    for (auto& value : frame.Quaternion) {
                        seed = seed * 1664525U + 1013904223U;
                        value = static_cast<float>(static_cast<std::int32_t>(seed >> 8) - 0x800000) / 16777216.0F;
                    }
                    frame.Translation = node.Applied.Value.Pos;
                    node.Interpolation = frame;
                    if (defaultCallback == 2) node.Application = NativePedFrameApplication::AnimBlend;
                    node.AppliedKnown = false;
                }
            }
            NativePedHierarchyInput input;
            input.HierarchyKnown = input.ParentKnown = input.SubParentKnown = input.RootFrameKnown = true;
            input.HasParent = index % 3 != 0;
            input.HasRootFrame = true;
            input.Flags = (mode & 3U) | ((mode & 4U) ? 0x1000U : 0U) |
                ((mode & 8U) ? 0x2000U : 0U) | ((mode & 16U) ? 0x4000U : 0U);
            input.ParentIndex = index % 2 ? 0 : -1;
            input.RootPrivateFlags = static_cast<std::uint8_t>(index % 16);
            input.ParentWorld = Matrix(seed, index % 7 == 0 ? 0x20003U : 3U);
            input.SubParent = Matrix(seed, 3U);
            input.Nodes = nodes;
            NativePedHierarchyPlan out;
            Check(NativePlanPedHierarchyUpdate(input, out) == S::Planned);
            Check(out.Nodes.size() == nodes.size());
            std::cout << (defaultCallback == 2 ? "HIERARCHY_BLEND " : defaultCallback ? "HIERARCHY_DEFAULT " : "HIERARCHY ") << index << ' ' << mode << ' ' << input.Flags << ' ' << input.HasParent
                << ' ' << input.ParentIndex << ' ' << unsigned(input.RootPrivateFlags);
            Emit(input.ParentWorld); Emit(input.SubParent);
            std::cout << ' ' << nodes.size();
            for (const auto& node : nodes) {
                std::cout << ' ' << node.Tag << ' ' << node.Flags << ' ' << node.HasFrame << ' ' << unsigned(node.FramePrivateFlags);
                Emit(node.Applied);
                if (node.Interpolation) {
                    for (float value : node.Interpolation->Quaternion) std::cout << ' ' << std::bit_cast<std::uint32_t>(value);
                    for (float value : node.Interpolation->Translation) std::cout << ' ' << std::bit_cast<std::uint32_t>(value);
                }
            }
            std::cout << ' ' << out.EnqueueRootDirty << ' ' << unsigned(out.RootPrivateFlags);
            for (const auto& node : out.Nodes) {
                std::cout << ' ' << unsigned(node.FramePrivateFlags) << ' ' << node.UpdateObjects;
                for (const auto* matrix : {&node.Matrix, &node.Modelling, &node.Ltm}) {
                    std::cout << ' ' << matrix->has_value();
                    if (*matrix) Emit(**matrix);
                }
            }
            std::cout << '\n';
            ++cases;
        }
    }
    std::array<NativePedHierarchyNode, 2> nodes;
    for (auto& node : nodes) {
        node.AppliedKnown = node.FrameKnown = true;
        node.Applied = {{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {0, 0, 0}}, 3};
    }
    NativePedHierarchyInput input;
    input.HierarchyKnown = input.ParentKnown = true;
    input.Nodes = nodes;
    NativePedHierarchyPlan out;
    Check(NativePlanPedHierarchyUpdate(input, out) == S::Planned);
    const auto prior = out;
    input.HierarchyKnown = false;
    Check(NativePlanPedHierarchyUpdate(input, out) == S::UnknownHierarchy && out == prior);
    input.HierarchyKnown = true; input.ParentKnown = false;
    Check(NativePlanPedHierarchyUpdate(input, out) == S::UnknownParent && out == prior);
    input.ParentKnown = true; nodes[1].AppliedKnown = false;
    Check(NativePlanPedHierarchyUpdate(input, out) == S::UnknownAppliedPose && out == prior);
    nodes[1].AppliedKnown = true; nodes[1].FrameKnown = false;
    Check(NativePlanPedHierarchyUpdate(input, out) == S::UnknownFrame && out == prior);
    nodes[1].FrameKnown = true; nodes[0].Flags = 1;
    Check(NativePlanPedHierarchyUpdate(input, out) == S::InvalidInput && out == prior);
    nodes[0].Flags = 0; nodes[1].Applied.Value.Pos[0] = std::numeric_limits<float>::infinity();
    Check(NativePlanPedHierarchyUpdate(input, out) == S::InvalidInput && out == prior);
    nodes[1].Applied.Value.Pos[0] = 0; input.Flags = 0x2000;
    Check(NativePlanPedHierarchyUpdate(input, out) == S::UnknownRootFrame && out == prior);
    input.RootFrameKnown = true;
    Check(NativePlanPedHierarchyUpdate(input, out) == S::InvalidInput && out == prior);
    input.HasRootFrame = true; input.Flags = 1;
    Check(NativePlanPedHierarchyUpdate(input, out) == S::UnknownParent && out == prior);
    input.SubParentKnown = true; input.ParentIndex = 256;
    Check(NativePlanPedHierarchyUpdate(input, out) == S::InvalidInput && out == prior);
    input.ParentIndex = -1; input.Flags = 0; input.Nodes = {};
    Check(NativePlanPedHierarchyUpdate(input, out) == S::InvalidInput && out == prior);
    input.Nodes = nodes; input.Flags = 0x4000; input.ParentKnown = false;
    input.ParentWorld.Value.Pos[0] = std::numeric_limits<float>::quiet_NaN();
    Check(NativePlanPedHierarchyUpdate(input, out) == S::Planned);
    input.Flags = 1; input.ParentIndex = 0;
    Check(NativePlanPedHierarchyUpdate(input, out) == S::Planned);
    const auto noParentPlan = out;
    input.Flags = 0x6001;
    Check(NativePlanPedHierarchyUpdate(input, out) == S::UnknownParent && out == noParentPlan);
    input.Flags = 0x4002;
    Check(NativePlanPedHierarchyUpdate(input, out) == S::UnknownParent && out == noParentPlan);
    std::array<NativePedHierarchyNode, 34> deep;
    deep.fill(nodes[0]);
    for (auto& node : deep) node.Flags = 2;
    input.Flags = 0x4000; input.Nodes = deep;
    Check(NativePlanPedHierarchyUpdate(input, out) == S::InvalidInput && out == noParentPlan);
    NativePedHitMatrix applied = nodes[0].Applied;
    const auto priorApplied = applied;
    NativePedInterpolationFrame frame;
    Check(NativeApplyPedInterpolationFrame(frame, applied) == S::UnknownAppliedPose && applied == priorApplied);
    Check(NativeApplyPedBlendFrame(frame, applied) == S::UnknownAppliedPose && applied == priorApplied);
    frame.Known = true; frame.Quaternion[0] = std::numeric_limits<float>::quiet_NaN();
    Check(NativeApplyPedInterpolationFrame(frame, applied) == S::InvalidInput && applied == priorApplied);
    Check(NativeApplyPedBlendFrame(frame, applied) == S::InvalidInput && applied == priorApplied);
    frame.Quaternion[0] = std::numeric_limits<float>::max();
    Check(NativeApplyPedInterpolationFrame(frame, applied) == S::InvalidInput && applied == priorApplied);
    frame.Quaternion = {0, 0, 0, 0}; frame.Translation[2] = std::numeric_limits<float>::infinity();
    Check(NativeApplyPedInterpolationFrame(frame, applied) == S::InvalidInput && applied == priorApplied);
    frame.Translation[2] = 0;
    Check(NativeApplyPedInterpolationFrame(frame, applied) == S::Planned && applied.Flags == 3);
    input.Nodes = nodes; nodes[1].Interpolation = frame; nodes[1].Interpolation->Known = false;
    Check(NativePlanPedHierarchyUpdate(input, out) == S::UnknownAppliedPose && out == noParentPlan);
    nodes[1].Interpolation->Known = true; nodes[1].Interpolation->Quaternion[2] = std::numeric_limits<float>::quiet_NaN();
    Check(NativePlanPedHierarchyUpdate(input, out) == S::InvalidInput && out == noParentPlan);
    nodes[1].Interpolation->Quaternion[2] = 0;
    nodes[1].Application = static_cast<NativePedFrameApplication>(2);
    Check(NativePlanPedHierarchyUpdate(input, out) == S::InvalidInput && out == noParentPlan);
    std::cout << "native-ped-hierarchy-ok checks=" << Checks << " cases=" << cases
        << " interpolation=8192 blend-application=8192 bind-positions=" << bindCases << " blend-init=" << bindCases
        << " blend-production=6144 callbacks=explicit,default,GTA frame-effects=planned loaded-state=unowned census=incomplete\n";
}
