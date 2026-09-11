// Compiled by NativeSourceGroundProbe.py, which injects independently extracted
// reversed source functions into the marker below. No hand-written hit oracle.
#include "NativeSourceGround.h"
#include "NativeWorldEntityInfo.h"
#include "extensions/FixedFloat.hpp"
#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <tuple>

using int32=int32_t; using int64=int64_t; using uint32=uint32_t; using uint64=uint64_t;
#define __stdcall
#include "oswrapper/oswrapper.h"
static size_t s_FileOpens{};
int32 OS_FileOpen(OSFileDataArea,void** out,const char* path,OSFileAccessType access) {
    if (access!=FILE_ACCESS_READ) throw std::runtime_error("probe read-only IO");
    ++s_FileOpens; *out=std::fopen(path,"rb"); return *out ? 0 : 1;
}
int32 OS_FileClose(void* f) { return std::fclose(static_cast<FILE*>(f)); }
int32 OS_FileSize(void* file) {
    auto* f=static_cast<FILE*>(file); const auto pos=std::ftell(f); std::fseek(f,0,SEEK_END);
    const auto size=std::ftell(f); std::fseek(f,pos,SEEK_SET); return int32(size);
}
int32 OS_FileRead(void* f,void* out,int32 size) { return std::fread(out,1,size,static_cast<FILE*>(f))==size_t(size) ? 0 : 3; }
int32 OS_FileGetPosition(void* f) { return int32(std::ftell(static_cast<FILE*>(f))); }
void OS_FileSetPosition(void* f,int32 pos) { std::fseek(static_cast<FILE*>(f),pos,SEEK_SET); }

namespace oracle {
struct CVector {
    float x{}, y{}, z{};
    void Set(float a,float b,float c) { x=a; y=b; z=c; }
    CVector operator+(CVector b) const { return {x+b.x,y+b.y,z+b.z}; }
    CVector operator-(CVector b) const { return {x-b.x,y-b.y,z-b.z}; }
    CVector operator-() const { return {-x,-y,-z}; }
    CVector operator*(float f) const { return {x*f,y*f,z*f}; }
    friend CVector operator*(float f,CVector v) { return v*f; }
    float Dot(const CVector&) const;
    CVector Cross(const CVector&) const;
    float NormaliseAndMag();
    CVector Normalized() const { auto c=*this; c.NormaliseAndMag(); return c; }
};
struct CVector2D {
    float x{},y{};
    CVector2D operator-(CVector2D b) const { return {x-b.x,y-b.y}; }
    float Cross(CVector2D b) const { return x*b.y-y*b.x; }
};
template<typename T, float Scale> struct FixedVector {
    FixedFloat<T,Scale> x{},y{},z{};
    FixedVector() = default;
    FixedVector(CVector v) : x(v.x),y(v.y),z(v.z) {}
    operator CVector() const { return {x,y,z}; }
};
using CompressedVector = FixedVector<int16_t,128.0f>;
struct CColTrianglePlane {
    enum class Orientation : uint8_t { POS_X,NEG_X,POS_Y,NEG_Y,POS_Z,NEG_Z };
    FixedVector<int16_t,4096.0f> m_normal;
    FixedFloat<int16_t,128.0f> m_normalOffset;
    Orientation m_orientation;
    CColTrianglePlane(const CVector&,const CVector&,const CVector&);
    CVector GetNormal() const { return m_normal; }
    float GetPtDotNormal(const CVector& p) const { return p.Dot(m_normal)-m_normalOffset; }
};
struct CQuaternion { CVector imag; float real; };
struct CMatrix {
    CVector m_right{1,0,0},m_forward{0,1,0},m_up{0,0,1},m_pos{};
    CMatrix Inverted() const;
    CVector TransformPoint(CVector) const;
    CVector TransformVector(CVector) const;
    void SetRotate(const CQuaternion&);
    void SetRotateZOnly(float);
};
struct CColLine {
    CVector m_vecStart,m_vecEnd;
    bool IsVertical() const { return m_vecStart.x==m_vecEnd.x && m_vecStart.y==m_vecEnd.y; }
};
struct CBox { CVector m_vecMin,m_vecMax; };
struct CBoundingBox : CBox {
    CBoundingBox(CBox b) : CBox(b) {}
    bool IsPointWithin(const CVector&) const;
};
enum eSurfaceType : uint8_t { SURFACE_DEFAULT };
using tColLighting = uint8_t;
using uint8 = uint8_t;
// SOURCE_SURFACE_INSERT
static_assert(offsetof(CColSurface,m_nLighting)==2 && sizeof(CColSurface)==4);
struct CColSphere {
    CVector m_vecCenter; float m_fRadius; CColSurface m_Surface;
    eSurfaceType GetSurfaceType() const { return m_Surface.m_nMaterial; }
};
struct CColBox : CBox {
    CColSurface m_Surface;
    eSurfaceType GetSurfaceType() const { return m_Surface.m_nMaterial; }
};
struct CStoredCollPoly { std::array<CVector,3> verts{}; bool valid = false; };
struct CColTriangle {
    std::array<uint32_t,3> indices;
    eSurfaceType m_nMaterial{}; uint8_t m_nLight{};
    eSurfaceType GetSurfaceType() const { return m_nMaterial; }
    CStoredCollPoly GetPoly(const CompressedVector* v) const { return {{v[indices[0]],v[indices[1]],v[indices[2]]},true}; }
    struct Rect {
        float left,right,bottom,top;
        bool IsPointInside(CVector p) const { return p.x>=left && p.x<=right && p.y>=bottom && p.y<=top; }
    };
    static Rect GetBoundingRect(CVector a,CVector b,CVector c) {
        return {std::min({a.x,b.x,c.x}),std::max({a.x,b.x,c.x}),std::min({a.y,b.y,c.y}),std::max({a.y,b.y,c.y})};
    }
};
struct CColPoint {
    CVector m_vecPoint{},m_vecNormal{};
    eSurfaceType m_nSurfaceTypeA{},m_nSurfaceTypeB{};
    uint8_t m_nLightingA{},m_nLightingB{},m_nPieceTypeA{},m_nPieceTypeB{};
};
enum class Shape { SSPHERE,SLINE,STRI };
struct DebugSettings {
    struct Enabled { bool IsEnabled(Shape,Shape) const { return true; } } ShapeShapeCollision;
    bool AllowLineOriginInsideSphere = false;
};
struct CCollision {
    static inline DebugSettings s_DebugSettings{};
    static inline int ms_iProcessLineNumCrossings{};
    static bool ProcessLineSphere(const CColLine&,const CColSphere&,CColPoint&,float&);
    static bool ProcessLineBox(const CColLine&,const CColBox&,CColPoint&,float&);
    static bool TestLineBox_DW(const CColLine&,const CBox&);
    static bool ProcessLineTriangle(const CColLine&,const CompressedVector*,const CColTriangle&,const CColTrianglePlane&,CColPoint&,float&,CStoredCollPoly*);
};
template<class T> T sq(T x) { return x*x; }
#define ZoneScoped
#define NOTSA_FORCEINLINE
#define NOTSA_VANILLA_COLLISIONS
#define NOTSA_UNREACHABLE() throw std::runtime_error("source unreachable")
// SOURCE_ORACLE_INSERT
#undef NOTSA_UNREACHABLE
#undef NOTSA_VANILLA_COLLISIONS
#undef NOTSA_FORCEINLINE
#undef ZoneScoped
}

using V = NativeCollisionVector;
using Status = NativeSourceGroundStatus;
static void Require(bool value,const char* message) { if (!value) throw std::runtime_error(message); }
static oracle::CVector OV(V v) { return {v[0],v[1],v[2]}; }
static V NV(oracle::CVector v) { return {v.x,v.y,v.z}; }
static bool Bits(float a,float b) { return std::bit_cast<uint32_t>(a)==std::bit_cast<uint32_t>(b); }
static size_t s_Comparisons{};

// Only the model array dispatch/provenance wrapper is adapted here. Every
// primitive predicate, plane constructor, broadphase, lerp, normalization and
// matrix operation below calls the separately extracted original source body.
static NativeSourceGroundResult Oracle(const NativeCollisionModel& model,const NativeSourceGroundTransform& transform,const NativeSourceGroundRequest& request) {
    using namespace oracle;
    const CMatrix matrix{OV(transform.Basis[0]),OV(transform.Basis[1]),OV(transform.Basis[2]),OV(transform.Position)};
    const auto inverse=matrix.Inverted();
    CColLine line{inverse.TransformPoint(OV(request.Start)),inverse.TransformPoint(OV(request.End))};
    line.m_vecEnd.x=line.m_vecStart.x; line.m_vecEnd.y=line.m_vecStart.y;
    NativeSourceGroundResult result; result.Status=Status::Miss; result.Fraction=request.MaxFraction;
    if (!CCollision::TestLineBox_DW(line,{OV(model.Min),OV(model.Max)})) return result;
    CColPoint point;
    const auto surface=[&](NativeCollisionSurface s) {
        return CColSurface{static_cast<eSurfaceType>(s.Material),s.Flags,model.Version==1 ? s.Light : s.Brightness,s.Light};
    };
    const auto eligible=[&](NativeCollisionSurface s) { return !request.SeeThroughCheck || (*request.VerifiedSeeThroughMaterials)[s.Material]; };
    const auto accept=[&](NativeCollisionPrimitive primitive,size_t i,NativeCollisionSurface s) {
        result.Status=Status::Hit; result.Primitive=primitive; result.PrimitiveIndex=i;
        result.Surface=s; result.MaterialB=point.m_nSurfaceTypeB; result.LightingB=point.m_nLightingB;
    };
    for (size_t i=0;i<model.Spheres.size();++i) {
        const auto& s=model.Spheres[i];
        if (eligible(s.Surface) && CCollision::ProcessLineSphere(line,{OV(s.Center),s.Radius,surface(s.Surface)},point,result.Fraction))
            accept(NativeCollisionPrimitive::Sphere,i,s.Surface);
    }
    for (size_t i=0;i<model.Boxes.size();++i) {
        const auto& b=model.Boxes[i];
        if (eligible(b.Surface) && CCollision::ProcessLineBox(line,{{OV(b.Min),OV(b.Max)},surface(b.Surface)},point,result.Fraction))
            accept(NativeCollisionPrimitive::Box,i,b.Surface);
    }
    std::vector<CompressedVector> vertices;
    for (auto v:model.Vertices) vertices.emplace_back(OV(v));
    for (size_t i=0;i<model.Faces.size();++i) {
        const auto& f=model.Faces[i];
        CColTriangle tri{f.Vertices,static_cast<eSurfaceType>(f.Surface.Material),f.Surface.Light};
        const auto poly=tri.GetPoly(vertices.data());
        const CColTrianglePlane plane{poly.verts[0],poly.verts[1],poly.verts[2]};
        NativeSourceGroundPlane packed;
        Require(NativeSourceGround::CalculatePlane(NV(poly.verts[0]),NV(poly.verts[1]),NV(poly.verts[2]),packed),"plane representable");
        const auto n=plane.GetNormal();
        Require(Bits(float(packed.Normal[0])/4096,n.x) && Bits(float(packed.Normal[1])/4096,n.y) && Bits(float(packed.Normal[2])/4096,n.z),"independent packed normal");
        Require(Bits(float(packed.Distance)/128,float(plane.m_normalOffset)) && unsigned(packed.Direction)==unsigned(plane.m_orientation),"independent packed offset/orientation");
        if (eligible(f.Surface) && CCollision::ProcessLineTriangle(line,vertices.data(),tri,plane,point,result.Fraction,nullptr)) {
            accept(NativeCollisionPrimitive::Triangle,i,f.Surface); result.Plane=packed;
        }
    }
    if (result.Status==Status::Hit) {
        result.Point=NV(matrix.TransformPoint(point.m_vecPoint)); result.Normal=NV(matrix.TransformVector(point.m_vecNormal));
    }
    return result;
}

static NativeSourceGroundResult Compare(const NativeCollisionModel& model,const NativeSourceGroundTransform& transform,const NativeSourceGroundRequest& request) {
    const auto expected=Oracle(model,transform,request);
    const auto actual=NativeSourceGround::ProcessVerticalLine(model,transform,request);
    Require(actual.Status==expected.Status,"source primitive status");
    Require(Bits(actual.Fraction,expected.Fraction),"source fraction bitwise");
    if (actual.Status==Status::Hit) {
        Require(actual.Primitive==expected.Primitive && actual.PrimitiveIndex==expected.PrimitiveIndex,"source first-tie primitive ownership");
        Require(actual.MaterialB==expected.MaterialB && actual.LightingB==expected.LightingB,"source material/lighting");
        for (size_t j=0;j<3;++j) {
            if (!Bits(actual.Point[j],expected.Point[j]) || !Bits(actual.Normal[j],expected.Normal[j])) {
                std::cerr<<"mismatch axis="<<j<<" point="<<actual.Point[j]<<'/'<<expected.Point[j]<<" normal="<<actual.Normal[j]<<'/'<<expected.Normal[j]<<'\n';
                Require(false,"source point/normal bitwise");
            }
        }
    }
    ++s_Comparisons; return actual;
}
static NativeCollisionModel Model() {
    NativeCollisionModel m; m.Version=2; m.Min={-20,-20,-20}; m.Max={20,20,20}; return m;
}
static NativeSourceGroundRequest Line(V a,V b,float max=1.0f) { NativeSourceGroundRequest r; r.Start=a; r.End=b; r.MaxFraction=max; return r; }
static NativeCollisionModel TriangleModel(V a,V b,V c) {
    auto m=Model(); m.Vertices={a,b,c}; m.Faces.push_back({{0,1,2},{4,255,93,28}}); return m;
}
static void Fixtures() {
    const NativeSourceGroundTransform identity;
    // Independent review witness: the former shared X/Y/Z Dot shim masked a
    // two-ULP error. Dot and DotProduct are now extracted from source as well.
    const V orderA{-5.515625f,-12.8671875f,15.875f};
    const V orderB{-8.671875f,11.3046875f,13.0703125f};
    const V orderC{-1.359375f,6.9921875f,7.4765625f};
    const float orderX=(orderA[0]+orderB[0]+orderC[0])/3.0f;
    const float orderY=(orderA[1]+orderB[1]+orderC[1])/3.0f;
    const auto orderHit=Compare(TriangleModel(orderA,orderB,orderC),identity,
        Line({orderX,orderY,30},{orderX,orderY,-30}));
    Require(orderHit.Status==Status::Hit && std::bit_cast<uint32_t>(orderHit.Fraction)==1050174927u,
        "source Z/Y/X dot order counterexample, not former 1050174925 fraction");
    auto m=Model(); m.Spheres.push_back({{0,0,0},1,{7,255,42,99}});
    Require(Compare(m,identity,Line({0,0,3},{0,0,-3})).Point[2]==1,"sphere entry");
    Require(Compare(m,identity,Line({0,0,0},{0,0,-3})).Status==Status::Miss,"sphere initial inside rejected");
    Require(Compare(m,identity,Line({0,0,1},{0,0,-3})).Status==Status::Miss,"sphere on surface rejected");
    Require(Compare(m,identity,Line({1,0,3},{1,0,-3})).Status==Status::Hit,"sphere tangent accepted");
    Require(Compare(m,identity,Line({0,0,3},{0,0,1})).Status==Status::Miss,"fraction one excluded");
    auto sphereHit=Compare(m,identity,Line({0,0,3},{0,0,-3}));
    Require(Compare(m,identity,Line({0,0,3},{0,0,-3},sphereHit.Fraction)).Status==Status::Miss,"max fraction exact tie excluded");
    Require(Compare(m,identity,Line({0,0,3},{0,0,-3},std::nextafter(sphereHit.Fraction,1.0f))).Status==Status::Hit,"max fraction next float included");
    m=Model(); m.Boxes.push_back({{-1,-1,-1},{1,1,1},{61,255,24,78}});
    Require(Compare(m,identity,Line({0,0,0},{0,0,-3})).Point[2]==-1,"box inside exits bottom");
    Require(Compare(m,identity,Line({0,0,0},{0,0,3})).Normal[2]==1,"box inside exits top");
    Require(Compare(m,identity,Line({1,0,3},{1,0,-3})).Status==Status::Miss,"box face edge strict");
    Require(Compare(m,identity,Line({1,1,3},{1,1,-3})).Status==Status::Miss,"box corner strict");
    Require(Compare(m,identity,Line({0,0,3},{0,0,1})).Status==Status::Miss,"box endpoint crossing strict");
    const auto upward=TriangleModel({-4,-4,1},{0,4,1},{4,-4,1});
    m=upward;
    m.Boxes.push_back({{-1,-1,-1},{1,1,1},{61,255,24,78}});
    m.Spheres.push_back({{0,0,0},1,{7,255,42,99}});
    m.Spheres.push_back({{0,0,0},1,{8,0,43,98}});
    const auto tie=Compare(m,identity,Line({0,0,3},{0,0,-3}));
    Require(tie.Primitive==NativeCollisionPrimitive::Sphere && tie.PrimitiveIndex==0,"sphere->box->triangle equal ownership");
    m.Spheres.clear();
    Require(Compare(m,identity,Line({0,0,3},{0,0,-3})).Primitive==NativeCollisionPrimitive::Box,"box wins triangle tie");
    m.Boxes.clear(); m.Faces.push_back(m.Faces.front()); m.Faces.back().Surface.Material=9;
    Require(Compare(m,identity,Line({0,0,3},{0,0,-3})).PrimitiveIndex==0,"triangle array first tie");
    Require(Compare(m,identity,Line({0,0,0},{0,0,3})).Status==Status::Hit,"triangle backface accepted");
    std::swap(m.Faces[0].Vertices[1],m.Faces[0].Vertices[2]);
    Require(Compare(m,identity,Line({0,0,3},{0,0,-3})).Normal[2]==-1,"downward normal retained");
    Require(Compare(m,identity,Line({-4,-4,3},{-4,-4,-3})).Status==Status::Hit,"triangle vertex/edge boundary inclusive");
    std::array<bool,256> see{}; see[4]=true;
    auto r=Line({0,0,3},{0,0,-3}); r.SeeThroughCheck=true; r.VerifiedSeeThroughMaterials=&see; r.ShootThroughCheck=true;
    Require(Compare(m,identity,r).Status==Status::Hit,"see-through true selects source IsSeeThrough, shoot ignored");
    see[4]=false; Require(Compare(m,identity,r).Status==Status::Miss,"source see-through table excludes");
    r.VerifiedSeeThroughMaterials=nullptr;
    Require(NativeSourceGround::ProcessVerticalLine(m,identity,r).Reason==NativeSourceGroundReason::UnknownSurfaceTable,"unknown table unsupported");
    auto legacy=TriangleModel({-4.003f,-4.004f,0.129f},{0,4.004f,0.129f},{4.003f,-4.004f,0.129f});
    legacy.Version=1;
    Require(Compare(legacy,identity,Line({0,0,3},{0,0,-3})).Plane.Distance==16,"COL1 vertices truncate to128 before plane construction");
    legacy=Model(); legacy.Version=1; legacy.Boxes.push_back({{-1,-1,-1},{1,1,1},{61,255,24,78}});
    Require(Compare(legacy,identity,Line({0,0,3},{0,0,-3})).LightingB==78,"COL1 TBox conversion uses file light byte3");
    legacy.Boxes.clear(); legacy.Spheres.push_back({{0,0,0},1,{7,255,42,99}});
    Require(Compare(legacy,identity,Line({0,0,3},{0,0,-3})).LightingB==99,"COL1 TSphere conversion uses file light byte3");
    auto rotated=NativeSourceGroundTransform{{7,11,13},{{{1,0,0},{0,0.6f,0.8f},{0,-0.8f,0.6f}}}};
    auto wide=TriangleModel({-16,-16,0},{0,16,0},{16,-16,0});
    const auto forced=Compare(wide,rotated,Line({7,11,17},{7,11,-17}));
    Require(forced.Status==Status::Hit && forced.Point[1]!=11.0f,"rotated forced local XY is not world vertical ray");
    // Deterministic valid-domain differential grid, includes all six packed
    // plane orientations, signed winding, face edges, initial inside and caps.
    for (int axis=0;axis<3;++axis) for (int sign:{-1,1}) {
        V a={-4,-4,0.125f},b={0,4,0.375f},c={4,-4,-0.25f};
        for (auto* v:{&a,&b,&c}) { std::swap((*v)[axis],(*v)[2]); (*v)[axis]*=sign; }
        auto test=TriangleModel(a,b,c);
        test.Spheres={{{-2,0,0},1.5f,{7,128,31,2}}};
        test.Boxes={{{1,-2,-1},{3,2,2},{9,64,29,3}}};
        for (int x=-16;x<=16;++x) for (int y=-8;y<=8;++y) for (float cap:{0.0f,0.25f,0.5f,1.0f}) {
            Compare(test,identity,Line({x*0.25f,y*0.5f,5},{x*0.25f,y*0.5f,-5},cap));
        }
    }
    const auto sentinel=NativeSourceGroundRequest::Generator({0,0,-100});
    Require(sentinel.Start[2]==1000 && sentinel.End[2]==-1000,"generator sentinel/top/end");
    Require(NativeSourceGroundRequest::Generator({0,0,-99.875f}).Start[2]==-98.875f,"generator strict -100 boundary");
    Require(NativeSourceGround::ProcessVerticalLine(wide,identity,Line({0,0,5},{0,0,-5},2)).Reason==NativeSourceGroundReason::InvalidInput,"invalid cap unsupported");
    auto unrepresentable=TriangleModel({250,250,250},{249,251,250},{249,250,251});
    unrepresentable.Min={-256,-256,-256}; unrepresentable.Max={255,255,255};
    Require(NativeSourceGround::ProcessVerticalLine(unrepresentable,identity,Line({250,250,255},{250,250,-255})).Reason==NativeSourceGroundReason::UnrepresentablePlane,"out-of-int16 plane domain unsupported");
    std::cout<<"PASS extracted primitive oracle comparisons="<<s_Comparisons<<" bitwise fraction/point/normal/material/light\n";
}

static void SnapshotFixtures() {
    using K=NativeSourceGroundKnown;
    NativeSourceGroundSnapshot snapshot;
    Require(NativeSourceGround::QueryBuildings(snapshot,{0,0,3},0,0).Reason==NativeSourceGroundReason::UnknownCoverage,"default scope unsupported");
    snapshot.CompleteNormalSector=snapshot.MembershipAndOverridesVerified=snapshot.Deduplicated=true;
    snapshot.MinXY={-10,-10}; snapshot.MaxXY={10,10}; snapshot.WorldGeneration=12; snapshot.MetadataRevision=9;
    auto query=[&]() { return NativeSourceGround::QueryBuildings(snapshot,{0,0,3},12,9); };
    Require(query().Status==Status::Miss,"sealed empty fixture miss");
    NativeSourceGroundTarget target;
    target.Model=std::make_shared<NativeCollisionModel>(TriangleModel({-4,-4,1},{0,4,1},{4,-4,1}));
    snapshot.Targets.push_back(target);
    Require(query().Reason==NativeSourceGroundReason::UnknownEntityMetadata,"COL alone cannot prove building");
    auto& t=snapshot.Targets[0]; t.VerifiedClassification=true; t.EffectiveClass=NativeSourceGroundClass::Building;
    t.InWorld=t.UsesCollision=t.NormalSector=K::Yes; t.BigBuilding=t.Ignored=K::No;
    Require(query().Reason==NativeSourceGroundReason::UnknownTransform,"effective transform proof required");
    t.SourceEffectiveTransformKnown=true;
    Require(query().Reason==NativeSourceGroundReason::UnknownCollisionModel,"COL authority required");
    t.CollisionModelKnown=true;
    Require(query().Status==Status::Hit,"unique hit does not need source order");
    snapshot.Targets.push_back(t);
    Require(query().Reason==NativeSourceGroundReason::UnknownSourceOrder,"world tie unknown order unsupported");
    snapshot.Targets[0].SourceListOrdinal=9; snapshot.Targets[1].SourceListOrdinal=3;
    Require(query().TargetIndex==1,"source ordinal not target vector order");
    snapshot.Targets[1].EffectiveClass=NativeSourceGroundClass::Other;
    Require(query().TargetIndex==0,"verified dummy/object excluded");
    snapshot.Targets[0].BigBuilding=K::Yes; Require(query().Status==Status::Miss,"BigBuilding excluded");
    snapshot.Targets[0].BigBuilding=K::No; snapshot.Targets[0].UsesCollision=K::No;
    Require(query().Status==Status::Miss,"UsesCollision false excluded");
    snapshot.Targets[0].UsesCollision=K::Unknown; Require(query().Status==Status::Unsupported,"UsesCollision unknown unsupported");
    Require(NativeSourceGround::QueryBuildings(snapshot,{11,0,3},12,9).Reason==NativeSourceGroundReason::OutsideCoverage,"outside complete sector");
    Require(NativeSourceGround::QueryBuildings(snapshot,{0,0,3},11,9).Reason==NativeSourceGroundReason::StaleWorld,"world stamp");
    NativeCollisionInstance instance; instance.Placement.ModelId=1; instance.Placement.Model="fixture";
    NativeWorldModelInfo modelInfo; modelInfo.ModelId=1; modelInfo.Name="fixture";
    modelInfo.InitialClass=NativeWorldInitialClass::Building; modelInfo.ObjectInfo=NativeWorldObjectAssignment::Unassigned;
    NativeWorldPlacementInfo placementInfo; placementInfo.Identity=NativePlacementIdentity::From(instance.Placement);
    NativeWorldEntityMetadata metadata{NativeWorldInfoStatus::Ready,&modelInfo,&placementInfo};
    Require(!NativeSourceGround::BindInitialMetadata(instance,metadata,true).VerifiedClassification,"full instance type proof required");
    placementInfo.SourceInstanceType=256; placementInfo.Area=0;
    Require(!NativeSourceGround::BindInitialMetadata(instance,metadata).VerifiedClassification,"initial class still effective authority required");
    const auto bound=NativeSourceGround::BindInitialMetadata(instance,metadata,true);
    Require(bound.VerifiedClassification && bound.EffectiveClass==NativeSourceGroundClass::Building && bound.UsesCollision==K::Unknown && !bound.SourceEffectiveTransformKnown && !bound.CollisionModelKnown,"metadata import preserves unknown world fields");
    placementInfo.Identity.Record=1;
    Require(!NativeSourceGround::BindInitialMetadata(instance,metadata,true).VerifiedClassification,"metadata placement identity mismatch");
    std::cout<<"PASS typed coverage/classification/eligibility/transform/COL/order/revision gates\n";
}

// All asset IO is probe-only, read-only and bounded. No asset bytes are written.
static uint32_t Word(std::span<const uint8_t> b,size_t p,size_t n=4) {
    Require(p<=b.size() && n<=b.size()-p,"word bounds"); uint32_t v=0;
    for (size_t i=0;i<n;++i) v|=uint32_t(b[p+i])<<(8*i); return v;
}
static float Float(std::span<const uint8_t> b,size_t p) { return std::bit_cast<float>(Word(b,p)); }
static std::vector<uint8_t> Read(std::ifstream& f,size_t p,size_t n) {
    Require(n<32*1024*1024,"bounded read"); std::vector<uint8_t> b(n); f.seekg(p);
    Require(bool(f.read(reinterpret_cast<char*>(b.data()),n)),"asset read"); return b;
}
static std::vector<uint8_t> Img(const std::string& game,const char* name) {
    std::ifstream f(game+"/models/gta3.img",std::ios::binary); const auto header=Read(f,0,8);
    Require(std::memcmp(header.data(),"VER2",4)==0,"IMG signature"); const auto directory=Read(f,8,Word(header,4)*32);
    for (size_t p=0;p<directory.size();p+=32) {
        const std::string entry(reinterpret_cast<const char*>(directory.data()+p+8),strnlen(reinterpret_cast<const char*>(directory.data()+p+8),24));
        if (entry==name) return Read(f,size_t(Word(directory,p))*2048,size_t(Word(directory,p+6,2) ? Word(directory,p+6,2) : Word(directory,p+4,2))*2048);
    }
    throw std::runtime_error("IMG entry missing");
}
static NativeCollisionModel Col(std::span<const uint8_t> b,const char* name,const char* library) {
    for (size_t p=0;p+32<=b.size();) {
        const auto n=size_t(Word(b,p+4))+8; Require(n>=32 && n<=b.size()-p,"COL chunk bounds");
        if (std::string(reinterpret_cast<const char*>(b.data()+p+8),strnlen(reinterpret_cast<const char*>(b.data()+p+8),22))==name) {
            NativeCollisionModel m; std::string error; Require(NativeCollisionAssets::Parse(b.subspan(p,n),library,m,error),error.c_str()); return m;
        }
        p+=n;
    }
    throw std::runtime_error("COL model missing");
}
static NativeSourceGroundTransform Placement(std::span<const uint8_t> b,uint32_t record,int modelId) {
    Require(std::memcmp(b.data(),"bnry",4)==0 && record<Word(b,4),"binary IPL header/count");
    const auto p=size_t(Word(b,28))+record*40;
    Require(int(Word(b,p+28))==modelId,"IPL record model");
    const V pos{Float(b,p),Float(b,p+4),Float(b,p+8)};
    // FileLoader.cpp:1036-1052, using source-extracted CMatrix rotations.
    // No native full-quaternion normalization/equivalence assumption.
    oracle::CQuaternion q{{Float(b,p+12),Float(b,p+16),Float(b,p+20)},Float(b,p+24)};
    oracle::CMatrix matrix;
    if (std::abs(q.imag.x)>0.05f || std::abs(q.imag.y)>0.05f || ((Word(b,p+32)&512u) && q.imag.x!=0 && q.imag.y!=0)) {
        q.imag=-q.imag; matrix.SetRotate(q);
    } else {
        const auto heading=std::acos(q.real)*(q.imag.z<0.0f ? 2.0f : -2.0f);
        matrix.SetRotateZOnly(heading);
    }
    return {pos,{NV(matrix.m_right),NV(matrix.m_forward),NV(matrix.m_up)}};
}
static NativeCollisionPlacement PlacementIdentity(std::span<const uint8_t> b,uint32_t record,int modelId,const char* name) {
    const auto p=size_t(Word(b,28))+record*40;
    NativeCollisionPlacement placement;
    placement.Model=name; placement.ModelId=modelId; placement.Ipl="models/gta3.img:countn2_stream2.ipl";
    placement.Record=record; placement.Binary=true; placement.Flags=Word(b,p+32); placement.Interior=placement.Flags&255;
    placement.Lod=static_cast<int32_t>(Word(b,p+36));
    placement.Position={Float(b,p),Float(b,p+4),Float(b,p+8)};
    placement.Quaternion={Float(b,p+12),Float(b,p+16),Float(b,p+20),Float(b,p+24)};
    return placement;
}
static void Assets(const std::string& game) {
    const auto library=Img(game,"countn2_5.col");
    const auto terrain=Col(library,"ne_bit_07","models/gta3.img:countn2_5.col");
    const auto ipl=Img(game,"countn2_stream2.ipl");
    const auto transform=Placement(ipl,32,16177);
    const auto rustler=Compare(terrain,transform,NativeSourceGroundRequest::Generator({325,2537,17.5f}));
    Require(rustler.Status==Status::Hit && rustler.PrimitiveIndex==342 && rustler.MaterialB==4 && rustler.LightingB==28,"actual Rustler terrain provenance");
    std::cout<<"actual model=ne_bit_07 id=16177 ipl=countn2_stream2.ipl record=32 primitive=triangle:342 Z="<<rustler.Point[2]<<" packedNormal="<<rustler.Plane.Normal[0]<<','<<rustler.Plane.Normal[1]<<','<<rustler.Plane.Normal[2]<<" packedDistance="<<rustler.Plane.Distance<<" light="<<int(rustler.LightingB)<<'\n';
    Require(Bits(rustler.Point[2],15.80784416f),"report actual-COL Z bits");
    Require(rustler.Plane.Normal==std::array<int16_t,3>{2,0,4095} && rustler.Plane.Distance==-3300,"actual packed plane provenance");
    auto dff=Img(game,"rustler.dff"); NativeCollisionModel vehicle; size_t plugins=0;
    const auto visit=[&](auto&& self,size_t p,size_t end,int depth)->void {
        Require(depth<5,"DFF depth");
        while (p<end) {
            Require(p+12<=end,"DFF header"); const auto type=Word(dff,p),n=Word(dff,p+4);
            Require(n<=end-p-12,"DFF chunk bounds");
            if (type==0x253f2fa) { std::string error; Require(NativeCollisionAssets::Parse(std::span(dff).subspan(p+12,n),"rustler.dff:COL-plugin",vehicle,error),error.c_str()); ++plugins; }
            if (type==16 || type==3) self(self,p+12,p+12+n,depth+1);
            p+=12+n;
        }
    };
    visit(visit,0,12+Word(dff,4),0); Require(plugins==1,"one embedded vehicle COL");
    Require(Bits(vehicle.Min[2],-1.258473873f),"actual embedded Rustler bbox minZ bits");
    std::cout<<"actual rustler_col minZ="<<vehicle.Min[2]<<" origin-formula-only="<<rustler.Point[2]-vehicle.Min[2]<<'\n';
    NativeCollisionPopulation population;
    population.Models[16177]={"ne_bit_07",false}; population.Models[1224]={"woodenbox",false};
    population.Instances.push_back(PlacementIdentity(ipl,32,16177,"ne_bit_07"));
    population.Instances.push_back(PlacementIdentity(ipl,63,1224,"woodenbox"));
    NativeWorldEntityInfo info; std::string error;
    Require(info.LoadBeforeWorker(game.c_str(),population,error),error.c_str());
    const auto terrainMetadata=info.Query(population.Instances[0]), crateMetadata=info.Query(population.Instances[1]);
    Require(terrainMetadata.InitialBuildingMask()==NativeWorldKnownBool::True && crateMetadata.InitialBuildingMask()==NativeWorldKnownBool::False,"actual source-based Building/Dummy metadata");
    Require(crateMetadata.Model->ObjectInfo==NativeWorldObjectAssignment::Assigned && crateMetadata.Model->ObjectRows.front().Line==126,"woodenbox Object.dat row126");
    const auto dynamic=Img(game,"dynamic.col");
    const auto crate=Col(dynamic,"woodenbox","models/gta3.img:dynamic.col");
    const auto crateTransform=Placement(ipl,63,1224);
    V center=crateTransform.Position; center[2]=20;
    const auto crateHit=Compare(crate,crateTransform,NativeSourceGroundRequest::Generator(center));
    const auto terrainHit=Compare(terrain,transform,NativeSourceGroundRequest::Generator(center));
    Require(crateHit.Status==Status::Hit && terrainHit.Status==Status::Hit && crateHit.Point[2]>terrainHit.Point[2]+0.9f,"actual crate vs terrain");
    Require(Bits(crateHit.Point[2],16.77078056f) && Bits(terrainHit.Point[2],15.81216049f),"actual crate/terrain Z bits");
    std::cout<<"actual woodenbox1224 Dummy boxZ="<<crateHit.Point[2]<<" terrainZ="<<terrainHit.Point[2]<<" crateLightingB="<<int(crateHit.LightingB)<<" rawBrightness="<<int(crateHit.Surface.Brightness)<<" rawLight="<<int(crateHit.Surface.Light)<<'\n';
    NativeSourceGroundSnapshot fixture; fixture.CompleteNormalSector=fixture.MembershipAndOverridesVerified=fixture.Deduplicated=true;
    fixture.MinXY={300,2500}; fixture.MaxXY={350,2550};
    NativeCollisionInstance instance; instance.Placement=population.Instances[1]; instance.Basis=crateTransform.Basis; instance.Model=std::make_shared<NativeCollisionModel>(crate);
    auto target=NativeSourceGround::BindInitialMetadata(instance,crateMetadata,true);
    Require(target.VerifiedClassification && target.EffectiveClass==NativeSourceGroundClass::Other,"actual dummy metadata binding");
    target.SourceEffectiveTransformKnown=target.CollisionModelKnown=true;
    target.InWorld=target.UsesCollision=target.NormalSector=NativeSourceGroundKnown::Yes;
    target.BigBuilding=target.Ignored=NativeSourceGroundKnown::No;
    fixture.Targets.push_back(target);
    instance.Placement=population.Instances[0]; instance.Basis=transform.Basis; instance.Model=std::make_shared<NativeCollisionModel>(terrain);
    target=NativeSourceGround::BindInitialMetadata(instance,terrainMetadata,true);
    Require(target.VerifiedClassification && target.EffectiveClass==NativeSourceGroundClass::Building,"actual building metadata binding");
    target.InWorld=target.UsesCollision=target.NormalSector=NativeSourceGroundKnown::Yes;
    target.BigBuilding=target.Ignored=NativeSourceGroundKnown::No;
    target.SourceEffectiveTransformKnown=target.CollisionModelKnown=true;
    fixture.Targets.push_back(target);
    const auto opensBeforeQuery=s_FileOpens;
    const auto filtered=NativeSourceGround::QueryBuildings(fixture,center,0,0);
    Require(filtered.Status==Status::Hit && filtered.TargetIndex==1 && Bits(filtered.Point[2],terrainHit.Point[2]),"actual COL fixture verified Dummy excluded");
    fixture.CompleteNormalSector=false;
    Require(NativeSourceGround::QueryBuildings(fixture,center,0,0).Status==Status::Unsupported,"actual candidate census is not complete world authority");
    Require(s_FileOpens==opensBeforeQuery,"queries perform no IO");
    const auto sweeperLibrary=Img(game,"las2_2.col");
    const auto plaza=Col(sweeperLibrary,"las2plaza1bit","models/gta3.img:las2_2.col");
    const auto sweeperIpl=Img(game,"las2_stream3.ipl");
    const auto plazaTransform=Placement(sweeperIpl,150,5142);
    const auto requested=NativeSourceGroundRequest::Generator({2482.75f,-1953.5f,11});
    Require(Compare(plaza,plazaTransform,requested).Status==Status::Miss,"actual Sweeper requested top12 misses");
    auto diagnostic=requested; diagnostic.Start[2]=1000;
    const auto raised=Compare(plaza,plazaTransform,diagnostic);
    Require(raised.Status==Status::Hit && raised.PrimitiveIndex==85,"separate raised diagnostic sees plaza");
    std::cout<<"actual Sweeper top12=Miss; separate top1000 diagnostic Z="<<raised.Point[2]<<" never substituted for request\n";
    std::cout<<"PASS actual-COL source arithmetic witnesses; production world coverage remains Unsupported\n";
}
int main(int argc,char** argv) try {
    std::cout<<std::setprecision(10); std::cerr<<std::setprecision(10);
    Fixtures(); SnapshotFixtures();
    if (argc==2) Assets(argv[1]);
    std::cout<<"PASS NativeSourceGround comparisons="<<s_Comparisons<<'\n'; return 0;
} catch (const std::exception& e) { std::cerr<<"FAIL "<<e.what()<<'\n'; return 1; }
