#include "NativeAmbientPedSelection.h"

#include <algorithm>
#include <cmath>

namespace {
bool ValidTargets(const NativeAmbientPedSelectionInput& input) {
    for (const float value : {input.TargetCivilian, input.TargetCop, input.TargetDealer, input.TargetGang}) {
        if (!std::isfinite(value) || value < 0) return false;
    }
    for (const auto count : {input.CivilianMale, input.CivilianFemale, input.Cops, input.Dealers}) {
        if (count < 0 || count > 140) return false;
    }
    for (const auto count : input.Gangs) {
        if (count < 0 || count > 140) return false;
    }
    return true;
}

NativeAmbientPedSelectionStatus ChoosePolice(NativeSourceRngRef rng,
    NativeAmbientPedModelObservations& observations, NativeAmbientPedSelection& out) {
    NativeAmbientPedSelection candidate{6, -1};
    if (!observations.Police(rng, candidate.Model))
        return NativeAmbientPedSelectionStatus::UnsupportedModelObservation;
    if (candidate.Model < 0) return NativeAmbientPedSelectionStatus::InvalidInput;
    out = candidate;
    return NativeAmbientPedSelectionStatus::Selected;
}
}

NativeAmbientPedSelectionStatus NativeSelectAmbientPed(const NativeAmbientPedSelectionInput& input,
    NativeSourceRngRef rng, NativeAmbientPedModelObservations& observations,
    NativeAmbientPedSelection& out) {
    if (!input.HasZone) return NativeAmbientPedSelectionStatus::NoSelection;
    if (rng.Readiness() != NativeSourceRngStatus::Ready) return NativeAmbientPedSelectionStatus::UnknownRng;
    if (input.PoliceStation) {
        const auto draw = rng.NextRand15();
        if (!draw.Value) return NativeAmbientPedSelectionStatus::UnknownRng;
        const float unit = float(double(*draw.Value) * (1.0 / 32768.0));
        const int chance = int(double(unit) * 100.0);
        if (chance < 70) return ChoosePolice(rng, observations, out);
    }
    if (!ValidTargets(input)) return NativeAmbientPedSelectionStatus::InvalidInput;
    float dealers = float(double(input.TargetDealer) - double(input.Dealers));
    float gangCount = float(input.Gangs[0]);
    for (std::size_t i = 1; i < input.Gangs.size(); ++i) gangCount += float(input.Gangs[i]);
    float gangs = input.TargetGang - gangCount;
    if (input.OnlyGangs) gangs = 50.0f;
    if (input.DontCreateGangs || input.NoGangs) gangs = -10.0f;
    float cops = float(double(input.TargetCop) - double(input.Cops));
    if (input.GangWar || input.DontCreateCops || input.NoCops || input.ZoneNoCops) cops = -10.0f;
    const auto civilianCount = input.CivilianMale + input.CivilianFemale;
    float civilians = float(double(input.TargetCivilian) - double(civilianCount));
    // Order and the inclusive /32767 float helper differ from the station's
    // /32768 integer percentage draw. Do not merge the two source consumers.
    for (auto* chance : {&civilians, &cops, &dealers, &gangs}) {
        if (*chance < 2.0f) {
            const auto draw = rng.NextRand15();
            if (!draw.Value) return NativeAmbientPedSelectionStatus::UnknownRng;
            const float unit = float(double(*draw.Value) / 32767.0);
            *chance *= unit;
        }
    }
    if (!input.Exterior) dealers = -10.0f;
    for (const auto chance : {civilians, cops, dealers, gangs}) {
        if (!std::isfinite(chance)) return NativeAmbientPedSelectionStatus::Overflow;
    }
    while (true) {
        const float highest = std::max({civilians, cops, dealers, gangs});
        if (highest <= 0.0f) return NativeAmbientPedSelectionStatus::NoSelection;
        if (highest == dealers) {
            if (!input.DealerGroupKnown) return NativeAmbientPedSelectionStatus::UnsupportedModelObservation;
            if (input.DealerGroup.size() > 21) return NativeAmbientPedSelectionStatus::InvalidInput;
            NativeAmbientPedSelection candidate{17, -1};
            for (const auto& model : input.DealerGroup) {
                if (model.Model <= 0) return NativeAmbientPedSelectionStatus::InvalidInput;
                if (model.Loaded) candidate.Model = model.Model;
            }
            if (candidate.Model >= 0) {
                out = candidate;
                return NativeAmbientPedSelectionStatus::Selected;
            }
            dealers = 0.0f;
        } else if (highest == gangs) {
            NativeAmbientPedSelection candidate;
            if (!observations.Gang(rng, candidate))
                return NativeAmbientPedSelectionStatus::UnsupportedModelObservation;
            if (candidate.PedType != 0 && (candidate.PedType < 7 || candidate.PedType > 16))
                return NativeAmbientPedSelectionStatus::InvalidInput;
            if (candidate.PedType != 0 && candidate.Model >= 0) {
                if (!candidate.Model) return NativeAmbientPedSelectionStatus::InvalidInput;
                out = candidate;
                return NativeAmbientPedSelectionStatus::Selected;
            }
            if (input.OnlyGangs) return NativeAmbientPedSelectionStatus::NoSelection;
            gangs = 0.0f;
        } else if (highest == cops) {
            return ChoosePolice(rng, observations, out);
        } else {
            NativeAmbientPedSelection candidate;
            if (!observations.Civilian(rng, candidate))
                return NativeAmbientPedSelectionStatus::UnsupportedModelObservation;
            if (candidate.Model < 0 || candidate.Model == 7) return NativeAmbientPedSelectionStatus::NoSelection;
            if (!candidate.Model || candidate.PedType < 2 || candidate.PedType > 31)
                return NativeAmbientPedSelectionStatus::InvalidInput;
            out = candidate;
            return NativeAmbientPedSelectionStatus::Selected;
        }
    }
}
