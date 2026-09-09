// Standalone headless probe, deliberately not part of mad-sa-linux's main.
// Build/run with RealtimeGameplayProbe.py in the existing dev container.
#include "app/platform/linux/RealtimeGameplay.h"
#include "app/platform/linux/CarPose.h"
#include "app/platform/linux/StreamPager.h"
#include <rw.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

static void Check(bool condition,const char* message) {
    if (!condition) { std::fprintf(stderr,"realtime-gameplay-probe FAIL %s\n",message); std::exit(1); }
}
static void Triangle(WorldShotMesh& m,RealtimeVec3 a,RealtimeVec3 b,RealtimeVec3 c) {
    for (auto p:{a,b,c}) { m.pos.push_back(p.X); m.pos.push_back(p.Y); m.pos.push_back(p.Z); }
    ++m.tris;
}
static WorldShotScene Fixture() {
    WorldShotScene s{};
    s.meshes.emplace_back(); auto& m=s.meshes.back();
    Triangle(m,{-100,-100,7},{100,-100,7},{100,100,7});
    Triangle(m,{-100,-100,7},{100,100,7},{-100,100,7});
    Triangle(m,{8,-20,7},{8,20,7},{8,20,15});
    Triangle(m,{8,-20,7},{8,20,15},{8,-20,15});
    Triangle(m,{20,20,14},{30,20,14},{30,30,14});
    Triangle(m,{20,20,14},{30,30,14},{20,30,14});
    return s;
}
static void Frames(RealtimeGameplay& game,const RealtimeGameplayWorld& world,int count,RealtimeGameplayInput input={}) {
    for (int i=0;i<count;++i) { game.Tick(1.0/60.0,input,world); input.Jump=input.Interact=false; }
}
static void WheelAudit(const char* gameDir,const RealtimeGameplay& game) {
    const auto& state=game.State();
    WorldShotScene reference{}; CarPoseStats stats{}; CarPoseAudit audit{};
    char error[512]={};
    auto* saved=rw::TexDictionary::getCurrent();
    constexpr float pi=3.14159265358979323846f;
    Check(CarPose_Init(gameDir,"landstal",state.Steer*180/pi,state.WheelSpin*180/pi,reference,stats,audit,error,sizeof(error)),error);
    CarPose_Shutdown(); rw::TexDictionary::setCurrent(saved);
    const auto& actors=game.Actors();
    const size_t base=actors.meshes.size()-reference.meshes.size();
    const float yaw=state.CarHeading-pi*0.5f,c=std::cos(yaw),s=std::sin(yaw);
    float maxError=0;
    for (size_t i=0;i<reference.meshes.size();++i) {
        const auto& expected=reference.meshes[i].pos;
        const auto& actual=actors.meshes[base+i].pos;
        Check(expected.size()==actual.size(),"car topology matches CarPose");
        for (size_t v=0;v<expected.size();v+=3) {
            const float x=c*expected[v]-s*expected[v+1]+state.Car.X;
            const float y=s*expected[v]+c*expected[v+1]+state.Car.Y;
            maxError=std::max({maxError,std::abs(x-actual[v]),std::abs(y-actual[v+1]),std::abs(expected[v+2]+state.Car.Z-actual[v+2])});
        }
    }
    std::printf("wheel audit maxVertexError=%.8f carposeTris=%d wheels=%d\n",maxError,stats.tris,stats.wheels);
    Check(maxError<0.001f,"persistent wheel pose equals real CarPose dummy transforms");
}
static RealtimeGameplayState Replay(const char* gameDir,RealtimeGameplay& game,const RealtimeGameplayWorld& world,bool audit=false) {
    std::string error;
    Check(game.Spawn(world,0,0,20,0,error),error.c_str());
    const auto idle=game.Actors().meshes[0].pos;
    Frames(game,world,30);
    Check(idle!=game.Actors().meshes[0].pos,"real IFP idle deforms stationary ped");
    const float firstVertex=game.Actors().meshes[0].pos[0];
    Frames(game,world,60,{.Forward=1});
    Check(game.State().Ped.X>1.9f && game.State().Grounded,"persistent grounded walk");
    Check(game.Actors().meshes[0].pos[0]!=firstVertex,"animated/placed actor geometry updates");
    Frames(game,world,1,{.Jump=true});
    float apex=game.State().Ped.Z;
    for (int i=0;i<90;++i) { Frames(game,world,1); apex=std::max(apex,game.State().Ped.Z); }
    Check(apex>8.0f && game.State().Grounded && std::abs(game.State().Ped.Z-7)<0.01f,"gravity jump and real triangle landing");
    Check(game.State().Jumps==1 && game.State().Landings==1,"jump edge once and landing transition");
    Frames(game,world,45,{.Forward=-1});
    Frames(game,world,30,{.Side=1});
    Frames(game,world,1,{.Interact=true});
    std::printf("replay entry ped=(%.3f %.3f %.3f) car=(%.3f %.3f %.3f) entered=%d\n",
        game.State().Ped.X,game.State().Ped.Y,game.State().Ped.Z,game.State().Car.X,game.State().Car.Y,game.State().Car.Z,game.State().InVehicle);
    Check(game.State().InVehicle && game.State().Entries==1,"nearby real car entry");
    Check(game.Actors().meshes[0].tris==0,"ped hidden while seated");
    Frames(game,world,20,{.Forward=1,.Side=0.25f});
    Check(game.State().Speed>1 && std::abs(game.State().Steer)>0.01f,"handling acceleration and steering");
    if (audit) WheelAudit(gameDir,game);
    Frames(game,world,200,{.Forward=1});
    Check(game.State().DriveDistance>2 && game.State().BlockedSteps>0 && game.State().Car.X<7,"solid wall blocks moving car");
    Check(std::abs(game.State().WheelSpin)>0.01f,"wheel roll integrates actual travel");
    Frames(game,world,90,{.Brake=true});
    Check(std::abs(game.State().Speed)<0.01f,"handling braking stops car");
    Frames(game,world,1,{.Interact=true});
    Check(!game.State().InVehicle && game.State().Exits==1 && game.State().Grounded,"clearance-checked stationary exit");
    Check(game.Actors().meshes[0].tris>0,"ped geometry restored on exit");
    return game.State();
}
int main(int argc,char** argv) {
    std::setvbuf(stdout,nullptr,_IONBF,0);
    const char* gameDir=argc>1 ? argv[1]:"/game";
    char err[512]={}; std::string error;
    E2ELoadInfo load{}; E2EPagerFrame frame{};
    Check(StreamPager_Init(gameDir,load,err,sizeof(err),{true,900.0f,4096}),err);
    WorldShotScene visible{};
    Check(StreamPager_Update(1600,-1700,70,visible,frame,err,sizeof(err)),err);
    auto* dictionary=rw::TexDictionary::getCurrent();
    RealtimeGameplay game;
    const auto start=std::chrono::steady_clock::now();
    Check(game.Initialize(gameDir,error),error.c_str());
    Check(rw::TexDictionary::getCurrent()==dictionary,"pager current dictionary survives actor initialization");
    RealtimeGameplayWorld world;
    Check(world.Rebuild(visible,error),error.c_str());
    const auto indexed=std::chrono::steady_clock::now();
    float realGround=0;
    Check(world.Ground(1600,-1700,70,-80,realGround),"real binary-IPL scene ground");
    Check(game.Spawn(world,1600,-1700,70,0,error),error.c_str());
    Frames(game,world,120,{.Forward=1});
    Frames(game,world,1,{.Jump=true});
    Frames(game,world,90);
    Check(game.State().Grounded && game.State().Landings>0,"real streamed scene jump/land");
    const auto walked=std::chrono::steady_clock::now();
    std::printf("real scene triangles=%zu binaryRows=%d ground=%.4f walked=%.4f blocked=%llu triangleTests=%llu initIndexMs=%.1f tickMs=%.1f textures=%zu\n",
        world.TriangleCount(),load.binaryInstances,realGround,game.State().WalkDistance,
        static_cast<unsigned long long>(game.State().BlockedSteps),static_cast<unsigned long long>(world.TriangleTests()),
        std::chrono::duration<double,std::milli>(indexed-start).count(),std::chrono::duration<double,std::milli>(walked-indexed).count(),game.Actors().images.size());
    // Repage after initialization to expose dangling global dictionaries.
    Check(StreamPager_Update(1610,-1700,30,visible,frame,err,sizeof(err)),err);
    Check(world.Rebuild(visible,error),error.c_str());
    Frames(game,world,5);
    Check(world.Rebuild(Fixture(),error),error.c_str()); // temporary scene is safe: BVH owns vertices
    float ground=0;
    Check(world.Ground(25,25,13,0,ground) && ground==7,"nearest floor below ray, not roof above");
    Check(world.Ground(25,25,20,0,ground) && ground==14,"nearest upper floor");
    Check(!world.Ground(200,200,20,-100,ground),"no invented ground outside geometry");
    Check(world.SphereBlocked({7.8f,0,9},0.34f),"sphere/triangle wall intersection");
    const auto first=Replay(gameDir,game,world,true),second=Replay(gameDir,game,world);
    Check(first.Ped.X==second.Ped.X && first.Ped.Y==second.Ped.Y && first.Car.X==second.Car.X && first.Car.Y==second.Car.Y && first.DriveDistance==second.DriveDistance,"deterministic action replay");
    Check(game.Spawn(world,0,0,20,0,error),error.c_str());
    Frames(game,world,300,{.Forward=1,.Sprint=true});
    Check(game.State().Ped.X<7.67f && game.State().Ped.X>7 && game.State().BlockedSteps>0,"solid wall blocks sprinting ped");
    const double before=game.State().SimulatedSeconds;
    game.Tick(10.0,{.Forward=1},world);
    Check(game.State().SimulatedSeconds-before<0.101 && game.State().DroppedSeconds>9.8,"bounded dt prevents stall tunnelling");
    RealtimeVec3 hit;
    Check(!world.Raycast(game.Camera().Target,game.Camera().Position,hit),"camera stays in front of blocking world");
    std::printf("realtime-gameplay-probe PASS replay walk=%.3f drive=%.3f jumps=%llu landings=%llu entries=%llu exits=%llu blocked=%llu\n",
        first.WalkDistance,first.DriveDistance,static_cast<unsigned long long>(first.Jumps),static_cast<unsigned long long>(first.Landings),
        static_cast<unsigned long long>(first.Entries),static_cast<unsigned long long>(first.Exits),static_cast<unsigned long long>(first.BlockedSteps));
    StreamPager_Shutdown();
    return 0;
}
