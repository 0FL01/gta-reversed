#include "NativeSourceAutomobile.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <exception>
#include <ranges>

namespace {
using Status = NativeSourceAutomobileStatus;
bool Finite(const NativeGarageMatrix& matrix) {
    const auto finite=[](const NativeCollisionVector& v) { return std::ranges::all_of(v,[](float value){return std::isfinite(value);}); };
    return finite(matrix.Position)&&std::ranges::all_of(matrix.Basis,finite);
}
bool ValidCreatedBy(NativeVehicleCreatedBy value) {
    return value >= NativeVehicleCreatedBy::Random && value <= NativeVehicleCreatedBy::Permanent;
}
std::uint8_t PassengerSeats(const NativeGeneratedVehiclePacket& asset) {
    const auto doors=std::ranges::count_if(asset.Frames,[](const auto& frame) {
        return frame.Name.starts_with("door_") && frame.Name.ends_with("_dummy");
    });
    return doors ? std::uint8_t(std::min<std::ptrdiff_t>(doors-1,8)) : 1;
}
}

NativeSourceAutomobileStatus NativeSourceAutomobile::Publish(NativeSourceAutomobileState&& next,
    std::string& error) try {
    m_State=std::make_shared<const NativeSourceAutomobileState>(std::move(next));
    error.clear(); return Status::Ready;
} catch (const std::exception& e) { error=e.what(); return Status::Error; }


NativeSourceAutomobileStatus NativeSourceAutomobile::Construct(std::uint64_t identity,
    const NativeCarGeneratorModelDefinition& definition, NativeGeneratedVehicleAsset asset,
    const HandlingParams& handling, NativeVehicleCreatedBy createdBy, const NativeGarageMatrix& matrix,
    std::string& error) {
    if (!identity || !asset || !ValidCreatedBy(createdBy) || !Finite(matrix)) {
        error="invalid automobile construction input"; return Status::InvalidInput;
    }
    if (definition.ModelId < 0 || definition.Type != NativeVehicleType::Automobile || definition.TypeName != "car") {
        error="source definition is not an automobile"; return Status::Unsupported;
    }
    if (asset->Definition.ModelId != definition.ModelId || asset->Definition.ModelName != definition.ModelName ||
        asset->Definition.TextureName != definition.TextureName || asset->Definition.HandlingName != definition.HandlingName ||
        asset->Definition.Type != definition.Type || !asset->Collision || asset->Collision->Empty ||
        asset->Scene.meshes.empty() || asset->Frames.empty()) {
        error="vehicle asset identity does not match source definition"; return Status::InvalidInput;
    }
    if (definition.ModelName == "landstal" || definition.HandlingName == "LANDSTAL") {
        if (definition.ModelId != 400 || definition.ModelName != "landstal" || definition.TextureName != "landstal" ||
            definition.HandlingName != "LANDSTAL") {
            error="partial LANDSTAL identity substitution rejected"; return Status::InvalidInput;
        }
    }
    if (handling.model != definition.HandlingName || handling.mass <= 0 || handling.turnMass <= 0 ||
        handling.drag < 0 || handling.vmaxFileKmh <= 0 || handling.accelFile <= 0 || handling.inertia < 0 ||
        handling.gears < 1 || handling.gears > 6 || !std::isfinite(handling.mass) ||
        !std::isfinite(handling.turnMass) || !std::isfinite(handling.drag) ||
        !std::isfinite(handling.vmaxFileKmh) || !std::isfinite(handling.accelFile) ||
        !std::isfinite(handling.inertia)) {
        error="handling row does not match source vehicle identity"; return Status::InvalidInput;
    }
    NativeSourceAutomobileState next;
    next.Identity=identity; next.Revision=m_State ? m_State->Revision+1 : 1;
    next.ModelId=definition.ModelId; next.ModelName=definition.ModelName;
    next.HandlingName=definition.HandlingName; next.TextureName=definition.TextureName;
    next.Type=definition.Type; next.Status=NativeVehicleStatus::Simple; next.CreatedBy=createdBy; next.Matrix=matrix;
    next.Mass=float(handling.mass); next.TurnMass=float(handling.turnMass); next.Drag=float(handling.drag);
    std::ranges::transform(handling.centreOfMass,next.CentreOfMass.begin(),[](double value){return float(value);});
    next.PercentSubmerged=float(handling.percentSubmerged); next.TractionMult=float(handling.tractionMult);
    next.TractionLoss=float(handling.tractionLoss); next.TractionBias=float(handling.tractionBias);
    next.MaxVelocityKmh=float(handling.vmaxFileKmh); next.EngineAcceleration=float(handling.accelFile);
    next.EngineInertia=float(handling.inertia); next.Gears=std::uint8_t(handling.gears);
    next.DriveType=handling.driveType; next.EngineType=handling.engineType;
    next.Occupants.MaxPassengers=PassengerSeats(*asset); next.Assets=std::move(asset);
    return Publish(std::move(next),error);
}

NativeSourceAutomobileStatus NativeSourceAutomobile::SetDriver(std::uint64_t occupant, std::string& error) {
    if (!m_State) { error="automobile not constructed"; return Status::InvalidInput; }
    if (!occupant) { error="driver identity is required"; return Status::InvalidInput; }
    if (m_State->Occupants.Driver && m_State->Occupants.Driver != occupant) { error="driver seat occupied"; return Status::Occupied; }
    if (std::ranges::find(m_State->Occupants.Passengers,occupant) != m_State->Occupants.Passengers.end()) {
        error="occupant already a passenger"; return Status::Occupied;
    }
    auto next=*m_State; next.Revision++; next.Occupants.Driver=occupant;
    return Publish(std::move(next),error);
}

NativeSourceAutomobileStatus NativeSourceAutomobile::AddPassenger(std::uint64_t occupant, std::uint8_t seat,
    std::string& error) {
    if (!m_State) { error="automobile not constructed"; return Status::InvalidInput; }
    if (!occupant || seat >= m_State->Occupants.MaxPassengers) { error="invalid passenger seat"; return Status::InvalidInput; }
    if (m_State->Occupants.PassengerCount >= m_State->Occupants.MaxPassengers) { error="passenger capacity reached"; return Status::Full; }
    if (m_State->Occupants.Driver == occupant ||
        std::ranges::find(m_State->Occupants.Passengers,occupant) != m_State->Occupants.Passengers.end() ||
        m_State->Occupants.Passengers[seat]) { error="occupant or passenger seat already assigned"; return Status::Occupied; }
    auto next=*m_State; next.Revision++; next.Occupants.Passengers[seat]=occupant; next.Occupants.PassengerCount++;
    return Publish(std::move(next),error);
}

NativeSourceAutomobileStatus NativeSourceAutomobile::RemoveOccupant(std::uint64_t occupant, std::string& error) {
    if (!m_State || !occupant) { error="invalid occupant removal"; return Status::InvalidInput; }
    auto next=*m_State;
    if (next.Occupants.Driver == occupant) next.Occupants.Driver=0;
    else {
        const auto found=std::ranges::find(next.Occupants.Passengers,occupant);
        if (found == next.Occupants.Passengers.end()) { error="occupant not found"; return Status::InvalidInput; }
        *found=0; --next.Occupants.PassengerCount;
    }
    next.Revision++;
    return Publish(std::move(next),error);
}
