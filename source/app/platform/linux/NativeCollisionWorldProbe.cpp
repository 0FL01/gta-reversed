// Source collision stage only: no renderer triangle fallback or image oracle.
#include "app/platform/linux/NativeCollisionAssets.h"
#include "app/platform/linux/RealtimeGameplay.h"
#include "app/platform/linux/StreamPager.h"
#include "app/platform/linux/RealtimeStreaming.h"
#include "app/platform/linux/NativePlayerAssets.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <bit>
#include <numeric>
#include <tuple>
#include <stdexcept>

using int32 = int32_t;
using uint32 = uint32_t;
using int64 = int64_t;
using uint64 = uint64_t;
#ifndef __stdcall
#define __stdcall
#endif
#include "oswrapper/oswrapper.h"

namespace {
static void Require(bool ok,const std::string& message) { if (!ok) throw std::runtime_error(message); }
static void Near(float a,float b,const char* message,float epsilon=0.0002f) { Require(std::abs(a-b)<epsilon,message); }
static NativeCollisionSnapshot Fixture(std::shared_ptr<const NativeCollisionModel> model,float angle=0) {
    NativeCollisionInstance inst; inst.Model=std::move(model);
    inst.Basis={NativeCollisionVector{std::cos(angle),std::sin(angle),0},NativeCollisionVector{-std::sin(angle),std::cos(angle),0},NativeCollisionVector{0,0,1}};
    NativeCollisionSnapshot snapshot;
    snapshot.Instances.push_back(std::move(inst));
    return snapshot;
}
static void Volumes() {
    RealtimeGameplayWorld world; std::string error; float f; RealtimeVec3 n; NativeCollisionHit hit;
    auto sphere=std::make_shared<NativeCollisionModel>(); sphere->Name="sphere-fixture";
    sphere->Spheres.push_back({{0,0,0},2,{42,3,4,5}});
    Require(world.Rebuild(Fixture(sphere),error),error);
    Require(world.SphereBlocked({0,0,0},0.1f),"source sphere deep containment");
    Require(!world.SphereBlocked({3,0,0},1),"sphere tangency is not penetration");
    Require(world.SweepSphere({-5,0,0},{5,0,0},1,f,false,INFINITY,&n,&hit),"analytic source sphere sweep");
    Near(f,0.2f,"sphere Minkowski radius time"); Near(n.X,-1,"sphere normal");
    Require(hit.Surface.Material==42 && hit.Surface.Light==5 && hit.Primitive==NativeCollisionPrimitive::Sphere,"sphere source metadata");
    Require(world.SweepSphere({1,0,0},{0,0,0},0.2f,f,false,INFINITY,&n) && f==0,"sphere started inside approaching");
    Require(!world.SweepSphere({1,0,0},{4,0,0},0.2f,f),"sphere separating initial containment");
    Require(world.Ground(0,0,5,-5,f),"sphere ground"); Near(f,2,"sphere ground height");
    Require(!world.SweepSphere({0,0,5},{0,0,-5},1,f,true,1.9f),"sphere support maxContactHeight");
    auto box=std::make_shared<NativeCollisionModel>(); box->Name="box-fixture";
    box->Boxes.push_back({{-1,-1,-1},{1,1,1},{21,2,3,4}});
    constexpr float angle=0.63f;
    Require(world.Rebuild(Fixture(box,angle),error),error);
    auto rotate=[](RealtimeVec3 p) { const float c=std::cos(angle),s=std::sin(angle); return RealtimeVec3{c*p.X-s*p.Y,s*p.X+c*p.Y,p.Z}; };
    auto sweep=[&](RealtimeVec3 a,RealtimeVec3 b,RealtimeVec3 normal,float time) {
        Require(world.SweepSphere(rotate(a),rotate(b),1,f,false,INFINITY,&n,&hit),"rotated box sweep");
        Near(f,time,"rotated box face/edge/corner time"); const auto expected=rotate(normal);
        Near(n.X,expected.X,"rotated box normal x"); Near(n.Y,expected.Y,"rotated box normal y"); Near(n.Z,expected.Z,"rotated box normal z");
        Require(hit.Primitive==NativeCollisionPrimitive::Box && hit.Surface.Material==21,"box provenance retained");
    };
    sweep({0,0,4},{0,0,-4},{0,0,1},0.25f);
    sweep({0,1.6f,4},{0,1.6f,-4},{0,0.6f,0.8f},0.275f);
    const float z=std::sqrt(0.68f);
    sweep({1.4f,1.4f,4},{1.4f,1.4f,-4},{0.4f,0.4f,z},(3-z)/8);
    Require(world.SphereBlocked({0,0,0},0.01f),"rotated box deep containment");
    Require(world.SweepSphere(rotate({0.5f,0,0}),rotate({0,0,0}),0.01f,f) && f==0,"box started inside approaching");
    Require(!world.SweepSphere(rotate({0.5f,0,0}),rotate({3,0,0}),0.01f,f),"box separating containment");
    Require(world.SweepSphere({0,0,-4},{0,0,4},0.34f,f,false,INFINITY,&n) && n.Z< -0.99f,"box ceiling normal");
    Require(!world.SweepSphere({0,0,-4},{0,0,4},0.34f,f,true),"ceiling not walkable");
    box->Boxes[0].Min={-0.001f,-2,-2}; box->Boxes[0].Max={0.001f,2,2};
    Require(world.Rebuild(Fixture(box,angle),error),error);
    Require(world.SweepSphere(rotate({-100,0,0}),rotate({100,0,0}),0.34f,f),"thin OBB continuous sweep");
    Near(f,(100-0.341f)/200,"thin OBB time",0.00001f);
    auto collapsed=std::make_shared<NativeCollisionModel>(); collapsed->Name="collapsed-source-face";
    collapsed->Vertices={{0,-2,0},{0,2,0},{0,2,0},{4,0,0}};
    collapsed->Faces={{{0,1,2},{33,0,0,0}},{{3,3,3},{34,0,0,0}}};
    Require(world.Rebuild(Fixture(collapsed),error),error);
    Require(world.TriangleCount()==2 && world.CollapsedTriangleCount()==2,"collapsed source faces retained");
    Require(world.SphereBlocked({0.1f,0,0},0.2f),"collapsed segment overlap");
    Require(world.SweepSphere({-2,0,0},{2,0,0},0.5f,f,false,INFINITY,&n,&hit),"collapsed segment continuous sweep");
    Near(f,0.375f,"collapsed segment time"); Near(n.X,-1,"collapsed segment normal");
    Require(hit.Surface.Material==33,"collapsed segment source metadata");
    Require(world.SweepSphere({4,0,2},{4,0,-2},0.5f,f,false,INFINITY,&n,&hit),"collapsed point continuous sweep");
    Near(f,0.375f,"collapsed point time"); Near(n.Z,1,"collapsed point normal");
    Require(hit.Surface.Material==34 && !world.Ground(4,0,2,-2,f),"collapsed point has no invented walkable face");
    std::puts("PASS analytic source spheres and rotated box volumes/face/edge/corner/inside/separating/ceiling/thin-wall");
}
static void Body(const RealtimeGameplayWorld& world,RealtimeVec3 p) {
    for (float z:{0.344f,0.61f,0.88f,1.15f,1.42f})
        Require(!world.SphereBlocked({p.X,p.Y,p.Z+z},0.34f),"real COL controller whole sphere-stack penetration");
}
static void VolumeController(RealtimeGameplay& game) {
    RealtimeGameplayWorld world; std::string error;
    for (float height:{0.15f,0.25f,0.27f,0.60f}) {
        auto model=std::make_shared<NativeCollisionModel>(); model->Name="solid-curb-fixture";
        model->Boxes={{{-20,-15,6},{2,15,7},{}},{{2,-15,6},{20,15,7+height},{}}};
        Require(world.Rebuild(Fixture(model),error),error);
        Require(game.Spawn(world,-2,0,20,0,error),error);
        int air=0;
        for (int i=0;i<240;++i) { game.Tick(1.0/60,{.Forward=1},world); Body(world,game.State().Ped); air+=!game.State().Grounded; }
        if (height<0.26f) {
            Require(game.State().Ped.X>5.9f && std::abs(game.State().Ped.Z-7-height)<0.01f && !air,"source box curb controller ascent");
            for (int i=0;i<240;++i) { game.Tick(1.0/60,{.Forward=-1},world); Body(world,game.State().Ped); air+=!game.State().Grounded; }
            Require(game.State().Ped.X< -1.9f && game.State().Ped.Z<7.01f && !air,"source box curb controller descent");
        } else Require(game.State().Ped.X<1.8f && game.State().Ped.Z<7.05f,"over-limit source box cannot ratchet");
    }
    auto roof=std::make_shared<NativeCollisionModel>(); roof->Name="solid-roof-fixture";
    roof->Boxes={{{-20,-15,6},{2,15,7},{}},{{2,-15,6},{20,15,7.15f},{}},{{1,-15,8.8f},{6,15,10},{}}};
    Require(world.Rebuild(Fixture(roof),error),error); Require(game.Spawn(world,-2,0,8,0,error),error);
    for (int i=0;i<240;++i) { game.Tick(1.0/60,{.Forward=1},world); Body(world,game.State().Ped); }
    Require(game.State().Ped.X<2 && game.State().Ped.Z<7.1f,"whole capsule source box roof clearance");
    std::puts("PASS source volume five-sphere controller curb ascent/descent/over-limit/headroom");
}
static void Parser(const NativeCollisionSnapshot& snapshot) {
    Require(!snapshot.Instances.empty(),"parser source fixture missing");
    const auto& original=*snapshot.Instances.front().Model;
    NativeCollisionModel parsed; std::string error;
    Require(NativeCollisionAssets::Parse(original.SourceChunk,original.Library,parsed,error),error);
    const auto expected=parsed.Name;
    auto bad=original.SourceChunk; bad[3]='9';
    Require(!NativeCollisionAssets::Parse(bad,"unknown-version",parsed,error) && parsed.Name==expected,"unknown COL geometry must fail atomically");
    Require(!NativeCollisionAssets::Parse(std::span(original.SourceChunk).first(31),"truncated",parsed,error),"truncated source header accepted");
    // Real compressed triangle index corruption must not become a partial model.
    for (const auto& inst:snapshot.Instances) if (inst.Model->Version>=2 && !inst.Model->Faces.empty()) {
        bad=inst.Model->SourceChunk;
        const auto offset=uint32_t(bad[100]) | uint32_t(bad[101])<<8 | uint32_t(bad[102])<<16 | uint32_t(bad[103])<<24;
        bad.at(offset+4)=bad.at(offset+5)=255;
        Require(!NativeCollisionAssets::Parse(bad,"bad-face-index",parsed,error),"invalid source triangle silently discarded"); break;
    }
    // V1 TSurface and zero-radius points are preserved, never filtered like the
    // legacy ray-only loader's r>0 path. Fields match ColHelpers::V1 exactly.
    std::vector<uint8_t> v1(112);
    auto u32=[&](size_t p,uint32_t n) { for (size_t j=0;j<4;++j) v1[p+j]=uint8_t(n>>(j*8)); };
    auto scalar=[&](size_t p,float f) { uint32_t n; std::memcpy(&n,&f,4); u32(p,n); };
    std::copy_n("COLL",4,v1.begin()); std::copy_n("zero-sphere",11,v1.begin()+8); u32(4,104);
    scalar(32,1); scalar(48,-1); scalar(52,-1); scalar(56,-1); scalar(60,1); scalar(64,1); scalar(68,1);
    u32(72,1); v1[92]=42; v1[93]=7; v1[94]=8; v1[95]=9;
    // sphere + line/box/vertex/face counts: 72+4+20+4*4 =112.
    Require(NativeCollisionAssets::Parse(v1,"V1-fixture",parsed,error),error);
    Require(parsed.Spheres.size()==1 && parsed.Spheres[0].Radius==0 && parsed.Spheres[0].Surface.Material==42 &&
        parsed.Spheres[0].Surface.Flags==7 && parsed.Spheres[0].Surface.Brightness==8 && parsed.Spheres[0].Surface.Light==9,"V1 point/surface metadata lost");
    std::puts("PASS COL parser source roundtrip, V1 zero-radius/surface, atomic bad-version/header/index rejection");
}
static auto Metadata(const NativeCollisionHit& h) {
    return std::tie(h.Model,h.Library,h.Ipl,h.ModelId,h.HeaderId,h.Record,h.PrimitiveIndex,h.Binary,h.ValidatedHeaderId,
        h.TimeShared,h.Primitive,h.Surface.Material,h.Surface.Flags,h.Surface.Brightness,h.Surface.Light);
}
static void CompareQueries(const NativeCollisionSnapshot& snapshot,const char* label) {
    RealtimeGameplayWorld indexed,brute; std::string error;
    Require(indexed.Rebuild(snapshot,error),error); Require(brute.Rebuild(snapshot,error,false),error);
    uint32_t seed=0x31415926;
    auto random=[&]() { seed=seed*1664525u+1013904223u; return float(seed>>8)/16777216.0f; };
    auto jitter=[&](float r) { return (random()*2-1)*r; };
    std::vector<RealtimeVec3> centers;
    for (const auto& inst:snapshot.Instances) {
        auto world=[&](NativeCollisionVector p) {
            auto v=inst.Placement.Position;
            for (int i=0;i<3;++i) for (int j=0;j<3;++j) v[j]+=p[i]*inst.Basis[i][j];
            return RealtimeVec3{v[0],v[1],v[2]};
        };
        for (const auto& s:inst.Model->Spheres) centers.push_back(world(s.Center));
        for (const auto& b:inst.Model->Boxes) centers.push_back(world({(b.Min[0]+b.Max[0])/2,(b.Min[1]+b.Max[1])/2,(b.Min[2]+b.Max[2])/2}));
    }
    Require(!centers.empty(),"volume query oracle needs volume centers");
    size_t hits=0;
    for (int i=0;i<1800;++i) {
        const auto c=centers[size_t(random()*centers.size())%centers.size()];
        const RealtimeVec3 from=i%7==0 ? c:RealtimeVec3{c.X+jitter(8),c.Y+jitter(8),c.Z+jitter(8)};
        const RealtimeVec3 to{from.X+jitter(12),from.Y+jitter(12),from.Z+jitter(12)};
        const float radius=i%13==0 ? 0:0.01f+random()*2;
        NativeCollisionHit a,b; float fa=-999,fb=-999; RealtimeVec3 va{},vb{};
        auto check=[&](bool x,bool y,const char* query) {
            Require(x==y, std::string(label)+" "+query+" hit mismatch at "+std::to_string(i));
            if (x) { ++hits; Require(Metadata(a)==Metadata(b),std::string(label)+" "+query+" source/order mismatch"); }
        };
        check(indexed.Ground(from.X,from.Y,from.Z+30,from.Z-30,fa,&a),brute.Ground(from.X,from.Y,from.Z+30,from.Z-30,fb,&b),"Ground");
        Require(fa==fb,"volume index changed exact ground height");
        check(indexed.Raycast(from,to,va,&a),brute.Raycast(from,to,vb,&b),"Raycast");
        Require(va.X==vb.X && va.Y==vb.Y && va.Z==vb.Z,"volume index changed exact ray position");
        check(indexed.SphereBlocked(from,radius,&a),brute.SphereBlocked(from,radius,&b),"SphereBlocked");
        for (bool walkable:{false,true}) {
            fa=fb=-999; va=vb={}; const float ceiling=i%3==0 ? from.Z:INFINITY;
            check(indexed.SweepSphere(from,to,radius,fa,walkable,ceiling,&va,&a),
                brute.SweepSphere(from,to,radius,fb,walkable,ceiling,&vb,&b),"SweepSphere");
            Require(fa==fb && va.X==vb.X && va.Y==vb.Y && va.Z==vb.Z,"volume index changed exact sweep time/normal");
        }
    }
    Require(indexed.TriangleTests()==brute.TriangleTests(),"volume broadphase changed triangle query statistics");
    Require(hits>100 && indexed.VolumeTests()<brute.VolumeTests(),"volume oracle vacuous or no pruning");
    std::printf("PASS volume index exact exhaustive oracle %s queries=9000 hits=%zu volumeTests=%llu brute=%llu triangleTests=%llu\n",
        label,hits,(unsigned long long)indexed.VolumeTests(),(unsigned long long)brute.VolumeTests(),(unsigned long long)indexed.TriangleTests());
}
static void IndexFixtures() {
    NativeCollisionSnapshot snapshot;
    for (int i=0;i<96;++i) {
        auto m=std::make_shared<NativeCollisionModel>(); m->Name="index-fixture-"+std::to_string(i);
        // Coincident primitives exercise first/last source-order tie selection.
        m->Spheres={{{0,0,0},float(i%5),{1,2,3,4}},{{0,0,0},float(i%5),{5,6,7,8}}};
        m->Boxes={{{-2,-3,-4},{2,3,4},{9,10,11,12}},{{-2,-3,-4},{2,3,4},{13,14,15,16}}};
        auto inst=Fixture(m,i*0.37f).Instances.front();
        const float angle=i*0.51f,c=std::cos(angle),s=std::sin(angle);
        for (auto& axis:inst.Basis) { const float y=axis[1],z=axis[2]; axis[1]=c*y-s*z; axis[2]=s*y+c*z; }
        // Accepted near-rigid basis: containment bounds must use inverse B^T.
        if (i%3==0) for (auto& f:inst.Basis[0]) f*=1.0003f;
        inst.Placement.Position={float(i%12)*11-60,float(i/12)*13-50,float(i%7)*3-9};
        snapshot.Instances.push_back(inst);
    }
    auto huge=std::make_shared<NativeCollisionModel>(); huge->Name="large-volume-no-center-pruning";
    huge->Spheres={{{10000,0,0},10001,{42,0,0,0}}};
    huge->Boxes={{{-12000,-2,-2},{12000,2,2},{43,0,0,0}},{{-0.001f,-200,-2},{0.001f,200,2},{44,0,0,0}}};
    snapshot.Instances.push_back(Fixture(huge,0.63f).Instances.front());
    auto cancelled=std::make_shared<NativeCollisionModel>(); cancelled->Name="large-local-offset-cancellation";
    cancelled->Boxes={{{-1000000,-1000000,-1000000},{-999999,-999999,-999999},{45,0,0,0}}};
    auto inst=Fixture(cancelled).Instances.front(); inst.Placement.Position={1000000,1000000,1000000};
    snapshot.Instances.push_back(inst);
    RealtimeGameplayWorld indexed,brute; std::string error;
    NativeCollisionSnapshot cancellation; cancellation.Instances.push_back(inst);
    Require(indexed.Rebuild(cancellation,error),error);
    Require(brute.Rebuild(cancellation,error,false),error);
    for (float x:{-0.02f,0.0f,1.0f,1.02f}) {
        // Local float subtraction rounds these points into the box. Broadphase
        // must conservatively retain the established containment semantics.
        const RealtimeVec3 p{x,0.5f,0.5f};
        Require(indexed.SphereBlocked(p,0)==brute.SphereBlocked(p,0),"cancellation containment culled by volume bounds");
    }
    CompareQueries(snapshot,"rotated/coincident/near-rigid/large/thin");
}
static uint64_t Benchmark(RealtimeGameplay& game,const RealtimeGameplayWorld& world,const char* label,
                          RealtimeVec3 origin={1540,-1736,20}) {
    std::string error; std::vector<double> times; uint64_t digest=1469598103934665603ull;
    auto hash=[&](float f) { digest^=std::bit_cast<uint32_t>(f); digest*=1099511628211ull; };
    const auto triangles=world.TriangleTests();
    const auto volumes=world.VolumeTests();
    for (int run=0;run<3;++run) {
        Require(game.Spawn(world,origin.X,origin.Y,origin.Z,-1.57079632679f,error),error);
        for (int i=0;i<300;++i) {
            const auto start=realtime_streaming::Milliseconds();
            game.Tick(1.0/60,{.Forward=i<120 ? 1.0f:i<240 ? -1.0f:0},world);
            times.push_back(realtime_streaming::Milliseconds()-start);
            const auto& s=game.State();
            hash(s.Ped.X); hash(s.Ped.Y); hash(s.Ped.Z); hash(s.WalkDistance); hash(s.LocomotionPhase);
            hash(float(s.Grounded)); hash(float(s.BlockedSteps));
        }
    }
    const double mean=std::accumulate(times.begin(),times.end(),0.0)/times.size();
    std::sort(times.begin(),times.end());
    std::printf("COL Tick benchmark %s n=%zu fixedHz=60 meanMs=%.6f p50Ms=%.6f p95Ms=%.6f maxMs=%.6f triangleTests=%llu volumeTests=%llu digest=%016llx\n",
        label,times.size(),mean,times[times.size()/2],times[times.size()*95/100],times.back(),
        (unsigned long long)(world.TriangleTests()-triangles),(unsigned long long)(world.VolumeTests()-volumes),(unsigned long long)digest);
    return digest;
}
static void Real(const char* dir) {
    E2ELoadInfo info; char err[512]{}; std::string error; NativeCollisionPopulation population;
    Require(StreamPager_Init(dir,info,err,sizeof(err),{true,300,80}),err);
    Require(StreamPager_CollisionPopulation(population,error),error);
    // Pager's real nonempty prefix stays frozen: both absolute and relative
    // OS_File reads must resolve the same source bytes without prefix mutation.
    void *absolute{}, *relative{};
    const auto path=(std::filesystem::absolute(dir)/"data/gta.dat").string();
    Require(OS_FileOpen(FILE_DATA_AREA_DEFAULT,&absolute,path.c_str(),FILE_ACCESS_READ)==0,"absolute OS_File read under pager prefix");
    Require(OS_FileOpen(FILE_DATA_AREA_DEFAULT,&relative,"data/gta.dat",FILE_ACCESS_READ)==0,"relative OS_File prefix regression");
    std::array<char,128> a{},b{};
    Require(OS_FileRead(absolute,a.data(),a.size())==0 && OS_FileRead(relative,b.data(),b.size())==0 && a==b &&
        OS_FileSize(absolute)==OS_FileSize(relative),"absolute/relative OS_File mismatch");
    OS_FileClose(absolute); OS_FileClose(relative);
    std::puts("PASS absolute and relative OS_File under unchanged nonempty pager prefix");
    const auto loadStart=realtime_streaming::Milliseconds();
    auto context=NativeCollisionContext::LoadBeforeWorker(dir,900,error); Require(bool(context),error);
    const auto& assets=context->Assets;
    std::printf("source COL catalog loadMs=%.2f (once)\n",realtime_streaming::Milliseconds()-loadStart);
    const auto& stats=assets.Stats();
    std::printf("source COL models=%zu spheres=%zu boxes=%zu triangles=%zu empty=%zu unsupported=%zu nameFallback=%zu IPL=%zu binary=%d\n",
        stats.Models,stats.Spheres,stats.Boxes,stats.Triangles,stats.Empty,stats.Unsupported,stats.HeaderNameFallback,population.Instances.size(),info.binaryInstances);
    for (const auto& unsupported:stats.UnsupportedModels) std::printf("explicit unsupported: %s\n",unsupported.c_str());
    RealtimeGameplayWorld world; NativeCollisionSnapshot snapshot; float ground; NativeCollisionHit source;
    auto groundAt=[&](float x,float y,float expected,const char* model,uint32_t row) {
        Require(assets.Snapshot(population,x,y,900,snapshot,error),error);
        Require(world.Rebuild(snapshot,error),error);
        std::printf("source COL radius=900 collapsed-boundaries=%zu (retained)\n",world.CollapsedTriangleCount());
        Require(world.Ground(x,y,20,5,ground,&source),"real source COL ground missing");
        Near(ground,expected,"real source COL ground differs");
        Require(source.Model==model && source.Binary && source.Record==row,"ground not bound to actual binary IPL/source model");
        std::printf("PASS source ground %.6f,%.6f=%.9f model=%s IDE=%d header=%u library=%s IPL=%s row=%u material=%u tris=%zu spheres=%zu boxes=%zu missing=%zu empty=%zu\n",
            x,y,ground,source.Model.c_str(),source.ModelId,source.HeaderId,source.Library.c_str(),source.Ipl.c_str(),source.Record,source.Surface.Material,
            world.TriangleCount(),world.SphereCount(),world.BoxCount(),snapshot.MissingModels,snapshot.EmptyModels);
    };
    groundAt(2488.562f,-1666.864f,12.343750f,"lae2_roads89",4);
    CompareQueries(snapshot,"source-spawn-900m");
    Require(!source.ValidatedHeaderId && source.HeaderId==6503 && source.ModelId==17613,"COL header ID incorrectly trusted");
    groundAt(1540,-1736,12.3828125f,"roads18_lan",69);
    CompareQueries(snapshot,"source-curb-900m");
    Require(world.Ground(1540,-1740.013672f,20,5,ground),"curb high ground"); Near(ground,12.546875f,"source curb high patch");
    Parser(snapshot);
    RealtimeGameplay game; Require(game.Initialize(dir,error),error);
    RealtimeGameplayWorld brute; Require(brute.Rebuild(snapshot,error,false),error);
    const auto expected=Benchmark(game,brute,"exhaustive");
    Require(Benchmark(game,world,"indexed")==expected,"production Tick indexed/exhaustive replay differs");
    VolumeController(game);
    RealtimeVec3 first{};
    for (double hz:{59.7,60.0,120.0}) {
        RealtimeVec3 replay{};
        for (int run=0;run<2;++run) {
            Require(game.Spawn(world,1540,-1736,20,-1.57079632679f,error),error);
            int airborne=0; const auto start=game.State().Ped;
            const int count=static_cast<int>(std::round(2*hz));
            for (int i=0;i<count;++i) {
                game.Tick(1/hz,{.Forward=1},world); Body(world,game.State().Ped); airborne+=!game.State().Grounded;
            }
            const auto peak=game.State().Ped;
            Require(peak.Y<-1739.9f && peak.Z>12.53f && !airborne,"real COL curb controller ascent");
            for (int i=0;i<count;++i) {
                game.Tick(1/hz,{.Forward=-1},world); Body(world,game.State().Ped); airborne+=!game.State().Grounded;
            }
            const auto end=game.State().Ped;
            Require(std::hypot(end.X-start.X,end.Y-start.Y)<0.03f && std::abs(end.Z-start.Z)<0.005f && !airborne,"real COL curb controller descent");
            if (run==0) replay=end;
            else Require(replay.X==end.X && replay.Y==end.Y && replay.Z==end.Z,"source COL replay nondeterministic");
            std::printf("PASS COL curb hz=%.1f replay=%d peak=%.6f,%.6f,%.6f end=%.6f,%.6f,%.6f air=%d walk=%.6f blocked=%llu\n",
                hz,run,peak.X,peak.Y,peak.Z,end.X,end.Y,end.Z,airborne,game.State().WalkDistance,(unsigned long long)game.State().BlockedSteps);
        }
        if (hz==59.7) first=replay;
        else Require(std::hypot(first.X-replay.X,first.Y-replay.Y,first.Z-replay.Z)<0.03f,"COL frame-rate consistency");
    }
    {
        NativeCollisionSnapshot cjSnapshot;
        Require(context->Snapshot(1600,-1700,cjSnapshot,error),error);
        RealtimeGameplayWorld indexedCj,bruteCj;
        Require(indexedCj.Rebuild(cjSnapshot,error),error); Require(bruteCj.Rebuild(cjSnapshot,error,false),error);
        const auto clothes=NativePlayerClothes_Startup();
        RealtimeGameplay cj; Require(cj.Initialize(dir,error,&clothes),error);
        const auto digest=Benchmark(cj,bruteCj,"CJ-source-900m-exhaustive",{1600,-1700,70});
        Require(Benchmark(cj,indexedCj,"CJ-source-900m-indexed",{1600,-1700,70})==digest,"CJ indexed/exhaustive Tick replay differs");
    }
    // Real pager render cap is 80; source collision still owns the full 900m
    // window. All startup parsers have returned before worker ownership starts.
    std::weak_ptr<const NativeCollisionContext> lifetime=context;
    {
        realtime_streaming::Worker worker(true,context);
        context.reset();
        auto build=[&](realtime_streaming::Center center,float expected,uint64_t generation) {
            worker.Request(center,true);
            std::unique_ptr<realtime_streaming::CpuWorld> cpu;
            const auto deadline=realtime_streaming::Milliseconds()+30000;
            while (!(cpu=worker.TakeReady()) && realtime_streaming::Milliseconds()<deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            Require(bool(cpu),"source worker publication timed out"); Require(cpu->Error.empty(),cpu->Error);
            Require(cpu->Generation==generation && cpu->Position.X==center.X && cpu->Position.Y==center.Y &&
                cpu->Position.Z==center.Z && cpu->SourceCollision && cpu->SourceCollision->Instances.size()>size_t(cpu->Frame.instances),
                "source worker generation/position or render-independent population mismatch");
            Require(cpu->Collision.Ground(center.X,center.Y,20,5,ground),"source worker ground missing");
            Near(ground,expected,"source worker ground");
            Require(!lifetime.expired(),"worker lost immutable source context");
            worker.Retire(std::move(cpu)); worker.Release();
        };
        build({2488.562f,-1666.864f,13.3757f},12.34375f,2);
        build({1540,-1736,20},12.3828125f,3);
        worker.Stop({},{});
    }
    Require(lifetime.expired(),"source context leaked beyond joined worker");
    std::puts("PASS source worker paired generations, render-cap-independent residency, shared catalog lifetime/join");
    StreamPager_Shutdown();
    Require(world.Ground(1540,-1736,20,5,ground),"collision world retained pager memory");
}
} // namespace
int main(int argc,char** argv) try {
    std::setvbuf(stdout,nullptr,_IONBF,0); Volumes(); IndexFixtures();
    if (argc>1) Real(argv[1]);
    std::puts("native-collision-world PASS (source collision stage; full port not closed)"); return 0;
} catch (const std::exception& e) {
    std::fprintf(stderr,"native-collision-world FAIL: %s\n",e.what()); return 1;
}
