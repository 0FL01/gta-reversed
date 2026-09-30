#include "NativePedHitCollision.h"

#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace {
std::size_t Checks = 0;
void Check(bool value) {
    ++Checks;
    if (!value) { std::fprintf(stderr, "ped-hit-col-check-fail %zu\n", Checks); std::abort(); }
}
NativePedHitMatrix Matrix(std::uint32_t& seed, std::uint32_t flags) {
    const auto value = [&]() {
        seed = seed * 1664525U + 1013904223U;
        return static_cast<float>(static_cast<std::int32_t>(seed)) / 2147483648.0F;
    };
    NativePedHitMatrix m;
    m.Flags = flags;
    m.Value = {{value() + 2.0F, value(), value()}, {value(), value() + 2.0F, value()},
               {value(), value(), value() + 2.0F}, {value() * 10000.0F, value() * 10000.0F, value() * 10000.0F}};
    return m;
}
void Print(const NativePedHitMatrix& m) {
    std::printf(" %u", m.Flags);
    for (const auto& v : {m.Value.Right, m.Value.Up, m.Value.At, m.Value.Pos})
        for (float f : v) std::printf(" %u", std::bit_cast<std::uint32_t>(f));
}
void Print(const NativePedHitCollision& c) {
    for (const auto& sphere : c.Spheres) {
        for (float f : sphere.Center) std::printf(" %u", std::bit_cast<std::uint32_t>(f));
        std::printf(" %u %u %u", std::bit_cast<std::uint32_t>(sphere.Radius), sphere.Material, sphere.Piece);
    }
}
}
int main() {
    using S = NativePedHitCollisionStatus;
    std::array<NativePedHitBone, 32> bones;
    // Original bone lookup selects the first matching tag, including tag 3's
    // two distinct collision nodes. Bone array ordering is not node ordering.
    constexpr std::array<int, 32> tags{0, 1, 2, 3, 4, 5, 6, 7, 8, 21, 22, 23, 24, 25, 26,
        31, 32, 33, 34, 35, 36, 41, 42, 43, 44, 51, 52, 53, 54, 9, 10, 11};
    NativePedHitCollisionInput input;
    input.RootKnown = input.HierarchyKnown = true;
    input.Bones = bones;
    NativePedHitCollision out;
    for (std::uint32_t i = 0; i < 4112; ++i) {
        auto seed = i * 0x9E3779B9U + 1;
        input.RootLocal = Matrix(seed, i % 3 == 0 ? 0 : i % 3 == 1 ? 3 : 0x20003);
        for (std::size_t j = 0; j < bones.size(); ++j)
            bones[j] = {tags[j], true, Matrix(seed, (i + j) % 7 == 0 ? 0x20003 : 0)};
        if (i >= 4096) {
            const float z = (i & 1) ? -0.0F : 0.0F;
            const NativePlayerMatrix identity{{1, z, z}, {z, 1, z}, {z, z, 1}, {z, z, z}};
            input.RootLocal.Value = identity;
            for (auto& bone : bones) { bone.Matrix.Value = identity; bone.Matrix.Flags = i % 3 == 0 ? 0x20003 : 0; }
        }
        Check(NativeConstructPedHitCollision(input, out) == S::Constructed);
        Check(out.ColSlot == 0 && out.BoundRadius == 1.5F && out.BoundCenter == std::array<float, 3>{});
        Check(out.BoxMin == std::array<float, 3>{-0.5F, -0.5F, -1.2F} && out.BoxMax == std::array<float, 3>{0.5F, 0.5F, 1.2F});
        std::printf("HIT_COL %u", i);
        Print(input.RootLocal);
        std::printf(" %zu", bones.size());
        for (const auto& bone : bones) { std::printf(" %d", bone.Tag); Print(bone.Matrix); }
        Print(out);
        std::puts("");
    }
    const auto previous = out;
    input.RootKnown = false;
    Check(NativeConstructPedHitCollision(input, out) == S::UnknownRoot && out == previous);
    input.RootKnown = true; input.HierarchyKnown = false;
    Check(NativeConstructPedHitCollision(input, out) == S::UnknownHierarchy && out == previous);
    input.HierarchyKnown = true; bones[27].MatrixKnown = false;
    Check(NativeConstructPedHitCollision(input, out) == S::UnknownBoneMatrix && out == previous);
    bones[27].MatrixKnown = true;
    bones[27].Matrix.Value.Pos[1] = std::numeric_limits<float>::quiet_NaN();
    Check(NativeConstructPedHitCollision(input, out) == S::InvalidInput && out == previous);
    input.RootLocal.Value.Pos[0] = std::numeric_limits<float>::infinity();
    Check(NativeConstructPedHitCollision(input, out) == S::InvalidInput && out == previous);
    input.RootLocal.Value.Pos[0] = 0;
    input.Bones = {};
    Check(NativeConstructPedHitCollision(input, out) == S::InvalidInput && out == previous);
    std::printf("native-ped-hit-col-ok checks=%zu cases=4112 matrices=explicit loaded-state=unowned census=incomplete\n", Checks);
}
