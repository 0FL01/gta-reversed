#include "app/platform/linux/NativeStuntJumps.h"

#include <cmath>
#include <limits>

namespace {
bool Finite(NativeStuntJumpVector value) {
    return std::isfinite(value.X) && std::isfinite(value.Y) && std::isfinite(value.Z);
}

bool NonNegative(NativeStuntJumpVector value) {
    return value.X >= 0 && value.Y >= 0 && value.Z >= 0;
}

bool MakeBox(NativeStuntJumpVector center, NativeStuntJumpVector halfSize, NativeStuntJumpBox& output) {
    output = {{center.X - halfSize.X, center.Y - halfSize.Y, center.Z - halfSize.Z},
        {center.X + halfSize.X, center.Y + halfSize.Y, center.Z + halfSize.Z}};
    return Finite(output.Min) && Finite(output.Max);
}
}

NativeStuntJumpStatus NativeStuntJumps::Add(NativeStuntJumpVector startCenter,
    NativeStuntJumpVector startHalfSize, NativeStuntJumpVector endCenter,
    NativeStuntJumpVector endHalfSize, NativeStuntJumpVector camera,
    std::int32_t reward, std::size_t& index, std::string& error) {
    if (!Finite(startCenter) || !Finite(startHalfSize) || !NonNegative(startHalfSize) ||
        !Finite(endCenter) || !Finite(endHalfSize) || !NonNegative(endHalfSize) || !Finite(camera)) {
        error = "invalid stunt-jump registration values";
        return NativeStuntJumpStatus::InvalidInput;
    }
    NativeStuntJump candidate;
    if (!MakeBox(startCenter, startHalfSize, candidate.Start) ||
        !MakeBox(endCenter, endHalfSize, candidate.End)) {
        error = "stunt-jump bounds overflow";
        return NativeStuntJumpStatus::Overflow;
    }
    if (m_Count == Capacity) {
        error = "stunt-jump source pool exhausted";
        return NativeStuntJumpStatus::Full;
    }
    if (m_Revision == std::numeric_limits<std::uint64_t>::max()) {
        error = "stunt-jump revision exhausted";
        return NativeStuntJumpStatus::Overflow;
    }
    candidate.Camera = camera;
    candidate.Reward = reward;
    m_Entries[m_Count] = candidate;
    index = m_Count++;
    ++m_Revision;
    error.clear();
    return NativeStuntJumpStatus::Ok;
}
