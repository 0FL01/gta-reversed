#include "NativeSourceAutomobile.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <exception>
#include <numbers>
#include <ranges>
#include <string_view>

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
    next.BrakeDeceleration=float(handling.brakeDeceleration); next.BrakeBias=float(handling.brakeBias);
    next.SteeringLockDegrees=float(handling.steeringLockDegrees); next.Abs=handling.Abs;
    next.HandlingFlags=handling.HandlingFlags; next.ModelFlags=handling.ModelFlags;
    next.SuspensionForce=float(handling.suspensionForce); next.SuspensionDamping=float(handling.suspensionDamping);
    next.SuspensionHighSpeedDamping=float(handling.suspensionHighSpeedDamping);
    next.SuspensionUpper=float(handling.suspensionUpper); next.SuspensionLower=float(handling.suspensionLower);
    next.SuspensionBias=float(handling.suspensionBias); next.SuspensionAntiDive=float(handling.suspensionAntiDive);
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

NativeSourceAutomobileStatus NativeSourceAutomobile::ProcessPlayerControls(std::uint8_t accelerate,
    std::uint8_t brake, std::int16_t steering, bool handbrake, bool automaticHandbrake,
    float forwardVelocity, float timeStep, std::string& error) {
    if (!m_State || !m_State->Occupants.Driver || steering < -128 || steering > 128 ||
        !std::isfinite(forwardVelocity) || !std::isfinite(timeStep) || timeStep < 0) {
        error="invalid automobile control input"; return Status::InvalidInput;
    }
    auto next=*m_State;
    next.Revision++;
    next.Handbrake=automaticHandbrake||handbrake;
    const float steerDelta=(-float(steering)/128.0f-next.RawSteerAngle)/5.0f*timeStep;
    next.RawSteerAngle=std::clamp(next.RawSteerAngle+steerDelta,-1.0f,1.0f);
    next.SteerAngle=next.SteeringLockDegrees*std::numbers::pi_v<float>/180.0f*
        std::copysign(next.RawSteerAngle*next.RawSteerAngle,next.RawSteerAngle);
    if (automaticHandbrake) { next.GasPedal=0; next.BrakePedal=1; next.DoingBurnout=false; }
    else {
        const float gasInput=(float(accelerate)-float(brake))/255.0f;
        next.DoingBurnout=std::abs(forwardVelocity)<0.01f&&accelerate>150&&brake>150;
        if (next.DoingBurnout) { next.GasPedal=float(accelerate)/255; next.BrakePedal=float(brake)/255; }
        else if (std::abs(forwardVelocity)<0.01f) { next.GasPedal=gasInput; next.BrakePedal=0; }
        else if (forwardVelocity>=0&&gasInput<0) { next.GasPedal=0; next.BrakePedal=std::abs(gasInput); }
        else if (forwardVelocity<0&&gasInput>=0&&(m_State->GasPedal<=0.5f||forwardVelocity<=-0.15f)) {
            next.GasPedal=0; next.BrakePedal=gasInput;
        } else { next.GasPedal=gasInput; next.BrakePedal=0; }
    }
    return Publish(std::move(next),error);
}

NativeSourceAutomobileStatus NativeSourceAutomobile::AdvanceDrive(float timeStep,
    bool drivenWheelsOnGround, std::string& error) {
    if (!m_State || !m_State->Occupants.Driver || !std::isfinite(timeStep) || timeStep <= 0) {
        error="invalid automobile drive step"; return Status::InvalidInput;
    }
    NativeTransmission transmission;
    transmission.Initialize({m_State->MaxVelocityKmh,m_State->EngineAcceleration,m_State->EngineInertia,
        m_State->Drag,m_State->Gears,m_State->DriveType,m_State->HandlingFlags});
    auto next=*m_State;
    const float drive=transmission.DriveAcceleration(next.GasPedal,next.Transmission,
        next.ForwardSpeed/NativeTransmission::UnitsPerSecond,timeStep,drivenWheelsOnGround);
    next.ForwardSpeed+=drive*transmission.DrivenWheels()*NativeTransmission::UnitsPerSecond;
    if (next.BrakePedal>0) {
        const float brake=next.BrakeDeceleration*next.BrakePedal*timeStep/NativeTransmission::UnitsPerSecond;
        if (next.ForwardSpeed>0) next.ForwardSpeed=std::max(0.0f,next.ForwardSpeed-brake);
        else next.ForwardSpeed=std::min(0.0f,next.ForwardSpeed+brake);
    }
    if (next.Handbrake) next.ForwardSpeed=0;
    next.ForwardSpeed=transmission.AirResistance(next.ForwardSpeed,timeStep);
    if (!std::isfinite(next.ForwardSpeed)) { error="automobile drive overflow"; return Status::InvalidInput; }
    next.Revision++;
    return Publish(std::move(next),error);
}

NativeSourceAutomobileStatus NativeSourceAutomobile::SetupSuspension(const CarPoseMeasure& measure,
    std::string& error) {
    if (!m_State || std::string_view(measure.model)!=m_State->ModelName || measure.wheels!=4 ||
        !std::isfinite(measure.frontY)||!std::isfinite(measure.rearY)||!std::isfinite(measure.wheelR)||
        measure.wheelR<=0||m_State->SuspensionForce<=0||m_State->SuspensionUpper<m_State->SuspensionLower) {
        error="invalid automobile suspension identity"; return Status::InvalidInput;
    }
    const auto named=[&](std::string_view name)->const NativeGeneratedVehicleFrame* {
        const auto found=std::ranges::find_if(m_State->Assets->Frames,[&](const auto& frame){return frame.Name==name;});
        return found==m_State->Assets->Frames.end()?nullptr:&*found;
    };
    const std::array<std::string_view,4> names{"wheel_lf_dummy","wheel_lb_dummy","wheel_rf_dummy","wheel_rb_dummy"};
    std::array<const NativeGeneratedVehicleFrame*,4> wheels{};
    for (std::size_t i=0;i<4;++i) {
        wheels[i]=named(names[i]);
        if (!wheels[i]) { error="missing source wheel dummy"; return Status::InvalidInput; }
    }
    auto next=*m_State; next.Revision++;
    const float wheelR=float(measure.wheelR),spring=next.SuspensionUpper-next.SuspensionLower;
    for (std::size_t i=0;i<4;++i) {
        const float x=wheels[i]->ModelBind[9],y=wheels[i]->ModelBind[10],z=wheels[i]->ModelBind[11];
        next.SuspensionLines[i]={{x,y,z+next.SuspensionUpper},{x,y,z+next.SuspensionLower-wheelR/2},
            spring,next.SuspensionUpper-next.SuspensionLower+wheelR/2};
    }
    const auto height=[&](std::size_t i) {
        return (1-1/(next.SuspensionForce*4))*spring+wheelR/2-next.SuspensionLines[i].Start[2];
    };
    next.FrontHeightAboveRoad=height(0); next.RearHeightAboveRoad=height(1);
    return Publish(std::move(next),error);
}
