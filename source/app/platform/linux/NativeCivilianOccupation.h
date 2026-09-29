// Source loaded-ped occupation filtering. No model loading, ped birth or census.
#pragma once

#include <array>
#include <cstdint>
#include <string_view>

enum class NativeCivilianOccupationStatus {
    Chosen, NoOccupation, InvalidInput, UnknownStreaming,
    UnknownModel, UnknownPolicy
};

struct NativeCivilianLoadedPed {
    std::int32_t Model = -1;
    bool StreamingKnown = true;
    bool Loaded = false;
    bool ModelInfoKnown = false;
    std::int16_t References = 0;
    std::int32_t PedType = 4;
    std::int32_t AnimationGroup = 0;
    std::int32_t StatsType = 0;
    std::uint16_t CarsCanDrive = 0;
};

struct NativeCivilianOccupationInput {
    std::array<NativeCivilianLoadedPed, 8> LoadedPeds{};
    bool Exterior = true;
    std::int32_t InteriorPedsUsed = 0;
    bool MustBeMale = false;
    bool MustBeFemale = false;
    std::int32_t AnimationGroup = -1;
    std::int32_t ExcludedModel = -1;
    std::int32_t CompatibleStats = -1;
    bool OnlyOnFoot = false;
    bool TestUsedOccupations = true;
    bool AtAttractor = false;
    std::string_view AttractorScript;
    float Rain = 0.0f;
};

class NativeCivilianOccupationObservations {
public:
    virtual ~NativeCivilianOccupationObservations() = default;
    // False is unavailable policy, never a rejected model. These are read-only
    // source observations and cannot load models or consume a different RNG.
    virtual bool ZoneAccepts(std::int32_t model, bool& accepted) noexcept = 0;
    virtual bool AttractorAccepts(std::int32_t model, std::string_view script,
        bool& accepted) noexcept = 0;
    virtual bool StatsCompatible(std::int32_t actual, std::int32_t requested,
        bool& accepted) noexcept = 0;
};

// Preserve the original nested traversal: refcount pass, then all eight slots.
// The refcount limit is 3/5 with TestUsedOccupations, otherwise 7. The current
// upstream refactor's first-N-slots traversal is not the retail algorithm.
// Chosen can return source MALE01(7) when occupation testing is disabled; the
// caller's FindNewPedType still rejects that fallback as its source does.
// Every non-Chosen result retains out; no IO, birth, pool or world mutation.
NativeCivilianOccupationStatus NativeChooseCivilianOccupation(
    const NativeCivilianOccupationInput&, NativeCivilianOccupationObservations&,
    std::int32_t& out);
