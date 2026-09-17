// Source CSetPieces registration owner. Runtime police generation is separate.
#pragma once

#include "app/platform/linux/NativeScriptSession.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

struct NativeSetPiecePoint {
    float X = 0, Y = 0;
    bool operator==(const NativeSetPiecePoint&) const = default;
};

struct NativeSetPiece {
    std::uint8_t Type = 0;
    NativeSetPiecePoint CornerMin, CornerMax;
    NativeSetPiecePoint Spawn1, Target1, Spawn2, Target2;
    bool operator==(const NativeSetPiece&) const = default;
};

enum class NativeSetPieceStatus : std::uint8_t { Ok, InvalidInput, CapacityExceeded };

class NativeSetPieces {
public:
    static constexpr std::size_t Capacity = 210;
    static constexpr bool RuntimeUpdate = false;

    NativeSetPieceStatus Add(const NativeScriptSetPieceRequest&, std::string& error);
    std::span<const NativeSetPiece> Entries() const { return {m_Entries.data(), m_Count}; }
    std::uint64_t Revision() const { return m_Revision; }

private:
    std::array<NativeSetPiece, Capacity> m_Entries{};
    std::size_t m_Count = 0;
    std::uint64_t m_Revision = 0;
};
