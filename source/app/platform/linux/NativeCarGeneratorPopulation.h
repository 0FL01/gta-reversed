#pragma once

#include "app/platform/linux/NativeCarGenerators.h"
#include "app/platform/linux/NativeSourceRng.h"
#include "app/platform/linux/NativeZonePopulation.h"
#include "app/platform/linux/ZoneInfo.h"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

struct NativeCarPopulationSelection {
    std::string Zone;
    std::uint8_t PopulationType = 0;
    std::vector<std::int32_t> AppropriateLoadedCars;
    std::int32_t ModelId = -1;
    std::uint32_t WeightSum = 0;
    std::uint8_t Draws = 0;
};

struct NativeCarLoadedModel {
    std::int32_t ModelId = -1;
    std::uint32_t RefCount = 0;
    bool ScriptSuppressed = false;
    bool ScriptBlocked = false;
    bool StreamingPhaseOut = false;
};

// Read-only source membership/weight calculation. The caller supplies the
// streaming owner's *ordered* loaded roster and its one shared RNG reference.
// No independent model loader, source RNG seed, or fallback car is created.
class NativeCarGeneratorPopulation {
public:
    bool LoadBeforeWorker(const char* gameDir,
        std::span<const NativeCarGeneratorModelDefinition> definitions, std::string& error);
    bool Select(NativeScriptPosition player, std::uint8_t hour, bool weekend,
        std::span<const NativeZonePopulationEntry> zoneStates,
        std::span<const NativeCarLoadedModel> orderedLoadedModels, NativeSourceRngRef rng,
        NativeCarPopulationSelection& out, std::string& error) const;

    std::size_t CarGroups() const { return m_Groups.size(); }
    std::size_t CycleRows() const { return m_Rows.size(); }
private:
    struct Model {
        std::int32_t Id = -1;
        std::string Name;
        std::string Class;
        std::uint32_t Frequency = 0;
    };
    std::vector<Model> m_Models;
    std::vector<std::vector<std::int32_t>> m_Groups;
    std::vector<std::array<std::uint8_t, 24>> m_Rows;
    ZoneData m_Zones;
    bool m_Loaded = false;
};
