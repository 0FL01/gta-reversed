// Source info.zon population-setting owner; population spawning is separate.
#pragma once

#include "app/platform/linux/NativeScriptSession.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

struct NativeZonePopulationEntry {
    std::string Label;
    std::uint8_t PopulationType = 5; // RESIDENTIAL_AVERAGE
    std::uint8_t Races = 15;
    std::uint8_t DealerStrength = 0;
    std::array<std::uint8_t,10> GangStrength{};
    bool NoCops = false;
    bool operator==(const NativeZonePopulationEntry&) const = default;
};

class NativeZonePopulation {
public:
    static constexpr bool RuntimeConsumer = false;
    bool LoadBeforeWorker(const char* gameDir, std::string& error);
    NativeScriptServiceResult Reset();
    NativeScriptServiceResult SetType(const NativeScriptZonePopulationRequest&);
    NativeScriptServiceResult SetRaces(const NativeScriptZonePopulationRequest&);
    NativeScriptServiceResult SetDealer(const NativeScriptZonePopulationRequest&);
    NativeScriptServiceResult SetGang(const NativeScriptZoneGangRequest&);
    NativeScriptServiceResult SetNoCops(const NativeScriptZonePopulationRequest&);
    std::span<const NativeZonePopulationEntry> Entries() const { return m_Entries; }
    std::uint64_t Revision() const { return m_Revision; }
private:
    NativeZonePopulationEntry* Find(const std::array<char,8>& name);
    std::vector<NativeZonePopulationEntry> m_Entries;
    std::uint64_t m_Revision = 0;
    bool m_Loaded = false;
};
