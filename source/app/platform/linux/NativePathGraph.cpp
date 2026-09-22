#include "NativePathGraph.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>
#include <unordered_map>

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
std::uint16_t Half(const std::uint8_t* p) {
    return std::uint16_t(p[0]) | std::uint16_t(p[1]) << 8;
}
std::int16_t SignedHalf(const std::uint8_t* p) { return static_cast<std::int16_t>(Half(p)); }
std::uint32_t Word(const std::uint8_t* p) {
    return std::uint32_t(p[0]) | std::uint32_t(p[1]) << 8 |
        std::uint32_t(p[2]) << 16 | std::uint32_t(p[3]) << 24;
}
std::uint64_t Hash(std::span<const std::uint8_t> bytes) {
    std::uint64_t value = 1469598103934665603ull;
    for (const auto byte : bytes) value = (value ^ byte) * 1099511628211ull;
    return value;
}
bool ReadAll(const std::string& path, std::vector<std::uint8_t>& bytes, std::string& error) {
    void* file = nullptr;
    if (OS_FileOpen(FILE_DATA_AREA_DEFAULT, &file, path.c_str(), FILE_ACCESS_READ) != 0 || !file) {
        error = "cannot open path graph " + path;
        return false;
    }
    const auto size = OS_FileSize(file);
    if (size < 20 || size > 64 * 1024 * 1024) {
        OS_FileClose(file);
        error = "path graph file size is invalid";
        return false;
    }
    bytes.resize(std::size_t(size));
    const bool ok = OS_FileRead(file, bytes.data(), int32(bytes.size())) == 0;
    OS_FileClose(file);
    if (!ok) error = "path graph read failed";
    return ok;
}
std::uint32_t Key(NativePathAddress address) {
    return std::uint32_t(address.Area) << 16u | address.Node;
}
NativePathAddress Address(std::uint32_t key) {
    return {std::uint16_t(key >> 16u), std::uint16_t(key)};
}
}

bool NativePathGraph::LoadBeforeWorker(const char* gameDir, std::string& error) {
    if (!gameDir || !*gameDir) { error = "path graph requires game root"; return false; }
    OS_SetFilePathOffset(gameDir);
    std::array<Area, 64> next;
    for (std::uint32_t areaId = 0; areaId < next.size(); ++areaId) {
        std::vector<std::uint8_t> bytes;
        if (!ReadAll("data/Paths/NODES" + std::to_string(areaId) + ".DAT", bytes, error)) return false;
        auto& area = next[areaId];
        area.Metadata.Area = std::uint8_t(areaId);
        area.Metadata.Nodes = Word(bytes.data());
        area.Metadata.VehicleNodes = Word(bytes.data() + 4);
        area.Metadata.PedNodes = Word(bytes.data() + 8);
        area.Metadata.CarPathLinks = Word(bytes.data() + 12);
        area.Metadata.Addresses = Word(bytes.data() + 16);
        if (area.Metadata.Nodes != area.Metadata.VehicleNodes + area.Metadata.PedNodes) {
            error = "path graph node partition mismatch";
            return false;
        }
        const std::uint64_t extended = area.Metadata.Addresses ? area.Metadata.Addresses + 16ull * 12ull : 0;
        const std::uint64_t expected = 20ull + std::uint64_t(area.Metadata.Nodes) * 0x1Cull +
            std::uint64_t(area.Metadata.CarPathLinks) * 0xEull + extended * 4ull +
            std::uint64_t(area.Metadata.Addresses) * 2ull + extended + extended;
        if (expected != bytes.size()) { error = "path graph payload size mismatch"; return false; }
        area.Metadata.Bytes = bytes.size();
        area.Metadata.Fingerprint = Hash(bytes);
        area.Nodes.reserve(area.Metadata.Nodes);
        const std::size_t nodesOffset = 20;
        for (std::uint32_t nodeId = 0; nodeId < area.Metadata.Nodes; ++nodeId) {
            const auto* p = bytes.data() + nodesOffset + std::size_t(nodeId) * 0x1C;
            NativePathGraphNode node;
            node.Address = {std::uint16_t(areaId), std::uint16_t(nodeId)};
            node.Position = {float(SignedHalf(p + 8)) / 8.0f, float(SignedHalf(p + 10)) / 8.0f,
                float(SignedHalf(p + 12)) / 8.0f};
            node.BaseLink = Half(p + 16);
            node.Links = p[24] & 0x0Fu;
            node.Water = (p[24] & 0x80u) != 0;
            node.SwitchedOff = (p[24] & 0x20u) != 0;
            node.Vehicle = nodeId < area.Metadata.VehicleNodes;
            area.Nodes.push_back(node);
        }
        const std::size_t addressOffset = nodesOffset + std::size_t(area.Metadata.Nodes) * 0x1C +
            std::size_t(area.Metadata.CarPathLinks) * 0xE;
        area.Links.reserve(std::size_t(extended));
        for (std::size_t i = 0; i < extended; ++i) {
            const auto* p = bytes.data() + addressOffset + i * 4;
            area.Links.push_back({Half(p), Half(p + 2)});
        }
        const std::size_t lengthsOffset = addressOffset + std::size_t(extended) * 4 +
            std::size_t(area.Metadata.Addresses) * 2;
        area.Lengths.assign(bytes.begin() + std::ptrdiff_t(lengthsOffset),
            bytes.begin() + std::ptrdiff_t(lengthsOffset + extended));
        for (const auto& node : area.Nodes) {
            if (std::size_t(node.BaseLink) + node.Links > area.Links.size()) {
                error = "path graph node links exceed payload";
                return false;
            }
        }
    }
    m_Areas = std::move(next);
    m_Generation = 0;
    m_Loaded = true;
    error.clear();
    return true;
}

NativePathGraphStatus NativePathGraph::Adopt(std::uint64_t generation,
    std::span<const NativePathAreaResidency> areas, std::string& error) {
    if (!m_Loaded) { error = "path graph is not loaded"; return NativePathGraphStatus::NotLoaded; }
    if (!generation || generation <= m_Generation || areas.empty()) {
        error = "path graph generation or area set is stale";
        return NativePathGraphStatus::StaleGeneration;
    }
    std::array<bool, 64> active{};
    for (const auto& requested : areas) {
        if (requested.Area >= m_Areas.size() || active[requested.Area] ||
            m_Areas[requested.Area].Metadata != requested) {
            error = "path graph residency identity mismatch";
            return NativePathGraphStatus::InvalidInput;
        }
        active[requested.Area] = true;
    }
    for (std::size_t i = 0; i < m_Areas.size(); ++i) m_Areas[i].Active = active[i];
    m_Generation = generation;
    error.clear();
    return NativePathGraphStatus::Ok;
}

const NativePathGraphNode* NativePathGraph::Resolve(NativePathAddress address) const noexcept {
    if (address.Area >= m_Areas.size() || !m_Areas[address.Area].Active ||
        address.Node >= m_Areas[address.Area].Nodes.size()) return nullptr;
    return &m_Areas[address.Area].Nodes[address.Node];
}

std::span<const NativePathGraphNode> NativePathGraph::Nodes(std::uint8_t area) const {
    return area < m_Areas.size() && m_Areas[area].Active
        ? std::span<const NativePathGraphNode>{m_Areas[area].Nodes} : std::span<const NativePathGraphNode>{};
}

const NativePathAreaResidency* NativePathGraph::Metadata(std::uint8_t area) const noexcept {
    return m_Loaded && area < m_Areas.size() ? &m_Areas[area].Metadata : nullptr;
}

NativePathGraphStatus NativePathGraph::Search(NativePathAddress start, NativePathAddress end,
    bool vehicle, NativePathRoute& out, std::string& error) const {
    const auto* first = Resolve(start);
    const auto* last = Resolve(end);
    if (!first || !last || first->Vehicle != vehicle || last->Vehicle != vehicle || first->SwitchedOff || last->SwitchedOff) {
        error = "path graph endpoints are unavailable or wrong type";
        return NativePathGraphStatus::Unavailable;
    }
    using QueueEntry = std::pair<std::uint32_t, std::uint32_t>;
    std::priority_queue<QueueEntry, std::vector<QueueEntry>, std::greater<QueueEntry>> queue;
    std::unordered_map<std::uint32_t, std::uint32_t> distance, parent;
    const auto startKey = Key(start), endKey = Key(end);
    distance[startKey] = 0;
    queue.push({0, startKey});
    while (!queue.empty()) {
        const auto [cost, key] = queue.top();
        queue.pop();
        if (distance[key] != cost) continue;
        if (key == endKey) break;
        const auto address = Address(key);
        const auto* node = Resolve(address);
        if (!node) continue;
        const auto& area = m_Areas[address.Area];
        for (std::uint8_t link = 0; link < node->Links; ++link) {
            const auto index = std::size_t(node->BaseLink) + link;
            const auto linkedAddress = area.Links[index];
            const auto* linked = Resolve(linkedAddress);
            if (!linked || linked->Vehicle != vehicle || linked->SwitchedOff || linked->Water != node->Water) continue;
            const auto weight = std::uint32_t(std::max<std::uint8_t>(area.Lengths[index], 1));
            if (cost > std::numeric_limits<std::uint32_t>::max() - weight) {
                error = "path graph route distance overflow";
                return NativePathGraphStatus::Overflow;
            }
            const auto nextCost = cost + weight, linkedKey = Key(linkedAddress);
            const auto old = distance.find(linkedKey);
            if (old == distance.end() || nextCost < old->second) {
                distance[linkedKey] = nextCost;
                parent[linkedKey] = key;
                queue.push({nextCost, linkedKey});
            }
        }
    }
    const auto found = distance.find(endKey);
    if (found == distance.end()) { error = "path graph route is unavailable"; return NativePathGraphStatus::NoRoute; }
    NativePathRoute next;
    next.Generation = m_Generation;
    next.Start = start;
    next.End = end;
    next.Distance = found->second;
    for (auto key = endKey;; key = parent.at(key)) {
        next.Nodes.push_back(Address(key));
        if (key == startKey) break;
    }
    std::reverse(next.Nodes.begin(), next.Nodes.end());
    out = std::move(next);
    error.clear();
    return NativePathGraphStatus::Ok;
}
