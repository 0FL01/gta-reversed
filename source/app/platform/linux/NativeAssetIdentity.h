#pragma once

#include <compare>
#include <cstdint>
#include <string>
#include <vector>

namespace NativeAssetIdentity {

struct ArchiveMember {
    std::string archive;
    std::string member;

    bool operator==(const ArchiveMember&) const = default;
    auto operator<=>(const ArchiveMember&) const = default;
};

struct Model {
    ArchiveMember dff;
    int modelId{-1};

    bool operator==(const Model&) const = default;
    auto operator<=>(const Model&) const = default;
};

struct Geometry {
    Model model;
    int index{-1};

    bool operator==(const Geometry&) const = default;
    auto operator<=>(const Geometry&) const = default;
};

struct Material {
    Geometry geometry;
    int slot{-1};

    bool operator==(const Material&) const = default;
    auto operator<=>(const Material&) const = default;
};

struct Texture {
    // First to last is the actual authored dictionary search order.
    std::vector<ArchiveMember> lineage;
    ArchiveMember owner;
    std::string name;
    // The authored sampler filter is identity-bearing to prevent aliasing.
    uint32_t filter{};

    bool operator==(const Texture&) const = default;
    auto operator<=>(const Texture&) const = default;
};

} // namespace NativeAssetIdentity
