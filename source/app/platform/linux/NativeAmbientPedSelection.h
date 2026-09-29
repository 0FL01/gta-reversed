#pragma once

#include "NativeSourceRng.h"

#include <array>
#include <cstdint>
#include <span>

enum class NativeAmbientPedSelectionStatus : std::uint8_t {
    Selected,
    NoSelection,
    InvalidInput,
    UnknownRng,
    UnsupportedModelObservation,
    Overflow,
};

struct NativeAmbientDealerModel {
    std::int32_t Model = -1;
    bool Loaded = false;
};

struct NativeAmbientPedSelectionInput {
    bool HasZone = false;
    bool PoliceStation = false;
    bool OnlyGangs = false;
    bool DontCreateGangs = false;
    bool NoGangs = false;
    bool GangWar = false;
    bool DontCreateCops = false;
    bool NoCops = false;
    bool ZoneNoCops = false;
    bool Exterior = true;
    float TargetCivilian = 0, TargetCop = 0, TargetDealer = 0, TargetGang = 0;
    std::int32_t CivilianMale = 0, CivilianFemale = 0, Cops = 0, Dealers = 0;
    std::array<std::int32_t, 10> Gangs{};
    bool DealerGroupKnown = false;
    std::span<const NativeAmbientDealerModel> DealerGroup;
};

struct NativeAmbientPedSelection {
    std::int32_t PedType = -1;
    // For PedType==6 this is the CCopPed constructor's eCopType key, NOT a
    // model-info ID. Source ChoosePolicePedOccupation returns CITYCOP (zero).
    std::int32_t Model = -1;
    bool operator==(const NativeAmbientPedSelection&) const = default;
};

// These observations require the current source loaded-ped/model owners. A
// false return is missing authority, not MODEL_INVALID. Implementations may
// consume the SAME borrowed source stream, never fork or reseed it.
class NativeAmbientPedModelObservations {
public:
    virtual ~NativeAmbientPedModelObservations() = default;
    virtual bool Police(NativeSourceRngRef, std::int32_t& model) noexcept = 0;
    virtual bool Gang(NativeSourceRngRef, NativeAmbientPedSelection&) noexcept = 0;
    virtual bool Civilian(NativeSourceRngRef, NativeAmbientPedSelection&) noexcept = 0;
};

// Source FindNewPedType decision only. Deficits, zone and dealer-group identity
// must be supplied by their actual owners; this neither creates a ped nor
// certifies the source population census. Every non-Selected result retains
// out, but genuine source RNG/model-observation prefixes are not rolled back.
NativeAmbientPedSelectionStatus NativeSelectAmbientPed(const NativeAmbientPedSelectionInput&,
    NativeSourceRngRef, NativeAmbientPedModelObservations&, NativeAmbientPedSelection& out);
