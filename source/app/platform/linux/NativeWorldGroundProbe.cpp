#include "NativeWorldGround.h"
#include "StreamPager.h"
#include <algorithm>
#include <bit>
#include <cstdio>
#include <functional>
#include <iostream>
#include <limits>
#include <set>
#include <stdexcept>

using int32 = int32_t; using uint32 = uint32_t; using int64 = int64_t; using uint64 = uint64_t;
#ifndef __stdcall
#define __stdcall
#endif
#include "oswrapper/oswrapper.h"
static bool s_Sealed{};
static size_t s_Opens{}, s_Checks{};
static void Check(bool ok, const std::string& why) { ++s_Checks; if (!ok) throw std::runtime_error(why); }
int32 OS_FileOpen(OSFileDataArea, void** out, const char* path, OSFileAccessType access) {
    Check(!s_Sealed && access == FILE_ACCESS_READ,"postpublication IO or write"); ++s_Opens;
    *out = std::fopen(path,"rb"); return *out ? 0 : 1;
}
int32 OS_FileClose(void* f) { return std::fclose(static_cast<FILE*>(f)); }
int32 OS_FileSize(void* f) {
    auto* file = static_cast<FILE*>(f); const auto p = std::ftell(file);
    std::fseek(file,0,SEEK_END); const auto n = std::ftell(file); std::fseek(file,p,SEEK_SET);
    Check(n >= 0 && n <= INT32_MAX,"file range"); return static_cast<int32>(n);
}
int32 OS_FileRead(void* f, void* out, int32 n) {
    Check(!s_Sealed && n >= 0,"postpublication read"); return std::fread(out,1,n,static_cast<FILE*>(f)) == size_t(n) ? 0 : 3;
}
int32 OS_FileGetPosition(void* f) { return static_cast<int32>(std::ftell(static_cast<FILE*>(f))); }
void OS_FileSetPosition(void* f, int32 p) { Check(!s_Sealed,"postpublication seek"); std::fseek(static_cast<FILE*>(f),p,SEEK_SET); }
void OS_SetFilePathOffset(const char*) {}

namespace oracle {
enum { ENTITY_TYPE_BUILDING, ENTITY_TYPE_DUMMY, ENTITY_TYPE_VEHICLE, ENTITY_TYPE_PED, ENTITY_TYPE_OBJECT };
struct CVector {
    float x{}, y{}, z{};
    CVector operator+(CVector b) const { return {x+b.x,y+b.y,z+b.z}; }
    void Set(float a,float b,float c) { x=a; y=b; z=c; }
};
struct CQuaternion { CVector imag; float real{}; };
struct CMatrix {
    CVector m_right{1,0,0}, m_forward{0,1,0}, m_up{0,0,1};
    void SetRotate(const CQuaternion&); void SetRotateZOnly(float);
};
struct CRect {
    float left = INFINITY, bottom = INFINITY, right = -INFINITY, top = -INFINITY;
    void StretchToPoint(float x,float y) { left=std::min(left,x); right=std::max(right,x); bottom=std::min(bottom,y); top=std::max(top,y); }
};
struct Box { CVector m_vecMin, m_vecMax; };
struct Col { Box box; const Box& GetBoundingBox() const { return box; } };
struct Model {
    Col col;
    Col* CurrentCol = &col;
    float m_fDrawDistance = 100;
    bool bDoWeOwnTheColModel{};
    Col* GetColModel() { return CurrentCol; }
    void SetOwnsColModel(bool value) { bDoWeOwnTheColModel=value; }
    void DeleteCollisionModel() { CurrentCol=nullptr; }
    void SetColModel(Col* value) { CurrentCol=value; }
};
struct CEntity {
    Model* model{}; NativeSourceGroundTransform transform; bool collision{}, m_bIsBIGBuilding{}, m_bStreamingDontDelete{}, building{};
    bool m_bUnderwater{};
    int32 LodIndex=-1;
    CEntity* Lod{};
    uint8_t LodChildren{};
    Model* GetModelInfo() const { return model; }
    int32 GetLodIndex() const { return LodIndex; }
    uint8_t GetNumLodChildren() const { return LodChildren; }
    CEntity* GetLod() const { return Lod; }
    void SetLod(CEntity* value) { Lod=value; }
    void AddLodChildren() { ++LodChildren; }
    void RemoveLodChildren() { --LodChildren; }
    void SetUsesCollision(bool b) { collision=b; } void SetTypeBuilding() { building=true; }
    int GetType() const { return building ? ENTITY_TYPE_BUILDING : ENTITY_TYPE_DUMMY; }
    void TransformFromObjectSpace(CVector& out,CVector p) const {
        const auto& b=transform.Basis; const auto& t=transform.Position;
        out = {((b[0][0]*p.x+b[1][0]*p.y)+b[2][0]*p.z)+t[0],
               ((b[0][1]*p.x+b[1][1]*p.y)+b[2][1]*p.z)+t[1],
               ((b[0][2]*p.x+b[1][2]*p.y)+b[2][2]*p.z)+t[2]};
    }
    CRect GetBoundRect() const; void SetupBigBuilding(); void Add(const CRect&);
};
struct CBuilding : CEntity { CBuilding(); };
struct List {
    using ItemType=CEntity*; std::vector<CEntity*> Items;
    void AddItem(CEntity* p) { Items.insert(Items.begin(),p); }
};
struct SectorLists { List Buildings,Dummies,Vehicles,Peds,Objects; };
struct CWorld {
    static inline std::array<SectorLists,120*120> Sectors;
    static inline List Lods;
    static SectorLists& GetSector(int x,int y) { Check(x>=0 && x<120 && y>=0 && y<120,"source sector range"); return Sectors[y*120+x]; }
    static SectorLists& GetRepeatSector(int x,int y) { return GetSector(x,y); }
    static List& GetLodPtrList(int,int) { return Lods; }
    static void Add(CEntity* entity) { entity->Add(entity->GetBoundRect()); }
    static float GetSectorfX(float x) { return x/50.0f+60.0f; }
    static float GetSectorfY(float y) { return y/50.0f+60.0f; }
    // SOURCE_SECTOR_ORACLE_INSERT
    template<class Fn> static bool IterateLodSectorsOverlappedByRect(CRect,Fn fn) { return fn(0,0); }
};
static std::array<CEntity*,256> gpLoadedBuildings;
static uint32 gNumLoadedBuildings;
static std::span<CEntity*> GetLoadedBuildings() { return {gpLoadedBuildings.data(),gNumLoadedBuildings}; }
struct Camera { float m_fLODDistMultiplier=1; };
static Camera TheCamera;
struct CColAccel {
    static bool isCacheLoading() { return false; }
    static void addIPLEntity(CEntity**,uint32,uint32) {}
    static void cacheIPLSection(CEntity**,uint32) {}
};
// SOURCE_WORLD_ORACLE_INSERT
#define VERIFY(value) Check((value),"extracted LinkLods source verification")
// SOURCE_LINK_LODS_ORACLE_INSERT
#undef VERIFY
}

using Status = NativeSourceGroundStatus;
using Reason = NativeSourceGroundReason;
using V = NativeCollisionVector;
static NativeCollisionPlacement Placement(int id, const char* model, uint32_t row, bool binary=true) {
    NativeCollisionPlacement p; p.ModelId=id; p.Model=model; p.Record=row; p.Binary=binary;
    p.Ipl=binary ? "fixture_stream0.ipl" : "fixture.ipl"; p.Position={10,10,0}; return p;
}
static std::shared_ptr<NativeCollisionModel> Plane() {
    auto m=std::make_shared<NativeCollisionModel>(); m->Version=1; m->Name="floor";
    m->Min={-200,-200,-1}; m->Max={200,200,1};
    m->Vertices={{-200,-200,0},{200,-200,0},{0,200,0}};
    m->Faces.push_back({{0,1,2},{4,0,0,28}}); return m;
}
static NativeCollisionInstance Bind(const NativeCollisionPlacement& p, const std::shared_ptr<NativeCollisionModel>& m) {
    NativeCollisionInstance i; i.Placement=p; i.Model=m; NativeSourceGroundTransform t;
    Check(NativeWorldGround::SourceTransform(p,t),"fixture transform"); i.Basis=t.Basis; return i;
}
static void Arithmetic() {
    oracle::CBuilding b; oracle::Model om; b.model=&om;
    Check(b.building && b.collision && !b.m_bIsBIGBuilding,"source CBuilding constructor flags");
    b.SetupBigBuilding(); Check(!b.collision && b.m_bIsBIGBuilding && b.m_bStreamingDontDelete,"source SetupBigBuilding flags");
    oracle::CBuilding first,second;
    const oracle::CRect extent{-100,-50,50,0};
    first.Add(extent); second.Add(extent);
    for (int y=0;y<120;++y) for (int x=0;x<120;++x) {
        const auto& list=oracle::CWorld::GetSector(x,y).Buildings.Items;
        const bool included=x>=58 && x<=61 && y>=59 && y<=60;
        Check(list.size()==(included ? 2u : 0u),"extracted source Add inclusive upper sector membership");
        if (included) Check(list[0]==&second && list[1]==&first,"source Add head-insertion ordinal");
    }
    b.Add(extent); Check(oracle::CWorld::Lods.Items.size()==1,"source BigBuilding LOD-sector insertion");
    oracle::CBuilding clipped;
    clipped.Add({-3100,-3100,3100,3100});
    Check(oracle::CWorld::GetSector(0,0).Buildings.Items.front()==&clipped &&
          oracle::CWorld::GetSector(119,119).Buildings.Items.front()==&clipped,"source asymmetric +/-3000 clamp");
    for (const auto q : {std::array<float,4>{0,0,0,1}, {0.03f,0.04f,0.2f,0.98f}, {0.1f,0.2f,-0.3f,0.9f}, {0,0,-0.7f,0.7f}}) {
        for (uint32_t flags : {0u,512u,0x80001f00u}) {
            auto p=Placement(1,"floor",0); p.Quaternion=q; p.Flags=flags;
            NativeSourceGroundTransform t; Check(NativeWorldGround::SourceTransform(p,t),"valid source transform");
            oracle::CMatrix matrix;
            if (std::abs(q[0])>0.05f || std::abs(q[1])>0.05f || ((flags&512) && q[0]!=0 && q[1]!=0))
                matrix.SetRotate({{-q[0],-q[1],-q[2]},q[3]});
            else matrix.SetRotateZOnly(std::acos(q[3])*(q[2]<0 ? 2.0f : -2.0f));
            const std::array<V,3> expected{V{matrix.m_right.x,matrix.m_right.y,matrix.m_right.z},
                V{matrix.m_forward.x,matrix.m_forward.y,matrix.m_forward.z},V{matrix.m_up.x,matrix.m_up.y,matrix.m_up.z}};
            for (size_t i=0;i<3;++i) for (size_t j=0;j<3;++j)
                Check(std::bit_cast<uint32_t>(t.Basis[i][j])==std::bit_cast<uint32_t>(expected[i][j]),"extracted source matrix bits");
            auto m=Plane(); m->Min={-170,-33,-12}; m->Max={231,11,40};
            om.col.box={{-170,-33,-12},{231,11,40}}; b.transform=t;
            const auto r=b.GetBoundRect(); const auto actual=NativeWorldGround::SourceRect(*m,t);
            Check(actual==std::array<float,4>{r.left,r.bottom,r.right,r.top},"extracted four-corner source rectangle");
        }
    }
    for (auto [x,sector] : {std::pair{-3000.0f,0}, {-50.01f,58}, {-50.0f,59}, {-0.01f,59}, {0.0f,60}, {49.99f,60}, {50.0f,61}, {2999.0f,119}})
        Check(NativeWorldGround::Sector(x)==sector,"50m source sector boundary");
}
static void ContextualLodInvariant() {
    // Execute the complete source body, not a restatement of its condition.
    // Every text-parent/child order must keep at least one child even when
    // the multi-child branch removes earlier siblings with non-owned COL.
    for (uint32 children : {1u,2u,3u}) {
        for (uint32 parentIndex=0; parentIndex<=children; ++parentIndex) {
            std::array<oracle::CBuilding,4> entities;
            std::array<oracle::Model,4> models;
            for (uint32 index=0; index<=children; ++index) {
                entities[index].model=&models[index];
                entities[index].LodIndex=index==parentIndex ? -1 : int32(parentIndex);
                oracle::gpLoadedBuildings[index]=&entities[index];
            }
            oracle::gNumLoadedBuildings=children+1;
            oracle::LinkLods(0);
            Check(entities[parentIndex].GetNumLodChildren()>=1 &&
                entities[parentIndex].m_bIsBIGBuilding && !entities[parentIndex].collision,
                "extracted source LinkLods never removes the last text child in any encounter order");
        }
    }
    std::array<oracle::CBuilding,2> entities;
    std::array<oracle::Model,2> models;
    entities[0].model=&models[0]; entities[1].model=&models[1];
    entities[1].SetLod(&entities[0]);
    entities[0].AddLodChildren(); // exact LoadIplBoundingBox parent handoff
    oracle::gpLoadedBuildings[0]=&entities[0]; oracle::gpLoadedBuildings[1]=&entities[1];
    oracle::gNumLoadedBuildings=1;
    oracle::LinkLods(1);
    Check(entities[0].m_bIsBIGBuilding && !entities[0].collision &&
        models[0].GetColModel()==models[1].GetColModel(),
        "source related-binary tail aliases COL only after text parent becomes collision-disabled");
}
static void Fixtures() {
    Arithmetic(); ContextualLodInvariant(); std::string error;
    NativeCollisionPopulation p; p.IncludesStreamed=true;
    p.Models={{1,{"floor",false}},{2,{"dummy",false}},{3,{"unknowncol",false}},{4,{"big",false}}};
    p.Instances={Placement(1,"floor",0),Placement(2,"dummy",1)};
    p.Instances[0].Flags=0x80001f00u; // No initial entity flag inference from authored bits.
    const std::vector<NativeWorldEntitySourceText> ide{{"fixture.ide","objs\n1 floor txd 100 0\n2 dummy txd 100 0\n3 unknowncol txd 100 0\n4 big txd 400 0\nend\n"}};
    const NativeWorldEntitySourceText objects{"Object.dat","dummy 1 2 3 4 5 6 7 0 0 0 0 0\n*\n"};
    const auto load=[&](const auto& population) { NativeWorldEntityInfo info; Check(info.LoadSources(population,ide,objects,error),error); return info; };
    auto model=Plane(); auto bindings=std::make_shared<NativeCollisionSnapshot>(); bindings->Instances={Bind(p.Instances[0],model)};
    bindings->MissingModels=1; bindings->KnownAbsence["dummy"]=1;
    auto world=NativeWorldGround::PreparePopulation(p,load(p),*bindings,error,7); Check(bool(world),error);
    NativeWorldGroundCommit commit{10,7,10,10,500,bindings,1.0f};
    auto published=world->Publish(commit,10,10); auto r=NativeWorldGround::Query(published,{10,10,10},10,7);
    Check(r.Status==Status::Hit && r.Point[2]==0 && published.Snapshot->Targets.size()==1,"missing Dummy excluded by source class, not missing COL count");
    Check(published.Snapshot->Targets[0].UsesCollision==NativeSourceGroundKnown::Yes,"authored high bits don't disable building");
    Check(NativeWorldGround::Query(published,{10,10,10},9,7).Reason==Reason::StaleWorld,"generation pin");
    Check(NativeWorldGround::Query(published,{50,10,10},10,7).Reason==Reason::OutsideCoverage,"half-open source sector");
    Check(world->Publish(commit,325,2537).Diagnostics.front().Issue==NativeWorldGroundIssue::CommittedGeometry,"first far query needs actual committed world snapshot");
    auto old=published.Snapshot; auto stale=commit; --stale.WorldGeneration;
    Check(!world->Publish(stale,10,10,published,error) && old==published.Snapshot,"generation rollback atomic");
    stale=commit; ++stale.MetadataRevision;
    Check(!world->Publish(stale,10,10,published,error) && old==published.Snapshot,"metadata mismatch atomic");
    auto moved=std::make_shared<NativeCollisionSnapshot>(*bindings);
    NativePlacementOverride override; override.Identity=NativePlacementIdentity::From(p.Instances[0]);
    override.Position={300,10,5}; override.Basis={V{1,0,0},V{0,1,0},V{0,0,1}};
    moved->Overrides=std::make_shared<const NativePlacementOverrides>(std::vector{override});
    moved->Instances[0].Placement.Position=override.Position; moved->Instances[0].Basis=override.Basis;
    stale=commit; stale.SourceCollision=moved;
    Check(!world->Publish(stale,300,10,published,error) && old==published.Snapshot,"same generation changed owner rejected");
    ++stale.WorldGeneration;
    Check(world->Publish(stale,300,10,published,error),error);
    r=NativeWorldGround::Query(published,{300,10,10},11,7);
    Check(r.Status==Status::Hit && r.Point[2]==5,"effective override transform, no authored-origin window");
    Check(NativeWorldGround::Query({old,bindings,{}},{10,10,10},10,7).Point[2]==0,"old immutable publication survives replacement");
    auto disabled=std::make_shared<NativeCollisionSnapshot>(*moved); override.CollisionEnabled=false;
    disabled->Overrides=std::make_shared<const NativePlacementOverrides>(std::vector{override});
    auto disabledCommit=stale; ++disabledCommit.WorldGeneration; disabledCommit.SourceCollision=disabled;
    Check(NativeWorldGround::Query(world->Publish(disabledCommit,300,10),{300,10,10},12,7).Status==Status::Miss,
        "committed collision-disable overrides source constructor flag");
    // Whole source sector sees a large-bound building whose origin is elsewhere.
    auto large=world->Publish(commit,150,10);
    Check(large.Snapshot->Targets.size()==1 && large.Snapshot->Targets[0].BigBuilding==NativeSourceGroundKnown::No,"large bounds are not BigBuilding");
    auto bad=p; bad.Instances.push_back(Placement(3,"unknowncol",2)); bad.Instances.back().Position={2000,2000,0};
    auto unresolved=NativeWorldGround::PreparePopulation(bad,load(bad),*bindings,error,7);
    auto unknown=unresolved->Publish(commit,10,10);
    Check(NativeWorldGround::Query(unknown,{10,10,10},10,7).Status==Status::Unsupported &&
        unknown.Diagnostics.front().Candidate.Model=="unknowncol","unresolved building retained despite far origin");
    auto duplicate=p; duplicate.Instances.push_back(p.Instances[0]);
    Check(!NativeWorldGround::PreparePopulation(duplicate,load(p),*bindings,error),"duplicate identity rejected");
    auto incomplete=p; incomplete.IncludesStreamed=false;
    auto partial=NativeWorldGround::PreparePopulation(incomplete,load(p),*bindings,error,7);
    Check(NativeWorldGround::Query(partial->Publish(commit,10,10),{10,10,10},10,7).Reason==Reason::UnknownCoverage,"filtered population cannot assert completeness");
    auto text=p; text.Instances[0].Binary=false; text.Instances[0].Ipl="fixture.ipl";
    auto tb=std::make_shared<NativeCollisionSnapshot>(); tb->Instances={Bind(text.Instances[0],model)};
    auto tw=NativeWorldGround::PreparePopulation(text,load(text),*tb,error,7); auto tc=commit; tc.SourceCollision=tb;
    Check(NativeWorldGround::Query(tw->Publish(tc,10,10),{10,10,10},10,7).Status==Status::Hit,"text high-bit row with no possible inbound LOD and pinned multiplier");
    tc.LodDistanceMultiplier.reset();
    Check(NativeWorldGround::Query(tw->Publish(tc,10,10),{10,10,10},10,7).Status==Status::Unsupported,"unknown text multiplier");
    // Incoming child makes a Lod==-1 text parent uncertain; never use old bounds.
    text.Instances[1].Lod=0;
    tw=NativeWorldGround::PreparePopulation(text,load(text),*tb,error,7); tc.LodDistanceMultiplier=1;
    auto lod=tw->Publish(tc,450,450);
    Check(lod.Snapshot->Targets.size()==1 && !lod.Snapshot->Targets[0].CollisionModelKnown,"LOD model-wide rebinding cannot be windowed by old box");
    auto big=p; big.Instances[0]=Placement(4,"big",0,false); big.Instances[0].Lod=-1;
    auto bb=std::make_shared<NativeCollisionSnapshot>(); bb->Instances={Bind(big.Instances[0],model)};
    auto bw=NativeWorldGround::PreparePopulation(big,load(big),*bb,error,7); auto bc=commit; bc.SourceCollision=bb;
    Check(NativeWorldGround::Query(bw->Publish(bc,10,10),{10,10,10},10,7).Status==Status::Miss,"source text draw-distance BigBuilding disabled");
    auto tie=p; tie.Instances.push_back(Placement(1,"floor",2));
    auto cb=std::make_shared<NativeCollisionSnapshot>(*bindings); cb->Instances.push_back(Bind(tie.Instances.back(),model));
    auto cw=NativeWorldGround::PreparePopulation(tie,load(tie),*cb,error,7); auto cc=commit; cc.SourceCollision=cb;
    auto same=cw->Publish(cc,10,10); r=NativeWorldGround::Query(same,{10,10,10},10,7);
    Check(r.Status==Status::Hit && same.Snapshot->Targets[r.TargetIndex].Identity.Record==2,"same binary source AddItem head reverse record ordinal");
    tie.Instances.back().Ipl="other_stream0.ipl"; cb->Instances.back().Placement=tie.Instances.back();
    cw=NativeWorldGround::PreparePopulation(tie,load(tie),*cb,error,7);
    Check(NativeWorldGround::Query(cw->Publish(cc,10,10),{10,10,10},10,7).Reason==Reason::UnknownSourceOrder,"equal closest cross-IPL order unknown");
    auto empty=std::make_shared<NativeCollisionModel>(); empty->Version=2; empty->Empty=true;
    empty->Min={-10,-10,-1}; empty->Max={10,10,1};
    auto eb=std::make_shared<NativeCollisionSnapshot>(); eb->Instances={Bind(p.Instances[0],empty)};
    auto ew=NativeWorldGround::PreparePopulation(p,load(p),*eb,error,7); auto ec=commit; ec.SourceCollision=eb;
    Check(ew->Publish(ec,10,10).Snapshot->Targets.empty(),"V2 HasCollisionVolumes false source disables collision");
    empty=std::make_shared<NativeCollisionModel>(*empty); empty->Flags=2;
    eb=std::make_shared<NativeCollisionSnapshot>(); eb->Instances={Bind(p.Instances[0],empty)}; ec.SourceCollision=eb;
    ew=NativeWorldGround::PreparePopulation(p,load(p),*eb,error,7); auto ep=ew->Publish(ec,10,10);
    Check(ep.Snapshot->Targets.size()==1 && ep.Snapshot->Targets[0].UsesCollision==NativeSourceGroundKnown::Yes &&
        NativeWorldGround::Query(ep,{10,10,10},10,7).Status==Status::Miss,"V2 primitive-empty != HasCollisionVolumes false");
    // The actual reader omits these from geometric Instances, but must retain
    // their exact constructor headers in the same publication.
    auto retainedEmpty=std::make_shared<NativeCollisionSnapshot>();
    retainedEmpty->EmptyBindings.emplace("floor",empty);
    ec.SourceCollision=retainedEmpty;
    ew=NativeWorldGround::PreparePopulation(p,load(p),*retainedEmpty,error,7);
    Check(bool(ew),error);
    ep=ew->Publish(ec,10,10);
    Check(retainedEmpty->Instances.empty() && ep.Snapshot->Targets.size()==1 &&
        ep.Snapshot->Targets.front().Model==empty &&
        NativeWorldGround::Query(ep,{10,10,10},10,7).Status==Status::Miss,
        "retained primitive-empty header supplies bounds and flags without invented geometry");
    auto missingEmpty=std::make_shared<NativeCollisionSnapshot>();
    ec.SourceCollision=missingEmpty;
    Check(NativeWorldGround::Query(ew->Publish(ec,10,10),{10,10,10},10,7).Status==Status::Unsupported,
        "absent committed empty header is not known absence or clearance");
    auto changedEmpty=std::make_shared<NativeCollisionSnapshot>(*retainedEmpty);
    changedEmpty->EmptyBindings["floor"]=std::make_shared<NativeCollisionModel>(*empty);
    ec.SourceCollision=changedEmpty;
    Check(NativeWorldGround::Query(ew->Publish(ec,10,10),{10,10,10},10,7).Status==Status::Unsupported,
        "same-name changed empty header cannot satisfy old prepared owner");
    Check(NativeWorldGround::Query(ep,{10,10,10},10,7).Status==Status::Miss,
        "held empty-header publication remains immutable");
    auto noVolumes=std::make_shared<NativeCollisionModel>(*empty); noVolumes->Flags=0;
    auto noVolumesBinding=std::make_shared<NativeCollisionSnapshot>();
    noVolumesBinding->EmptyBindings.emplace("floor",noVolumes);
    ec.SourceCollision=noVolumesBinding;
    ew=NativeWorldGround::PreparePopulation(p,load(p),*noVolumesBinding,error,7);
    Check(bool(ew) && ew->Publish(ec,10,10).Snapshot->Targets.empty(),
        "retained no-volume header applies source collision-disabled constructor state");
    ec.SourceCollision=missingEmpty;
    Check(NativeWorldGround::Query(ew->Publish(ec,10,10),{10,10,10},10,7).Status==Status::Unsupported,
        "missing no-volume proof cannot disable collision by an old header");
    auto invalidEmpty=std::make_shared<NativeCollisionSnapshot>();
    auto invalidModel=std::make_shared<NativeCollisionModel>(*empty);
    invalidModel->Faces.push_back({{0,1,2},{}});
    invalidEmpty->EmptyBindings.emplace("floor",invalidModel);
    Check(!NativeWorldGround::PreparePopulation(p,load(p),*invalidEmpty,error,7),
        "empty header with nonempty primitive array rejected");
    invalidModel=std::make_shared<NativeCollisionModel>(*empty);
    invalidModel->Min[0]=std::numeric_limits<float>::quiet_NaN();
    invalidEmpty->EmptyBindings["floor"]=invalidModel;
    Check(!NativeWorldGround::PreparePopulation(p,load(p),*invalidEmpty,error,7),
        "nonfinite empty header cannot provide collision-disable or bound proof");
    invalidModel=std::make_shared<NativeCollisionModel>(*empty);
    invalidModel->Min[0]=invalidModel->Max[0]+1.0f;
    invalidEmpty->EmptyBindings["floor"]=invalidModel;
    Check(!NativeWorldGround::PreparePopulation(p,load(p),*invalidEmpty,error,7),
        "inverted empty header bounds rejected");
    auto unresolvedEmptyLod=p;
    unresolvedEmptyLod.Instances[0].Binary=false;
    unresolvedEmptyLod.Instances[0].Ipl="fixture.ipl";
    unresolvedEmptyLod.Instances[1].Lod=0;
    ew=NativeWorldGround::PreparePopulation(unresolvedEmptyLod,load(unresolvedEmptyLod),*noVolumesBinding,error,7);
    ec.SourceCollision=noVolumesBinding;
    Check(bool(ew) && NativeWorldGround::Query(ew->Publish(ec,10,10),{10,10,10},10,7).Status==Status::Unsupported,
        "unresolved model-wide LOD reassignment cannot inherit old no-volume clearance");
    auto malformed=std::make_shared<NativeCollisionModel>(); malformed->Unsupported="fixture unverified header";
    eb=std::make_shared<NativeCollisionSnapshot>(); eb->Instances={Bind(p.Instances[0],malformed)}; ec.SourceCollision=eb;
    ew=NativeWorldGround::PreparePopulation(p,load(p),*eb,error,7);
    Check(NativeWorldGround::Query(ew->Publish(ec,450,450),{450,450,10},10,7).Status==Status::Unsupported,
        "unsupported COL cannot advertise no volumes or trustworthy remote bounds");
    std::cout<<"PASS independent source constructor/matrix/sector/class/flags/coverage/override/LOD/order/rollback fixtures\n";
}

static void Actual(const char* game) {
    char message[512]{}; std::string error; E2ELoadInfo stats;
    Check(StreamPager_Init(game,stats,message,sizeof(message),{true,900,4096}),message);
    auto context=NativeCollisionContext::LoadBeforeWorker(game,900,error); Check(bool(context),error);
    NativeWorldEntityInfo info; Check(info.LoadBeforeWorker(game,context->Population,error),error);
    auto world=NativeWorldGround::Prepare(*context,info,error,23); Check(bool(world),error);
    const auto catalog=NativeLodCatalog::LoadBeforeWorker(game,context->Population,error);
    Check(bool(catalog),error);
    auto contextual=NativeWorldGround::PrepareWithCatalog(*context,info,*catalog,{false,1.0f},error,23);
    Check(bool(contextual),error);
    Check(!NativeWorldGround::PrepareWithCatalog(*context,info,*catalog,{true,1.0f},error,23),
        "cache-loading lane cannot be inferred from the initial non-cache graph");
    Check(!NativeWorldGround::PrepareWithCatalog(*context,info,*catalog,{false,0.0f},error,23),
        "invalid source initial LOD profile rejected");
    auto changedContext=*context; changedContext.Population.Instances.front().Position[0]+=1.0f;
    Check(!NativeWorldGround::PrepareWithCatalog(changedContext,info,*catalog,{false,1.0f},error,23),
        "changed placement cannot reuse disk graph constructor dependencies");
    auto startup=std::make_shared<NativeCollisionSnapshot>(), airfield=std::make_shared<NativeCollisionSnapshot>(), downtown=std::make_shared<NativeCollisionSnapshot>();
    auto missionWorld=std::make_shared<NativeCollisionSnapshot>();
    Check(context->Snapshot(2488.562255859375f,-1666.864501953125f,*startup,error),error);
    Check(context->Snapshot(325,2537,*airfield,error),error);
    Check(context->Snapshot(1534,-1747,*downtown,error),error);
    Check(context->Snapshot(2358.65625f,-1246.34814453125f,*missionWorld,error),error);
    // Regression for the real shared LOD model that used to be omitted using
    // its old empty/no-volume header. Far envelopes can be excluded, but a
    // nearby possible collider must remain Unsupported, not guessed as clear.
    const auto sharedParent=std::ranges::find_if(catalog->Nodes(),[](const auto& node) {
        return node.Identity.ModelId==3296 && !node.Identity.Binary && !node.Children.empty();
    });
    const auto sharedInstance=std::ranges::find_if(context->Population.Instances,[](const auto& placement) {
        return placement.ModelId==3296 && placement.Binary && placement.Interior==0;
    });
    Check(sharedParent!=catalog->Nodes().end() && sharedInstance!=context->Population.Instances.end(),
        "real shared lod_oiltank constructor/reassignment fixture");
    const auto possibleCollider=context->Assets.LookupModel(catalog->Nodes()[sharedParent->Children.front()].Identity.Model);
    Check(possibleCollider.Status==NativeCollisionModelStatus::Ready && bool(possibleCollider.Model),
        "real contextual child supplies possible collider, not an invented default box");
    NativeSourceGroundTransform sharedTransform;
    Check(NativeWorldGround::SourceTransform(*sharedInstance,sharedTransform),"real shared LOD transform");
    const auto sharedRect=NativeWorldGround::SourceRect(*possibleCollider.Model,sharedTransform,&*sharedInstance);
    const float sharedX=(sharedRect[0]+sharedRect[2])*0.5f, sharedY=(sharedRect[1]+sharedRect[3])*0.5f;
    auto sharedCollision=std::make_shared<NativeCollisionSnapshot>();
    Check(context->Snapshot(sharedX,sharedY,*sharedCollision,error),error);
    Check(context->Population.Instances.size()==50935 && startup->Instances.size()==5805,"actual full IPL/source COL startup census");
    NativeCollisionSnapshot all; Check(context->Assets.Snapshot(context->Population,0,0,1e30f,all,error),error);
    Check(!all.EmptyBindings.empty() && all.EmptyBindings.size()<=all.EmptyModels,
        "reader preserves primitive-empty headers alongside unchanged geometric census");
    for (const auto& [name,model] : all.EmptyBindings) {
        const auto exact=context->Assets.LookupModel(name);
        Check(exact.Status==NativeCollisionModelStatus::Empty && exact.Model==model &&
            model->Empty && model->Spheres.empty() && model->Boxes.empty() && model->Faces.empty(),
            "retained empty header is exact source catalog ownership, not a default box");
    }
    std::set<std::tuple<std::string,uint32_t,bool>> bound;
    for (const auto& i:all.Instances) bound.emplace(i.Placement.Ipl,i.Placement.Record,i.Placement.Binary);
    std::vector<NativePlacementIdentity> omittedDummies;
    size_t highText{}, restoredLegacyCandidates{}, absentDummies{};
    for (const auto& p:context->Population.Instances) {
        const auto md=info.Query(p); Check(md.Status==NativeWorldInfoStatus::Ready,"actual metadata complete");
        highText+=!p.Binary && p.Interior==0 && (p.Flags&~255u)!=0;
        // Census only: this historical renderer prefix rule is NOT authority.
        restoredLegacyCandidates+=!p.Binary && p.Interior==0 && (p.Flags&~255u)!=0 && !p.Model.starts_with("lod");
        absentDummies+=p.Interior==0 && md.Model->InitialClass==NativeWorldInitialClass::DummyObject && all.KnownAbsence.contains(p.Model);
        if (p.Interior==0 && md.Model->InitialClass==NativeWorldInitialClass::DummyObject &&
            !bound.contains({p.Ipl,p.Record,p.Binary}))
            omittedDummies.push_back(NativePlacementIdentity::From(p));
    }
    std::cout<<"CENSUS flaggedOutdoorText="<<highText<<" omittedDummies="<<omittedDummies.size()
        <<" catalogMissingModels="<<all.MissingModels<<" primitiveEmptyModels="<<all.EmptyModels<<'\n';
    Check(highText==995 && restoredLegacyCandidates==498 && omittedDummies.size()==55 && absentDummies==0 &&
        all.MissingModels==4287 && all.EmptyModels==5987,
        "actual flagged text/omitted Dummy fixtures; catalog absence distinct from empty omission");
    std::cout<<"OMITTED_DUMMY model="<<omittedDummies.front().Model<<" id="<<omittedDummies.front().ModelId
        <<" ipl="<<omittedDummies.front().Ipl<<" record="<<omittedDummies.front().Record<<" reason=primitive-empty-COL (not catalog absence)\n";
    StreamPager_Shutdown(); s_Sealed=true; const auto opens=s_Opens;
    NativeWorldGroundCommit sc{31,23,2488.562255859375f,-1666.864501953125f,900,startup,1.0f};
    auto far=world->Publish(sc,325,2537);
    Check(NativeWorldGround::Query(far,{325,2537,17.5f},31,23).Reason==Reason::UnknownCoverage,"far world not startup geometry");
    const NativeWorldGroundCommit sharedCommit{35,23,sharedX,sharedY,900,sharedCollision,1.0f};
    const auto sharedPublication=contextual->Publish(sharedCommit,sharedX,sharedY);
    Check(std::ranges::any_of(sharedPublication.Snapshot->Targets,[&](const auto& target) {
        return target.Identity.Matches(*sharedInstance) && !target.CollisionModelKnown &&
            target.UsesCollision==NativeSourceGroundKnown::Unknown;
    }) && NativeWorldGround::Query(sharedPublication,{sharedX,sharedY,sharedInstance->Position[2]},35,23).Status==Status::Unsupported,
        "nearby shared LOD collider envelope retains unknown effective constructor/owner");
    auto missingSharedHeader=std::make_shared<NativeCollisionSnapshot>(*missionWorld);
    Check(missingSharedHeader->EmptyBindings.erase("lod_oiltank")==1,
        "exact shared empty header exists in the full-interior source publication");
    const NativeWorldGroundCommit missingSharedCommit{36,23,2358.65625f,-1246.34814453125f,900,missingSharedHeader,1.0f};
    Check(NativeWorldGround::Query(contextual->Publish(missingSharedCommit,2229.5f,-1342.0f),
        {2229.5f,-1342.0f,23.125f},36,23).Status==Status::Unsupported,
        "missing shared constructor header forbids exclusion by an old collider envelope");
    for (const auto& [x,y,z,air] : {std::tuple{2488.562255859375f,-1666.864501953125f,30.0f,false},
         {325.0f,2537.0f,17.5f,true}, {1534.0f,-1747.0f,11.0f,false},
         {2229.5f,-1342.0f,23.125f,false}}) {
        auto commit=sc; if (air) { commit={32,23,325,2537,900,airfield,1.0f}; }
        if (x==1534) commit={33,23,1534,-1747,900,downtown,1.0f};
        if (x==2229.5f) commit={34,23,2358.65625f,-1246.34814453125f,900,missionWorld,1.0f};
        auto pub=world->Publish(commit,x,y); auto r=NativeWorldGround::Query(pub,{x,y,z},commit.WorldGeneration,23);
        Check(r.Status==Status::Unsupported,
            "actual world query remains Unsupported without full LinkLods/model bindings and source-order authority");
        size_t omittedDummyCandidates{};
        for (const auto& t:pub.Snapshot->Targets)
            omittedDummyCandidates+=std::ranges::find(omittedDummies,t.Identity)!=omittedDummies.end();
        Check(omittedDummyCandidates==0,"actual snapshot-omitted Dummy is not building candidate");
        std::map<NativeWorldGroundIssue,size_t> issues; for (const auto& d:pub.Diagnostics) ++issues[d.Issue];
        std::cout<<"ACTUAL query="<<x<<','<<y<<','<<z<<" status="<<int(r.Status)<<" reason="<<int(r.Reason)
            <<" candidates="<<pub.Snapshot->Targets.size()<<" diagnostics="<<pub.Diagnostics.size()<<"\n";
        for (const auto& [issue,n]:issues) {
            const auto d=std::ranges::find_if(pub.Diagnostics,[&](const auto& v){return v.Issue==issue;});
            std::cout<<"LIMIT issue="<<int(issue)<<" count="<<n<<" model="<<d->Candidate.Model<<" id="<<d->Candidate.ModelId
                <<" ipl="<<d->Candidate.Ipl<<" record="<<d->Candidate.Record<<" source="<<d->SourceReason<<'\n';
        }
        const auto contextualPublication=contextual->Publish(commit,x,y);
        const auto contextualResult=NativeWorldGround::Query(contextualPublication,{x,y,z},commit.WorldGeneration,23);
        Check(contextualPublication.Snapshot->Targets.size()<=pub.Snapshot->Targets.size(),
            "exact contextual IPL dependency set does not invent additional potential buildings");
        std::map<NativeWorldGroundIssue,size_t> contextualIssues;
        for (const auto& diagnostic:contextualPublication.Diagnostics) ++contextualIssues[diagnostic.Issue];
        std::cout<<"CONTEXTUAL query="<<x<<','<<y<<','<<z<<" status="<<int(contextualResult.Status)
            <<" reason="<<int(contextualResult.Reason)<<" candidates="<<contextualPublication.Snapshot->Targets.size()
            <<" diagnostics="<<contextualPublication.Diagnostics.size()
            <<" z="<<contextualResult.Point[2]<<" primitive="<<contextualResult.PrimitiveIndex<<'\n';
        for (const auto& [issue,count]:contextualIssues) {
            const auto diagnostic=std::ranges::find_if(contextualPublication.Diagnostics,
                [&](const auto& value){return value.Issue==issue;});
            std::cout<<"CONTEXTUAL_LIMIT issue="<<int(issue)<<" count="<<count
                <<" model="<<diagnostic->Candidate.Model<<" id="<<diagnostic->Candidate.ModelId
                <<" source="<<diagnostic->SourceReason<<'\n';
        }
        Check(contextualResult.Status==Status::Hit && contextualPublication.Diagnostics.empty(),
            "complete contextual native initial-world sector yields a source building-only hit");
        const auto expectedZ=x==2488.562255859375f ? 12.34375f : x==325.0f ? 15.80784416f :
            x==1534.0f ? 10.484375f : 22.99150276f;
        Check(std::bit_cast<uint32_t>(contextualResult.Point[2])==std::bit_cast<uint32_t>(expectedZ),
            "actual contextual building-only height bits");
        auto changedProfile=commit; changedProfile.LodDistanceMultiplier=1.2f;
        Check(contextual->Publish(changedProfile,x,y).Diagnostics.front().Issue==NativeWorldGroundIssue::StaleCommit,
            "contextual initial LOD state is pinned to the prepared profile");
        if (air) {
            const auto it=std::ranges::find_if(airfield->Instances,[](const auto& i){return i.Placement.ModelId==16177 && i.Placement.Record==32;});
            Check(it!=airfield->Instances.end(),"Rustler source terrain binding"); NativeSourceGroundTransform t;
            Check(NativeWorldGround::SourceTransform(it->Placement,t),"Rustler source transform");
            const auto local=NativeSourceGround::ProcessVerticalLine(*it->Model,t,NativeSourceGroundRequest::Generator({325,2537,17.5f}));
            Check(local.Status==Status::Hit && std::bit_cast<uint32_t>(local.Point[2])==std::bit_cast<uint32_t>(15.80784416f),"source local Rustler witness, not world authority");
            std::cout<<"LOCAL Rustler ne_bit_07 triangle="<<local.PrimitiveIndex<<" z="<<local.Point[2]<<" (not substituted for world query)\n";
            const auto candidate=std::ranges::find_if(pub.Snapshot->Targets,[&](const auto& v){return v.Identity.Matches(it->Placement);});
            const auto rect=NativeWorldGround::SourceRect(*it->Model,t,&it->Placement);
            Check(candidate!=pub.Snapshot->Targets.end() && candidate->BigBuilding==NativeSourceGroundKnown::No &&
                candidate->NormalSector==NativeSourceGroundKnown::Yes && rect[2]-rect[0]>50,
                "actual large-bound binary terrain building remains source normal-sector candidate");
            std::cout<<"LARGE_BUILDING ne_bit_07 rect="<<rect[0]<<','<<rect[1]<<','<<rect[2]<<','<<rect[3]<<" normalSector=Yes bigBuilding=No\n";
        }
    }
    Check(opens==s_Opens,"publication/query reopened global assets");
    std::cout<<"PASS actual population=50935 startupCOL=5805 flaggedOutdoorText="<<highText<<" restoredLegacyCandidates="<<restoredLegacyCandidates<<" omittedDummies="<<omittedDummies.size()
        <<" postpublicationIO=0; native-definition coverage != retail CIplStore parity\n";
}
int main(int argc,char** argv) try {
    std::cout.precision(10); Fixtures(); if (argc==2) Actual(argv[1]);
    std::cout<<"PASS NativeWorldGround checks="<<s_Checks<<'\n'; return 0;
} catch (const std::exception& e) { std::cerr<<"FAIL "<<e.what()<<'\n'; return 1; }
