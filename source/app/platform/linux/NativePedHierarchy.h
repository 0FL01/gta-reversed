#pragma once

#include "NativePedHitCollision.h"

#include <optional>
#include <span>
#include <vector>

// Already interpolated HAnim frame values, not a DFF frame or an IFP key.
// The source application does not normalize the quaternion.
struct NativePedInterpolationFrame {
    bool Known = false;
    std::array<float, 4> Quaternion{};
    std::array<float, 3> Translation{};
};

// A sampled interpolation callback result, not an authored DFF frame LTM.
// Frame attachment/flags and parent matrices are separate live observations.
struct NativePedHierarchyNode {
    std::int32_t Tag = -1;
    std::uint32_t Flags = 0;
    bool AppliedKnown = false;
    NativePedHitMatrix Applied;
    bool FrameKnown = false;
    bool HasFrame = false;
    std::uint8_t FramePrivateFlags = 0;
    // Explicit selection of the original default callback/optimized inline
    // application. When present, Applied/AppliedKnown are not consulted.
    std::optional<NativePedInterpolationFrame> Interpolation = std::nullopt;
};

struct NativePedHierarchyInput {
    bool HierarchyKnown = false;
    std::uint32_t Flags = 0;
    bool ParentKnown = false;
    bool HasParent = false;
    // Selected root-parent world observation (parent hierarchy's root for a
    // local-space subhierarchy). Ignored when the source branch does not read it.
    NativePedHitMatrix ParentWorld;
    bool SubParentKnown = false;
    std::int32_t ParentIndex = -1;
    NativePedHitMatrix SubParent;
    bool RootFrameKnown = false;
    bool HasRootFrame = false;
    std::uint8_t RootPrivateFlags = 0;
    std::span<const NativePedHierarchyNode> Nodes;
};

struct NativePedHierarchyNodeUpdate {
    std::int32_t Tag = -1;
    std::optional<NativePedHitMatrix> Matrix, Modelling, Ltm;
    std::uint8_t FramePrivateFlags = 0;
    bool UpdateObjects = false;
    bool operator==(const NativePedHierarchyNodeUpdate&) const = default;
};

struct NativePedHierarchyPlan {
    bool EnqueueRootDirty = false;
    std::uint8_t RootPrivateFlags = 0;
    std::vector<NativePedHierarchyNodeUpdate> Nodes;
    bool operator==(const NativePedHierarchyPlan&) const = default;
};

enum class NativePedHierarchyStatus {
    Planned, UnknownHierarchy, UnknownParent, UnknownAppliedPose, UnknownFrame,
    UnknownRootFrame, InvalidInput, UnknownSkin, UnknownInverseBind
};

struct NativePedBindBone {
    std::uint32_t Flags = 0; // Authored HAnim node flags, not matrix flags.
    bool InverseKnown = false;
    NativePedHitMatrix InverseBind;
};
struct NativePedBindPositionInput {
    bool HierarchyKnown = false, SkinKnown = false;
    std::span<const NativePedBindBone> Bones;
};
// SkinGetBonePositionsToTable used by RpAnimBlendClumpInitSkinned. These are
// authored rest translations for blend-frame data, NOT current keyframes or
// hierarchy matrices. Root translation is the literal source zero; later
// translations require the actual skin-to-bone matrix and source node order.
// Bounded to 64 bones and the original 32-slot stack (31 usable saves). A terminal
// unused POP may exhaust the stack; consumed underflow is never guessed.
// All failure paths preserve out; no pose, binding, refs or Loaded publication.
NativePedHierarchyStatus NativePlanPedBindPositions(const NativePedBindPositionInput& input,
    std::vector<std::array<float, 3>>& out);

NativePedHierarchyStatus NativeApplyPedInterpolationFrame(const NativePedInterpolationFrame& input,
    NativePedHitMatrix& out);

// RpHAnimHierarchyUpdateMatrices: ordered node traversal and matrix/frame
// writes under RWDEFAULT. The caller must fulfill the root dirty-list intent
// and frame writes before publishing a live pose. Custom callback matrices or
// default callback interpolation frames must be explicitly observed; this does
// not produce/interpolate IFP frames, bind atomics, or grant Loaded.
// Traversal is bounded to 256 nodes/32 saved parents. Terminal POP restoration
// is unused and may exhaust the stack; a consumed underflow is rejected.
// Unknown/invalid input preserves the complete previous plan.
NativePedHierarchyStatus NativePlanPedHierarchyUpdate(const NativePedHierarchyInput& input,
    NativePedHierarchyPlan& out);
