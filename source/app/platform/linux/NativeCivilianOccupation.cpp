#include "NativeCivilianOccupation.h"

#include <cmath>

NativeCivilianOccupationStatus NativeChooseCivilianOccupation(
    const NativeCivilianOccupationInput& input,
    NativeCivilianOccupationObservations& observations, std::int32_t& out) {
    if (input.InteriorPedsUsed < 0 || input.InteriorPedsUsed > 140 ||
        !std::isfinite(input.Rain) || input.Rain < 0.0f || input.Rain > 1.0f) {
        return NativeCivilianOccupationStatus::InvalidInput;
    }

    const auto passes = !input.TestUsedOccupations ? 7 :
        (!input.Exterior && input.InteriorPedsUsed > 20 ? 5 : 3);
    for (int references = 0; references < passes; ++references) {
        for (const auto& model : input.LoadedPeds) {
            if (model.Model < 0) continue;
            if (!model.StreamingKnown) return NativeCivilianOccupationStatus::UnknownStreaming;
            if (!model.Loaded) continue;
            if (!model.ModelInfoKnown) return NativeCivilianOccupationStatus::UnknownModel;
            if (model.References != references || model.Model == input.ExcludedModel) continue;
            if (model.PedType < 0 || model.PedType > 31 || model.StatsType < 0 ||
                model.AnimationGroup < 0) {
                return NativeCivilianOccupationStatus::InvalidInput;
            }

            bool accepted = false;
            const bool zoneKnown = observations.ZoneAccepts(model.Model, accepted);
            if (input.Exterior && !zoneKnown) return NativeCivilianOccupationStatus::UnknownPolicy;
            if (input.Exterior && !accepted) continue;
            if (input.OnlyOnFoot && !(model.CarsCanDrive & 0x1000u)) continue;
            if (input.MustBeMale && model.PedType != 4) continue;
            if (input.MustBeFemale && model.PedType != 5) continue;
            if (input.AnimationGroup >= 0 && model.AnimationGroup != input.AnimationGroup) continue;

            if (input.AtAttractor) {
                if (model.PedType >= 17 && model.PedType <= 22) continue;
                if (!observations.AttractorAccepts(model.Model, input.AttractorScript, accepted))
                    return NativeCivilianOccupationStatus::UnknownPolicy;
                if (!accepted) continue;
            }
            if (!input.Exterior && model.PedType >= 6 && model.PedType <= 22) continue;
            if (input.CompatibleStats >= 0) {
                if (!observations.StatsCompatible(model.StatsType, input.CompatibleStats, accepted))
                    return NativeCivilianOccupationStatus::UnknownPolicy;
                if (!accepted) continue;
            }
            if (input.Rain >= 0.1f && (model.StatsType == 38 || model.StatsType == 39)) continue;
            out = model.Model;
            return NativeCivilianOccupationStatus::Chosen;
        }
    }
    if (!input.TestUsedOccupations) {
        out = 7;
        return NativeCivilianOccupationStatus::Chosen;
    }
    return NativeCivilianOccupationStatus::NoOccupation;
}
