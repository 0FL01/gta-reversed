#include "app/platform/linux/NativeWorldResidency.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

using int8 = int8_t;
using int16 = int16_t;
using int32 = int32_t;
using int64 = int64_t;
using uint8 = uint8_t;
using uint16 = uint16_t;
using uint32 = uint32_t;
using uint64 = uint64_t;
#ifndef __stdcall
#define __stdcall
#endif
#include "oswrapper/oswrapper.h"

namespace {
std::uint32_t Word(const std::uint8_t* data) {
    return std::uint32_t(data[0]) | std::uint32_t(data[1]) << 8 |
        std::uint32_t(data[2]) << 16 | std::uint32_t(data[3]) << 24;
}

std::uint64_t Hash(std::span<const std::uint8_t> bytes) {
    std::uint64_t value = 1469598103934665603ull;
    for (const auto byte : bytes) value = (value ^ byte) * 1099511628211ull;
    return value;
}

bool ReadAll(const std::string& path, std::vector<std::uint8_t>& bytes, std::string& error) {
    void* file = nullptr;
    if (OS_FileOpen(FILE_DATA_AREA_DEFAULT, &file, path.c_str(), FILE_ACCESS_READ) != 0 || !file) {
        error = "cannot open path residency " + path;
        return false;
    }
    const auto size = OS_FileSize(file);
    if (size < 20 || size > 64 * 1024 * 1024) {
        OS_FileClose(file);
        error = "path residency file size is invalid";
        return false;
    }
    bytes.resize(std::size_t(size));
    const bool read = OS_FileRead(file, bytes.data(), int32(bytes.size())) == 0;
    OS_FileClose(file);
    if (!read) error = "path residency read failed";
    return read;
}

bool Finite(const NativeCollisionVector& value) {
    return std::isfinite(value[0]) && std::isfinite(value[1]) && std::isfinite(value[2]);
}

std::uint32_t Region(float value) {
    const float raw = std::clamp((value + 3000.0f) / 750.0f, 0.0f, 7.0f);
    return static_cast<std::uint32_t>(raw);
}

bool SameIdentities(std::span<const NativePlacementIdentity> first,
    std::span<const NativePlacementIdentity> second) {
    return first.size() == second.size() && std::equal(first.begin(), first.end(), second.begin());
}
}

bool NativePathResidencyCatalog::LoadBeforeWorker(const char* gameDir, std::string& error) {
    if (!gameDir || !*gameDir) {
        error = "path residency requires game root";
        return false;
    }
    OS_SetFilePathOffset(gameDir);
    std::vector<NativePathAreaResidency> next;
    next.reserve(64);
    for (std::uint32_t area = 0; area < 64; ++area) {
        std::vector<std::uint8_t> bytes;
        if (!ReadAll("data/Paths/NODES" + std::to_string(area) + ".DAT", bytes, error)) return false;
        NativePathAreaResidency metadata;
        metadata.Area = static_cast<std::uint8_t>(area);
        metadata.Nodes = Word(bytes.data());
        metadata.VehicleNodes = Word(bytes.data() + 4);
        metadata.PedNodes = Word(bytes.data() + 8);
        metadata.CarPathLinks = Word(bytes.data() + 12);
        metadata.Addresses = Word(bytes.data() + 16);
        if (metadata.Nodes != metadata.VehicleNodes + metadata.PedNodes) {
            error = "path residency node partition mismatch";
            return false;
        }
        const std::uint64_t extended = metadata.Addresses ? metadata.Addresses + 16ull * 12ull : 0;
        const std::uint64_t expected = 20ull + std::uint64_t(metadata.Nodes) * 0x1Cull +
            std::uint64_t(metadata.CarPathLinks) * 0xEull + extended * 4ull +
            std::uint64_t(metadata.Addresses) * 2ull + extended + extended;
        if (expected != bytes.size()) {
            error = "path residency payload size mismatch";
            return false;
        }
        metadata.Bytes = bytes.size();
        metadata.Fingerprint = Hash(bytes);
        next.push_back(metadata);
    }
    m_Areas = std::move(next);
    error.clear();
    return true;
}

bool NativePathResidencyCatalog::Select(float x, float y, float radius,
    std::vector<NativePathAreaResidency>& out, std::string& error) const {
    if (m_Areas.size() != 64 || !std::isfinite(x) || !std::isfinite(y) ||
        !std::isfinite(radius) || radius <= 0.0f) {
        error = "path residency selection is invalid";
        return false;
    }
    const auto minX = Region(x - radius), maxX = Region(x + radius);
    const auto minY = Region(y - radius), maxY = Region(y + radius);
    std::vector<NativePathAreaResidency> next;
    for (auto areaX = minX; areaX <= maxX; ++areaX) {
        for (auto areaY = minY; areaY <= maxY; ++areaY) {
            next.push_back(m_Areas[areaY * 8u + areaX]);
        }
    }
    out = std::move(next);
    error.clear();
    return true;
}

bool NativeWorldResidency::Prepare(const NativeLodCatalog& catalog,
    const NativeCollisionAssets& assets, const NativeCollisionPopulation& population,
    const NativePathResidencyCatalog& paths, std::uint64_t generation, float x, float y,
    float radius, int area, std::span<const NativePlacementIdentity> rendered,
    NativeWorldResidencyCandidate& out, std::string& error) {
    if (!generation) {
        error = "world residency generation must be nonzero";
        return false;
    }
    NativeWorldResidencyCandidate next;
    next.Generation = generation;
    next.Area = area;
    next.X = x;
    next.Y = y;
    next.Radius = radius;
    if (!catalog.SelectResidency(x, y, radius, area, next.Selection, error)) return false;
    std::vector<NativePlacementIdentity> expected = next.Selection.Visible;
    expected.insert(expected.end(), next.Selection.HiddenTargets.begin(), next.Selection.HiddenTargets.end());
    if (!SameIdentities(expected, rendered)) {
        error = "rendered residency identity/order mismatch";
        return false;
    }
    NativeCollisionSnapshot collision;
    if (!assets.SnapshotSelected(population, expected, collision, error)) return false;
    next.Collision = std::make_shared<const NativeCollisionSnapshot>(std::move(collision));
    if (!paths.Select(x, y, 350.0f, next.Paths, error)) return false;
    next.Rendered.assign(rendered.begin(), rendered.end());
    out = std::move(next);
    error.clear();
    return true;
}

bool NativeWorldResidency::SpawnDynamic(std::int32_t modelId, NativeCollisionVector position,
    std::uint8_t area, NativeDynamicWorldRef& out, std::string& error) {
    if (modelId < 0 || !Finite(position)) {
        error = "dynamic world spawn is invalid";
        return false;
    }
    for (std::size_t index = 0; index < m_Dynamic.size(); ++index) {
        auto& slot = m_Dynamic[index];
        if (slot.Alive) continue;
        slot.Generation = std::uint8_t((slot.Generation % 0x7Fu) + 1u);
        slot.Alive = true;
        slot.State = {{std::uint32_t((index << 8u) | slot.Generation)}, modelId, position, area, true};
        out = slot.State.Reference;
        error.clear();
        return true;
    }
    error = "dynamic world capacity exceeded";
    return false;
}

const NativeDynamicWorldEntity* NativeWorldResidency::Resolve(NativeDynamicWorldRef reference) const noexcept {
    const auto index = std::size_t(reference.Value >> 8u);
    const auto generation = std::uint8_t(reference.Value & 0xFFu);
    if (index >= m_Dynamic.size()) return nullptr;
    const auto& slot = m_Dynamic[index];
    return slot.Alive && slot.Generation == generation ? &slot.State : nullptr;
}

bool NativeWorldResidency::MoveDynamic(NativeDynamicWorldRef reference,
    NativeCollisionVector position, std::uint8_t area, std::string& error) {
    if (!Finite(position)) {
        error = "dynamic world move is invalid";
        return false;
    }
    const auto index = std::size_t(reference.Value >> 8u);
    if (!Resolve(reference)) {
        error = "dynamic world reference is stale";
        return false;
    }
    m_Dynamic[index].State.Position = position;
    m_Dynamic[index].State.Area = area;
    error.clear();
    return true;
}

bool NativeWorldResidency::RemoveDynamic(NativeDynamicWorldRef reference, std::string& error) {
    const auto index = std::size_t(reference.Value >> 8u);
    if (!Resolve(reference)) {
        error = "dynamic world reference is stale";
        return false;
    }
    m_Dynamic[index].Alive = false;
    m_Dynamic[index].State.InWorld = false;
    error.clear();
    return true;
}

bool NativeWorldResidency::Adopt(const NativeWorldResidencyCandidate& candidate, std::string& error) try {
    if (!candidate.Generation || (m_Published && candidate.Generation <= m_Published->Generation) ||
        !candidate.Collision || candidate.Rendered.empty() || candidate.Paths.empty()) {
        error = "world residency candidate is stale or incomplete";
        return false;
    }
    std::vector<NativePlacementIdentity> expected = candidate.Selection.Visible;
    expected.insert(expected.end(), candidate.Selection.HiddenTargets.begin(), candidate.Selection.HiddenTargets.end());
    if (!SameIdentities(expected, candidate.Rendered)) {
        error = "world residency candidate render identity mismatch";
        return false;
    }
    auto next = std::make_shared<NativeWorldResidencySnapshot>();
    next->Generation = candidate.Generation;
    next->Revision = m_Revision + 1;
    next->Area = candidate.Area;
    next->X = candidate.X;
    next->Y = candidate.Y;
    next->Radius = candidate.Radius;
    next->Visible = candidate.Selection.Visible;
    next->HiddenTargets = candidate.Selection.HiddenTargets;
    next->Rendered = candidate.Rendered;
    next->Collision = candidate.Collision;
    next->Paths = candidate.Paths;
    for (const auto& instance : candidate.Collision->Instances) {
        const auto identity = NativePlacementIdentity::From(instance.Placement);
        if (std::find(expected.begin(), expected.end(), identity) == expected.end()) {
            error = "world residency collision identity is outside render selection";
            return false;
        }
        if (std::find(next->CollisionIdentities.begin(), next->CollisionIdentities.end(), identity) !=
            next->CollisionIdentities.end()) {
            error = "world residency collision identity is duplicated";
            return false;
        }
        next->CollisionIdentities.push_back(identity);
    }
    for (const auto& slot : m_Dynamic) if (slot.Alive) next->Dynamic.push_back(slot.State);
    next->PairedRenderCollision = true;
    next->CompletePlacementIdentity = true;
    next->PathSearchAuthority = false;
    m_Revision = next->Revision;
    m_Published = std::move(next);
    error.clear();
    return true;
} catch (const std::exception& exception) {
    error = exception.what();
    return false;
}
