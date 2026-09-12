#include "NativeSourceAutomobile.h"
#include "NativeCarGenerators.h"
#include "NativeCollisionAssets.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>

namespace {
std::size_t s_Checks;
void Check(bool condition,const std::string& message) {
    ++s_Checks; if (!condition) throw std::runtime_error(message);
}
NativeGarageMatrix Matrix() { return {{10,20,30},{{{1,0,0},{0,1,0},{0,0,1}}}}; }
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
    Check(constructed->Elasticity==0.05f&&constructed->BrakeCount==20&&constructed->TireTemperature==1,
        "source automobile literal initial state");
    Check(constructed->WheelRotation==std::array<float,4>{}&&constructed->WheelSpeed==std::array<float,4>{}&&
        constructed->SuspensionCompression==std::array<float,4>{1,1,1,1},"source wheel initial state");
    Check(constructed->Occupants.MaxPassengers==3&&!constructed->Occupants.Driver&&
        constructed->Occupants.PassengerCount==0,"source occupant initial state");
    Check(constructed->Assets==asset,"exact asset owner retained");
    const auto constructedCopy=*constructed;

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
    Check(automobile.LastCommitted()==constructed&&*constructed==constructedCopy,"construction failures retain publication");

    Check(automobile.SetDriver(1001,error)==NativeSourceAutomobileStatus::Ready,error);
    auto driver=automobile.LastCommitted();
    Check(driver->Revision==2&&driver->Occupants.Driver==1001,"exact driver identity attached");
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
