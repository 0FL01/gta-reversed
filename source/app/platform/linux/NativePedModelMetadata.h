// Owned source ped namespace. Metadata readiness is NOT streaming or birth.
#pragma once

#include "NativeCivilianOccupation.h"
#include "NativePedModelPolicies.h"
#include "NativeWorldEntityInfo.h"

struct NativePedStatMetadata {
    std::string Name;
    NativeWorldSourceRow Source;
    float FleeDistance{}, HeadingChangeRate{}, AttackStrength{}, DefendWeakness{};
    std::uint8_t Fear{}, Temper{}, Lawfulness{}, Sexiness{};
    std::uint16_t Flags{};
    std::int8_t DefaultDecisionMaker{};
};

struct NativeResolvedPedModel {
    NativeWorldPedModelInfo Source;
    std::int32_t PedType = -1, StatsType = -1, AnimationGroup = -1;
    std::int32_t Race = -1;
    bool StatsUsedSourceFallback = false;
};

struct NativePedMetadataGroup {
    static constexpr std::int16_t UnusedModel = 2000; // CPopulation source sentinel.
    std::array<std::int16_t, 21> Models{};
    std::uint16_t Count{};
    NativeWorldSourceRow Source;
};

enum class NativePedMetadataStatus {
    Ready, NotLoaded, UnknownModel, UnknownStreaming, InvalidInput
};

class NativePedModelMetadata {
public:
    // Exclusive startup IO only. The IDE namespace is the existing complete
    // ordered reader, not a second peds.ide parser. All publication is atomic.
    bool LoadBeforeWorker(const char* gameDir, const NativeWorldEntityInfo&, std::string& error);
    bool LoadSources(const NativeWorldEntityInfo&, const NativeWorldEntitySourceText& stats,
        const NativeWorldEntitySourceText& animations, const NativeWorldEntitySourceText& groups,
        std::string& error);
    const NativeResolvedPedModel* Find(std::int32_t model) const noexcept;
    const std::map<int, NativeResolvedPedModel>& Models() const { return m_Models; }
    const std::array<NativePedStatMetadata, 43>& Stats() const { return m_Stats; }
    const std::vector<std::string>& AnimationGroups() const { return m_AnimationGroups; }
    const std::array<NativePedMetadataGroup, 57>& Groups() const { return m_Groups; }
    NativePedPolicyModel PolicyModel(std::int32_t model) const noexcept;
    // Caller supplies an actual streaming/pool observation. Never infer Loaded
    // or References from an IDE declaration. Unknown input retains out.
    NativePedMetadataStatus QualifyCivilianSlot(std::int32_t model, bool streamingKnown,
        bool loaded, std::int16_t references, NativeCivilianLoadedPed& out) const noexcept;
private:
    bool m_Loaded{};
    std::map<int, NativeResolvedPedModel> m_Models;
    std::array<NativePedStatMetadata, 43> m_Stats{};
    std::vector<std::string> m_AnimationGroups;
    std::array<NativePedMetadataGroup, 57> m_Groups{};
};

struct NativePedZonePolicy {
    bool HasZone = false, StreamingCheat = false;
    std::uint8_t RaceMask = 0;
};

// Bind the already verified helpers to actual owned model metadata. Current
// zone/cheat state must still come from its owner; nullopt is unavailable.
class NativePedMetadataPolicies final : public NativeCivilianOccupationObservations {
public:
    NativePedMetadataPolicies(const NativePedModelMetadata& metadata,
        std::optional<NativePedZonePolicy> zone) : m_Metadata(metadata), m_Zone(zone) {}
    bool ZoneAccepts(std::int32_t model, bool& accepted) noexcept override;
    bool AttractorAccepts(std::int32_t model, std::string_view script, bool& accepted) noexcept override;
    bool StatsCompatible(std::int32_t actual, std::int32_t requested, bool& accepted) noexcept override;
private:
    const NativePedModelMetadata& m_Metadata;
    std::optional<NativePedZonePolicy> m_Zone;
};
