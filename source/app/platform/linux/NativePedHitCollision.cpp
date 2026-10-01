#include "NativePedHitCollision.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>

namespace {
static_assert(std::numeric_limits<long double>::digits == 64, "source x87 extended arithmetic required");
using V = std::array<float, 3>;
using M = NativePedHitMatrix;
struct Node { std::int32_t Tag; std::uint8_t Piece; float X, Radius; };
// Canonical CPedModelInfo hit nodes, source order (not skeletal frame indices).
constexpr std::array<Node, 12> Nodes{{
    {5, 9, 0.05F, 0.15F}, {3, 3, 0.2F, 0.2F}, {3, 3, 0.0F, 0.2F},
    {2, 4, -0.1F, 0.2F}, {32, 5, 0.06F, 0.14F}, {22, 6, 0.06F, 0.14F},
    {33, 5, 0.05F, 0.14F}, {23, 6, 0.05F, 0.14F}, {42, 7, -0.1F, 0.18F},
    {52, 8, -0.1F, 0.18F}, {43, 7, -0.18F, 0.16F}, {53, 8, -0.18F, 0.16F}
}};
long double P(float a, float b) { return static_cast<long double>(a) * b; }
bool Finite(const V& v) {
    return std::all_of(v.begin(), v.end(), [](float f) { return std::isfinite(f); });
}
bool Finite(const M& m) {
    return Finite(m.Value.Right) && Finite(m.Value.Up) && Finite(m.Value.At) && Finite(m.Value.Pos);
}
M Inverse(const M& source) {
    if (source.Flags & 0x20000U) return source;
    const auto& [r, u, a, p] = source.Value;
    M out;
    auto& [ir, iu, ia, ip] = out.Value;
    if ((source.Flags & 3U) == 3U) {
        ir = {r[0], u[0], a[0]}; iu = {r[1], u[1], a[1]}; ia = {r[2], u[2], a[2]};
        ip = {static_cast<float>(-((P(p[1], r[1]) + P(p[0], r[0])) + P(r[2], p[2]))),
              static_cast<float>(-((P(p[0], u[0]) + P(p[1], u[1])) + P(p[2], u[2]))),
              static_cast<float>(-((P(a[0], p[0]) + P(p[1], a[1])) + P(a[2], p[2])))};
        out.Flags = 3;
        return out;
    }
    // Original general inverse: cofactor spills, extended determinant term,
    // and retained extended first coefficient in translation are significant.
    ir[0] = static_cast<float>(P(a[2], u[1]) - P(a[1], u[2]));
    ir[1] = static_cast<float>(-(P(a[2], r[1]) - P(a[1], r[2])));
    const auto c02 = P(u[2], r[1]) - P(u[1], r[2]);
    ir[2] = static_cast<float>(c02);
    const float determinant = static_cast<float>((P(u[0], ir[1]) + static_cast<long double>(a[0]) * c02) + P(ir[0], r[0]));
    const long double reciprocal = std::bit_cast<std::uint32_t>(determinant) == 0 ? 1.0L : 1.0L / determinant;
    const auto x00 = reciprocal * ir[0];
    ir[0] = static_cast<float>(x00);
    ir[1] = static_cast<float>(reciprocal * ir[1]);
    ir[2] = static_cast<float>(reciprocal * ir[2]);
    iu[0] = static_cast<float>(-((P(a[2], u[0]) - P(a[0], u[2])) * reciprocal));
    iu[1] = static_cast<float>((P(a[2], r[0]) - P(a[0], r[2])) * reciprocal);
    iu[2] = static_cast<float>(-((P(u[2], r[0]) - P(u[0], r[2])) * reciprocal));
    ia[0] = static_cast<float>((P(a[1], u[0]) - P(a[0], u[1])) * reciprocal);
    ia[1] = static_cast<float>(-((P(a[1], r[0]) - P(a[0], r[1])) * reciprocal));
    ia[2] = static_cast<float>((P(u[1], r[0]) - P(u[0], r[1])) * reciprocal);
    ip = {static_cast<float>(-((P(ia[0], p[2]) + P(p[1], iu[0])) + static_cast<long double>(p[0]) * x00)),
          static_cast<float>(-((P(ia[1], p[2]) + P(p[1], iu[1])) + P(p[0], ir[1]))),
          static_cast<float>(-((P(p[1], iu[2]) + P(p[0], ir[2])) + P(ia[2], p[2])))};
    return out;
}
M PreConcat(const M& inverse, const M& bone) {
    if (bone.Flags & 0x20000U) return inverse;
    if (inverse.Flags & 0x20000U) return bone;
    const auto& [r, u, a, p] = inverse.Value;
    const std::array<V, 4> rows{bone.Value.Right, bone.Value.Up, bone.Value.At, bone.Value.Pos};
    M out;
    std::array<V, 4> result;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const auto& v = rows[i];
        auto& o = result[i];
        const auto x = i == 2 ? (P(v[1], u[0]) + P(v[2], a[0])) + P(v[0], r[0]) :
                               (P(v[0], r[0]) + P(v[2], a[0])) + P(v[1], u[0]);
        o[0] = static_cast<float>(i == 3 ? x + p[0] : x);
        for (std::size_t j = 1; j < 3; ++j) {
            const auto sum = i == 0 ? (P(v[1], u[j]) + P(v[0], r[j])) + P(v[2], a[j]) :
                             i == 2 ? (P(v[2], a[j]) + P(v[1], u[j])) + P(v[0], r[j]) :
                                      (P(v[0], r[j]) + P(v[2], a[j])) + P(v[1], u[j]);
            o[j] = static_cast<float>(i == 3 ? sum + p[j] : sum);
        }
    }
    out.Value = {result[0], result[1], result[2], result[3]};
    out.Flags = bone.Flags & inverse.Flags;
    return out;
}
V TransformPoint(const V& point, const M& m) {
    const auto& [x, y, z] = point;
    const auto& [r, u, a, p] = m.Value;
    // RwV3dTransformPoints spills Y's accumulated coordinate before adding
    // translation, unlike X/Z. Zero terms retain original signed-zero math.
    const auto y0 = static_cast<float>(P(x, r[1]));
    const auto z0 = static_cast<float>(P(x, r[2]));
    const float accumulatedY = static_cast<float>((P(u[1], y) + y0) + P(a[1], z));
    return {static_cast<float>(((P(u[0], y) + P(x, r[0])) + P(a[0], z)) + p[0]),
            static_cast<float>(static_cast<long double>(p[1]) + accumulatedY),
            static_cast<float>(static_cast<long double>(p[2]) + ((P(u[2], y) + z0) + P(a[2], z)))};
}
V Center(float x, const M& m) { return TransformPoint({x, 0.0F, 0.0F}, m); }
}

NativePedHitMatrix NativeMultiplyPedMatrices(const NativePedHitMatrix& left, const NativePedHitMatrix& right) {
    return PreConcat(right, left);
}
NativePedHitMatrix NativeInvertPedMatrix(const NativePedHitMatrix& matrix) { return Inverse(matrix); }
std::array<float, 3> NativeTransformPedPoint(const std::array<float, 3>& point, const NativePedHitMatrix& matrix) {
    return TransformPoint(point, matrix);
}

NativePedHitCollisionStatus NativeConstructPedHitCollision(const NativePedHitCollisionInput& input, NativePedHitCollision& out) {
    using S = NativePedHitCollisionStatus;
    if (!input.RootKnown) return S::UnknownRoot;
    if (!input.HierarchyKnown) return S::UnknownHierarchy;
    if (!Finite(input.RootLocal) || input.Bones.empty() || input.Bones.size() > 256) return S::InvalidInput;
    const auto inverse = Inverse(input.RootLocal);
    if (!Finite(inverse)) return S::InvalidInput;
    NativePedHitCollision candidate;
    for (std::size_t i = 0; i < Nodes.size(); ++i) {
        const auto& node = Nodes[i];
        const auto bone = std::find_if(input.Bones.begin(), input.Bones.end(), [&](const auto& b) { return b.Tag == node.Tag; });
        if (bone == input.Bones.end() || !bone->MatrixKnown) return S::UnknownBoneMatrix;
        if (!Finite(bone->Matrix)) return S::InvalidInput;
        const auto matrix = PreConcat(inverse, bone->Matrix);
        if (!Finite(matrix)) return S::InvalidInput;
        const auto center = Center(node.X, matrix);
        if (!Finite(center)) return S::InvalidInput;
        candidate.Spheres[i] = {center, node.Radius, 62, node.Piece};
    }
    out = candidate;
    return S::Constructed;
}

NativePedHitCollisionStatus NativeAnimatePedHitCollision(const NativePedHitCollisionInput& input,
    NativePedHitCollisionSpace space, NativePedHitCollisionState& state) {
    using S = NativePedHitCollisionStatus;
    if (!state.Known) return S::UnknownHitModel;
    if (space != NativePedHitCollisionSpace::Local && space != NativePedHitCollisionSpace::World) return S::InvalidInput;
    auto candidate = state;
    if (!candidate.Present) {
        const auto result = NativeConstructPedHitCollision(input, candidate.Value);
        if (result != S::Constructed) return result;
        candidate.Present = true;
        if (space == NativePedHitCollisionSpace::Local) {
            state = candidate;
            return S::Constructed;
        }
    }
    if (!input.HierarchyKnown) return S::UnknownHierarchy;
    if (input.Bones.empty() || input.Bones.size() > 256) return S::InvalidInput;
    const bool local = space == NativePedHitCollisionSpace::Local;
    M inverse;
    if (local) {
        if (!input.RootKnown) return S::UnknownRoot;
        if (!Finite(input.RootLocal)) return S::InvalidInput;
        inverse = Inverse(input.RootLocal);
        if (!Finite(inverse)) return S::InvalidInput;
    }
    const auto transform = [&](std::int32_t tag, float x, V& out) {
        const auto bone = std::find_if(input.Bones.begin(), input.Bones.end(), [&](const auto& b) { return b.Tag == tag; });
        if (bone == input.Bones.end() || !bone->MatrixKnown) return S::UnknownBoneMatrix;
        if (!Finite(bone->Matrix)) return S::InvalidInput;
        const auto matrix = local ? PreConcat(inverse, bone->Matrix) : bone->Matrix;
        if (!Finite(matrix)) return S::InvalidInput;
        out = Center(x, matrix);
        return Finite(out) ? S::Updated : S::InvalidInput;
    };
    for (std::size_t i = 0; i < Nodes.size(); ++i) {
        auto& sphere = candidate.Value.Spheres[i];
        if (!std::isfinite(sphere.Radius) || sphere.Radius < 0) return S::InvalidInput;
        const auto result = transform(Nodes[i].Tag, Nodes[i].X, sphere.Center);
        if (result != S::Updated) return result;
    }
    auto& collision = candidate.Value;
    const auto result = transform(3, 0.0F, collision.BoundCenter); // BONE_SPINE1
    if (result != S::Updated) return result;
    collision.BoundRadius = 1.5F;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        collision.BoxMin[axis] = static_cast<float>(static_cast<long double>(collision.BoundCenter[axis]) - 1.2F);
        collision.BoxMax[axis] = static_cast<float>(static_cast<long double>(collision.BoundCenter[axis]) + 1.2F);
    }
    if (!Finite(collision.BoxMin) || !Finite(collision.BoxMax)) return S::InvalidInput;
    state = candidate;
    return S::Updated;
}
