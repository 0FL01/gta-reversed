#include "NativeDynamicSectorLists.h"
#include "NativeWorldGround.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <limits>
#include <set>

namespace {
bool Finite(NativeCollisionVector v) {
    return std::ranges::all_of(v, [](float x) { return std::isfinite(x); });
}

bool ValidBounds(const NativeLiveModelBounds& b) {
    return b.Knowledge == NativeLiveBoundsKnowledge::SourceCol && Finite(b.Min) &&
        Finite(b.Max) && Finite(b.Center) && std::isfinite(b.Radius) && b.Radius >= 0 &&
        b.Min[0] <= b.Max[0] && b.Min[1] <= b.Max[1] && b.Min[2] <= b.Max[2];
}

bool Vehicle(NativeDynamicEntityKey key) {
    return key.Kind == NativeLiveEntityKind::Vehicle;
}

std::size_t Repeat(int x, int y) {
    // Original GetRepeatSector uses the low four bits, including negative grid
    // coordinates. abs(coord)%16 is NOT equivalent for those coordinates.
    return (static_cast<std::uint32_t>(y) & 15u) * 16u +
        (static_cast<std::uint32_t>(x) & 15u);
}

bool Grid(float value, int& out) {
    // Retail CPhysical::Add (0x555980): double /50 +60, then a binary32
    // spill BEFORE floor. An intermediate binary32 divide changes boundaries.
    const float coordinate = static_cast<float>(static_cast<double>(value) / 50.0 + 60.0);
    const float grid = std::floor(coordinate);
    if (!std::isfinite(grid) || static_cast<double>(grid) < std::numeric_limits<int>::min() ||
        static_cast<double>(grid) > std::numeric_limits<int>::max()) return false;
    out = static_cast<int>(grid);
    return true;
}

bool EntitySectors(const NativeLiveEntityBound& b, std::array<int, 4>& out) {
    if (b.Reference < 0 || b.ModelId < 0 ||
        (b.Kind != NativeLiveEntityKind::Player && b.Kind != NativeLiveEntityKind::Vehicle &&
         b.Kind != NativeLiveEntityKind::MissionPed) ||
        !ValidBounds(b.Model) || !Finite(b.WorldCenter) || !Finite(b.Transform.Position) ||
        !std::ranges::all_of(b.Transform.Basis, Finite)) return false;
    if (NativeWorldGround::SourcePoint({b.Transform.Position, b.Transform.Basis},
        b.Model.Center) != b.WorldCenter) return false;

    // CPhysical overrides GetBoundRect: dynamic entities use the transformed
    // COL sphere, NOT CEntity's four static box corners. Physical::Add does NOT
    // apply the base Entity::Add rectangle clamp (retail 0x555980).
    const std::array<float, 4> rect{b.WorldCenter[0] - b.Model.Radius,
        b.WorldCenter[1] - b.Model.Radius, b.WorldCenter[0] + b.Model.Radius,
        b.WorldCenter[1] + b.Model.Radius};
    if (!std::ranges::all_of(rect, [](float x) { return std::isfinite(x); })) return false;
    if (rect[0] > rect[2] || rect[1] > rect[3]) return false;
    return Grid(rect[0], out[0]) && Grid(rect[1], out[1]) &&
        Grid(rect[2], out[2]) && Grid(rect[3], out[3]);
}

std::size_t SectorLinkCount(const std::array<int, 4>& sectors) {
    const auto width = static_cast<std::int64_t>(sectors[2]) - sectors[0] + 1;
    const auto height = static_cast<std::int64_t>(sectors[3]) - sectors[1] + 1;
    if (width <= 0 || height <= 0 || width > 500 || height > 500 || width * height > 500)
        return 501; // cannot fit the source EntryInfoNode pool, even in isolation
    return static_cast<std::size_t>(width * height);
}
}

void NativeDynamicSectorLists::AddLinks(NativeDynamicEntityKey key, const Entry& entry) {
    for (int y = entry.Sectors[1]; y <= entry.Sectors[3]; ++y) {
        for (int x = entry.Sectors[0]; x <= entry.Sectors[2]; ++x) {
            auto& sector = m_Sectors[Repeat(x, y)];
            auto& list = Vehicle(key) ? sector.Vehicles : sector.Peds;
            list.insert(list.begin(), key); // CPtrListDoubleLink::AddItem
        }
    }
}

void NativeDynamicSectorLists::RemoveLinks(NativeDynamicEntityKey key, const Entry& entry) {
    for (int y = entry.Sectors[1]; y <= entry.Sectors[3]; ++y) {
        for (int x = entry.Sectors[0]; x <= entry.Sectors[2]; ++x) {
            auto& sector = m_Sectors[Repeat(x, y)];
            auto& list = Vehicle(key) ? sector.Vehicles : sector.Peds;
            const auto item = std::ranges::find(list, key);
            // Removal is from this owner's proven original membership, not
            // today's transform. Repeated-list aliases may have duplicate links.
            assert(item != list.end());
            list.erase(item);
        }
    }
}

NativeDynamicSectorStatus NativeDynamicSectorLists::Insert(const NativeLiveEntityBound& bound,
    bool replace, std::string& error) {
    std::array<int, 4> sectors{};
    if (!EntitySectors(bound, sectors)) {
        error = "dynamic entity has no finite source sector/bound identity";
        return NativeDynamicSectorStatus::InvalidInput;
    }
    const NativeDynamicEntityKey key{bound.Kind, bound.Reference};
    const auto current = m_Entries.find(key);
    if (replace && current == m_Entries.end()) {
        error = "dynamic reinsert reference is stale";
        return NativeDynamicSectorStatus::StaleEntity;
    }
    if (!replace && current != m_Entries.end()) {
        error = "dynamic entity is already linked";
        return NativeDynamicSectorStatus::DuplicateEntity;
    }
    const auto newLinks = SectorLinkCount(sectors);
    const auto oldLinks = replace ? SectorLinkCount(current->second.Sectors) : 0;
    assert(oldLinks <= m_LinkCount);
    if (newLinks > 500 || m_LinkCount - oldLinks > 500 - newLinks) {
        error = "dynamic replay exceeds source EntryInfoNode pool capacity";
        return NativeDynamicSectorStatus::CapacityExceeded;
    }
    if (!replace) {
        const auto count = std::ranges::count_if(m_Entries, [&](const auto& item) {
            return Vehicle(item.first) == Vehicle(key);
        });
        if (count >= (Vehicle(key) ? 110 : 140)) {
            error = "source dynamic pool capacity exceeded";
            return NativeDynamicSectorStatus::CapacityExceeded;
        }
    }
    if (m_Revision == std::numeric_limits<std::uint64_t>::max()) {
        error = "dynamic sector revision exhausted";
        return NativeDynamicSectorStatus::Overflow;
    }
    auto next = *this;
    if (replace) next.RemoveLinks(key, next.m_Entries.at(key));
    const Entry entry{bound, sectors};
    next.m_Entries.insert_or_assign(key, entry);
    next.AddLinks(key, entry);
    next.m_LinkCount = m_LinkCount - oldLinks + newLinks;
    ++next.m_Revision;
    m_Entries.swap(next.m_Entries);
    m_Sectors.swap(next.m_Sectors);
    m_Revision = next.m_Revision;
    m_LinkCount = next.m_LinkCount;
    error.clear();
    return NativeDynamicSectorStatus::Ok;
}

NativeDynamicSectorStatus NativeDynamicSectorLists::Add(const NativeLiveEntityBound& bound,
    std::string& error) {
    return Insert(bound, false, error);
}

NativeDynamicSectorStatus NativeDynamicSectorLists::Reinsert(const NativeLiveEntityBound& bound,
    std::string& error) {
    return Insert(bound, true, error);
}

NativeDynamicSectorStatus NativeDynamicSectorLists::Remove(NativeDynamicEntityKey key,
    std::string& error) {
    const auto entry = m_Entries.find(key);
    if (entry == m_Entries.end()) {
        error = "dynamic remove reference is stale";
        return NativeDynamicSectorStatus::StaleEntity;
    }
    if (m_Revision == std::numeric_limits<std::uint64_t>::max()) {
        error = "dynamic sector revision exhausted";
        return NativeDynamicSectorStatus::Overflow;
    }
    // All validation precedes no-allocation removal.
    RemoveLinks(key, entry->second);
    m_LinkCount -= SectorLinkCount(entry->second.Sectors);
    m_Entries.erase(entry);
    ++m_Revision;
    error.clear();
    return NativeDynamicSectorStatus::Ok;
}

const NativeLiveEntityBound* NativeDynamicSectorLists::Resolve(NativeDynamicEntityKey key) const noexcept {
    const auto entry = m_Entries.find(key);
    return entry == m_Entries.end() ? nullptr : &entry->second.Bound;
}

NativeDynamicSectorStatus NativeDynamicSectorLists::Query(NativeCollisionVector position,
    const NativeLiveModelBounds& candidate, NativeDynamicSectorQuery& out, std::string& error) const {
    const bool sourceNull = candidate.Knowledge == NativeLiveBoundsKnowledge::ProvenSourceNull;
    const float radius = sourceNull ? 2.0f : candidate.Radius;
    std::array<int, 4> grid{};
    if (!Finite(position) || (!sourceNull && !ValidBounds(candidate)) ||
        !Grid(position[0] - radius, grid[0]) || !Grid(position[1] - radius, grid[1]) ||
        !Grid(position[0] + radius, grid[2]) || !Grid(position[1] + radius, grid[3])) {
        error = "dynamic query requires source-valid finite position and candidate bounds";
        return NativeDynamicSectorStatus::InvalidInput;
    }
    NativeDynamicSectorQuery next;
    next.Revision = m_Revision;
    next.Blockage.StoredPosition = position;
    std::set<NativeDynamicEntityKey> scanned; // source scan-code dedup, local to this query
    for (int yi = 0; yi < 16 && static_cast<std::int64_t>(grid[1]) + yi <= grid[3]; ++yi) {
        for (int xi = 0; xi < 16 && static_cast<std::int64_t>(grid[0]) + xi <= grid[2]; ++xi) {
            const auto& sector = m_Sectors[Repeat(grid[0] + xi, grid[1] + yi)];
            const auto visit = [&](const auto& list) {
                for (const auto key : list) {
                    if (next.Count == next.Selected.size()) break; // BEFORE scan marking
                    if (!scanned.insert(key).second) continue;
                    const auto& bound = m_Entries.at(key).Bound;
                    const float dx = bound.WorldCenter[0] - position[0];
                    const float dy = bound.WorldCenter[1] - position[1];
                    const float distance = std::sqrt(dx * dx + dy * dy);
                    if (distance >= bound.Model.Radius + radius) continue;
                    next.Selected[next.Count++] = key;
                }
            };
            visit(sector.Vehicles); // source always visits vehicles before peds
            visit(sector.Peds);
        }
    }
    // Repeat-index periods >16 only revisit fully scan-marked lists; truncating
    // those repetitions does not change source candidate order or the capped8.
    next.ScannedEntities = scanned.size();
    next.Blockage.XYCandidates = next.Count;
    next.Blockage.Status = NativeLiveBlockageStatus::Clear;
    const float lower = position[2] + (sourceNull ? -1.0f : candidate.Min[2]);
    const float upper = position[2] + (sourceNull ? 1.0f : candidate.Max[2]);
    for (std::size_t i = 0; i < next.Count; ++i) {
        const auto& bound = m_Entries.at(next.Selected[i]).Bound;
        ++next.Blockage.ZTests;
        if (bound.Transform.Position[2] + bound.Model.Max[2] + 1.0f > lower &&
            bound.Transform.Position[2] + bound.Model.Min[2] - 1.0f < upper) {
            next.Blockage.Status = NativeLiveBlockageStatus::Blocked;
            break;
        }
    }
    out = next;
    error.clear();
    return NativeDynamicSectorStatus::Ok;
}
