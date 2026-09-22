#include "NativePcSaveSemantics.h"

#include <bit>
#include <cmath>
#include <limits>

namespace {
void Put32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) out.push_back(std::uint8_t(value >> (i * 8u)));
}

std::uint32_t Get32(const std::vector<std::uint8_t>& in, std::size_t offset) {
    return std::uint32_t(in[offset]) | std::uint32_t(in[offset + 1]) << 8u |
        std::uint32_t(in[offset + 2]) << 16u | std::uint32_t(in[offset + 3]) << 24u;
}

void PutFloat(std::vector<std::uint8_t>& out, float value) {
    Put32(out, std::bit_cast<std::uint32_t>(value));
}

float GetFloat(const std::vector<std::uint8_t>& in, std::size_t offset) {
    return std::bit_cast<float>(Get32(in, offset));
}

bool Valid(const NativePcPathSwitch& value) {
    return std::isfinite(value.MinX) && std::isfinite(value.MaxX) &&
        std::isfinite(value.MinY) && std::isfinite(value.MaxY) &&
        std::isfinite(value.MinZ) && std::isfinite(value.MaxZ) &&
        value.MinX <= value.MaxX && value.MinY <= value.MaxY && value.MinZ <= value.MaxZ;
}
}

NativePcSemanticStatus NativePcSaveSemantics::ExportPaths(const NativePcSemanticState& state,
    NativePcSaveImage& image, std::string& error) {
    if (state.PathSwitches.size() > PathSwitchCapacity) {
        error = "path-switch count exceeds source capacity";
        return NativePcSemanticStatus::Overflow;
    }
    for (const auto& value : state.PathSwitches) {
        if (!Valid(value)) {
            error = "path-switch state is invalid";
            return NativePcSemanticStatus::InvalidInput;
        }
    }
    std::vector<std::uint8_t> payload;
    payload.reserve(4 + state.PathSwitches.size() * PathSwitchBytes);
    Put32(payload, std::uint32_t(state.PathSwitches.size()));
    for (const auto& value : state.PathSwitches) {
        PutFloat(payload, value.MinX); PutFloat(payload, value.MaxX);
        PutFloat(payload, value.MinY); PutFloat(payload, value.MaxY);
        PutFloat(payload, value.MinZ); PutFloat(payload, value.MaxZ);
        payload.push_back(value.Off ? 1u : 0u);
        payload.push_back(value.Cars ? 1u : 0u);
        payload.push_back(0); payload.push_back(0);
    }
    image.Blocks[static_cast<std::size_t>(NativePcSaveBlock::Paths)] = std::move(payload);
    error.clear();
    return NativePcSemanticStatus::Ok;
}

NativePcSemanticStatus NativePcSaveSemantics::ImportPaths(const NativePcSaveImage& image,
    NativePcSemanticState& out, std::string& error) {
    const auto& payload = image.Blocks[static_cast<std::size_t>(NativePcSaveBlock::Paths)];
    if (payload.size() < 4) {
        error = "path block is truncated";
        return NativePcSemanticStatus::InvalidBlock;
    }
    const auto count = Get32(payload, 0);
    if (count > PathSwitchCapacity || payload.size() != 4 + std::size_t(count) * PathSwitchBytes) {
        error = "path block size/count mismatch";
        return NativePcSemanticStatus::InvalidBlock;
    }
    NativePcSemanticState candidate;
    candidate.PathSwitches.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const auto offset = 4 + i * PathSwitchBytes;
        NativePcPathSwitch value{
            GetFloat(payload, offset), GetFloat(payload, offset + 4),
            GetFloat(payload, offset + 8), GetFloat(payload, offset + 12),
            GetFloat(payload, offset + 16), GetFloat(payload, offset + 20),
            payload[offset + 24] != 0, payload[offset + 25] != 0,
        };
        if (payload[offset + 24] > 1 || payload[offset + 25] > 1 || !Valid(value)) {
            error = "path block contains invalid source state";
            return NativePcSemanticStatus::InvalidBlock;
        }
        candidate.PathSwitches.push_back(value);
    }
    out = std::move(candidate);
    error.clear();
    return NativePcSemanticStatus::Ok;
}
