#include "NativeSourceAutomobile.h"
#include "NativeCarGenerators.h"
#include "NativeCollisionAssets.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <ranges>
#include <stdexcept>
#include <string>

namespace {
std::size_t s_Checks;
void Check(bool condition,const std::string& message) {
    ++s_Checks; if (!condition) throw std::runtime_error(message);
}
NativeGarageMatrix Matrix() { return {{10,20,30},{{{1,0,0},{0,1,0},{0,0,1}}}}; }
std::string AdhesiveMatrix() {
    std::string text;
    for (int row=0;row<6;++row) { text+="ignored"; for (int column=0;column<=row;++column) text+=" 1"; text+='\n'; }
    return text;
}
std::string SurfaceRow(const char* name,const char* group) {
    std::string text=std::string(name)+" "+group+" 1 0 DEFAULT NONE";
    for (int i=0;i<29;++i) text+=" 0";
    return text+" NONE\n";
}
NativeSourceSurfaces Surfaces() {
    NativeSourceSurfaces surfaces; std::string error;
    Check(surfaces.LoadBytes(AdhesiveMatrix(),SurfaceRow("DEFAULT","RUBBER")+SurfaceRow("TARMAC","HARD")+
        SurfaceRow("CAR","HARD")+SurfaceRow("WHEELBASE","ROAD"),error),error);
    return surfaces;
}
std::shared_ptr<NativeCollisionModel> CollisionFloor() {
    auto model=std::make_shared<NativeCollisionModel>();
    model->Name="automobile-floor"; model->Version=2; model->Flags=2;
    model->Min={-4,-4,-.125f}; model->Max={4,4,.125f}; model->BoundRadius=6;
    model->Vertices={{-4,-4,0},{4,-4,0},{4,4,0},{-4,4,0}};
    model->Faces={{{0,1,2},{1,0,0,10}},{{0,2,3},{1,0,0,10}}};
    return model;
}
std::shared_ptr<NativeCollisionModel> CollisionWall(float y) {
    auto model=std::make_shared<NativeCollisionModel>();
    model->Name="automobile-wall"; model->Version=2; model->Flags=2;
    model->Min={-4,y-.125f,-2}; model->Max={4,y+.125f,3}; model->BoundRadius=6;
    model->Boxes.push_back({model->Min,model->Max,{1,0,0,10}});
    return model;
}
NativeSourceAutomobileTarget Target(std::shared_ptr<const NativeCollisionModel> model) {
    NativeSourceAutomobileTarget target;
    target.Identity=9001; target.Collision=std::move(model); target.Kind=NativeSourceAutomobileContactKind::Building;
    target.InWorld=target.UsesCollision=target.Static=target.Collidable=true;
    return target;
}
}

int main(int argc,char** argv) try {
    Check(argc==2,"usage: NativeSourceAutomobileProbe /game");
    std::string error;
    NativeCarGenerators definitions;
    Check(definitions.LoadBeforeWorker(argv[1],0,error),error);
    Check(definitions.ModelDefinitions().size()==212,"actual vehicle definition census");
    const auto* landstal=definitions.FindModel(400);
    Check(landstal,"actual model400 definition");
    Check(landstal->ModelName=="landstal"&&landstal->TextureName=="landstal"&&
        landstal->HandlingName=="LANDSTAL"&&landstal->Type==NativeVehicleType::Automobile&&
        landstal->TypeName=="car","exact actual model/txd/handling/type identity");
    NativeCollisionPopulation population; population.IncludesStreamed=true;
    for (const auto& definition:definitions.ModelDefinitions())
        population.Models.emplace(definition.ModelId,NativeCollisionIde{definition.ModelName,false});
    NativeCollisionAssets collision;
    Check(collision.Load(argv[1],population,error),error);
    NativeGeneratedVehicleAsset asset;
    const auto loaded=NativeGeneratedVehicleAssets_Load(argv[1],*landstal,collision,asset);
    Check(bool(loaded),loaded.Detail);
    Check(asset->Definition.ModelId==400&&asset->DffSource=="gta3.img:landstal.dff"&&
        asset->ModelTxdSource=="gta3.img:landstal.txd"&&asset->CommonTxdSource=="models/generic/vehicle.txd",
        "exact source DFF/TXD identity");
    Check(asset->Collision&&asset->Collision->Name=="landstal_col"&&asset->EmbeddedCollision,
        "exact embedded source collision identity");
    HandlingParams handling; char message[512]{};
    Check(Handling_Load(argv[1],landstal->HandlingName.c_str(),handling,message,sizeof(message)),message);
    Check(std::strcmp(handling.model,"LANDSTAL")==0,"actual handling row identity");

    NativeSourceAutomobile automobile;
    Check(automobile.Construct(77,*landstal,asset,handling,NativeVehicleCreatedBy::Mission,Matrix(),error)==
        NativeSourceAutomobileStatus::Ready,error);
    auto constructed=automobile.LastCommitted();
    Check(constructed&&constructed->Identity==77&&constructed->Revision==1,"constructed owner identity");
    Check(constructed->ModelId==400&&constructed->ModelName=="landstal"&&constructed->TextureName=="landstal"&&
        constructed->HandlingName=="LANDSTAL"&&constructed->Type==NativeVehicleType::Automobile,
        "constructed exact source identities");
    Check(constructed->Status==NativeVehicleStatus::Simple&&constructed->CreatedBy==NativeVehicleCreatedBy::Mission,
        "source constructor status/created-by");
    Check(constructed->Mass==float(handling.mass)&&constructed->TurnMass==float(handling.turnMass)&&
        constructed->Drag==float(handling.drag)&&constructed->MaxVelocityKmh==float(handling.vmaxFileKmh)&&
        constructed->EngineAcceleration==float(handling.accelFile)&&constructed->EngineInertia==float(handling.inertia)&&
        constructed->Gears==handling.gears&&constructed->DriveType==handling.driveType&&
        constructed->EngineType==handling.engineType,"exact actual handling construction");
    Check(constructed->CentreOfMass==std::array<float,3>{float(handling.centreOfMass[0]),float(handling.centreOfMass[1]),float(handling.centreOfMass[2])}&&
        constructed->PercentSubmerged==float(handling.percentSubmerged)&&constructed->TractionMult==float(handling.tractionMult)&&
        constructed->TractionLoss==float(handling.tractionLoss)&&constructed->TractionBias==float(handling.tractionBias),
        "source constructor centre/buoyancy/traction handling");
    Check(constructed->BrakeDeceleration==float(handling.brakeDeceleration)&&
        constructed->BrakeBias==float(handling.brakeBias)&&constructed->Abs==handling.Abs&&
        constructed->SteeringLockDegrees==float(handling.steeringLockDegrees)&&
        constructed->HandlingFlags==handling.HandlingFlags,"source brake/steering/flag handling");
    Check(constructed->ModelFlags==handling.ModelFlags&&constructed->SuspensionForce==float(handling.suspensionForce)&&
        constructed->SuspensionDamping==float(handling.suspensionDamping)&&
        constructed->SuspensionHighSpeedDamping==float(handling.suspensionHighSpeedDamping)&&
        constructed->SuspensionUpper==float(handling.suspensionUpper)&&constructed->SuspensionLower==float(handling.suspensionLower)&&
        constructed->SuspensionBias==float(handling.suspensionBias)&&constructed->SuspensionAntiDive==float(handling.suspensionAntiDive),
        "source complete suspension handling");
    Check(constructed->Elasticity==0.05f&&constructed->BrakeCount==20&&constructed->TireTemperature==1,
        "source automobile literal initial state");
    Check(constructed->WheelRotation==std::array<float,4>{}&&constructed->WheelSpeed==std::array<float,4>{}&&
        constructed->SuspensionCompression==std::array<float,4>{1,1,1,1},"source wheel initial state");
    Check(constructed->Occupants.MaxPassengers==3&&!constructed->Occupants.Driver&&
        constructed->Occupants.PassengerCount==0,"source occupant initial state");
    Check(constructed->Assets==asset,"exact asset owner retained");
    const auto constructedCopy=*constructed;

    CarPoseMeasure measure{};
    Check(CarPose_Measure(argv[1],"landstal",measure,message,sizeof(message)),message);
    Check(automobile.SetupSuspension(measure,error)==NativeSourceAutomobileStatus::Ready,error);
    auto suspended=automobile.LastCommitted();
    Check(suspended->SuspensionLines[0].Start[1]==float(measure.frontY)&&
        suspended->SuspensionLines[1].Start[1]==float(measure.rearY),"source front/rear wheel dummy suspension identity");
    Check(suspended->SuspensionLines[0].SpringLength==suspended->SuspensionUpper-suspended->SuspensionLower&&
        suspended->SuspensionLines[0].LineLength==suspended->SuspensionUpper-suspended->SuspensionLower+float(measure.wheelR)/2,
        "source suspension line arithmetic");
    auto wrongMeasure=measure; std::strcpy(wrongMeasure.model,"rustler");
    Check(automobile.SetupSuspension(wrongMeasure,error)==NativeSourceAutomobileStatus::InvalidInput&&
        automobile.LastCommitted()==suspended,"mismatched suspension asset atomic");

    auto wrong=*landstal; wrong.ModelId=476;
    Check(automobile.Construct(88,wrong,asset,handling,NativeVehicleCreatedBy::Mission,Matrix(),error)==
        NativeSourceAutomobileStatus::InvalidInput,"no 476 fallback/substitution");
    wrong=*landstal; wrong.ModelName="fallback400";
    Check(automobile.Construct(88,wrong,asset,handling,NativeVehicleCreatedBy::Mission,Matrix(),error)==
        NativeSourceAutomobileStatus::InvalidInput,"no name fallback to400");
    wrong=*landstal; wrong.Type=NativeVehicleType::Plane;
    Check(automobile.Construct(88,wrong,asset,handling,NativeVehicleCreatedBy::Mission,Matrix(),error)==
        NativeSourceAutomobileStatus::Unsupported,"non-automobile rejected");
    auto wrongHandling=handling; std::strcpy(wrongHandling.model,"RUSTLER");
    Check(automobile.Construct(88,*landstal,asset,wrongHandling,NativeVehicleCreatedBy::Mission,Matrix(),error)==
        NativeSourceAutomobileStatus::InvalidInput,"mismatched handling owner rejected");
    auto nonfinite=Matrix(); nonfinite.Position[0]=__builtin_nanf("");
    Check(automobile.Construct(88,*landstal,asset,handling,NativeVehicleCreatedBy::Mission,nonfinite,error)==
        NativeSourceAutomobileStatus::InvalidInput,"nonfinite matrix rejected");
    Check(automobile.LastCommitted()==suspended&&*constructed==constructedCopy,"construction failures retain publication");

    Check(automobile.SetDriver(1001,error)==NativeSourceAutomobileStatus::Ready,error);
    auto driver=automobile.LastCommitted();
    Check(driver->Revision==3&&driver->Occupants.Driver==1001,"exact driver identity attached");
    Check(automobile.SetDriver(1002,error)==NativeSourceAutomobileStatus::Occupied,"second driver rejected");
    Check(automobile.LastCommitted()==driver,"driver collision atomic");
    Check(automobile.ProcessPlayerControls(255,0,128,false,false,0,1,error)==NativeSourceAutomobileStatus::Ready,error);
    auto controls=automobile.LastCommitted();
    Check(controls->GasPedal==1&&controls->BrakePedal==0&&controls->RawSteerAngle==-0.2f&&
        controls->SteerAngle<0&&!controls->Handbrake&&!controls->DoingBurnout,"source accelerate/steer input");
    Check(automobile.ProcessPlayerControls(0,255,0,false,false,1,1,error)==NativeSourceAutomobileStatus::Ready,error);
    Check(automobile.LastCommitted()->GasPedal==0&&automobile.LastCommitted()->BrakePedal==1,"source reverse input brakes forward motion");
    Check(automobile.ProcessPlayerControls(255,255,0,false,false,0,1,error)==NativeSourceAutomobileStatus::Ready,error);
    Check(automobile.LastCommitted()->DoingBurnout&&automobile.LastCommitted()->GasPedal==1&&
        automobile.LastCommitted()->BrakePedal==1,"source stationary burnout controls");
    Check(automobile.ProcessPlayerControls(255,0,0,false,true,0,1,error)==NativeSourceAutomobileStatus::Ready,error);
    Check(automobile.LastCommitted()->Handbrake&&automobile.LastCommitted()->GasPedal==0&&
        automobile.LastCommitted()->BrakePedal==1,"source automatic handbrake controls");
    const auto beforeBadControl=automobile.LastCommitted();
    Check(automobile.ProcessPlayerControls(1,2,129,false,false,0,1,error)==NativeSourceAutomobileStatus::InvalidInput&&
        automobile.LastCommitted()==beforeBadControl,"invalid controls retain publication");
    Check(automobile.ProcessPlayerControls(255,0,0,false,false,0,1,error)==NativeSourceAutomobileStatus::Ready,error);
    Check(automobile.AdvanceDrive(NativeTransmission::TimeStep,true,error)==NativeSourceAutomobileStatus::Ready,error);
    const auto launch=automobile.LastCommitted();
    Check(launch->ForwardSpeed>0&&launch->Transmission.CurrentGear==1,"source transmission launches common automobile");
    Check(automobile.ProcessPlayerControls(0,255,0,false,false,launch->ForwardSpeed,1,error)==NativeSourceAutomobileStatus::Ready,error);
    Check(automobile.AdvanceDrive(NativeTransmission::TimeStep,true,error)==NativeSourceAutomobileStatus::Ready,error);
    Check(automobile.LastCommitted()->ForwardSpeed<launch->ForwardSpeed,"source brake decelerates common automobile");
    const auto beforeBadDrive=automobile.LastCommitted();
    Check(automobile.AdvanceDrive(0,true,error)==NativeSourceAutomobileStatus::InvalidInput&&
        automobile.LastCommitted()==beforeBadDrive,"invalid drive step retains publication");
    std::array<NativeSourceWheelContact,4> contacts;
    for (auto& contact:contacts) contact={{0,1,0},{1,0,0},{1,2,0},{0,1,-1},0.1f,true};
    Check(automobile.ProcessWheels(contacts,NativeTransmission::TimeStep,error)==NativeSourceAutomobileStatus::Ready,error);
    auto wheels=automobile.LastCommitted();
    Check(wheels->MoveForce[0]<0&&std::isfinite(wheels->MoveForce[1]),"source wheel lateral/drive forces accumulated");
    Check(std::ranges::any_of(wheels->WheelStates,[](auto state){return state!=NativeSourceWheelState::Normal;}),
        "source wheel traction state transition");
    Check(wheels->MoveSpeed[0]<0&&wheels->ForwardSpeed==wheels->MoveSpeed[1]&&std::isfinite(wheels->TurnSpeed[2]),
        "source wheel forces update owned linear/angular speed");
    auto zeroContacts=contacts;
    for (auto& contact:zeroContacts) contact.Speed={};
    Check(automobile.ProcessPlayerControls(0,0,0,false,false,0,1,error)==NativeSourceAutomobileStatus::Ready,error);
    Check(automobile.ProcessWheels(zeroContacts,NativeTransmission::TimeStep,error)==NativeSourceAutomobileStatus::Ready,error);
    auto zeroForces=automobile.LastCommitted();
    Check(zeroForces->MoveForce==NativeSourcePhysicalVector{}&&zeroForces->TurnForce==NativeSourcePhysicalVector{}&&
        zeroForces->WheelAlreadySkidding,"per-call force trace resets and source skid scratch persists");
    const auto beforeBadWheels=zeroForces; contacts[0].Adhesion=-1;
    Check(automobile.ProcessWheels(contacts,1,error)==NativeSourceAutomobileStatus::InvalidInput&&
        automobile.LastCommitted()==beforeBadWheels,"invalid wheel contact retains publication");
    const std::array collisionContacts{
        NativeSourceAutomobileContact{NativeSourceAutomobileContactKind::Building,{1,2,3},{0,0,1},.2f,1,0},
        NativeSourceAutomobileContact{NativeSourceAutomobileContactKind::Vehicle,{2,3,4},{1,0,0},.1f,4,1}};
    Check(automobile.ProcessContacts(collisionContacts,error)==NativeSourceAutomobileStatus::Ready,error);
    auto collided=automobile.LastCommitted();
    Check(collided->ContactCount==2&&collided->HasHitWall&&collided->Contacts[0]==collisionContacts[0]&&
        collided->Contacts[1]==collisionContacts[1],"source automobile contact provenance/order");
    Check(!collided->VehicleCollisionProcessed,"simple status collision flag source behavior");
    const auto beforeBadContacts=collided; auto badContacts=collisionContacts; badContacts[0].Depth=-1;
    Check(automobile.ProcessContacts(badContacts,error)==NativeSourceAutomobileStatus::InvalidInput&&
        automobile.LastCommitted()==beforeBadContacts,"invalid collision retains publication");

    // Actual Landstal COL supplies A's authored18 spheres/10 triangles and
    // the DFF-derived suspension lines; only the isolated targets are fixtures.
    auto modelCar=automobile.LastCommitted();
    Check(modelCar->Assets->Collision->Spheres.size()==18&&modelCar->Assets->Collision->Faces.size()==10,
        "actual Landstal source collision primitives retained");
    auto floor=Target(CollisionFloor());
    floor.Transform.Position={10,20,29.5f};
    Check(automobile.ProcessCollision(floor,Surfaces(),NativeTransmission::TimeStep,error)==NativeSourceAutomobileStatus::Ready,error);
    auto grounded=automobile.LastCommitted();
    Check(std::ranges::any_of(grounded->SuspensionCompression,[](float value){return value<1;}),
        "source suspension lines hit loaded collision floor");
    Check(std::ranges::any_of(grounded->WheelContactPoints,[](const auto& point){return point.SurfaceB.Material==1;}),
        "source wheel contact retains target material");
    auto wall=Target(CollisionWall(1.9f)); wall.Transform.Position=Matrix().Position;
    auto beforeWall=*grounded; beforeWall.MoveSpeed={0,1,0}; beforeWall.ForwardSpeed=1;
    // Construct a second exact owner at the same source identity and drive it
    // to a deterministic wall-facing speed through public source transitions.
    NativeSourceAutomobile impact;
    Check(impact.Construct(78,*landstal,asset,handling,NativeVehicleCreatedBy::Mission,Matrix(),error)==NativeSourceAutomobileStatus::Ready,error);
    const auto preDriverStatus=impact.LastCommitted();
    Check(impact.SetStatus(NativeVehicleStatus::Player,error)==NativeSourceAutomobileStatus::InvalidInput&&
        impact.LastCommitted()==preDriverStatus,"player status requires driver owner");
    Check(impact.SetupSuspension(measure,error)==NativeSourceAutomobileStatus::Ready&&
        impact.SetDriver(1003,error)==NativeSourceAutomobileStatus::Ready,error);
    Check(impact.SetStatus(NativeVehicleStatus::Player,error)==NativeSourceAutomobileStatus::Ready,
        "entering driver owns source player automobile status");
    const auto playerStatus=impact.LastCommitted();
    Check(impact.SetStatus(static_cast<NativeVehicleStatus>(0xfe),error)==NativeSourceAutomobileStatus::InvalidInput&&
        impact.LastCommitted()==playerStatus,"unknown source status retains publication");
    Check(impact.ProcessPlayerControls(255,0,0,false,false,0,1,error)==NativeSourceAutomobileStatus::Ready,error);
    for (int i=0;i<8;++i) Check(impact.AdvanceDrive(NativeTransmission::TimeStep,true,error)==NativeSourceAutomobileStatus::Ready,error);
    const auto preImpact=impact.LastCommitted();
    Check(preImpact->ForwardSpeed>0&&preImpact->MoveSpeed[1]==preImpact->ForwardSpeed,"accelerated owner carries source forward velocity");
    NativeSourceSurfaces unloadedSurfaces;
    Check(impact.ProcessCollision(wall,unloadedSurfaces,NativeTransmission::TimeStep,error)==NativeSourceAutomobileStatus::InvalidInput&&
        impact.LastCommitted()==preImpact,"missing source surface owner retains automobile publication");
    Check(impact.ProcessCollision(wall,Surfaces(),NativeTransmission::TimeStep,error)==NativeSourceAutomobileStatus::Ready,error);
    auto hit=impact.LastCommitted();
    Check(hit->ContactCount>0&&hit->HasHitWall&&hit->Contacts[0].Kind==NativeSourceAutomobileContactKind::Building,
        "real Landstal spheres detect ordered wall contact");
    Check(hit->VehicleCollisionProcessed,"non-simple source automobile marks collision processed");
    Check(hit->ForwardSpeed<preImpact->ForwardSpeed&&hit->MoveSpeed[1]<preImpact->MoveSpeed[1],
        "source static collision response opposes incoming automobile speed");
    const auto beforeBadCollision=impact.LastCommitted(); auto badTarget=wall; badTarget.Kind=NativeSourceAutomobileContactKind::Vehicle;
    Check(impact.ProcessCollision(badTarget,Surfaces(),1,error)==NativeSourceAutomobileStatus::Unsupported&&
        impact.LastCommitted()==beforeBadCollision,"unsupported dynamic response retains publication");
    Check(automobile.AddPassenger(2001,0,error)==NativeSourceAutomobileStatus::Ready,error);
    Check(automobile.AddPassenger(2002,2,error)==NativeSourceAutomobileStatus::Ready,error);
    auto occupied=automobile.LastCommitted();
    Check(occupied->Occupants.PassengerCount==2&&occupied->Occupants.Passengers[0]==2001&&
        occupied->Occupants.Passengers[1]==0&&occupied->Occupants.Passengers[2]==2002,"sparse exact passenger seats");
    Check(automobile.AddPassenger(2003,2,error)==NativeSourceAutomobileStatus::Occupied,"occupied seat rejected");
    Check(automobile.AddPassenger(1001,1,error)==NativeSourceAutomobileStatus::Occupied,"driver cannot duplicate as passenger");
    Check(automobile.AddPassenger(2003,3,error)==NativeSourceAutomobileStatus::InvalidInput,"seat bound by model capacity");
    Check(automobile.LastCommitted()==occupied,"passenger rejections retain publication");
    Check(automobile.RemoveOccupant(2001,error)==NativeSourceAutomobileStatus::Ready,error);
    Check(automobile.LastCommitted()->Occupants.PassengerCount==1&&!automobile.LastCommitted()->Occupants.Passengers[0],
        "passenger removal exact");
    Check(automobile.RemoveOccupant(1001,error)==NativeSourceAutomobileStatus::Ready,error);
    Check(!automobile.LastCommitted()->Occupants.Driver,"driver removal exact");
    Check(automobile.RemoveOccupant(9999,error)==NativeSourceAutomobileStatus::InvalidInput,"foreign occupant rejected");
    Check(*constructed==constructedCopy,"held constructed publication immutable");

    std::printf("sa-core-automobile-ok checks=%zu model=400 name=landstal handling=LANDSTAL occupants=exact fallback=none tris=%d frames=%zu\n",
        s_Checks,constructed->Assets->Scene.stats.triangles,constructed->Assets->Frames.size());
    return 0;
} catch (const std::exception& e) {
    std::fprintf(stderr,"sa-core-automobile-failed %s\n",e.what()); return 1;
}
