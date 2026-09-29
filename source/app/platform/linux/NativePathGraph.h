#pragma once

#include "NativeWorldResidency.h"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

struct NativePathAddress {
    std::uint16_t Area = 0xffffu;
    std::uint16_t Node = 0xffffu;
    bool operator==(const NativePathAddress&) const = default;
};

struct NativePathGraphNode {
    NativePathAddress Address;
    NativeCollisionVector Position{};
    std::uint16_t BaseLink = 0;
    std::uint8_t Links = 0;
    bool Vehicle = false;
    bool Water = false;
    bool SwitchedOff = false;
    std::uint8_t Width = 0; // Source byte22, fixed-point width /16.
    std::uint8_t PedDensity = 0; // Source low nibble of byte26.
};

struct NativePathGraphLink {
    NativePathAddress Address;
    bool CrossesRoad = false; // Source CPathIntersectionInfo bit0.
};

struct NativePathRoute {
    std::uint64_t Generation = 0;
    NativePathAddress Start, End;
    std::uint32_t Distance = 0;
    std::vector<NativePathAddress> Nodes;
};

enum class NativePathGraphStatus : std::uint8_t {
    Ok,
    NotLoaded,
    InvalidInput,
    StaleGeneration,
    Unavailable,
    NoRoute,
    Overflow,
};

class NativePathGraph {
public:
    bool LoadBeforeWorker(const char* gameDir, std::string& error);
    NativePathGraphStatus Adopt(std::uint64_t generation,
        std::span<const NativePathAreaResidency> areas, std::string& error);
    NativePathGraphStatus Search(NativePathAddress start, NativePathAddress end,
        bool vehicle, NativePathRoute& out, std::string& error) const;
    const NativePathGraphNode* Resolve(NativePathAddress) const noexcept;
    bool Link(NativePathAddress origin, std::uint8_t offset, NativePathGraphLink& out) const noexcept;
    std::span<const NativePathGraphNode> Nodes(std::uint8_t area) const;
    const NativePathAreaResidency* Metadata(std::uint8_t area) const noexcept;
    std::uint64_t Generation() const noexcept { return m_Generation; }
    static constexpr bool DeterministicOwnership = true;

private:
    struct Area {
        NativePathAreaResidency Metadata;
        std::vector<NativePathGraphNode> Nodes;
        std::vector<NativePathAddress> Links;
        std::vector<std::uint8_t> Lengths;
        std::vector<std::uint8_t> Intersections;
        bool Active = false;
    };
    std::array<Area, 64> m_Areas;
    std::uint64_t m_Generation = 0;
    bool m_Loaded = false;
};
