#pragma once

#include "NativeLiveEntityBounds.h"

#include <array>
#include <compare>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

struct NativeDynamicEntityKey {
    NativeLiveEntityKind Kind{};
    std::int32_t Reference = -1;
    auto operator<=>(const NativeDynamicEntityKey&) const = default;
};

enum class NativeDynamicSectorStatus {
    Ok, InvalidInput, DuplicateEntity, StaleEntity, CapacityExceeded, Overflow,
};

struct NativeDynamicSectorQuery {
    NativeLiveBlockageResult Blockage;
    std::array<NativeDynamicEntityKey, 8> Selected{};
    std::size_t Count{};
    std::size_t ScannedEntities{};
    std::uint64_t Revision{};
    bool operator==(const NativeDynamicSectorQuery&) const = default;
};

// Replays regular (non-BigBuilding) CPhysical::Add/Remove and CWorld's vehicle/ped
// repeat lists from explicit owner events with source-effective matrices and COL bounds. A snapshot
// sorted by pool slot is NOT a substitute for these events. This owner does not
// generate population or certify its completeness. Its 500-link limit is only
// the shared source allocator's upper bound; unrelated object allocations are
// not owned here and cannot be declared free by this replay.
class NativeDynamicSectorLists {
public:
    NativeDynamicSectorStatus Add(const NativeLiveEntityBound&, std::string& error);
    NativeDynamicSectorStatus Remove(NativeDynamicEntityKey, std::string& error);
    // Source RemoveAndAdd also changes order when the bound/sector is unchanged.
    NativeDynamicSectorStatus Reinsert(const NativeLiveEntityBound&, std::string& error);
    NativeDynamicSectorStatus Query(NativeCollisionVector position,
        const NativeLiveModelBounds& candidate, NativeDynamicSectorQuery& out,
        std::string& error) const;

    const NativeLiveEntityBound* Resolve(NativeDynamicEntityKey) const noexcept;
    std::size_t Size() const noexcept { return m_Entries.size(); }
    std::size_t LinkCount() const noexcept { return m_LinkCount; }
    std::uint64_t Revision() const noexcept { return m_Revision; }
    static constexpr bool SourcePopulationComplete = false;

private:
    struct Sector {
        std::vector<NativeDynamicEntityKey> Vehicles;
        std::vector<NativeDynamicEntityKey> Peds;
    };
    struct Entry {
        NativeLiveEntityBound Bound;
        std::array<int, 4> Sectors{}; // minX, minY, maxX, maxY; physical path does not clamp
    };

    void AddLinks(NativeDynamicEntityKey, const Entry&);
    void RemoveLinks(NativeDynamicEntityKey, const Entry&);
    NativeDynamicSectorStatus Insert(const NativeLiveEntityBound&, bool replace,
        std::string& error);

    std::map<NativeDynamicEntityKey, Entry> m_Entries;
    std::array<Sector, 16 * 16> m_Sectors;
    std::size_t m_LinkCount{}; // bounded by source EntryInfoNode pool's 500 slots
    std::uint64_t m_Revision{};
};
