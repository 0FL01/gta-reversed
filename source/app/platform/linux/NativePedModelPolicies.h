#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

enum class NativePedModelPolicyStatus : std::uint8_t {
    Decided,
    InvalidInput,
    UnknownModel,
};

struct NativePedPolicyModel {
    bool Known = false;
    std::int32_t Model = -1;
    std::int32_t PedType = -1;
    std::int32_t Race = -1;
};

// Source policy arithmetic over explicit model/zone observations. These helpers
// neither load models nor certify a streaming roster, actor birth or world census.
// Non-Decided results preserve the caller's previous decision.
NativePedModelPolicyStatus NativePedZoneAccepts(bool hasZone, bool zoneStreamingCheat,
    std::uint8_t raceMask, const NativePedPolicyModel&, bool& out) noexcept;
NativePedModelPolicyStatus NativePedStatsCompatible(std::int32_t first,
    std::int32_t second, bool& out) noexcept;
NativePedModelPolicyStatus NativePedAttractorAccepts(const NativePedPolicyModel&,
    std::optional<std::string_view> name, bool& out) noexcept;
