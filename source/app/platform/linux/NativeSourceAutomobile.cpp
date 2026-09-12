#include "NativeSourceAutomobile.h"

#include <algorithm>
#include <cmath>
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
float Dot(const NativeSourcePhysicalVector& a,const NativeSourcePhysicalVector& b) {
    return a[2]*b[2]+a[1]*b[1]+a[0]*b[0];
}
bool Finite(const NativeSourcePhysicalVector& v) { return std::ranges::all_of(v,[](float n){return std::isfinite(n);}); }
NativeSourceGroundTransform Transform(const NativeGarageMatrix& matrix) { return {matrix.Position,matrix.Basis}; }
NativeSourcePhysicalVector Sub(const NativeSourcePhysicalVector& a,const NativeSourcePhysicalVector& b) {
    return {a[0]-b[0],a[1]-b[1],a[2]-b[2]};
}
NativeSourcePhysicalVector Add(const NativeSourcePhysicalVector& a,const NativeSourcePhysicalVector& b) {
    return {a[0]+b[0],a[1]+b[1],a[2]+b[2]};
}
NativeSourcePhysicalVector Scale(const NativeSourcePhysicalVector& v,float scale) {
    return {v[0]*scale,v[1]*scale,v[2]*scale};
}
NativeSourcePhysicalVector Cross(const NativeSourcePhysicalVector& a,const NativeSourcePhysicalVector& b) {
    return {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};
}
NativeSourcePhysicalVector Vector(const NativeGarageMatrix& matrix,const NativeSourcePhysicalVector& v) {
    NativeSourcePhysicalVector out;
    for (std::size_t i=0;i<3;++i) out[i]=(matrix.Basis[0][i]*v[0]+matrix.Basis[1][i]*v[1])+matrix.Basis[2][i]*v[2];
    return out;
}
float Squared(const NativeSourcePhysicalVector& v) { return (v[0]*v[0]+v[1]*v[1])+v[2]*v[2]; }
bool Normalise(NativeSourcePhysicalVector& v,float& magnitude) {
    magnitude=std::sqrt(Squared(v));
    if (!std::isfinite(magnitude)||magnitude<=0) return false;
    for (auto& component:v) component/=magnitude;
    return Finite(v);
}
void SourceNormalise(NativeSourcePhysicalVector& v) {
    const float squared=Squared(v);
    if (squared<=0) { v[0]=1; return; }
    const float reciprocal=1.0f/std::sqrt(squared);
    for (auto& component:v) component*=reciprocal;
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

NativeSourceAutomobileStatus NativeSourceAutomobile::SetStatus(NativeVehicleStatus status,std::string& error) {
    if (!m_State||status>NativeVehicleStatus::PlayerDisabled||
        ((status==NativeVehicleStatus::Player||status==NativeVehicleStatus::RemoteControlled||status==NativeVehicleStatus::PlayerDisabled)&&
         !m_State->Occupants.Driver)) { error="invalid automobile status"; return Status::InvalidInput; }
    auto next=*m_State; next.Revision++; next.Status=status;
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
    const float oldForward=Dot(next.MoveSpeed,next.Matrix.Basis[1]);
    next.MoveSpeed=Add(next.MoveSpeed,Scale(next.Matrix.Basis[1],next.ForwardSpeed-oldForward));
    if (!Finite(next.MoveSpeed)) { error="automobile drive velocity overflow"; return Status::InvalidInput; }
    next.Revision++;
    return Publish(std::move(next),error);
}

NativeSourceAutomobileStatus NativeSourceAutomobile::AdvancePosition(float timeStep,
    std::string& error) {
    if (!m_State || !std::isfinite(timeStep) || timeStep <= 0 || !Finite(m_State->MoveSpeed) ||
        !Finite(m_State->FrictionMoveSpeed) || !Finite(m_State->FrictionTurnSpeed)) {
        error="invalid automobile position step"; return Status::InvalidInput;
    }
    auto next=*m_State;
    const auto velocity=Add(next.MoveSpeed,next.FrictionMoveSpeed);
    const auto delta=Scale(velocity,timeStep);
    for (std::size_t axis=0;axis<3;++axis) next.Matrix.Position[axis]+=delta[axis];
    next.FrictionMoveSpeed={}; next.FrictionTurnSpeed={};
    if (!Finite(next.Matrix)||!Finite(velocity)||!Finite(delta)) {
        error="automobile position overflow"; return Status::InvalidInput;
    }
    next.Revision++;
    return Publish(std::move(next),error);
}

NativeSourceAutomobileStatus NativeSourceAutomobile::EndControlFrame(std::string& error) {
    if (!m_State) { error="automobile not constructed"; return Status::InvalidInput; }
    auto next=*m_State;
    next.SuspensionCompression.fill(1.0f);
    next.VehicleCollisionProcessed=false;
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

NativeSourceAutomobileStatus NativeSourceAutomobile::ProcessWheels(
    const std::array<NativeSourceWheelContact,4>& contacts,float timeStep,std::string& error) {
    if (!m_State||!m_State->Occupants.Driver||!std::isfinite(timeStep)||timeStep<=0) {
        error="invalid automobile wheel step"; return Status::InvalidInput;
    }
    for (const auto& c:contacts) {
        if (!Finite(c.Forward)||!Finite(c.Right)||!Finite(c.Speed)||!Finite(c.Point)||
            !std::isfinite(c.Adhesion)||c.Adhesion<0) { error="invalid automobile wheel contact"; return Status::InvalidInput; }
        if (c.OnGround) {
            const float forwardSquared=Squared(c.Forward),rightSquared=Squared(c.Right),orthogonal=Dot(c.Forward,c.Right);
            if (!std::isfinite(forwardSquared)||!std::isfinite(rightSquared)||!std::isfinite(orthogonal)||
                std::abs(forwardSquared-1)>.001f||std::abs(rightSquared-1)>.001f||std::abs(orthogonal)>.001f) {
                error="unnormalized automobile wheel frame"; return Status::InvalidInput;
            }
        }
    }
    auto next=*m_State; next.Revision++; next.MoveForce={}; next.TurnForce={};
    const auto grounded=std::ranges::count_if(contacts,[](const auto& c){return c.OnGround;});
    if (!grounded) { error="source wheel response requires contacts"; return Status::Unsupported; }
    NativeTransmission transmission;
    transmission.Initialize({next.MaxVelocityKmh,next.EngineAcceleration,next.EngineInertia,next.Drag,
        next.Gears,next.DriveType,next.HandlingFlags});
    const float thrust=transmission.DriveAcceleration(next.GasPedal,next.Transmission,
        next.ForwardSpeed/NativeTransmission::UnitsPerSecond,timeStep,true);
    const float brake=next.BrakeDeceleration*next.BrakePedal*timeStep;
    bool alreadySkidding=next.WheelAlreadySkidding;
    for (std::size_t i=0;i<4;++i) {
        const auto& c=contacts[i]; if (!c.OnGround) continue;
        const bool front=i==0||i==2;
        const bool driven=next.DriveType=='4'||(front&&next.DriveType=='F')||(!front&&next.DriveType=='R');
        float fwd=driven?thrust:0,right=-Dot(c.Right,c.Speed)/float(grounded);
        float adhesion=c.Adhesion*timeStep;
        if (next.WheelStates[i]!=NativeSourceWheelState::Normal) {
            alreadySkidding=next.WheelAlreadySkidding=true; adhesion*=next.TractionLoss;
            if (next.WheelStates[i]==NativeSourceWheelState::Spinning&&
                (next.Status==NativeVehicleStatus::Player||next.Status==NativeVehicleStatus::RemoteControlled))
                adhesion*=1.0f-std::abs(next.GasPedal)*0.2f;
        }
        next.WheelStates[i]=NativeSourceWheelState::Normal;
        if (driven&&fwd!=0) right=std::clamp(right,-adhesion,adhesion);
        else if (const float speed=Dot(c.Forward,c.Speed); speed!=0) {
            fwd=-speed/float(grounded);
            if (brake>adhesion&&std::abs(speed)>0.005f) next.WheelStates[i]=NativeSourceWheelState::Fixed;
            else fwd=std::clamp(fwd,-brake,brake);
        }
        const float sq=right*right+fwd*fwd;
        if (sq>adhesion*adhesion&&next.WheelStates[i]!=NativeSourceWheelState::Fixed) {
            const float tractionLimit=(Dot(c.Forward,c.Speed)>.15f&&(i==0||i==2))?.6f:.3f;
            next.WheelStates[i]=driven&&tractionLimit*adhesion<std::abs(fwd)?NativeSourceWheelState::Spinning:NativeSourceWheelState::Skidding;
            float loss=alreadySkidding?1.0f:next.TractionLoss;
            if (!alreadySkidding&&next.WheelStates[i]==NativeSourceWheelState::Spinning&&
                (next.Status==NativeVehicleStatus::Player||next.Status==NativeVehicleStatus::RemoteControlled))
                loss*=1.0f-std::abs(next.GasPedal)*0.2f;
            const float scale=adhesion*loss/std::sqrt(sq); fwd*=scale; right*=scale;
        }
        if (fwd==0&&right==0) continue;
        NativeSourcePhysicalVector total{},turnDirection{};
        for (std::size_t axis=0;axis<3;++axis) {
            total[axis]=fwd*c.Forward[axis]+right*c.Right[axis];
            turnDirection[axis]=total[axis];
        }
        bool separateTurnForce=false;
        if (next.SuspensionAntiDive>0) {
            const float factor=brake!=0?next.SuspensionAntiDive:driven&&fwd!=0?0.5f*next.SuspensionAntiDive:0;
            separateTurnForce=factor!=0;
            for (std::size_t axis=0;axis<3;++axis) turnDirection[axis]-=factor*c.Forward[axis]*fwd;
        }
        float speed,turnSpeed;
        auto direction=total;
        speed=std::sqrt(Squared(direction));
        turnSpeed=separateTurnForce?std::sqrt(Squared(turnDirection)):speed;
        SourceNormalise(direction);
        if (separateTurnForce) SourceNormalise(turnDirection);
        else { turnDirection=direction; turnSpeed=speed; }
        const auto force=Scale(direction,speed*next.Mass);
        const auto centre=Vector(next.Matrix,next.CentreOfMass);
        const auto point=Sub(c.Point,centre);
        const auto arm=Cross(point,turnDirection);
        const float effective=1.0f/(Squared(arm)/next.TurnMass+1.0f/next.Mass);
        const auto torqueForce=Scale(turnDirection,turnSpeed*effective);
        const auto turnDelta=Scale(Cross(point,torqueForce),1.0f/next.TurnMass);
        for (std::size_t axis=0;axis<3;++axis) {
            next.MoveForce[axis]+=force[axis];
            next.MoveSpeed[axis]+=force[axis]/next.Mass;
            next.TurnForce[axis]+=turnDelta[axis];
            next.TurnSpeed[axis]+=turnDelta[axis];
        }
    }
    next.ForwardSpeed=Dot(next.MoveSpeed,next.Matrix.Basis[1]);
    if (!Finite(next.MoveForce)||!Finite(next.TurnForce)||!Finite(next.MoveSpeed)||!Finite(next.TurnSpeed)||
        !std::isfinite(next.ForwardSpeed)) { error="automobile wheel force overflow"; return Status::InvalidInput; }
    return Publish(std::move(next),error);
}

NativeSourceAutomobileStatus NativeSourceAutomobile::ProcessContacts(
    std::span<const NativeSourceAutomobileContact> contacts,std::string& error) {
    if (!m_State||contacts.size()>32) { error="invalid automobile contact count"; return Status::InvalidInput; }
    for (const auto& contact:contacts) if (!Finite(contact.Point)||!Finite(contact.Normal)||
        !std::isfinite(contact.Depth)||contact.Depth<0) {
        error="invalid automobile contact"; return Status::InvalidInput;
    }
    auto next=*m_State; next.Revision++; next.VehicleCollisionProcessed=next.Status!=NativeVehicleStatus::Simple;
    next.ContactCount=std::uint8_t(contacts.size()); next.HasHitWall=false;
    std::ranges::copy(contacts,next.Contacts.begin());
    if (contacts.size()<next.Contacts.size()) std::fill(next.Contacts.begin()+contacts.size(),next.Contacts.end(),NativeSourceAutomobileContact{});
    next.HasHitWall=std::ranges::any_of(contacts,[](const auto& contact){return contact.Kind==NativeSourceAutomobileContactKind::Building;});
    return Publish(std::move(next),error);
}

NativeSourceAutomobileStatus NativeSourceAutomobile::ProcessCollision(const NativeSourceAutomobileTarget& target,
    const NativeSourceSurfaces& surfaces,float timeStep,std::string& error) {
    if (!m_State||!target.Identity||!target.Collision||!target.InWorld||!target.UsesCollision||
        !std::isfinite(timeStep)||timeStep<=0) { error="invalid automobile collision input"; return Status::InvalidInput; }
    if (target.Kind!=NativeSourceAutomobileContactKind::Building||!target.Static||target.DisableCollisionForce||!target.Collidable) {
        error="automobile collision target branch unsupported"; return Status::Unsupported;
    }
    if (!surfaces.Snapshot().Loaded) { error="automobile collision surfaces unavailable"; return Status::InvalidInput; }
    if (m_State->SuspensionLines[0].LineLength<=0) { error="automobile suspension not prepared"; return Status::InvalidInput; }
    std::array<NativeSourceModelLine,4> lines;
    for (std::size_t i=0;i<4;++i) lines[i]={m_State->SuspensionLines[i].Start,m_State->SuspensionLines[i].End};
    float effectiveRadius=m_State->Assets->Collision->BoundRadius;
    for (const auto& line:lines) effectiveRadius=std::max(effectiveRadius,std::sqrt(Squared(line.End)));
    if (!std::isfinite(effectiveRadius)) { error="automobile collision bound overflow"; return Status::InvalidInput; }
    NativeSourceModelContacts detected;
    const auto query=NativeSourceProcessModels(*m_State->Assets->Collision,Transform(m_State->Matrix),lines,effectiveRadius,
        *target.Collision,target.Transform,false,detected);
    if (query==NativeSourceModelStatus::InvalidInput) { error="invalid automobile model query"; return Status::InvalidInput; }
    if (query==NativeSourceModelStatus::Overflow) { error="automobile model query overflow"; return Status::InvalidInput; }
    if (query!=NativeSourceModelStatus::Ok) { error="automobile model query unsupported"; return Status::Unsupported; }

    auto next=*m_State;
    next.Revision++; next.ContactCount=0; next.HasHitWall=false; next.MoveForce={}; next.TurnForce={};
    std::fill(next.Contacts.begin(),next.Contacts.end(),NativeSourceAutomobileContact{});
    for (std::size_t i=0;i<4;++i) if (detected.LineHits[i]&&detected.LineFractions[i]<next.SuspensionCompression[i]) {
        next.SuspensionCompression[i]=detected.LineFractions[i];
        next.WheelContactPoints[i]=detected.Lines[i];
    }
    for (std::size_t i=0;i<detected.SphereCount;++i) {
        const auto& contact=detected.Spheres[i];
        next.Contacts[next.ContactCount++]={target.Kind,contact.Point,contact.Normal,contact.Depth,
            contact.SurfaceB.Material,contact.SurfaceA.Piece};
    }
    next.VehicleCollisionProcessed=next.Status!=NativeVehicleStatus::Simple;
    next.HasHitWall=next.ContactCount>0;

    NativeSourcePhysicalVector accumulatedMove{},accumulatedTurn{};
    std::size_t accepted{};
    const auto centre=Vector(next.Matrix,next.CentreOfMass);
    for (std::size_t i=0;i<detected.SphereCount;++i) {
        const auto& contact=detected.Spheres[i];
        const auto point=Sub(contact.Point,next.Matrix.Position);
        const auto distance=Sub(point,centre);
        const auto speed=Add(next.MoveSpeed,next.FrictionMoveSpeed);
        const auto angular=Cross(Add(next.TurnSpeed,next.FrictionTurnSpeed),distance);
        const auto contactSpeed=Add(speed,angular);
        const float incoming=Dot(contact.Normal,contactSpeed);
        const auto lever=Cross(distance,contact.Normal);
        const float denominator=Squared(lever)/next.TurnMass+1.0f/next.Mass;
        if (!std::isfinite(incoming)||!std::isfinite(denominator)||denominator<=0) {
            error="automobile collision response overflow"; return Status::InvalidInput;
        }
        if (incoming>=0) continue;
        const float collisionMass=1.0f/denominator;
        const float damage=-((next.Elasticity+1.0f)*collisionMass*incoming);
        auto impulse=Scale(contact.Normal,damage);
        const auto velocityImpulse=Scale(impulse,next.HasHitWall&&Squared(next.MoveSpeed)>.1f?1.0f/next.Mass:1.2f/next.Mass);
        impulse=Scale(impulse,.8f);
        const auto turnImpulse=Scale(Cross(distance,impulse),1.0f/next.TurnMass);
        accumulatedMove=Add(accumulatedMove,velocityImpulse);
        accumulatedTurn=Add(accumulatedTurn,turnImpulse);
        float adhesive;
        if (surfaces.AdhesiveLimit(contact.SurfaceA.Material,contact.SurfaceB.Material,adhesive)!=NativeSourceSurfaceStatus::Ok) {
            error="automobile collision surface unavailable"; return Status::InvalidInput;
        }
        // Physical.cpp 4081-4121: ordinary non-boat/train/model400 branch.
        // Every contact first gets its share; the status/up/speed branch then
        // either suppresses, preserves or damage-scales that share.
        float friction=adhesive/float(detected.SphereCount);
        if (next.Status==NativeVehicleStatus::Wrecked) friction*=3.0f;
        else if (next.Matrix.Basis[2][2]>.3f&&Squared(next.MoveSpeed)<.02f&&Squared(next.TurnSpeed)<.01f) friction=0;
        else if (next.Status==NativeVehicleStatus::Abandoned||Dot(contact.Normal,next.Matrix.Basis[2])<.707f)
            friction=150.0f/next.Mass*friction*damage;
        if (!std::isfinite(friction)) { error="automobile collision friction overflow"; return Status::InvalidInput; }
        auto tangent=Sub(contactSpeed,Scale(contact.Normal,Dot(contactSpeed,contact.Normal)));
        float tangentMagnitude;
        if (Normalise(tangent,tangentMagnitude)) {
            const auto tangentArm=Cross(distance,tangent);
            const float tangentDenominator=Squared(tangentArm)/next.TurnMass+1.0f/next.Mass;
            if (!std::isfinite(tangentDenominator)||tangentDenominator<=0) {
                error="automobile collision friction mass overflow"; return Status::InvalidInput;
            }
            float frictionMass=-(tangentMagnitude/tangentDenominator);
            frictionMass=std::max(frictionMass,-friction);
            const auto frictionForce=Scale(tangent,frictionMass);
            const auto frictionMove=Scale(frictionForce,1.0f/next.Mass);
            const auto frictionTurn=Scale(Cross(distance,frictionForce),1.0f/next.TurnMass);
            next.FrictionMoveSpeed=Add(next.FrictionMoveSpeed,frictionMove);
            next.FrictionTurnSpeed=Add(next.FrictionTurnSpeed,frictionTurn);
            next.HasContacted=true;
        }
        ++accepted;
    }
    if (accepted) {
        const auto move=Scale(accumulatedMove,1.0f/float(accepted));
        const auto turn=Scale(accumulatedTurn,1.0f/float(accepted));
        next.MoveForce=Add(next.MoveForce,move); next.TurnForce=Add(next.TurnForce,turn);
        next.MoveSpeed=Add(next.MoveSpeed,move); next.TurnSpeed=Add(next.TurnSpeed,turn);
    }
    (void)timeStep; // used by the source outer driver; adopted response branches do not read it directly
    next.ForwardSpeed=Dot(next.MoveSpeed,next.Matrix.Basis[1]);
    if (!Finite(next.MoveForce)||!Finite(next.TurnForce)||!Finite(next.MoveSpeed)||!Finite(next.TurnSpeed)||
        !Finite(next.FrictionMoveSpeed)||!Finite(next.FrictionTurnSpeed)||!std::isfinite(next.ForwardSpeed)) {
        error="automobile collision force overflow"; return Status::InvalidInput;
    }
    return Publish(std::move(next),error);
}
