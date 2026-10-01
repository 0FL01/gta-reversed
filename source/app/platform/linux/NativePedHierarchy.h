#pragma once

#include "NativePedHitCollision.h"

#include <optional>
#include <span>
#include <vector>

// Already interpolated HAnim/blend frame values, not a DFF frame or an IFP key.
// The source application does not normalize the quaternion.
struct NativePedInterpolationFrame {
    bool Known = false;
    std::array<float, 4> Quaternion{};
    std::array<float, 3> Translation{};
};

enum class NativePedFrameApplication { HAnimDefault, AnimBlend };

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
    // Explicit selection of the original default callback/optimized inline or
    // registered GTA blend application. Applied/AppliedKnown are not consulted.
    std::optional<NativePedInterpolationFrame> Interpolation = std::nullopt;
    NativePedFrameApplication Application = NativePedFrameApplication::HAnimDefault;
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
    UnknownRootFrame, InvalidInput, UnknownSkin, UnknownInverseBind, UnknownInterpolator,
    UnknownBlendContext, UnknownBlendNode, UnknownNodeUpdate, UnknownPedPosition,
    Velocity2DRequired, Velocity3DRequired
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

struct NativePedBlendInitInput {
    NativePedBindPositionInput Bind;
    bool InterpolatorKnown = false;
    std::uint32_t InterpolationStride = 0;
    std::span<const std::int32_t> Tags;
};
struct NativePedBlendFrameBinding {
    std::uint8_t Flags = 0;
    std::array<float, 3> RestPosition{};
    std::int32_t Tag = -1;
    std::uint32_t KeyFrameIndex = 0, KeyFrameByteOffset = 0;
    bool operator==(const NativePedBlendFrameBinding&) const = default;
};
// RpAnimBlendClumpInitSkinned: frame-data bindings/rest values and root velocity
// flag. Offsets refer to the explicitly observed interpolator, not process
// pointers. The caller must allocate/bind frame data before publishing it.
// This does NOT initialize interpolation-frame contents or produce a live pose.
NativePedHierarchyStatus NativePlanPedBlendInitialization(const NativePedBlendInitInput& input,
    std::vector<NativePedBlendFrameBinding>& out);

NativePedHierarchyStatus NativeApplyPedInterpolationFrame(const NativePedInterpolationFrame& input,
    NativePedHitMatrix& out);

// GTA's registered 28-byte blend frame differs from RW's 36-byte default frame:
// its quaternion products are ALL spilled to float. No quaternion normalization.
NativePedHierarchyStatus NativeApplyPedBlendFrame(const NativePedInterpolationFrame& input,
    NativePedHitMatrix& out);
// The registered interpolation callback ignores keys/time and writes identity
// q / zero t. Only an actual callback invocation authorizes these frame values;
// blend-frame allocation/initialization above never implies it was invoked.
void NativeResetPedBlendInterpolationFrame(NativePedInterpolationFrame& out);

// Results of genuine Update/UpdateCompressed calls at the supplied partial scale.
// These are already weighted contributions, not raw IFP keys or invented poses.
struct NativePedBlendContribution {
    bool Known = false, Valid = false;
    bool PartialKnown = false, Partial = false;
    bool BlendKnown = false;
    float BlendAmount = 0;
    bool TranslationKnown = false, HasTranslation = false;
    bool UpdateKnown = false;
    float UpdatedPartialScale = 0;
    std::array<float, 4> Quaternion{};
    std::array<float, 3> Translation{};
};
struct NativePedBlendProductionInput {
    bool FrameKnown = false;
    std::uint8_t Flags = 0;
    std::array<float, 3> RestPosition{};
    bool PedPositionKnown = false, HasPedPosition = false;
    bool Compressed = false;
    bool ContextKnown = false, IncludePartial = false;
    std::span<const NativePedBlendContribution> Nodes;
};
struct NativePedBlendProductionPlan {
    float PartialScale = 0;
    std::uint8_t AdvanceNodeArrays = 0;
    std::optional<std::array<float, 4>> Quaternion;
    std::optional<std::array<float, 3>> Translation;
    bool operator==(const NativePedBlendProductionPlan&) const = default;
};
// Shared pre-update scale authority. Does not read node Update outputs, so a
// genuine sampler can obtain the scale without fabricating contributions.
// Velocity/unknown/invalid observations preserve out.
NativePedHierarchyStatus NativeObservePedBlendPartialScale(const NativePedBlendProductionInput& input,
    float& out);
// Ordinary FrameUpdateCallBack[Compressed]Skinned accumulation. Velocity branches
// are typed required effects, never treated as ordinary/no movement. Ignored
// channels remain absent (not known previous values). Each node array advances,
// including invalid nodes; the caller must fulfill node updates/cursor intents
// before publishing a pose. No IFP sampling or interpolation-frame binding here.
// Empty context is not a callback invocation: the source main loop needs a node.
NativePedHierarchyStatus NativePlanPedBlendProduction(const NativePedBlendProductionInput& input,
    NativePedBlendProductionPlan& out);

// RpHAnimHierarchyUpdateMatrices: ordered node traversal and matrix/frame
// writes under RWDEFAULT. The caller must fulfill the root dirty-list intent
// and frame writes before publishing a live pose. Custom callback matrices or
// default/GTA callback interpolation frames must be explicitly observed; this does
// not produce/interpolate IFP frames, bind atomics, or grant Loaded.
// Traversal is bounded to 256 nodes/32 saved parents. Terminal POP restoration
// is unused and may exhaust the stack; a consumed underflow is rejected.
// Unknown/invalid input preserves the complete previous plan.
NativePedHierarchyStatus NativePlanPedHierarchyUpdate(const NativePedHierarchyInput& input,
    NativePedHierarchyPlan& out);
