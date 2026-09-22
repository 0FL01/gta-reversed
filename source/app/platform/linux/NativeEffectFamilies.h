#pragma once

#include <array>
#include <cstdint>

enum class NativeEffectFamily : std::uint8_t { Particle, Shadow, Reflection, Post };

struct NativeEffectColour {
    float Red = 0.0f, Green = 0.0f, Blue = 0.0f, Alpha = 0.0f;
    bool operator==(const NativeEffectColour&) const = default;
};

struct NativeEffectState {
    NativeEffectFamily Family = NativeEffectFamily::Particle;
    NativeEffectColour Primary, Secondary;
    float Coefficient = 0.0f;
    bool Additive = false, DepthWrite = false;
    bool operator==(const NativeEffectState&) const = default;
};

// Explicit PC-profile representatives. Particle uses source ONE/ONE low-cloud
// blending; shadow uses DEFAULT source alpha; reflection uses MatFX ENVMAP;
// post uses the PC two-pass colour filter. Geometry/system scheduling remains
// with the corresponding source owners.
class NativeEffectFamilies {
public:
    static constexpr std::size_t FamilyCount = 4;
    NativeEffectFamilies();
    const std::array<NativeEffectState, FamilyCount>& States() const noexcept { return m_States; }
    const NativeEffectState& State(NativeEffectFamily) const noexcept;
    static NativeEffectColour Composite(const NativeEffectState&, NativeEffectColour destination) noexcept;
    static constexpr bool PresentationFeedback = false;

private:
    std::array<NativeEffectState, FamilyCount> m_States{};
};
