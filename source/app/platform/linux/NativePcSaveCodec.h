#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

enum class NativePcSaveBlock : std::uint8_t {
    SimpleVariables, Scripts, Pools, Garages, GameLogic, Paths, Pickups, PhoneInfo,
    Restart, Radar, Zones, Gangs, CarGenerators, PedGenerators, AudioScriptObject,
    PlayerInfo, Stats, SetPieces, Streaming, PedTypes, Tags, Ipls, Shopping,
    GangWars, StuntJumps, EntryExits, RadioTracks, User3dMarkers, Count,
};

constexpr std::size_t NativePcSaveBlockCount = static_cast<std::size_t>(NativePcSaveBlock::Count);
constexpr std::size_t NativePcSaveDataBytes = 202748;
constexpr std::size_t NativePcSaveFileBytes = NativePcSaveDataBytes + sizeof(std::uint32_t);

enum class NativePcSaveStatus : std::uint8_t {
    Ok, InvalidInput, SizeMismatch, TagMismatch, ChecksumMismatch, ReferenceUnavailable, Overflow,
};

struct NativePcSaveLayout {
    std::array<std::uint32_t, NativePcSaveBlockCount> PayloadBytes{};
    bool operator==(const NativePcSaveLayout&) const = default;
};

struct NativePcSaveImage {
    std::array<std::vector<std::uint8_t>, NativePcSaveBlockCount> Blocks;
    bool operator==(const NativePcSaveImage&) const = default;
};

struct NativePcSaveReferenceField {
    NativePcSaveBlock Block = NativePcSaveBlock::SimpleVariables;
    std::uint32_t Offset = 0;
    bool NullAllowed = false;
};

class NativePcSaveCodec {
public:
    static NativePcSaveStatus Encode(const NativePcSaveImage&, std::vector<std::uint8_t>&,
        std::string& error);
    static NativePcSaveStatus Decode(std::span<const std::uint8_t>, const NativePcSaveLayout&,
        NativePcSaveImage&, std::string& error);
    static NativePcSaveStatus RepairReferences(NativePcSaveImage&,
        std::span<const NativePcSaveReferenceField>,
        std::span<const std::pair<std::int32_t, std::int32_t>>, std::string& error);
    static NativePcSaveLayout LayoutOf(const NativePcSaveImage&);
    static std::uint32_t Checksum(std::span<const std::uint8_t>) noexcept;
    static const std::array<const char*, NativePcSaveBlockCount>& BlockNames() noexcept;
};
