#include "NativePedModelPolicies.h"

#include <algorithm>
#include <array>

namespace {
bool NameIs(std::string_view name, std::string_view literal) noexcept {
    if (name.size() != literal.size()) return false;
    for (std::size_t i = 0; i < name.size(); ++i) {
        const auto c = static_cast<unsigned char>(name[i]);
        const auto upper = c >= 'a' && c <= 'z' ? c - ('a' - 'A') : c;
        if (upper != static_cast<unsigned char>(literal[i])) return false;
    }
    return true;
}

bool BlockedStats(std::int32_t stats) noexcept {
    return (stats >= 0 && stats <= 10) || (stats >= 26 && stats <= 42);
}

bool OldStats(std::int32_t stats) noexcept {
    return stats == 18 || stats == 24;
}
}

NativePedModelPolicyStatus NativePedZoneAccepts(bool hasZone, bool zoneStreamingCheat,
    std::uint8_t raceMask, const NativePedPolicyModel& model, bool& out) noexcept {
    if (!hasZone) {
        out = false;
        return NativePedModelPolicyStatus::Decided;
    }
    if (zoneStreamingCheat) {
        out = true;
        return NativePedModelPolicyStatus::Decided;
    }
    if (!model.Known) return NativePedModelPolicyStatus::UnknownModel;
    if (model.Model < 0 || model.Model >= 20000 || model.Race < 0 || model.Race > 4)
        return NativePedModelPolicyStatus::InvalidInput;
    out = model.Race == 0 || ((raceMask & 15u) & (1u << (model.Race - 1))) != 0;
    return NativePedModelPolicyStatus::Decided;
}

NativePedModelPolicyStatus NativePedStatsCompatible(std::int32_t first,
    std::int32_t second, bool& out) noexcept {
    if (first < -1 || first > 42 || second < -1 || second > 42)
        return NativePedModelPolicyStatus::InvalidInput;
    // Original Population::ArePedStatsCompatible checks BOTH stats. Its
    // old-person constraint is intentionally asymmetric: an old first requires
    // an old second, whereas a non-old first may select an old second.
    out = !BlockedStats(first) && !BlockedStats(second) &&
        (!OldStats(first) || OldStats(second));
    return NativePedModelPolicyStatus::Decided;
}

NativePedModelPolicyStatus NativePedAttractorAccepts(const NativePedPolicyModel& model,
    std::optional<std::string_view> name, bool& out) noexcept {
    if (!name) {
        out = true;
        return NativePedModelPolicyStatus::Decided;
    }
    if (name->size() > 255 || std::ranges::any_of(*name, [](unsigned char c) { return c < 32 || c >= 127; }))
        return NativePedModelPolicyStatus::InvalidInput;
    if (!model.Known) return NativePedModelPolicyStatus::UnknownModel;
    if (model.Model < 0 || model.Model >= 20000 || model.PedType < 0 || model.PedType > 31)
        return NativePedModelPolicyStatus::InvalidInput;

    if (NameIs(*name, "COPSIT") || NameIs(*name, "COPLOOK") || NameIs(*name, "BROWSE")) {
        out = model.PedType == 6;
    } else if (model.PedType == 6) {
        out = false;
    } else if (NameIs(*name, "DANCER")) {
        constexpr std::array models{12, 20, 22, 40, 46, 56, 58, 59, 60, 91, 93, 98, 101};
        out = std::ranges::find(models, model.Model) != models.end();
    } else if (NameIs(*name, "BARGUY") || NameIs(*name, "PEDROUL") ||
               NameIs(*name, "PEDCARD") || NameIs(*name, "PEDSLOT")) {
        constexpr std::array excluded{27, 35, 50, 69, 71, 72, 73, 77, 134, 135, 136, 137,
            142, 153, 170, 218, 225, 226, 229, 230, 232, 233, 236, 239};
        out = std::ranges::find(excluded, model.Model) == excluded.end();
    } else if (NameIs(*name, "STRIPW")) {
        constexpr std::array models{87, 244, 246, 256, 257};
        out = std::ranges::find(models, model.Model) != models.end();
    } else if (NameIs(*name, "STRIPM")) {
        // Preserve the original SETNE (not the upstream refactor's equality).
        out = model.PedType != 5;
    } else {
        // Original's default branch permits an unrecognised non-cop attractor.
        out = true;
    }
    return NativePedModelPolicyStatus::Decided;
}
