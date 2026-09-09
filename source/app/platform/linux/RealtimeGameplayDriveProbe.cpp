// Real LANDSTAL/IFP assets in a labelled flat geometry fixture. The separate
// NativeTransmissionProbe differentially checks original source arithmetic;
// this checks its gameplay integration, control transitions and render cadence.
#include "app/platform/linux/RealtimeGameplay.h"
#include "app/platform/linux/NativeTransmission.h"
#include "app/platform/linux/Handling.h"
#include "app/platform/linux/StreamPager.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

static void Check(bool condition,const char* message) {
    if (!condition) { std::fprintf(stderr,"gameplay-drive FAIL %s\n",message); std::exit(1); }
}
static void Advance(RealtimeGameplay& game,const RealtimeGameplayWorld& world,double seconds,int fps,RealtimeGameplayInput input) {
    double remaining=seconds;
    int frame=0;
    const double cadence[]={1.0/144,1.0/59,1.0/30,1.0/97};
    while (remaining>1e-10) {
        const double dt=std::min(remaining,fps ? 1.0/fps:cadence[frame++%4]);
        game.Tick(dt,input,world); remaining-=dt;
    }
}
static void Enter(RealtimeGameplay& game,const RealtimeGameplayWorld& world) {
    std::string error;
    Check(game.Spawn(world,0,0,20,0,error),error.c_str());
    Advance(game,world,0.5,60,{.Side=1});
    game.Tick(0.001,{.Interact=true},world);
    Check(game.State().InVehicle && game.State().Speed==0,"stationary entry before first engine tick");
}
int main(int argc,char** argv) {
    const char* gameDir=argc>1 ? argv[1]:"/game";
    char err[512]={}; std::string error;
    E2ELoadInfo load;
    Check(StreamPager_Init(gameDir,load,err,sizeof(err),{true,900.0f,4096}),err);
    RealtimeGameplay game; Check(game.Initialize(gameDir,error),error.c_str());
    WorldShotScene floor;
    floor.meshes.emplace_back();
    // Road-sized triangles, not a single kilometre-scale face (the pedestrian
    // controller performs single-precision closest-point queries at entry).
    for (float x=-100;x<1000;x+=50) for (float y=-100;y<100;y+=50) {
        const float triangles[]={x,y,7,x+50,y,7,x+50,y+50,7,x,y,7,x+50,y+50,7,x,y+50,7};
        floor.meshes[0].pos.insert(floor.meshes[0].pos.end(),std::begin(triangles),std::end(triangles));
        floor.meshes[0].tris+=2;
    }
    RealtimeGameplayWorld world; Check(world.Rebuild(floor,error),error.c_str());
    HandlingParams handling;
    Check(Handling_Load(gameDir,"landstal",handling,err,sizeof(err)),err);
    // Actual source-checked LANDSTAL configuration (flags checked by the
    // independent asset-reading oracle); no alternate gameplay speed formula.
    NativeTransmission reference;
    reference.Initialize({static_cast<float>(handling.vmaxFileKmh),static_cast<float>(handling.accelFile),
        static_cast<float>(handling.inertia),static_cast<float>(handling.drag),static_cast<std::uint8_t>(handling.gears),handling.driveType,0x500002});
    NativeTransmission::State transmission;
    float expected=0,atLaunch=0;
    std::uint8_t launchGear=1;
    for (int tick=1;tick<=150;++tick) {
        expected+=reference.DriveAcceleration(1,transmission,expected/50,NativeTransmission::TimeStep)*reference.DrivenWheels()*50;
        expected=reference.AirResistance(expected,NativeTransmission::TimeStep);
        if (tick==24) { atLaunch=expected; launchGear=transmission.CurrentGear; }
    }
    float baselineDistance=0;
    for (int fps:{30,60,144,0}) {
        Enter(game,world);
        Advance(game,world,0.8,fps,{.Forward=1});
        Check(std::abs(game.State().Speed-atLaunch)<1e-4f,"0.8s gameplay launch matches source-checked transmission");
        Check(game.State().Gear==launchGear,"source gear at 0.8s");
        Advance(game,world,4.2,fps,{.Forward=1});
        const float speed=game.State().Speed,distance=static_cast<float>(game.State().DriveDistance);
        Check(std::abs(speed-expected)<1e-4f,"5s velocity independent of render cadence");
        Check(game.State().Gear==transmission.CurrentGear && game.State().BlockedSteps==0,"gear continuity and unobstructed drive");
        if (fps==30) baselineDistance=distance;
        // Position uses the existing <=1/120s collision integrator. Its sample
        // phase can differ by at most one substep of travel; engine cadence cannot.
        Check(std::abs(distance-baselineDistance)<speed/120.0f,"distance consistent within collision sampling interval");
        std::printf("gameplay curve fps=%d t=.8 speedMs=%.6f t=5 speedMs=%.6f gear=%u distance=%.6f\n",
            fps,atLaunch,speed,game.State().Gear,distance);
        Advance(game,world,2,fps,{});
        Check(game.State().Speed>0 && game.State().Speed<speed,"throttle release coasts without reversal");
        const float coast=game.State().Speed;
        Advance(game,world,6,fps,{.Brake=true});
        Check(game.State().Speed==0,"service brake stops without reverse");
        Advance(game,world,1,fps,{.Forward=-1});
        Check(game.State().Speed< -1 && game.State().Gear==0,"reverse from rest");
        Advance(game,world,4,fps,{.Brake=true});
        Check(game.State().Speed==0,"reverse braking stops without forward creep");
        Advance(game,world,0.8,fps,{.Forward=1});
        Check(game.State().Speed>1 && game.State().Gear>=1,"forward drive after reverse");
        Advance(game,world,3,fps,{.Handbrake=true});
        Check(game.State().Speed==0,"handbrake controller stops car");
        Advance(game,world,1,fps,{.Forward=1});
        const float forward=game.State().Speed;
        Advance(game,world,0.2,fps,{.Forward=-1});
        Check(game.State().Speed>0 && game.State().Speed<forward,"opposite input brakes before reversing");
        std::printf("controls fps=%d coast2s=%.6f release/brake/reverse/handbrake/opposite=PASS\n",fps,coast);
    }
    std::puts("realtime-gameplay-drive-probe PASS (flat no-slip transmission; scalar brake controller, not tire parity)");
    StreamPager_Shutdown();
}
