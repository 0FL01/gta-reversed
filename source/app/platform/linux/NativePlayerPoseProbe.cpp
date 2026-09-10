// Numeric oracle: double-precision sequential inverse-bind/local transforms,
// never a product of the matrices used by the production skinning path.
// Decoder access only, matching NativePlayerAssetsProbe's isolated TU pattern.
#include "app/platform/linux/IfpAnim.cpp"
#include "app/platform/linux/CarPose.h"
#define main LegacyTerrainProbeMain
#include "app/platform/linux/RealtimeGameplayTerrainProbe.cpp"
#undef main
#include <array>
#include <cstring>

using Point = std::array<double,3>;
static Point Transform(Point p,const NativePlayerMatrix& m,bool normal=false) {
    Point out{};
    for (int c=0;c<3;++c) out[c]=p[0]*m.Right[c]+p[1]*m.Up[c]+p[2]*m.At[c]+(normal ? 0:m.Pos[c]);
    return out;
}
static Point Sequential(Point p,int bone,const NativePlayerAssets& a,const IfpAnimPlayerAudit& audit,bool normal=false) {
    p=Transform(p,a.Bones[bone].InverseBind,normal);
    for (int b=bone;b>=0;b=a.Bones[b].Parent) p=Transform(p,audit.Locals[b],normal);
    return p;
}
static NativePlayerClothes Outfit() {
    // Exactly NativePlayerAssetsProbe::TestAssembly's explicit descriptor.
    NativePlayerClothes d;
    d.Models={"vest","head","hands","jeans","sneaker"};
    d.Textures[0]={{"torso","torso"},{"vest","vest"}};
    d.Textures[1]={{"head","head8bit"}};
    d.Textures[2]={{"jeansdenim","legs"}};
    d.Textures[3]={{"sneakerbincblk","sneakerbincblk"}};
    return d;
}
static bool StaticEqual(const WorldShotMesh& a,const WorldShotMesh& b) {
    if (a.tris!=b.tris || a.uv!=b.uv || a.triImg!=b.triImg || a.triCol!=b.triCol ||
        a.dayColors!=b.dayColors || a.nightColors!=b.nightColors || a.surfaces.size()!=b.surfaces.size()) return false;
    for (size_t i=0;i<a.surfaces.size();++i) {
        const auto& x=a.surfaces[i]; const auto& y=b.surfaces[i];
        if (x.color!=y.color || x.ambient!=y.ambient || x.diffuse!=y.diffuse ||
            x.vehicleColorIndex!=y.vehicleColorIndex || x.vehicleAlpha!=y.vehicleAlpha) return false;
    }
    return true;
}
static double ReferenceLocals(const NativePlayerAssets& assets,const IfpAnimData& anim,double fraction,IfpAnimPlayerAudit& audit) {
    double error=0;
    const float time=static_cast<float>(fraction*anim.total);
    for (size_t bone=0;bone<32;++bone) {
        auto expected=assets.Bones[bone].BindLocal;
        auto seq=std::find_if(anim.seqs.begin(),anim.seqs.end(),[&](const auto& s) { return EffectiveTag(s)==assets.Bones[bone].Tag; });
        if (seq!=anim.seqs.end() && !seq->frames.empty()) {
            auto next=std::lower_bound(seq->frames.begin(),seq->frames.end(),time,[](const auto& f,float t) { return f.absTime<t; });
            if (next==seq->frames.end()) --next;
            auto prev=next;
            if (prev!=seq->frames.begin() && next->absTime>time) --prev;
            const double alpha=next->absTime>prev->absTime ? (double(time)-prev->absTime)/(double(next->absTime)-prev->absTime):0;
            std::array<double,4> a{},b{},q{};
            double la=0,lb=0,dot=0;
            for (int c=0;c<4;++c) { la+=double(prev->q[c])*prev->q[c]; lb+=double(next->q[c])*next->q[c]; }
            for (int c=0;c<4;++c) { a[c]=prev->q[c]/std::sqrt(la); b[c]=next->q[c]/std::sqrt(lb); dot+=a[c]*b[c]; }
            if (dot<0) { dot=-dot; for (auto& c:b) c=-c; }
            double wa=1-alpha,wb=alpha;
            if (dot<0.9995) { const double theta=std::acos(dot); wa=std::sin((1-alpha)*theta)/std::sin(theta); wb=std::sin(alpha*theta)/std::sin(theta); }
            double length=0;
            for (int c=0;c<4;++c) { q[c]=wa*a[c]+wb*b[c]; length+=q[c]*q[c]; }
            for (auto& c:q) c/=std::sqrt(length);
            const rw::Quat rotation{float(q[0]),float(q[1]),float(q[2]),float(q[3])};
            // Quaternion sandwich rotation is independent of QuatPosToMatrix.
            const auto r=rw::rotate({1,0,0},rotation),u=rw::rotate({0,1,0},rotation),at=rw::rotate({0,0,1},rotation);
            expected.Right={r.x,r.y,r.z}; expected.Up={u.x,u.y,u.z}; expected.At={at.x,at.y,at.z};
            if (prev->hasT || next->hasT) for (int c=0;c<3;++c) expected.Pos[c]=float((1-alpha)*prev->t[c]+alpha*next->t[c]);
        }
        const auto& actual=audit.Locals[bone];
        for (int c=0;c<3;++c) error=std::max({error,double(std::abs(expected.Right[c]-actual.Right[c])),double(std::abs(expected.Up[c]-actual.Up[c])),
            double(std::abs(expected.At[c]-actual.At[c])),double(std::abs(expected.Pos[c]-actual.Pos[c]))});
        audit.Locals[bone]=expected; // drive the vertex oracle independently too
    }
    return error;
}
int main(int argc,char** argv) {
    std::setvbuf(stdout,nullptr,_IONBF,0);
    const char* dir=argc>1 ? argv[1]:"/game";
    const bool startup = argc == 3 && std::string(argv[2]) == "--startup";
    if (argc > 3 || (argc == 3 && !startup)) return 2;
    char err[512]{}; std::string error;
    IfpAnimPlayerBank bank;
    Require(bank.Load(dir,"ped",err,sizeof(err)),err);
    const auto clothes=startup ? NativePlayerClothes_Startup() : Outfit(); NativePlayerAssets assets;
    std::printf("cj-descriptor %s\n", startup ? "startup-fat200-muscle50" : "explicit-Normal");
    Require(NativePlayerAssets_Load(clothes,assets,error),error);
    Check(assets.Bones.size()==32 && assets.Vertices.size()==1721 && assets.Triangles.size()==2332,
          "same explicit vest/jeans/sneaker counts as asset probe");
    WorldShotScene bind; IfpAnimStats stats{}; IfpAnimPlayerAudit audit;
    Require(IfpAnim_InitPlayer(assets,bank,"",0,bind,stats,err,sizeof(err),true,&audit),err);
    Check(bind.meshes.size()==5 && bind.images.size()==4,"five stable body mesh slots, four images");
    for (size_t i=0;i<4;++i) Check(bind.images[i].rgba==assets.Images[i].RGBA &&
        bind.images[i].filter==assets.Images[i].FilterAddressing,"exact asset images and addressing");
    double maxPosition=0,maxNormal=0,maxJoint=0,bindError=0,wrongOrder=0,maxLocal=0;
    size_t checked=0;
    auto oracle=[&](const WorldShotScene& scene,bool isBind) {
        for (size_t bone=0;bone<32;++bone) {
            Point p{};
            for (int b=static_cast<int>(bone);b>=0;b=assets.Bones[b].Parent) p=Transform(p,audit.Locals[b]);
            for (int c=0;c<3;++c) maxJoint=std::max(maxJoint,std::abs(p[c]-audit.Worlds[bone].Pos[c]));
        }
        for (size_t slot=0;slot<5;++slot) {
            const auto& mesh=scene.meshes[slot]; const auto& part=assets.Parts[slot];
            Require(StaticEqual(mesh,bind.meshes[slot]),"bind/idle/walk/run/jump static layout");
            for (size_t t=0;t<part.TriangleCount;++t) for (size_t k=0;k<3;++k) {
                const auto& v=assets.Vertices[assets.Triangles[part.FirstTriangle+t].Vertices[k]];
                Point position{},normal{},wrong{};
                for (size_t j=0;j<4;++j) if (v.Weights[j]>0) {
                    const int bone=v.Bones[j];
                    const Point input{v.Position[0],v.Position[1],v.Position[2]},inputNormal{v.Normal[0],v.Normal[1],v.Normal[2]};
                    const auto p=Sequential(input,bone,assets,audit);
                    const auto n=Sequential(inputNormal,bone,assets,audit,true);
                    const auto bad=Transform(Transform(input,audit.Worlds[bone]),assets.Bones[bone].InverseBind);
                    for (int c=0;c<3;++c) { position[c]+=p[c]*v.Weights[j]; normal[c]+=n[c]*v.Weights[j]; wrong[c]+=bad[c]*v.Weights[j]; }
                }
                const double length=std::hypot(normal[0],normal[1],normal[2]);
                if (length>1e-9) for (auto& c:normal) c/=length;
                for (int c=0;c<3;++c) {
                    const size_t index=t*9+k*3+c;
                    maxPosition=std::max(maxPosition,std::abs(position[c]-mesh.pos[index]));
                    maxNormal=std::max(maxNormal,std::abs(normal[c]-mesh.nrm[index]));
                    wrongOrder=std::max(wrongOrder,std::abs(wrong[c]-mesh.pos[index]));
                    if (isBind) bindError=std::max(bindError,std::abs(double(mesh.pos[index])-v.Position[c]));
                    Require(mesh.uv[t*6+k*2+c%2]==v.UV[c%2],"unchanged wrapped UV");
                }
                ++checked;
            }
        }
    };
    oracle(bind,true);
    WorldShotScene posed;
    for (const char* clip:{"IDLE_stance","WALK_civi","run_player","JUMP_glide"}) {
        const auto& animation=*std::find_if(bank.Data()->Anims.begin(),bank.Data()->Anims.end(),[&](const auto& a) { return ToLowerCopy(a.name)==ToLowerCopy(clip); });
        for (int frame=0;frame<33;++frame) {
            Require(IfpAnim_InitPlayer(assets,bank,clip,frame/32.0,posed,stats,err,sizeof(err),false,&audit),err);
            const int tracks=std::strcmp(clip,"JUMP_glide")==0 ? 26:32;
            Require(stats.bones==32 && stats.mapped==tracks && stats.unmapped==32-tracks && posed.images.empty(),
                    "all bones posed; authored tracks mapped; no repeated image export");
            maxLocal=std::max(maxLocal,ReferenceLocals(assets,animation,frame/32.0,audit));
            oracle(posed,false);
            if (!frame) std::printf("cj-clip name=%s mapped=%d bindRetained=%d duration=%.6f bbox=(%.5f %.5f %.5f)-(%.5f %.5f %.5f)\n",
                clip,stats.mapped,stats.unmapped,stats.animTotal,stats.animMin[0],stats.animMin[1],stats.animMin[2],stats.animMax[0],stats.animMax[1],stats.animMax[2]);
        }
    }
    Check(maxLocal<0.00001 && maxPosition<0.00001 && maxNormal<0.00001 && maxJoint<0.00001 && bindError<0.00001,
          "132 cached poses plus bind equal independent sequential hierarchy/skin/normal oracle");
    Check(wrongOrder>0.5,"negative control rejects world-before-inverse-bind matrix order");
    std::printf("cj-oracle poses=132 vertices=%zu maxPosition=%.9g maxNormal=%.9g maxJoint=%.9g maxLocal=%.9g bindError=%.9g wrongOrder=%.6f\n",
        checked,maxPosition,maxNormal,maxJoint,maxLocal,bindError,wrongOrder);
    for (int test=0;test<8;++test) {
        auto bad=assets;
        if (test==0) bad.Bones[3].Parent=3;
        if (test==1) bad.Bones[3].Tag=bad.Bones[2].Tag;
        if (test==2) bad.Vertices[0].Bones[0]=255;
        if (test==3) bad.Triangles[0].Vertices[0]=999999;
        if (test==4) bad.Materials[0].Image=99;
        if (test==5) bad.Parts[0].TriangleCount=999999;
        if (test==6) bad.Vertices[0].Weights[0]=-1;
        if (test==7) bad.Bones[0].Flags=1;
        const auto* storage=posed.meshes[0].pos.data();
        Check(!IfpAnim_InitPlayer(bad,bank,"WALK_civi",0.5,posed,stats,err,sizeof(err)) && *err &&
              storage==posed.meshes[0].pos.data(),"bad references fail atomically");
    }
    Check(!IfpAnim_InitPlayer(assets,bank,"missing",0,posed,stats,err,sizeof(err)),"absent clip fails without fallback");
    RealtimeGameplay game; RealtimeGameplayWorld world;
    Require(game.Initialize(dir,error,&clothes),error);
    Spawn(game,world,Step(0.25f));
    const auto layout=game.Actors();
    const auto* pixels=game.Actors().images[0].rgba.data();
    int pedTriangles=0; for (size_t i=0;i<5;++i) pedTriangles+=layout.meshes[i].tris;
    Check(pedTriangles==2332 && layout.meshes.size()>5,"runtime uses actual five-part CJ plus car");
    bool phase=true,stable=true; int airborne=0;
    for (int frame=0;frame<180;++frame) {
        const auto before=game.State(); Frames(game,world,1,{.Forward=1});
        airborne+=!game.State().Grounded;
        const float advance=std::fmod(game.State().LocomotionPhase-before.LocomotionPhase+1,1);
        phase &= advance>0 && advance<0.1f;
        for (size_t m=0;m<layout.meshes.size();++m) stable &= StaticEqual(layout.meshes[m],game.Actors().meshes[m]);
        stable &= pixels==game.Actors().images[0].rgba.data();
    }
    Check(phase && !airborne && game.State().Ped.X>5.9f && std::abs(game.State().Ped.Z-7.25f)<0.01f,
          "CJ walks 25cm curb with continuous distance phase and supported contact");
    // Compare persistent gameplay against the exact two cached walk frames,
    // including root removal, foot height, heading and weighted normalization.
    const float sample=game.State().LocomotionPhase*32;
    const int frame0=static_cast<int>(sample); const float alpha=sample-frame0;
    WorldShotScene first,a,b; IfpAnimStats firstStats{},aStats{},bStats{};
    Require(IfpAnim_InitPlayer(assets,bank,"WALK_civi",0,first,firstStats,err,sizeof(err),false),err);
    Require(IfpAnim_InitPlayer(assets,bank,"WALK_civi",frame0/32.0,a,aStats,err,sizeof(err),false),err);
    Require(IfpAnim_InitPlayer(assets,bank,"WALK_civi",(frame0+1)/32.0,b,bStats,err,sizeof(err),false),err);
    float cacheError=0,cacheNormal=0;
    for (size_t m=0;m<5;++m) for (size_t v=0;v<a.meshes[m].pos.size();v+=3) {
        float p[3],n[3];
        for (int c=0;c<3;++c) {
            const float offsetA=c<2 ? aStats.rootWorld[c]:firstStats.animMin[2];
            const float offsetB=c<2 ? bStats.rootWorld[c]:firstStats.animMin[2];
            p[c]=(1-alpha)*(a.meshes[m].pos[v+c]-offsetA)+alpha*(b.meshes[m].pos[v+c]-offsetB);
            n[c]=(1-alpha)*a.meshes[m].nrm[v+c]+alpha*b.meshes[m].nrm[v+c];
        }
        const float length=std::hypot(n[0],n[1],n[2]);
        const float expected[]{p[1]+game.State().Ped.X,-p[0]+game.State().Ped.Y,p[2]+game.State().Ped.Z};
        const float normal[]{n[1]/length,-n[0]/length,n[2]/length};
        for (int c=0;c<3;++c) {
            cacheError=std::max(cacheError,std::abs(expected[c]-game.Actors().meshes[m].pos[v+c]));
            cacheNormal=std::max(cacheNormal,std::abs(normal[c]-game.Actors().meshes[m].nrm[v+c]));
        }
    }
    Check(cacheError<0.00001f && cacheNormal<0.00001f,"runtime CJ cache positions/normals equal independently placed walk samples");
    std::printf("cj-cache maxPosition=%.9g maxNormal=%.9g\n",cacheError,cacheNormal);
    float apex=game.State().Ped.Z;
    for (int i=0;i<150;++i) {
        Frames(game,world,1,{.Forward=1,.Sprint=true,.Jump=i==0}); apex=std::max(apex,game.State().Ped.Z);
        for (size_t m=0;m<layout.meshes.size();++m) stable &= StaticEqual(layout.meshes[m],game.Actors().meshes[m]);
    }
    Check(apex>8 && game.State().Jumps==1 && game.State().Landings==1 && game.State().Grounded,
          "CJ run/jump/glide/landing cache path");
    Check(stable,"runtime mesh/material/prelight/UV/image storage stable across walk/run/jump");
    std::printf("cj-runtime triangles=%d meshes=%zu images=%zu distance=%.6f phase=%.6f jumpApex=%.6f\n",
        pedTriangles,layout.meshes.size(),layout.images.size(),game.State().WalkDistance,game.State().LocomotionPhase,apex);
    WorldShotScene flat; Floor(flat,-100,100,7,7); Spawn(game,world,flat);
    Frames(game,world,15,{.Forward=1}); Frames(game,world,30,{.Side=1}); Frames(game,world,1,{.Interact=true});
    bool hidden=game.State().InVehicle;
    for (size_t m=0;m<5;++m) hidden &= game.Actors().meshes[m].tris==0 && game.Actors().meshes[m].pos.empty();
    Check(hidden,"all five CJ slots hidden on car entry");
    Frames(game,world,20,{.Forward=1,.Side=0.25f});
    WorldShotScene car; CarPoseStats carStats{}; CarPoseAudit carAudit{};
    auto* saved=rw::TexDictionary::getCurrent();
    constexpr float pi=3.14159265358979323846f;
    Require(CarPose_Init(dir,"landstal",game.State().Steer*180/pi,game.State().WheelSpin*180/pi,
        car,carStats,carAudit,err,sizeof(err),CarPoseTextures::RealtimeVehicle),err);
    CarPose_Shutdown(); rw::TexDictionary::setCurrent(saved);
    float wheelError=0;
    const float yaw=game.State().CarHeading-pi*0.5f,c=std::cos(yaw),s=std::sin(yaw);
    for (size_t m=0;m<car.meshes.size();++m) for (size_t v=0;v<car.meshes[m].pos.size();v+=3) {
        const auto& p=car.meshes[m].pos; const auto& actual=game.Actors().meshes[5+m].pos;
        wheelError=std::max({wheelError,std::abs(c*p[v]-s*p[v+1]+game.State().Car.X-actual[v]),
            std::abs(s*p[v]+c*p[v+1]+game.State().Car.Y-actual[v+1]),std::abs(p[v+2]+game.State().Car.Z-actual[v+2])});
    }
    Check(carStats.wheels==4 && wheelError<0.00001f,"CJ actor offset preserves all four actual CarPose wheel transforms");
    std::printf("cj-wheel maxPosition=%.9g wheels=%d\n",wheelError,carStats.wheels);
    Frames(game,world,90,{.Brake=true}); Frames(game,world,1,{.Interact=true});
    bool restored=!game.State().InVehicle && game.State().Exits==1;
    for (size_t m=0;m<5;++m) restored &= StaticEqual(layout.meshes[m],game.Actors().meshes[m]);
    Check(restored,"all five CJ slots restored on stationary exit");
    E2ELoadInfo load{}; E2EPagerFrame page{}; WorldShotScene visible;
    Require(StreamPager_Init(dir,load,err,sizeof(err),{true,900,4096}),err);
    Require(StreamPager_Update(1600,-1700,70,visible,page,err,sizeof(err)),err);
    saved=rw::TexDictionary::getCurrent();
    Require(game.Initialize(dir,error,&clothes),error);
    Check(rw::TexDictionary::getCurrent()==saved,"CJ startup preserves live pager dictionary");
    Require(world.Rebuild(visible,error),error);
    Require(game.Spawn(world,1540,-1736,20,-pi*0.5f,error),error);
    const auto start=game.State().Ped;
    airborne=0;
    for (int i=0;i<120;++i) { Frames(game,world,1,{.Forward=1}); airborne+=!game.State().Grounded; }
    const float rise=game.State().Ped.Z-start.Z;
    for (int i=0;i<120;++i) { Frames(game,world,1,{.Forward=-1}); airborne+=!game.State().Grounded; }
    Check(rise>0.16f && rise<0.18f && !airborne && std::hypot(game.State().Ped.X-start.X,game.State().Ped.Y-start.Y)<0.05f &&
        std::abs(game.State().Ped.Z-start.Z)<0.01f,"CJ traverses measured streamed district curb up/down");
    std::printf("cj-field rise=%.6f air=%d triangles=%zu binaryRows=%d\n",rise,airborne,world.TriangleCount(),load.binaryInstances);
    StreamPager_Shutdown();
    std::printf("native-player-pose %s failures=%d explicitOutfit=1 defaultOutfitClaim=0\n",Failures ? "FAIL":"PASS",Failures);
    return Failures ? 1:0;
}
