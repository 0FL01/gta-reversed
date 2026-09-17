#include "app/platform/linux/NativeSetPieces.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace {
bool Compress(float value, float& output) {
    const float scaled = value * 4.0f;
    if (!std::isfinite(value) || !std::isfinite(scaled) ||
        scaled < std::numeric_limits<std::int16_t>::min() ||
        scaled > std::numeric_limits<std::int16_t>::max()) return false;
    output = float(std::int16_t(scaled)) / 4.0f;
    return true;
}
}

NativeSetPieceStatus NativeSetPieces::Add(const NativeScriptSetPieceRequest& request, std::string& error) {
    if (request.Type < 0 || request.Type > 8) {
        error = "source set-piece type is outside 0..8";
        return NativeSetPieceStatus::InvalidInput;
    }
    if (m_Count == Capacity) {
        error.clear(); // Source AddOne silently ignores registrations beyond 210.
        return NativeSetPieceStatus::CapacityExceeded;
    }
    NativeSetPiece candidate;
    candidate.Type = std::uint8_t(request.Type);
    const auto point = [&](std::size_t offset, NativeSetPiecePoint& out) {
        return Compress(request.Coordinates[offset], out.X) && Compress(request.Coordinates[offset + 1], out.Y);
    };
    const float left = std::min(request.Coordinates[0], request.Coordinates[2]);
    const float right = std::max(request.Coordinates[0], request.Coordinates[2]);
    const float bottom = std::min(request.Coordinates[1], request.Coordinates[3]);
    const float top = std::max(request.Coordinates[1], request.Coordinates[3]);
    if (!Compress(left, candidate.CornerMin.X) || !Compress(bottom, candidate.CornerMin.Y) ||
        !Compress(right, candidate.CornerMax.X) || !Compress(top, candidate.CornerMax.Y) ||
        !point(4, candidate.Spawn1) || !point(6, candidate.Target1) ||
        !point(8, candidate.Spawn2) || !point(10, candidate.Target2)) {
        error = "invalid/nonrepresentable source set-piece coordinate";
        return NativeSetPieceStatus::InvalidInput;
    }
    m_Entries[m_Count++] = candidate;
    ++m_Revision;
    error.clear();
    return NativeSetPieceStatus::Ok;
}
