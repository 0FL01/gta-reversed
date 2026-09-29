#include "NativePedModelMetadata.h"
#include "NativeMetadataText.h"
#include "game_sa/Animation/AnimAssocNames.h"

#include <algorithm>

namespace {
using NativeMetadataText::Lines;
using NativeMetadataText::Lower;
using NativeMetadataText::Require;
using NativeMetadataText::Scan;

constexpr std::array<std::string_view, 32> PedTypes{{
#define GTA_PED_TYPE_NAME(name, type) name,
#include "game_sa/PedTypeNames.inc"
#undef GTA_PED_TYPE_NAME
}};

std::int32_t Race(std::string_view name) {
    for (std::size_t i = 0; i < std::min(name.size(), std::size_t{2}); ++i) {
        const auto c = name[i] >= 'a' && name[i] <= 'z' ? name[i] - ('a' - 'A') : name[i];
        switch (c) {
        case 'B': return 1;
        case 'W': return 2;
        case 'O': case 'I': return 3;
        case 'H': return 4;
        }
    }
    return 0;
}
}

bool NativePedModelMetadata::LoadBeforeWorker(const char* gameDir, const NativeWorldEntityInfo& metadata,
    std::string& error) {
    std::array<NativeWorldEntitySourceText, 3> sources;
    constexpr std::array names{"data/pedstats.dat", "data/animgrp.dat", "data/pedgrp.dat"};
    std::size_t total = 0;
    for (std::size_t i = 0; i < names.size(); ++i)
        if (!NativeWorldEntitySourceText::ReadBeforeWorker(gameDir, names[i], total, sources[i], error)) return false;
    return LoadSources(metadata, sources[0], sources[1], sources[2], error);
}

bool NativePedModelMetadata::LoadSources(const NativeWorldEntityInfo& metadata,
    const NativeWorldEntitySourceText& stats, const NativeWorldEntitySourceText& animations,
    const NativeWorldEntitySourceText& groups, std::string& error) try {
    int identity = -1;
    Require(metadata.FindNamespaceModelId("male01", identity) != NativeWorldNameStatus::NotLoaded,
        "ped metadata requires the complete IDE namespace");
    NativePedModelMetadata next;
    std::size_t statCount = 0;
    Lines(stats, [&](const auto& line, const auto& row) {
        if (line[0] == '#') return true;
        Require(statCount < next.m_Stats.size(), "pedstats exceeds43 source records");
        auto& stat = next.m_Stats[statCount++];
        Scan scan(line);
        int fear{}, temper{}, lawfulness{}, sexiness{}, flags{}, decision{};
        Require(scan.Word(stat.Name, 23) && scan.Float(stat.FleeDistance) && scan.Float(stat.HeadingChangeRate) &&
            scan.Int(fear) && scan.Int(temper) && scan.Int(lawfulness) && scan.Int(sexiness) &&
            scan.Float(stat.AttackStrength) && scan.Float(stat.DefendWeakness) && scan.Int(flags) && scan.Int(decision),
            "invalid eleven-field pedstats row");
        stat.Source = row;
        stat.Fear = static_cast<std::uint8_t>(fear);
        stat.Temper = static_cast<std::uint8_t>(temper);
        stat.Lawfulness = static_cast<std::uint8_t>(lawfulness);
        stat.Sexiness = static_cast<std::uint8_t>(sexiness);
        stat.Flags = static_cast<std::uint16_t>(flags);
        stat.DefaultDecisionMaker = static_cast<std::int8_t>(decision);
        return true;
    });
    Require(statCount == next.m_Stats.size(), "pedstats lacks43 source records");

    for (const auto& [name, block] : g_AnimAssocNames) next.m_AnimationGroups.emplace_back(name);
    bool inGroup = false;
    int declared = 0, count = 0;
    Lines(animations, [&](const auto& line, const auto&) {
        if (line[0] == '#') return true;
        Scan scan(line);
        std::string name, block, type;
        Require(scan.Word(name, inGroup ? 23 : 15), "invalid animation association name");
        if (inGroup) {
            if (name == "end") inGroup = false;
            else Require(++count <= declared, "animation association exceeds its source name allocation");
        } else {
            Require(scan.Word(block, 15) && scan.Word(type, 31) && scan.Int(declared) &&
                declared >= 0 && declared <= 4096 && next.m_AnimationGroups.size() < 1024,
                "invalid animation association header");
            next.m_AnimationGroups.push_back(std::move(name));
            inGroup = true;
            count = 0;
        }
        return true;
    });
    Require(!inGroup, "unterminated animation association group");

    for (const auto& [id, source] : metadata.PedModels()) {
        NativeResolvedPedModel model;
        model.Source = source;
        const auto type = std::ranges::find(PedTypes, source.PedTypeName);
        Require(type != PedTypes.end(), "unrepresented source ped type: " + source.PedTypeName);
        model.PedType = static_cast<std::int32_t>(type - PedTypes.begin());
        const auto stat = std::ranges::find(next.m_Stats, source.StatName, &NativePedStatMetadata::Name);
        // GetPedStatType's exact source fallback is SENSIBLE_GUY16, not a
        // fabricated metadata row or loaded default model.
        model.StatsUsedSourceFallback = stat == next.m_Stats.end();
        model.StatsType = model.StatsUsedSourceFallback ? 16 : static_cast<std::int32_t>(stat - next.m_Stats.begin());
        const auto anim = std::ranges::find_if(next.m_AnimationGroups,
            [&](const auto& name) { return Lower(name) == Lower(source.AnimationGroupName); });
        Require(anim != next.m_AnimationGroups.end(), "unrepresented source animation group: " + source.AnimationGroupName);
        model.AnimationGroup = static_cast<std::int32_t>(anim - next.m_AnimationGroups.begin());
        model.Race = Race(source.Name);
        next.m_Models.emplace(id, std::move(model));
    }

    std::size_t groupCount = 0;
    Lines(groups, [&](const auto& line, const auto& row) {
        NativePedMetadataGroup group;
        group.Models.fill(NativePedMetadataGroup::UnusedModel);
        group.Source = row;
        Scan scan(line);
        std::string name;
        while (group.Count < group.Models.size()) {
            scan.Space();
            if (!*scan.At) break;
            Require(scan.Word(name), "invalid ped-group model name");
            if (name[0] == '#') break;
            int id = -1;
            const auto status = metadata.FindNamespaceModelId(name, id);
            if (status == NativeWorldNameStatus::Missing) continue; // Original skips unmatched names.
            Require(status == NativeWorldNameStatus::Found, "unavailable ped-group namespace key: " + name);
            Require(id > 0 && next.m_Models.contains(id), "ped-group model lacks represented ped metadata: " + name);
            group.Models[group.Count++] = static_cast<std::int16_t>(id);
        }
        if (group.Count) {
            Require(groupCount < next.m_Groups.size(), "pedgrp exceeds57 source groups");
            next.m_Groups[groupCount++] = std::move(group);
        }
        return true;
    });
    Require(groupCount == next.m_Groups.size(), "pedgrp lacks57 source groups");
    next.m_Loaded = true;
    *this = std::move(next);
    error.clear();
    return true;
} catch (const std::exception& e) { error = e.what(); return false; }

const NativeResolvedPedModel* NativePedModelMetadata::Find(std::int32_t model) const noexcept {
    if (!m_Loaded) return nullptr;
    const auto found = m_Models.find(model);
    return found == m_Models.end() ? nullptr : &found->second;
}

NativePedPolicyModel NativePedModelMetadata::PolicyModel(std::int32_t model) const noexcept {
    const auto* found = Find(model);
    return found ? NativePedPolicyModel{true, model, found->PedType, found->Race} : NativePedPolicyModel{};
}

NativePedMetadataStatus NativePedModelMetadata::QualifyCivilianSlot(std::int32_t model,
    bool streamingKnown, bool loaded, std::int16_t references, NativeCivilianLoadedPed& out) const noexcept {
    if (!streamingKnown) return NativePedMetadataStatus::UnknownStreaming;
    if (references < 0 || model < -1 || model >= 20000 || (model == -1 && (loaded || references)))
        return NativePedMetadataStatus::InvalidInput;
    if (!m_Loaded) return NativePedMetadataStatus::NotLoaded;
    NativeCivilianLoadedPed result;
    result.Model = model;
    result.Loaded = loaded;
    result.References = references;
    if (model != -1) {
        const auto* found = Find(model);
        if (!found) return NativePedMetadataStatus::UnknownModel;
        result.ModelInfoKnown = true;
        result.PedType = found->PedType;
        result.AnimationGroup = found->AnimationGroup;
        result.StatsType = found->StatsType;
        result.CarsCanDrive = found->Source.CarsCanDriveMask;
    }
    out = result;
    return NativePedMetadataStatus::Ready;
}

bool NativePedMetadataPolicies::ZoneAccepts(std::int32_t model, bool& accepted) noexcept {
    return m_Zone && NativePedZoneAccepts(m_Zone->HasZone, m_Zone->StreamingCheat,
        m_Zone->RaceMask, m_Metadata.PolicyModel(model), accepted) == NativePedModelPolicyStatus::Decided;
}
bool NativePedMetadataPolicies::AttractorAccepts(std::int32_t model, std::string_view script, bool& accepted) noexcept {
    return NativePedAttractorAccepts(m_Metadata.PolicyModel(model), script, accepted) == NativePedModelPolicyStatus::Decided;
}
bool NativePedMetadataPolicies::StatsCompatible(std::int32_t actual, std::int32_t requested, bool& accepted) noexcept {
    return NativePedStatsCompatible(actual, requested, accepted) == NativePedModelPolicyStatus::Decided;
}
