#include "app/platform/linux/NativeCarGeneratorPopulation.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string_view>

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
std::string Lower(std::string_view input) {
    std::string result(input);
    for (char& c : result) c = char(std::tolower(static_cast<unsigned char>(c)));
    return result;
}

bool ReadText(const char* path, std::string& text, std::string& error) {
    void* file = nullptr;
    if (OS_FileOpen(FILE_DATA_AREA_DEFAULT, &file, path, FILE_ACCESS_READ) != 0 || !file) {
        error = std::string("cannot open ") + path;
        return false;
    }
    const auto size = OS_FileSize(file);
    if (size <= 0 || size > 1024 * 1024) {
        OS_FileClose(file);
        error = std::string("invalid size of ") + path;
        return false;
    }
    std::string candidate(std::size_t(size), '\0');
    const bool read = OS_FileRead(file, candidate.data(), int32(candidate.size())) == 0;
    OS_FileClose(file);
    if (!read) {
        error = std::string("cannot read ") + path;
        return false;
    }
    text = std::move(candidate);
    return true;
}

std::string_view DataPart(std::string_view line) {
    const auto hash = line.find('#'), slash = line.find("//");
    return line.substr(0, std::min(hash, slash));
}

bool NormalFamily(std::string_view klass) {
    return klass == "normal" || klass == "poorfamily" || klass == "richfamily" ||
        klass == "motorbike";
}
} // namespace

bool NativeCarGeneratorPopulation::LoadBeforeWorker(const char* gameDir,
    std::span<const NativeCarGeneratorModelDefinition> definitions, std::string& error) try {
    if (m_Loaded || !gameDir || definitions.empty()) throw std::runtime_error("invalid population load");
    OS_SetFilePathOffset(gameDir);
    std::string groupsText, cycleText;
    if (!ReadText("data/cargrp.dat", groupsText, error) ||
        !ReadText("data/popcycle.dat", cycleText, error)) return false;
    ZoneData zones;
    char detail[256]{};
    if (!ZoneInfo_Load(gameDir, zones, detail, sizeof(detail))) throw std::runtime_error(detail);

    std::vector<Model> models;
    models.reserve(definitions.size());
    for (const auto& d : definitions) {
        if (d.ModelId < 400 || d.ModelId > 611 || d.ModelName.empty())
            throw std::runtime_error("invalid vehicle definition identity");
        if (std::ranges::find(models, Lower(d.ModelName), &Model::Name) != models.end())
            throw std::runtime_error("duplicate vehicle model name");
        models.push_back({d.ModelId, Lower(d.ModelName), Lower(d.ClassName), d.Frequency});
    }
    std::vector<std::vector<std::int32_t>> groups;
    std::istringstream groupStream(groupsText);
    std::string line;
    while (std::getline(groupStream, line)) {
        for (char& c : line) if (c == ',') c = ' ';
        auto data = DataPart(line);
        std::istringstream fields{std::string(data)};
        std::string name;
        std::vector<std::int32_t> group;
        while (fields >> name) {
            // Population.h m_CarGroups has 23 slots per row. Source LoadGroup
            // stops adding once the row is full (including the shipped longer
            // CASUAL_AVERAGE row), it does not expand the group dynamically.
            if (group.size() == 23) break;
            const auto found = std::ranges::find(models, Lower(name), &Model::Name);
            if (found == models.end()) throw std::runtime_error("cargrp model missing in vehicles.ide: " + name);
            group.push_back(found->Id);
        }
        if (!group.empty()) groups.push_back(std::move(group));
    }
    if (groups.size() != 34) throw std::runtime_error("cargrp source group count differs from34");

    std::vector<std::array<std::uint8_t, 24>> rows;
    std::istringstream cycleStream(cycleText);
    while (std::getline(cycleStream, line)) {
        auto data = DataPart(line);
        std::istringstream fields{std::string(data)};
        int value = -1;
        if (!(fields >> value)) {
            fields.clear();
            std::string first;
            if (fields >> first) throw std::runtime_error("non-numeric popcycle row");
            continue;
        }
        std::array<std::uint8_t, 24> row{};
        if (value < 0 || value > 255) throw std::runtime_error("popcycle byte out of range");
        row[0] = std::uint8_t(value);
        for (std::size_t i = 1; i < row.size(); ++i) {
            if (!(fields >> value) || value < 0 || value > 255)
                throw std::runtime_error("popcycle row lacks24 byte values");
            row[i] = std::uint8_t(value);
        }
        std::string extra;
        if (fields >> extra) throw std::runtime_error("popcycle row has extra values");
        std::uint32_t sum = 0;
        for (std::size_t i = 6; i < row.size(); ++i) sum += row[i];
        if (sum < 100) {
            if (!sum) throw std::runtime_error("popcycle empty group percentages");
            // PopCycle.cpp:87-95 retains the original game's integer rescale.
            for (std::size_t i = 6; i < row.size(); ++i)
                row[i] = std::uint8_t(std::uint32_t(row[i]) * 100u / sum);
        }
        rows.push_back(row);
    }
    if (rows.size() != 20 * 2 * 12) throw std::runtime_error("popcycle source row count differs from480");
    m_Models = std::move(models);
    m_Groups = std::move(groups);
    m_Rows = std::move(rows);
    m_Zones = std::move(zones);
    m_Loaded = true;
    error.clear();
    return true;
} catch (const std::exception& e) {
    error = e.what();
    return false;
}

bool NativeCarGeneratorPopulation::Select(NativeScriptPosition player, std::uint8_t hour,
    bool weekend, std::span<const NativeZonePopulationEntry> zoneStates,
    std::span<const NativeCarLoadedModel> orderedLoadedModels, NativeSourceRngRef rng,
    NativeCarPopulationSelection& out, std::string& error) const {
    if (!m_Loaded || hour >= 24 || !std::isfinite(player.X) || !std::isfinite(player.Y) ||
        !std::isfinite(player.Z) || zoneStates.size() != m_Zones.zones.size() ||
        rng.Readiness() != NativeSourceRngStatus::Ready) {
        error = "missing source population/zone/RNG authority";
        return false;
    }
    for (std::size_t i = 0; i < zoneStates.size(); ++i) {
        if (zoneStates[i].Label != m_Zones.zones[i].name) {
            error = "population-zone identity changed";
            return false;
        }
    }
    int selected = -1;
    float size = std::numeric_limits<float>::infinity();
    for (std::size_t i = 0; i < m_Zones.zones.size(); ++i) {
        const auto& z = m_Zones.zones[i];
        if (player.X < z.x1 || player.X > z.x2 || player.Y < z.y1 || player.Y > z.y2 ||
            player.Z < z.z1 || player.Z > z.z2 || zoneStates[i].Label != z.name) continue;
        const float area = (z.x2 - z.x1) + (z.y2 - z.y1);
        if (area < size) { size = area; selected = int(i); }
    }
    if (selected < 0) { error = "player has no verified 3D population zone"; return false; }
    const auto& zone = zoneStates[std::size_t(selected)];
    if (zone.PopulationType >= 20) { error = "zone population type outside source rows"; return false; }
    const auto& row = m_Rows[(std::size_t(zone.PopulationType) * 2 + (weekend ? 1 : 0)) * 12 + hour / 2];
    std::vector<std::int32_t> eligible;
    std::vector<std::int32_t> seen;
    std::uint32_t weightSum = 0;
    for (const auto& loaded : orderedLoadedModels) {
        const auto id = loaded.ModelId;
        const auto found = std::ranges::find(m_Models, id, &Model::Id);
        if (found == m_Models.end() ||
            std::ranges::find(seen, id) != seen.end()) {
            error = "loaded vehicle roster lacks unique vehicles.ide identity";
            return false;
        }
        seen.push_back(id);
        bool wanted = false;
        for (std::size_t group = 0; group < 18; ++group)
            if (row[6 + group] && std::ranges::find(m_Groups[group], id) != m_Groups[group].end()) wanted = true;
        // Streaming.cpp:2973-77 passes groupId itself to DoesCarGroupHaveModelId,
        // not the gang-group offset; retain this original behavior.
        for (std::size_t group = 0; group < 10; ++group)
            if (zone.GangStrength[group] &&
                std::ranges::find(m_Groups[group], id) != m_Groups[group].end()) wanted = true;
        if (!wanted || !NormalFamily(found->Class)) continue;
        if (eligible.size() == 23) continue; // CLoadedCarGroup ignores overflow.
        if (found->Frequency > std::numeric_limits<std::uint32_t>::max() - weightSum) {
            error = "loaded source frequency total overflow";
            return false;
        }
        weightSum += found->Frequency;
        eligible.push_back(id);
    }
    NativeCarPopulationSelection result{zone.Label, zone.PopulationType, eligible, -1, weightSum, 0};
    if (eligible.empty() || !weightSum) {
        out = std::move(result);
        error.clear();
        return true; // CLoadedCarGroup::PickRandomCar returns MODEL_INVALID.
    }
    for (std::uint8_t tries = 0; tries < 10; ++tries) {
        const auto draw = rng.NextRand15();
        if (draw.Status != NativeSourceRngStatus::Ready || !draw.Value) {
            error = "shared source RNG draw failed";
            return false;
        }
        ++result.Draws;
        // CGeneral::GetRandomNumberInRange(0, sum) maps rand15 into
        // [0,sum-1] using source float lerp/truncation, not modulo.
        const float fraction = float(*draw.Value) * (1.0f / 32767.0f);
        auto weight = std::uint32_t(float(weightSum - 1) * fraction);
        for (const auto id : eligible) {
            const auto found = std::ranges::find(m_Models, id, &Model::Id);
            if (found->Frequency >= weight) { result.ModelId = id; break; }
            weight -= found->Frequency;
        }
        if (result.ModelId < 0) { error = "source weighted choice had no member"; return false; }
        const auto selected = std::ranges::find(orderedLoadedModels, result.ModelId, &NativeCarLoadedModel::ModelId);
        if (selected == orderedLoadedModels.end()) { error = "loaded car vanished from roster"; return false; }
        if (selected->ScriptSuppressed || selected->ScriptBlocked ||
            selected->StreamingPhaseOut || selected->RefCount > 2) {
            result.ModelId = -1;
            continue;
        }
        break;
    }
    out = std::move(result);
    error.clear();
    return true;
}
