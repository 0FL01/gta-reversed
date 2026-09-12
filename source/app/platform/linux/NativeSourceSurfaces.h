#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

enum class NativeSourceSurfaceStatus : std::uint8_t {
    Ok,
    NotLoaded,
    InvalidMaterial,
};

struct NativeSourceSurfaceSnapshot {
    std::array<std::array<float, 6>, 6> AdhesiveLimits{};
    std::array<std::uint8_t, 179> AdhesionGroups{};
    std::size_t MaterialRows{};
    bool Loaded{};
    bool operator==(const NativeSourceSurfaceSnapshot&) const = default;
};

// Single-owner surface/adhesion binding, not a world or surface-effect owner.
// Load/reload is atomic. Missing/unknown group names retain the previous group
// (initially RUBBER), and unknown material names alias DEFAULT, as in the source.
// Complete six-row matrices are required; malformed/unsafe source inputs reject.
class NativeSourceSurfaces {
public:
    bool Load(const std::string& gameDir, std::string& error);
    bool LoadBytes(std::string_view adhesive, std::string_view materials, std::string& error);
    NativeSourceSurfaceStatus AdhesiveLimit(std::uint16_t materialA, std::uint16_t materialB, float& out) const noexcept;
    NativeSourceSurfaceSnapshot Snapshot() const noexcept { return m_Data; }

private:
    NativeSourceSurfaceSnapshot m_Data;
};
