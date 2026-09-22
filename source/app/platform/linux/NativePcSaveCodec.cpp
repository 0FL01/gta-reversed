#include "NativePcSaveCodec.h"

#include <algorithm>
#include <limits>

namespace {
constexpr std::array<std::uint8_t, 5> Tag{'B', 'L', 'O', 'C', 'K'};

std::uint32_t Read32(std::span<const std::uint8_t> data, std::size_t offset) {
    return std::uint32_t(data[offset]) |
        std::uint32_t(data[offset + 1]) << 8u |
        std::uint32_t(data[offset + 2]) << 16u |
        std::uint32_t(data[offset + 3]) << 24u;
}

void Write32(std::span<std::uint8_t> data, std::size_t offset, std::uint32_t value) {
    data[offset] = std::uint8_t(value);
    data[offset + 1] = std::uint8_t(value >> 8u);
    data[offset + 2] = std::uint8_t(value >> 16u);
    data[offset + 3] = std::uint8_t(value >> 24u);
}

NativePcSaveStatus Fail(NativePcSaveStatus status, const char* message, std::string& error) {
    error = message;
    return status;
}
}

const std::array<const char*, NativePcSaveBlockCount>& NativePcSaveCodec::BlockNames() noexcept {
    static const std::array<const char*, NativePcSaveBlockCount> names{
        "SIMPLE_VARIABLES", "SCRIPTS", "POOLS", "GARAGES", "GAMELOGIC", "PATHS",
        "PICKUPS", "PHONEINFO", "RESTART", "RADAR", "ZONES", "GANGS",
        "CAR_GENERATORS", "PED_GENERATORS", "AUDIO_SCRIPT_OBJECT", "PLAYERINFO",
        "STATS", "SET_PIECES", "STREAMING", "PED_TYPES", "TAGS", "IPLS",
        "SHOPPING", "GANGWARS", "STUNTJUMPS", "ENTRY_EXITS", "RADIOTRACKS",
        "USER3DMARKERS"
    };
    return names;
}

NativePcSaveLayout NativePcSaveCodec::LayoutOf(const NativePcSaveImage& image) {
    NativePcSaveLayout layout;
    for (std::size_t i = 0; i < NativePcSaveBlockCount; ++i) {
        layout.PayloadBytes[i] = image.Blocks[i].size() <= std::numeric_limits<std::uint32_t>::max()
            ? std::uint32_t(image.Blocks[i].size()) : std::numeric_limits<std::uint32_t>::max();
    }
    return layout;
}

std::uint32_t NativePcSaveCodec::Checksum(std::span<const std::uint8_t> data) noexcept {
    std::uint32_t checksum = 0;
    for (const auto byte : data) checksum += byte;
    return checksum;
}

NativePcSaveStatus NativePcSaveCodec::Encode(const NativePcSaveImage& image,
    std::vector<std::uint8_t>& out, std::string& error) {
    std::size_t used = NativePcSaveBlockCount * Tag.size();
    for (const auto& block : image.Blocks) {
        if (block.size() > NativePcSaveDataBytes - used)
            return Fail(NativePcSaveStatus::Overflow, "PC save blocks exceed source data size", error);
        used += block.size();
    }
    std::vector<std::uint8_t> candidate(NativePcSaveFileBytes, 0);
    std::size_t cursor = 0;
    for (const auto& block : image.Blocks) {
        std::copy(Tag.begin(), Tag.end(), candidate.begin() + std::ptrdiff_t(cursor));
        cursor += Tag.size();
        std::copy(block.begin(), block.end(), candidate.begin() + std::ptrdiff_t(cursor));
        cursor += block.size();
    }
    Write32(candidate, NativePcSaveDataBytes,
        Checksum(std::span<const std::uint8_t>{candidate}.first(NativePcSaveDataBytes)));
    out = std::move(candidate);
    error.clear();
    return NativePcSaveStatus::Ok;
}

NativePcSaveStatus NativePcSaveCodec::Decode(std::span<const std::uint8_t> bytes,
    const NativePcSaveLayout& layout, NativePcSaveImage& out, std::string& error) {
    if (bytes.size() != NativePcSaveFileBytes)
        return Fail(NativePcSaveStatus::SizeMismatch, "PC save file size is not 202752 bytes", error);
    if (Checksum(bytes.first(NativePcSaveDataBytes)) != Read32(bytes, NativePcSaveDataBytes))
        return Fail(NativePcSaveStatus::ChecksumMismatch, "PC save additive checksum mismatch", error);
    std::size_t used = NativePcSaveBlockCount * Tag.size();
    for (const auto size : layout.PayloadBytes) {
        if (size > NativePcSaveDataBytes - used)
            return Fail(NativePcSaveStatus::Overflow, "PC save layout exceeds source data size", error);
        used += size;
    }
    NativePcSaveImage candidate;
    std::size_t cursor = 0;
    for (std::size_t i = 0; i < NativePcSaveBlockCount; ++i) {
        if (!std::equal(Tag.begin(), Tag.end(), bytes.begin() + std::ptrdiff_t(cursor)))
            return Fail(NativePcSaveStatus::TagMismatch, "PC save BLOCK tag mismatch", error);
        cursor += Tag.size();
        const auto size = std::size_t(layout.PayloadBytes[i]);
        candidate.Blocks[i].assign(bytes.begin() + std::ptrdiff_t(cursor),
            bytes.begin() + std::ptrdiff_t(cursor + size));
        cursor += size;
    }
    if (std::any_of(bytes.begin() + std::ptrdiff_t(cursor),
        bytes.begin() + std::ptrdiff_t(NativePcSaveDataBytes), [](std::uint8_t value) { return value != 0; }))
        return Fail(NativePcSaveStatus::SizeMismatch, "PC save padding is not canonical zero data", error);
    out = std::move(candidate);
    error.clear();
    return NativePcSaveStatus::Ok;
}

NativePcSaveStatus NativePcSaveCodec::RepairReferences(NativePcSaveImage& image,
    std::span<const NativePcSaveReferenceField> fields,
    std::span<const std::pair<std::int32_t, std::int32_t>> references, std::string& error) {
    NativePcSaveImage candidate = image;
    for (const auto& field : fields) {
        const auto blockIndex = std::size_t(field.Block);
        if (blockIndex >= NativePcSaveBlockCount ||
            std::uint64_t(field.Offset) + sizeof(std::int32_t) > candidate.Blocks[blockIndex].size())
            return Fail(NativePcSaveStatus::InvalidInput, "PC save reference field is out of bounds", error);
        auto& block = candidate.Blocks[blockIndex];
        const auto oldValue = std::int32_t(Read32(block, field.Offset));
        if (oldValue == -1 && field.NullAllowed) continue;
        const auto it = std::find_if(references.begin(), references.end(),
            [oldValue](const auto& pair) { return pair.first == oldValue; });
        if (it == references.end())
            return Fail(NativePcSaveStatus::ReferenceUnavailable, "PC save reference has no repair target", error);
        Write32(block, field.Offset, std::uint32_t(it->second));
    }
    image = std::move(candidate);
    error.clear();
    return NativePcSaveStatus::Ok;
}
