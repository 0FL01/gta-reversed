// Deterministic triangle-contact and real IFP transition regression probe.
#include "app/platform/linux/RealtimeGameplay.h"
#include "app/platform/linux/StreamPager.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

static int Failures=0;
static bool BodyClear=true;
static void Check(bool ok,const char* label) {
    std::printf("%s %s\n",ok ? "PASS":"FAIL",label);
    if (!ok) ++Failures;
}
static void Require(bool ok,const std::string& error) {
    if (!ok) { std::fprintf(stderr,"terrain probe setup: %s\n",error.c_str()); std::exit(2); }
}
static void Quad(WorldShotScene& s,RealtimeVec3 a,RealtimeVec3 b,RealtimeVec3 c,RealtimeVec3 d) {
    if (s.meshes.empty()) s.meshes.emplace_back();
    auto& m=s.meshes[0];
    for (auto p:{a,b,c,a,c,d}) m.pos.insert(m.pos.end(),{p.X,p.Y,p.Z});
    m.tris+=2;
}
static void Floor(WorldShotScene& s,float x0,float x1,float z0,float z1) {
    Quad(s,{x0,-15,z0},{x1,-15,z1},{x1,15,z1},{x0,15,z0});
}
static WorldShotScene Step(float height,float roof=100) {
    WorldShotScene s;
    Floor(s,-20,2,7,7); Floor(s,2,20,7+height,7+height);
    Quad(s,{2,-15,7},{2,15,7},{2,15,7+height},{2,-15,7+height});
    if (roof<100) Floor(s,1,6,7+roof,7+roof);
    return s;
}
static void Frames(RealtimeGameplay& game,const RealtimeGameplayWorld& world,int count,RealtimeGameplayInput input={}) {
    for (int i=0;i<count;++i) {
        game.Tick(1.0/60,input,world); input.Jump=input.Interact=false;
        const auto p=game.State().Ped;
        for (float z:{0.344f,0.61f,0.88f,1.15f,1.42f}) BodyClear &= !world.SphereBlocked({p.X,p.Y,p.Z+z},0.34f);
    }
}
static void Spawn(RealtimeGameplay& g,RealtimeGameplayWorld& w,const WorldShotScene& s,float x=0,float top=20) {
    std::string error; Require(w.Rebuild(s,error),error); Require(g.Spawn(w,x,0,top,0,error),error);
}
static std::vector<float> LocalPose(const RealtimeGameplay& g) {
    auto p=g.Actors().meshes[0].pos;
    const auto& s=g.State(); const float c=std::cos(s.PedHeading),n=std::sin(s.PedHeading);
    for (size_t i=0;i<p.size();i+=3) {
        const float x=p[i]-s.Ped.X,y=p[i+1]-s.Ped.Y;
        p[i]=c*x+n*y; p[i+1]=-n*x+c*y; p[i+2]-=s.Ped.Z;
    }
    return p;
}
int main(int argc,char** argv) {
    std::setvbuf(stdout,nullptr,_IONBF,0);
    const char* dir=argc>1 ? argv[1]:"/game";
    RealtimeGameplay g; RealtimeGameplayWorld w; std::string error;
    Require(g.Initialize(dir,error),error);
    for (float height:{0.15f,0.25f}) {
        Spawn(g,w,Step(height));
        int airborne=0; float maxDz=0,previous=g.State().Ped.Z;
        for (int i=0;i<180;++i) {
            Frames(g,w,1,{.Forward=1}); airborne+=!g.State().Grounded;
            maxDz=std::max(maxDz,std::abs(g.State().Ped.Z-previous)); previous=g.State().Ped.Z;
        }
        std::printf("step %.2f up x=%.5f z=%.5f air=%d maxDz=%.5f blocked=%llu\n",height,g.State().Ped.X,g.State().Ped.Z,airborne,maxDz,(unsigned long long)g.State().BlockedSteps);
        Check(g.State().Ped.X>5.9f && std::abs(g.State().Ped.Z-7-height)<0.01f && !airborne,"bounded curb ascent maintains support");
        for (int i=0;i<180;++i) { Frames(g,w,1,{.Forward=-1}); airborne+=!g.State().Grounded; }
        Check(g.State().Ped.X<0.1f && std::abs(g.State().Ped.Z-7)<0.01f && !airborne,"curb descent returns to actual lower floor without contact jitter");
        const auto expected=g.State(); const auto pose=g.Actors().meshes[0].pos;
        Spawn(g,w,Step(height)); Frames(g,w,180,{.Forward=1}); Frames(g,w,180,{.Forward=-1});
        Check(g.State().Ped.X==expected.Ped.X && g.State().Ped.Z==expected.Ped.Z &&
              g.State().LocomotionPhase==expected.LocomotionPhase && g.Actors().meshes[0].pos==pose,"terrain contact and blended pose replay are bitwise deterministic");
        Spawn(g,w,Step(height)); airborne=0;
        for (int i=0;i<60;++i) { Frames(g,w,1,{.Forward=1,.Sprint=true}); airborne+=!g.State().Grounded; }
        Check(g.State().Ped.X>5.4f && std::abs(g.State().Ped.Z-7-height)<0.01f && !airborne,"sprinting curb traversal keeps supported contact");
    }
    WorldShotScene ramp; Floor(ramp,-20,2,7,7); Floor(ramp,2,8,7,10); Floor(ramp,8,20,10,10);
    Spawn(g,w,ramp); int air=0;
    for (int i=0;i<330;++i) { Frames(g,w,1,{.Forward=1}); air+=!g.State().Grounded; }
    std::printf("ramp up x=%.5f z=%.5f air=%d blocked=%llu\n",g.State().Ped.X,g.State().Ped.Z,air,(unsigned long long)g.State().BlockedSteps);
    Check(g.State().Ped.X>10.9f && std::abs(g.State().Ped.Z-10)<0.01f && !air,"continuous 1:2 ramp ascent");
    for (int i=0;i<330;++i) { Frames(g,w,1,{.Forward=-1}); air+=!g.State().Grounded; }
    Check(g.State().Ped.X<0.1f && std::abs(g.State().Ped.Z-7)<0.01f && !air,"continuous ramp descent");
    WorldShotScene steep; Floor(steep,-20,2,7,7); Floor(steep,2,5,7,10); Floor(steep,5,20,10,10);
    Spawn(g,w,steep,-2); air=0;
    for (int i=0;i<330;++i) { Frames(g,w,1,{.Forward=1}); air+=!g.State().Grounded; }
    Check(g.State().Ped.X>8.9f && !air,"walkable 45-degree ramp remains continuously supported");
    for (int i=0;i<330;++i) { Frames(g,w,1,{.Forward=-1}); air+=!g.State().Grounded; }
    Check(g.State().Ped.X<-1.9f && !air,"45-degree ramp descent maintains contact");
    WorldShotScene unwalkable; Floor(unwalkable,-20,2,7,7); Floor(unwalkable,2,5,7,13); Floor(unwalkable,5,20,13,13);
    Spawn(g,w,unwalkable,-2); Frames(g,w,180,{.Forward=1,.Sprint=true});
    Check(g.State().Ped.X<2 && g.State().Ped.Z<7.05f,"unwalkable 1:2 steep face blocks instead of climbing");
    for (float height:{0.27f,0.4f,0.6f,3.0f}) {
        Spawn(g,w,Step(height),-2); Frames(g,w,180,{.Forward=1,.Sprint=true});
        std::printf("rejected ledge %.2f x=%.4f z=%.4f\n",height,g.State().Ped.X,g.State().Ped.Z);
        Check(g.State().Ped.X<1.8f && g.State().Ped.Z<7.05f && g.State().BlockedSteps>0,"over-limit ledge cannot be ratcheted upward");
    }
    float hit=0;
    Check(w.SweepSphere({0,0,8},{4,0,8},0.34f,hit) && std::abs(hit-0.415f)<0.0001f,"continuous sphere sweep hits thin wall before crossing");
    Spawn(g,w,Step(0.15f,1.8f),-2); Frames(g,w,180,{.Forward=1});
    Check(g.State().Ped.X<2 && g.State().Ped.Z<7.1f,"step rejected when headroom would intersect roof");
    WorldShotScene ceiling; Floor(ceiling,-20,20,7,7);
    Quad(ceiling,{-2,-1,9.1f},{2,-1,9.1f},{2,1,9.1f},{-2,1,9.1f});
    Spawn(g,w,ceiling,0,9); float apex=7; bool headClear=true;
    for (int i=0;i<150;++i) {
        Frames(g,w,1,{.Jump=i==0}); apex=std::max(apex,g.State().Ped.Z);
        headClear &= !w.SphereBlocked({g.State().Ped.X,g.State().Ped.Y,g.State().Ped.Z+1.42f},0.34f);
    }
    Check(headClear && apex>7.1f && apex<7.35f && g.State().Grounded && g.State().Landings==1,"head sweep prevents penetration, cancels ascent and lands after low ceiling hit");
    WorldShotScene gap; Floor(gap,-20,2,7,7); Floor(gap,4,20,7,7); Floor(gap,2,4,3,3);
    Spawn(g,w,gap); Frames(g,w,85,{.Forward=1});
    Frames(g,w,150); // stop over the gap: must fall, never bridge it
    std::printf("gap x=%.4f z=%.4f landed=%llu\n",g.State().Ped.X,g.State().Ped.Z,(unsigned long long)g.State().Landings);
    Check(g.State().Ped.X>2.1f && g.State().Ped.Z<3.02f && g.State().Grounded && g.State().Landings==1,"unsupported edge falls to real lower floor");
    WorldShotScene edge; Floor(edge,-20,2,7,7);
    Spawn(g,w,edge); Frames(g,w,85,{.Forward=1}); Frames(g,w,150);
    Check(!g.State().Grounded && g.State().Ped.Z<-5 && g.State().Landings==0,"ray miss beyond edge never creates an infinite floor");
    const auto fallingPose=LocalPose(g); Frames(g,w,30); const auto heldPose=LocalPose(g);
    float fallDelta=0;
    for (size_t i=0;i<heldPose.size();++i) fallDelta=std::max(fallDelta,std::abs(heldPose[i]-fallingPose[i]));
    Check(fallDelta<0.001f && g.State().AirBlend==1,"long fall holds the real glide endpoint without cyclic pose snaps");
    WorldShotScene flat; Floor(flat,-20,100,7,7);
    Spawn(g,w,flat); Frames(g,w,21,{.Forward=1});
    auto prev=LocalPose(g); float jump=0; bool phaseContinuous=true;
    for (int i=0;i<90;++i) {
        const auto before=g.State();
        Frames(g,w,1,{.Forward=i<60 ? 1.0f:0.0f,.Sprint=i<30});
        const float advance=std::fmod(g.State().LocomotionPhase-before.LocomotionPhase+1,1);
        if (i<60) phaseContinuous &= advance>0 && advance<0.1f;
        else phaseContinuous &= advance==0;
        auto next=LocalPose(g);
        for (size_t v=0;v<next.size();v+=3) jump=std::max(jump,std::hypot(next[v]-prev[v],next[v+1]-prev[v+1],next[v+2]-prev[v+2]));
        prev=std::move(next);
    }
    std::printf("walk-run-walk-idle maxLocalVertexDelta=%.6f\n",jump);
    Check(jump<0.24f,"real IFP pose transitions are continuous");
    Check(phaseContinuous,"walk/run/idle changes preserve distance-driven foot cycle");
    Spawn(g,w,Step(3),-2); Frames(g,w,45,{.Forward=1});
    prev=LocalPose(g); jump=0; float stoppedPhase=0; bool stableStop=true;
    for (int i=0;i<180;++i) {
        Frames(g,w,1,{.Forward=1}); auto next=LocalPose(g);
        for (size_t v=0;v<next.size();v+=3) jump=std::max(jump,std::hypot(next[v]-prev[v],next[v+1]-prev[v+1],next[v+2]-prev[v+2]));
        prev=std::move(next);
        if (i==120) stoppedPhase=g.State().LocomotionPhase;
        if (i>120) stableStop &= g.State().LocomotionPhase==stoppedPhase;
    }
    std::printf("wall stop maxLocalVertexDelta=%.6f stoppedPhase=%.6f\n",jump,stoppedPhase);
    Check(jump<0.24f && stableStop && stoppedPhase>0.01f && g.State().LocomotionBlend==0,"wall stop blends to idle without resetting foot phase");
    // A render-triangle field check, including binary IPL rows. Search the
    // nearby streamed district for two level patches separated by a real curb.
    char err[512]={}; E2ELoadInfo load{}; E2EPagerFrame frame{}; WorldShotScene visible;
    Require(StreamPager_Init(dir,load,err,sizeof(err),{true,900,4096}),err);
    Require(StreamPager_Update(1600,-1700,70,visible,frame,err,sizeof(err)),err);
    Require(w.Rebuild(visible,error),error);
    bool field=false; int candidates=0,edges=0;
    for (float x=1540;x<1640 && !field;x+=0.25f) for (float y=-1780;y<-1640 && !field;y+=0.25f) {
        for (auto axis:{RealtimeVec3{1,0,0},RealtimeVec3{0,1,0}}) {
            float a,b,c,d;
            auto height=[&](float t,float& z) { return w.Ground(x+axis.X*t,y+axis.Y*t,20,5,z); };
            if (!height(-0.125f,a) || !height(0.125f,b) || std::abs(b-a)<0.1f || std::abs(b-a)>0.26f ||
                !height(-1,c) || !height(1,d) || std::abs(a-c)>0.015f || std::abs(b-d)>0.015f) continue;
            const float sign=b>a ? 1.0f:-1.0f;
            const float sx=x-axis.X*sign*1.25f,sy=y-axis.Y*sign*1.25f;
            const float heading=std::atan2(axis.Y*sign,axis.X*sign);
            ++edges;
            if (!g.Spawn(w,sx,sy,20,heading,error)) continue;
            ++candidates; const float initial=g.State().Ped.Z; int airborne=0;
            for (int i=0;i<120;++i) { Frames(g,w,1,{.Forward=1}); airborne+=!g.State().Grounded; }
            if (g.State().WalkDistance<3.9 || airborne || g.State().Ped.Z<initial+0.08f) {
                if (candidates<=5) std::printf("field rejected spawn=%.3f,%.3f heading=%.4f startZ=%.4f endZ=%.4f walk=%.3f air=%d\n",sx,sy,heading,initial,g.State().Ped.Z,g.State().WalkDistance,airborne);
                continue;
            }
            std::printf("real curb spawn=(%.5f,%.5f) rayTop=20 heading=%.8f startZ=%.6f rise=%.6f end=(%.5f,%.5f,%.6f) walk=%.5f blocked=%llu air=%d binaryRows=%d triangles=%zu candidates=%d\n",
                sx,sy,heading,initial,std::abs(b-a),g.State().Ped.X,g.State().Ped.Y,g.State().Ped.Z,g.State().WalkDistance,
                (unsigned long long)g.State().BlockedSteps,airborne,load.binaryInstances,w.TriangleCount(),candidates);
            for (int i=0;i<120;++i) { Frames(g,w,1,{.Forward=-1}); airborne+=!g.State().Grounded; }
            field=std::hypot(g.State().Ped.X-sx,g.State().Ped.Y-sy)<0.05f && std::abs(g.State().Ped.Z-initial)<0.01f && !airborne;
            if (field) break;
        }
    }
    std::printf("field edges=%d spawnable=%d triangleTests=%llu\n",edges,candidates,(unsigned long long)w.TriangleTests());
    Check(field,"real streamed district curb traversed up and down");
    Check(BodyClear,"all terrain replay frames keep the complete controller sphere stack outside solid triangles");
    StreamPager_Shutdown();
    std::printf("realtime-gameplay-terrain %s failures=%d\n",Failures ? "FAIL":"PASS",Failures);
    return Failures ? 1:0;
}
