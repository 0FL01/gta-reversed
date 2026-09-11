#include "app/platform/linux/RealtimeGameplay.h"

#include "app/platform/linux/CarPose.h"
#include "app/platform/linux/Collide.h"
#include "app/platform/linux/Handling.h"
#include "app/platform/linux/IfpAnim.h"
#include "app/platform/linux/NativeTransmission.h"
#include "app/platform/linux/NativeCollisionAssets.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <numeric>
#include <sstream>
#include <utility>
#include <vector>

#include <rw.h>
using int32 = int32_t;
using uint32 = uint32_t;
using int64 = int64_t;
using uint64 = uint64_t;
#ifndef __stdcall
#define __stdcall
#endif
#include "oswrapper/oswrapper.h"

namespace {
using V = RealtimeVec3;
constexpr float Pi = 3.14159265358979323846f;
constexpr float PedRadius = 0.34f;
constexpr float ContactSkin = 0.004f;
constexpr float FootCenter = PedRadius+ContactSkin;
constexpr float StepUp = 0.26f;
constexpr float StepDown = 0.30f;
constexpr float WalkableUp = 0.6f;
constexpr std::array<float,5> PedCenters{FootCenter,0.61f,0.88f,1.15f,1.42f};
constexpr float Gravity = 9.81f;
static V Add(V a, V b) { return {a.X + b.X, a.Y + b.Y, a.Z + b.Z}; }
static V Sub(V a, V b) { return {a.X - b.X, a.Y - b.Y, a.Z - b.Z}; }
static V Mul(V a, float b) { return {a.X * b, a.Y * b, a.Z * b}; }
static float Dot(V a, V b) { return a.X * b.X + a.Y * b.Y + a.Z * b.Z; }
static V Cross(V a, V b) { return {a.Y*b.Z-a.Z*b.Y, a.Z*b.X-a.X*b.Z, a.X*b.Y-a.Y*b.X}; }
static float Length(V a) { return std::sqrt(Dot(a, a)); }
static float Wrap(float a) { return std::remainder(a, 2.0f * Pi); }
static V Forward(float a) { return {std::cos(a), std::sin(a), 0.0f}; }
static V Right(float a) { return {std::sin(a), -std::cos(a), 0.0f}; }
static float Approach(float a, float b, float step) { return a + std::clamp(b-a, -step, step); }
static V Min(V a, V b) { return {std::min(a.X,b.X),std::min(a.Y,b.Y),std::min(a.Z,b.Z)}; }
static V Max(V a, V b) { return {std::max(a.X,b.X),std::max(a.Y,b.Y),std::max(a.Z,b.Z)}; }
static bool Finite(V a) { return std::isfinite(a.X) && std::isfinite(a.Y) && std::isfinite(a.Z); }
static V Load(const std::vector<float>& p, size_t i) { return {p[i],p[i+1],p[i+2]}; }
static void Store(std::vector<float>& p, size_t i, V v) { p[i]=v.X; p[i+1]=v.Y; p[i+2]=v.Z; }
static bool Overlap(V a, V b, V c, V d) {
    return a.X<=d.X && b.X>=c.X && a.Y<=d.Y && b.Y>=c.Y && a.Z<=d.Z && b.Z>=c.Z;
}
static V Closest(V p, V a, V b, V c) {
    // Triangle Voronoi regions, including edge/vertex contacts.
    const V ab=Sub(b,a), ac=Sub(c,a), ap=Sub(p,a);
    const V area=Cross(ab,ac);
    if (Dot(area,area)==0) {
        // Authored COL faces can collapse to a finite segment or point. Keep
        // that exact boundary for sphere queries instead of dropping the face.
        V best=a; float distance=Dot(ap,ap);
        const std::array<V,3> vertices{a,b,c};
        for (size_t i=0;i<3;++i) {
            const V start=vertices[i],edge=Sub(vertices[(i+1)%3],start);
            const float ee=Dot(edge,edge);
            const V q=ee>0 ? Add(start,Mul(edge,std::clamp(Dot(Sub(p,start),edge)/ee,0.0f,1.0f))):start;
            const float d=Dot(Sub(p,q),Sub(p,q));
            if (d<distance) { best=q; distance=d; }
        }
        return best;
    }
    const float d1=Dot(ab,ap), d2=Dot(ac,ap);
    if (d1<=0 && d2<=0) return a;
    const V bp=Sub(p,b);
    const float d3=Dot(ab,bp), d4=Dot(ac,bp);
    if (d3>=0 && d4<=d3) return b;
    const float vc=d1*d4-d3*d2;
    if (vc<=0 && d1>=0 && d3<=0) return Add(a,Mul(ab,d1/(d1-d3)));
    const V cp=Sub(p,c);
    const float d5=Dot(ab,cp), d6=Dot(ac,cp);
    if (d6>=0 && d5<=d6) return c;
    const float vb=d5*d2-d1*d6;
    if (vb<=0 && d2>=0 && d6<=0) return Add(a,Mul(ac,d2/(d2-d6)));
    const float va=d3*d6-d5*d4;
    if (va<=0 && d4-d3>=0 && d5-d6>=0) return Add(b,Mul(Sub(c,b),(d4-d3)/(d4-d3+d5-d6)));
    const float inv=1.0f/(va+vb+vc);
    return Add(a,Add(Mul(ab,vb*inv),Mul(ac,vc*inv)));
}
static V RotateX(V v, float a) { return {v.X, v.Y*std::cos(a)-v.Z*std::sin(a), v.Y*std::sin(a)+v.Z*std::cos(a)}; }
static V RotateY(V v, float a) { return {v.X*std::cos(a)+v.Z*std::sin(a),v.Y,-v.X*std::sin(a)+v.Z*std::cos(a)}; }
static V RotateZ(V v, float a) { return {v.X*std::cos(a)-v.Y*std::sin(a), v.X*std::sin(a)+v.Y*std::cos(a),v.Z}; }

// Handling_Load deliberately exposes only the old batch subset. Read the
// additional real columns locally rather than changing that shared contract.
static bool ExtraHandling(float& brake, float& lock, float& traction, uint32_t& flags, std::string& error) {
    void* file=nullptr;
    if (OS_FileOpen(FILE_DATA_AREA_DEFAULT,&file,"data/handling.cfg",FILE_ACCESS_READ)!=0 || !file) {
        error="cannot read handling brake/steering columns"; return false;
    }
    const auto size=OS_FileSize(file);
    std::string bytes(size>0 ? static_cast<size_t>(size) : 0, '\0');
    const bool ok=size>0 && OS_FileRead(file,bytes.data(),size)==0;
    OS_FileClose(file);
    if (!ok) { error="handling read failed"; return false; }
    std::istringstream lines(bytes);
    std::string line;
    while (std::getline(lines,line)) {
        std::istringstream row(line);
        std::vector<std::string> t;
        for (std::string token; row>>token;) t.push_back(token);
        if (t.empty() || t[0]!="LANDSTAL") continue;
        if (t.size()<33) break;
        auto number=[&](size_t i, float& out) {
            char* end=nullptr; out=std::strtof(t[i].c_str(),&end);
            return end && !*end && std::isfinite(out) && out>0.0f;
        };
        if (!number(17,brake) || !number(20,lock) || !number(8,traction)) break;
        char* end=nullptr;
        const auto parsed=std::strtoul(t[32].c_str(),&end,16);
        if (!end || end==t[32].c_str() || *end || parsed>UINT32_MAX) break;
        flags=static_cast<uint32_t>(parsed);
        lock*=Pi/180.0f;
        return true;
    }
    error="missing/invalid LANDSTAL braking, traction, steering lock or handling flags"; return false;
}
} // namespace

struct RealtimeGameplayWorld::Impl {
    struct Triangle { V A,B,C,Lo,Hi; float Up; size_t Source=SIZE_MAX, Volume=SIZE_MAX; };
    struct Sphere { V Center; float Radius; size_t Source; };
    struct Box { V Min,Max,Position; std::array<V,3> Basis; size_t Source; };
    struct Node { V Lo,Hi; uint32_t Begin=0,Count=0,Left=0,Right=0; };
    struct VolumeIndex {
        struct Bounds { V Lo,Hi; };
        std::vector<Bounds> BoundsList;
        std::vector<uint32_t> Order;
        std::vector<Node> Nodes;
        static Bounds Outward(V lo,V hi,float arithmeticMagnitude=0) {
            // Outward rounding also covers float world/local dot products at
            // kilometre coordinates. This only enlarges bounds, never geometry.
            const V pad{64*std::numeric_limits<float>::epsilon()*(1+arithmeticMagnitude+std::max(std::abs(lo.X),std::abs(hi.X))),
                        64*std::numeric_limits<float>::epsilon()*(1+arithmeticMagnitude+std::max(std::abs(lo.Y),std::abs(hi.Y))),
                        64*std::numeric_limits<float>::epsilon()*(1+arithmeticMagnitude+std::max(std::abs(lo.Z),std::abs(hi.Z)))};
            return {Sub(lo,pad),Add(hi,pad)};
        }
        void AddBounds(V lo,V hi,float arithmeticMagnitude=0) { BoundsList.push_back(Outward(lo,hi,arithmeticMagnitude)); }
        uint32_t Build(uint32_t begin,uint32_t end) {
            Node n; n.Lo={INFINITY,INFINITY,INFINITY}; n.Hi={-INFINITY,-INFINITY,-INFINITY};
            for (auto i=begin;i<end;++i) {
                const auto& b=BoundsList[Order[i]]; n.Lo=Min(n.Lo,b.Lo); n.Hi=Max(n.Hi,b.Hi);
            }
            const auto index=static_cast<uint32_t>(Nodes.size()); Nodes.push_back(n);
            if (end-begin<=8) { Nodes[index].Begin=begin; Nodes[index].Count=end-begin; return index; }
            const V extent=Sub(n.Hi,n.Lo);
            const int axis=extent.X>extent.Y ? (extent.X>extent.Z ? 0:2):(extent.Y>extent.Z ? 1:2);
            auto key=[&](uint32_t i) { const V c=Add(BoundsList[i].Lo,BoundsList[i].Hi); return axis==0 ? c.X:axis==1 ? c.Y:c.Z; };
            const auto mid=begin+(end-begin)/2;
            std::nth_element(Order.begin()+begin,Order.begin()+mid,Order.begin()+end,
                [&](uint32_t a,uint32_t b) { const float x=key(a),y=key(b); return x==y ? a<b:x<y; });
            const auto left=Build(begin,mid),right=Build(mid,end);
            Nodes[index].Left=left; Nodes[index].Right=right; return index;
        }
        void Build() {
            Order.resize(BoundsList.size()); std::iota(Order.begin(),Order.end(),0u);
            if (!Order.empty()) Build(0,static_cast<uint32_t>(Order.size()));
        }
        void Query(uint32_t index,V lo,V hi,std::vector<uint32_t>& candidates) const {
            if (Nodes.empty()) return;
            const auto& n=Nodes[index];
            if (!Overlap(lo,hi,n.Lo,n.Hi)) return;
            if (n.Count) {
                for (uint32_t i=n.Begin;i<n.Begin+n.Count;++i) {
                    const auto id=Order[i]; const auto& b=BoundsList[id];
                    if (Overlap(lo,hi,b.Lo,b.Hi)) candidates.push_back(id);
                }
            } else { Query(n.Left,lo,hi,candidates); Query(n.Right,lo,hi,candidates); }
        }
    };
    std::vector<Triangle> Triangles;
    std::vector<uint32_t> Order;
    std::vector<Node> Nodes;
    std::vector<Sphere> Spheres;
    std::vector<Box> Boxes;
    VolumeIndex SphereIndex,BoxIndex;
    bool IndexVolumes=true;
    float BoxRadiusScale=1;
    // Queries already require exclusive ownership (like Tests). Narrowphase
    // callbacks do not recurse; reuse this unbounded scratch instead of allocating.
    mutable std::vector<uint32_t> VolumeCandidates;
    mutable uint64_t VolumeTests=0;
    template<class T,class F> bool QueryVolumes(const VolumeIndex& index,const std::vector<T>& volumes,V lo,V hi,F&& visit) const {
        if (!IndexVolumes) {
            for (const auto& volume:volumes) { ++VolumeTests; if (visit(volume)) return true; }
            return false;
        }
        const auto bounds=VolumeIndex::Outward(lo,hi);
        VolumeCandidates.clear(); index.Query(0,bounds.Lo,bounds.Hi,VolumeCandidates);
        // Preserve the old array-order narrowphase and all <= tie winners.
        std::sort(VolumeCandidates.begin(),VolumeCandidates.end());
        for (auto id:VolumeCandidates) { ++VolumeTests; if (visit(volumes[id])) return true; }
        return false;
    }
    std::vector<NativeCollisionHit> Sources;
    size_t SourceTriangles=0, CollapsedTriangles=0;
    bool SourceWorld=false;
    static V Local(const Box& box,V p) {
        p=Sub(p,box.Position); return {Dot(p,box.Basis[0]),Dot(p,box.Basis[1]),Dot(p,box.Basis[2])};
    }
    static bool Inside(const Box& box,V p) {
        return p.X>=box.Min.X && p.X<=box.Max.X && p.Y>=box.Min.Y && p.Y<=box.Max.Y && p.Z>=box.Min.Z && p.Z<=box.Max.Z;
    }
    void Source(size_t index,NativeCollisionHit* hit) const {
        if (hit) *hit=index<Sources.size() ? Sources[index]:NativeCollisionHit{};
    }
    mutable uint64_t Tests=0;
    uint32_t Build(uint32_t begin, uint32_t end) {
        Node n;
        n.Lo={INFINITY,INFINITY,INFINITY}; n.Hi={-INFINITY,-INFINITY,-INFINITY};
        for (auto i=begin;i<end;++i) {
            n.Lo=Min(n.Lo,Triangles[Order[i]].Lo); n.Hi=Max(n.Hi,Triangles[Order[i]].Hi);
        }
        const auto index=static_cast<uint32_t>(Nodes.size());
        Nodes.push_back(n);
        if (end-begin<=8) { Nodes[index].Begin=begin; Nodes[index].Count=end-begin; return index; }
        const V extent=Sub(n.Hi,n.Lo);
        const int axis=extent.X>extent.Y ? (extent.X>extent.Z ? 0:2) : (extent.Y>extent.Z ? 1:2);
        auto key=[&](uint32_t i) {
            const V c=Add(Triangles[i].Lo,Triangles[i].Hi);
            return axis==0 ? c.X : axis==1 ? c.Y : c.Z;
        };
        const auto mid=begin+(end-begin)/2;
        std::nth_element(Order.begin()+begin,Order.begin()+mid,Order.begin()+end,
                         [&](uint32_t a,uint32_t b){ const float x=key(a),y=key(b); return x==y ? a<b : x<y; });
        const auto left=Build(begin,mid), right=Build(mid,end);
        Nodes[index].Left=left; Nodes[index].Right=right;
        return index;
    }
    template<class F> bool Query(uint32_t index,V lo,V hi,F&& visit) const {
        if (Nodes.empty()) return false;
        const auto& n=Nodes[index];
        if (!Overlap(lo,hi,n.Lo,n.Hi)) return false;
        if (n.Count) {
            for (uint32_t i=n.Begin;i<n.Begin+n.Count;++i) {
                const auto& t=Triangles[Order[i]];
                if (Overlap(lo,hi,t.Lo,t.Hi)) { ++Tests; if (visit(t)) return true; }
            }
            return false;
        }
        return Query(n.Left,lo,hi,visit) || Query(n.Right,lo,hi,visit);
    }
};

RealtimeGameplayWorld::RealtimeGameplayWorld():m_Impl(std::make_unique<Impl>()) {}
RealtimeGameplayWorld::~RealtimeGameplayWorld()=default;
bool RealtimeGameplayWorld::Rebuild(const NativeCollisionSnapshot& snapshot,std::string& error,bool indexVolumes) {
    auto next=std::make_unique<Impl>(); next->SourceWorld=true;
    next->IndexVolumes=indexVolumes;
    auto vec=[](const NativeCollisionVector& v) -> V {return {v[0],v[1],v[2]};};
    for (const auto& inst:snapshot.Instances) {
        if (!inst.Model || !inst.Model->Unsupported.empty()) { error="invalid/unsupported owned COL instance"; return false; }
        const auto& model=*inst.Model;
        const V position=vec(inst.Placement.Position);
        std::array<V,3> basis{vec(inst.Basis[0]),vec(inst.Basis[1]),vec(inst.Basis[2])};
        if (!Finite(position)) { error="invalid source COL placement"; return false; }
        for (size_t i=0;i<3;++i) for (size_t j=0;j<3;++j) {
            if (!Finite(basis[i]) || std::abs(Dot(basis[i],basis[j])-(i==j ? 1.0f:0.0f))>0.001f) {
                error="source COL basis is not rigid"; return false;
            }
        }
        auto world=[&](V p) {return Add(position,Add(Mul(basis[0],p.X),Add(Mul(basis[1],p.Y),Mul(basis[2],p.Z))));};
        auto source=[&](NativeCollisionPrimitive type,size_t index,NativeCollisionSurface surface) {
            NativeCollisionHit hit;
            hit.Model=model.Name; hit.Library=model.Library; hit.Ipl=inst.Placement.Ipl;
            hit.ModelId=inst.Placement.ModelId; hit.HeaderId=model.HeaderId; hit.Record=inst.Placement.Record;
            hit.Binary=inst.Placement.Binary; hit.ValidatedHeaderId=model.ValidatedHeaderId; hit.TimeShared=inst.TimeShared;
            hit.Primitive=type; hit.PrimitiveIndex=static_cast<uint32_t>(index); hit.Surface=surface;
            next->Sources.push_back(std::move(hit)); return next->Sources.size()-1;
        };
        auto triangle=[&](V a,V b,V c,size_t src) {
            const V n=Cross(Sub(b,a),Sub(c,a)); const float length=Length(n);
            if (!Finite(a)||!Finite(b)||!Finite(c)||!std::isfinite(length)) return false;
            next->Triangles.push_back({a,b,c,Min(a,Min(b,c)),Max(a,Max(b,c)),length>0 ? std::abs(n.Z)/length:0,src});
            if (length==0) ++next->CollapsedTriangles;
            return true;
        };
        for (size_t i=0;i<model.Faces.size();++i) {
            const auto& face=model.Faces[i];
            for (auto index:face.Vertices) if (index>=model.Vertices.size()) { error="invalid owned COL face index"; return false; }
            if (!triangle(world(vec(model.Vertices[face.Vertices[0]])),world(vec(model.Vertices[face.Vertices[1]])),
                          world(vec(model.Vertices[face.Vertices[2]])),source(NativeCollisionPrimitive::Triangle,i,face.Surface))) {
                error="nonfinite source COL face: "+model.Name+"["+std::to_string(i)+"]"; return false;
            }
            ++next->SourceTriangles;
        }
        for (size_t i=0;i<model.Spheres.size();++i) {
            const auto& s=model.Spheres[i]; const V center=world(vec(s.Center));
            if (!Finite(center)||!std::isfinite(s.Radius)||s.Radius<0) { error="invalid owned COL sphere"; return false; }
            next->Spheres.push_back({center,s.Radius,source(NativeCollisionPrimitive::Sphere,i,s.Surface)});
        }
        for (size_t i=0;i<model.Boxes.size();++i) {
            const auto& b=model.Boxes[i]; const V lo=vec(b.Min),hi=vec(b.Max);
            if (!Finite(lo)||!Finite(hi)||lo.X>hi.X||lo.Y>hi.Y||lo.Z>hi.Z) { error="invalid owned COL box"; return false; }
            const auto src=source(NativeCollisionPrimitive::Box,i,b.Surface);
            next->Boxes.push_back({lo,hi,position,basis,src});
            // Exact boundary tessellation supports face/finite-edge/corner CCD;
            // retain the oriented volume for containment (a surface alone cannot).
            std::array<V,8> v;
            for (size_t k=0;k<8;++k) v[k]=world({k&1 ? hi.X:lo.X,k&2 ? hi.Y:lo.Y,k&4 ? hi.Z:lo.Z});
            constexpr int faces[6][4]={{0,2,6,4},{1,5,7,3},{0,4,5,1},{2,3,7,6},{0,1,3,2},{4,6,7,5}};
            for (const auto& f:faces) {
                // Collapsed sides retain their segment/point CCD boundary too.
                if (triangle(v[f[0]],v[f[1]],v[f[2]],src)) next->Triangles.back().Volume=next->Boxes.size()-1;
                if (triangle(v[f[0]],v[f[2]],v[f[3]],src)) next->Triangles.back().Volume=next->Boxes.size()-1;
            }
        }
    }
    if (next->Triangles.empty() && next->Spheres.empty() && next->Boxes.empty()) { error="owned source collision is empty"; return false; }
    next->Order.resize(next->Triangles.size()); std::iota(next->Order.begin(),next->Order.end(),0u);
    if (!next->Order.empty()) next->Build(0,static_cast<uint32_t>(next->Order.size()));
    for (const auto& s:next->Spheres) {
        const V r{s.Radius,s.Radius,s.Radius}; next->SphereIndex.AddBounds(Sub(s.Center,r),Add(s.Center,r));
    }
    for (const auto& b:next->Boxes) {
        // Local() uses B^T, which is only approximately orthonormal in float.
        // Bound its actual inverse, not just forward-transformed box corners.
        const float det=Dot(b.Basis[0],Cross(b.Basis[1],b.Basis[2]));
        const std::array<V,3> inverse{Mul(Cross(b.Basis[1],b.Basis[2]),1/det),
            Mul(Cross(b.Basis[2],b.Basis[0]),1/det),Mul(Cross(b.Basis[0],b.Basis[1]),1/det)};
        V lo{INFINITY,INFINITY,INFINITY},hi{-INFINITY,-INFINITY,-INFINITY};
        for (int k=0;k<8;++k) {
            const V p=Add(b.Position,Add(Mul(inverse[0],k&1 ? b.Max.X:b.Min.X),
                Add(Mul(inverse[1],k&2 ? b.Max.Y:b.Min.Y),Mul(inverse[2],k&4 ? b.Max.Z:b.Min.Z))));
            lo=Min(lo,p); hi=Max(hi,p);
        }
        // Include intermediate operands even if translation cancels a large
        // local offset, leaving a small final world bound near the origin.
        next->BoxIndex.AddBounds(lo,hi,Length(b.Position)+Length(b.Min)+Length(b.Max));
        // A local radius maps to an ellipsoid. Each inverse row's norm bounds
        // its world-axis extent; use the largest for the query AABB expansion.
        for (V row:{V{inverse[0].X,inverse[1].X,inverse[2].X},V{inverse[0].Y,inverse[1].Y,inverse[2].Y},V{inverse[0].Z,inverse[1].Z,inverse[2].Z}})
            next->BoxRadiusScale=std::max(next->BoxRadiusScale,Length(row));
    }
    next->BoxRadiusScale=std::nextafter(next->BoxRadiusScale,INFINITY);
    if (indexVolumes) { next->SphereIndex.Build(); next->BoxIndex.Build(); }
    m_Impl=std::move(next); error.clear(); return true;
}
bool RealtimeGameplayWorld::Rebuild(const WorldShotScene& scene,std::string& error) {
    auto next=std::make_unique<Impl>();
    for (const auto& m:scene.meshes) {
        if (m.pos.size()%9) { error="collision scene has incomplete triangle"; return false; }
        for (size_t i=0;i<m.pos.size();i+=9) {
            const V a=Load(m.pos,i),b=Load(m.pos,i+3),c=Load(m.pos,i+6);
            if (!Finite(a)||!Finite(b)||!Finite(c)) { error="nonfinite collision triangle"; return false; }
            const V normal=Cross(Sub(b,a),Sub(c,a));
            const float length=Length(normal);
            if (length<1e-7f) continue;
            next->Triangles.push_back({a,b,c,Min(a,Min(b,c)),Max(a,Max(b,c)),std::abs(normal.Z)/length});
        }
    }
    if (next->Triangles.empty()) { error="collision scene is empty"; return false; }
    next->Order.resize(next->Triangles.size());
    std::iota(next->Order.begin(),next->Order.end(),0u);
    next->Nodes.reserve(next->Triangles.size()/3);
    next->Build(0,static_cast<uint32_t>(next->Triangles.size()));
    m_Impl=std::move(next); error.clear(); return true;
}
bool RealtimeGameplayWorld::Ground(float x,float y,float top,float bottom,float& height,NativeCollisionHit* source) const {
    if (top<bottom) return false;
    float best=top-bottom;
    bool found=false;
    const float origin[3]={x,y,top},direction[3]={0,0,-1};
    m_Impl->Query(0,{x,y,bottom},{x,y,top},[&](const Impl::Triangle& t) {
        if (t.Up<WalkableUp) return false; // reject unwalkable walls, accept real ramps
        const float a[3]={t.A.X,t.A.Y,t.A.Z},b[3]={t.B.X,t.B.Y,t.B.Z},c[3]={t.C.X,t.C.Y,t.C.Z};
        float hit;
        if (Collide::RayTri(origin,direction,a,b,c,hit) && hit<=best) { best=hit; found=true; m_Impl->Source(t.Source,source); }
        return false;
    });
    m_Impl->QueryVolumes(m_Impl->SphereIndex,m_Impl->Spheres,{x,y,bottom},{x,y,top},[&](const Impl::Sphere& s) {
        const float dx=x-s.Center.X,dy=y-s.Center.Y,zz=s.Radius*s.Radius-dx*dx-dy*dy;
        if (zz<0 || s.Radius==0) return false;
        const float z=std::sqrt(zz),distance=top-s.Center.Z-z;
        if (z/s.Radius>=WalkableUp && distance>=0 && distance<=best) {
            best=distance; found=true; m_Impl->Source(s.Source,source);
        }
        return false;
    });
    if (found) height=top-best;
    return found;
}
bool RealtimeGameplayWorld::Raycast(V from,V to,V& hit,NativeCollisionHit* source) const {
    const V delta=Sub(to,from);
    float best=Length(delta);
    if (best<1e-6f) return false;
    const V d=Mul(delta,1.0f/best);
    const float origin[3]={from.X,from.Y,from.Z},direction[3]={d.X,d.Y,d.Z};
    bool found=false;
    m_Impl->Query(0,Min(from,to),Max(from,to),[&](const Impl::Triangle& t) {
        const float a[3]={t.A.X,t.A.Y,t.A.Z},b[3]={t.B.X,t.B.Y,t.B.Z},c[3]={t.C.X,t.C.Y,t.C.Z};
        float distance;
        if (Collide::RayTri(origin,direction,a,b,c,distance) && distance<=best) { best=distance; found=true; m_Impl->Source(t.Source,source); }
        return false;
    });
    m_Impl->QueryVolumes(m_Impl->SphereIndex,m_Impl->Spheres,Min(from,to),Max(from,to),[&](const Impl::Sphere& s) {
        const V o=Sub(from,s.Center); const float od=Dot(o,d),cc=Dot(o,o)-s.Radius*s.Radius,disc=od*od-cc;
        if (disc<0) return false;
        const float distance=cc<=0 ? 0:-od-std::sqrt(disc);
        if (distance>=0 && distance<=best) { best=distance; found=true; m_Impl->Source(s.Source,source); }
        return false;
    });
    m_Impl->QueryVolumes(m_Impl->BoxIndex,m_Impl->Boxes,from,from,[&](const Impl::Box& b) {
        if (Impl::Inside(b,Impl::Local(b,from))) { best=0; found=true; m_Impl->Source(b.Source,source); }
        return false;
    });
    if (found) hit=Add(from,Mul(d,best));
    return found;
}
bool RealtimeGameplayWorld::SphereBlocked(V center,float radius,NativeCollisionHit* source) const {
    const V r{radius,radius,radius},br=Mul(r,m_Impl->BoxRadiusScale);
    if (m_Impl->QueryVolumes(m_Impl->SphereIndex,m_Impl->Spheres,Sub(center,r),Add(center,r),[&](const Impl::Sphere& s) {
        const V d=Sub(center,s.Center); const float r=radius+s.Radius;
        if (Dot(d,d)<r*r) { m_Impl->Source(s.Source,source); return true; }
        return false;
    })) return true;
    if (m_Impl->QueryVolumes(m_Impl->BoxIndex,m_Impl->Boxes,Sub(center,br),Add(center,br),[&](const Impl::Box& b) {
        const V p=Impl::Local(b,center),closest=Max(b.Min,Min(b.Max,p)),d=Sub(p,closest);
        if (Impl::Inside(b,p) || Dot(d,d)<radius*radius) { m_Impl->Source(b.Source,source); return true; }
        return false;
    })) return true;
    return m_Impl->Query(0,Sub(center,r),Add(center,r),[&](const Impl::Triangle& t) {
        const V d=Sub(center,Closest(center,t.A,t.B,t.C));
        if (Dot(d,d)>=radius*radius) return false;
        m_Impl->Source(t.Source,source); return true;
    });
}
bool RealtimeGameplayWorld::SweepSphere(V from,V to,float radius,float& fraction,bool walkableOnly,float maxContactHeight,V* contactNormal,NativeCollisionHit* source) const {
    const V delta=Sub(to,from),r{radius,radius,radius};
    const float dd=Dot(delta,delta);
    if (dd<1e-12f) return false;
    float best=1.0f; bool found=false; V bestNormal{};
    auto volumeContact=[&](float f,V normal,V contact,size_t src) {
        if (f<0 || f>best || Dot(normal,delta)>=-1e-8f) return;
        if (walkableOnly && (normal.Z<WalkableUp || contact.Z>maxContactHeight+0.001f)) return;
        best=f; found=true; bestNormal=normal; m_Impl->Source(src,source);
    };
    m_Impl->QueryVolumes(m_Impl->SphereIndex,m_Impl->Spheres,Sub(Min(from,to),r),Add(Max(from,to),r),[&](const Impl::Sphere& s) {
        const V o=Sub(from,s.Center); const float rr=radius+s.Radius;
        const float od=Dot(o,delta),cc=Dot(o,o)-rr*rr,disc=od*od-dd*cc;
        if (disc<0) return false;
        const float f=cc<=0 ? 0:(-od-std::sqrt(disc))/dd;
        const V d=Sub(Add(from,Mul(delta,f)),s.Center); const float length=Length(d);
        const V n=length>1e-8f ? Mul(d,1/length):Mul(delta,-1/std::sqrt(dd));
        volumeContact(f,n,Add(s.Center,Mul(n,s.Radius)),s.Source);
        return false;
    });
    m_Impl->QueryVolumes(m_Impl->BoxIndex,m_Impl->Boxes,from,from,[&](const Impl::Box& b) {
        const V p=Impl::Local(b,from);
        if (!Impl::Inside(b,p)) return false;
        const std::array<float,3> coord{p.X,p.Y,p.Z},lo{b.Min.X,b.Min.Y,b.Min.Z},hi{b.Max.X,b.Max.Y,b.Max.Z};
        float closest=INFINITY; V normal{},contact{};
        for (size_t j=0;j<3;++j) for (int side=0;side<2;++side) {
            const float distance=side ? hi[j]-coord[j]:coord[j]-lo[j]; const V n=Mul(b.Basis[j],side ? 1.0f:-1.0f);
            if (distance<closest || (distance==closest && Dot(n,delta)>Dot(normal,delta))) {
                closest=distance; normal=n; contact=Add(from,Mul(n,distance));
            }
        }
        volumeContact(0,normal,contact,b.Source);
        return false;
    });
    m_Impl->Query(0,Sub(Min(from,to),r),Add(Max(from,to),r),[&](const Impl::Triangle& t) {
        if (t.Volume!=SIZE_MAX && Impl::Inside(m_Impl->Boxes[t.Volume],Impl::Local(m_Impl->Boxes[t.Volume],from))) return false;
        if (walkableOnly && t.Up<WalkableUp) return false;
        auto accept=[&](float f,V contact) {
            if (f<0 || f>best) return;
            const V normal=Sub(Add(from,Mul(delta,f)),contact);
            if (walkableOnly && (normal.Z<0.05f*radius || contact.Z>maxContactHeight+0.001f)) return;
            // Touching while travelling away/tangentially isn't an obstruction.
            if (Dot(normal,delta)>=-1e-8f) return;
            best=f; found=true; bestNormal=Mul(normal,1.0f/Length(normal));
            m_Impl->Source(t.Source,source);
        };
        const V closest=Closest(from,t.A,t.B,t.C);
        if (Dot(Sub(from,closest),Sub(from,closest))<radius*radius) accept(0,closest);
        V normal=Cross(Sub(t.B,t.A),Sub(t.C,t.A));
        const float normalLength=Length(normal);
        if (normalLength>0) normal=Mul(normal,1/normalLength);
        const float nd=Dot(normal,delta),plane=Dot(normal,Sub(from,t.A));
        if (std::abs(nd)>1e-8f) for (float side:{-1.0f,1.0f}) {
            const float f=(side*radius-plane)/nd;
            if (f<0 || f>best) continue;
            const V p=Sub(Add(from,Mul(delta,f)),Mul(normal,side*radius));
            // World coordinates are kilometres from zero; one float ULP can
            // exceed 0.1 mm. Stay well inside the controller's 4 mm skin.
            if (Length(Sub(p,Closest(p,t.A,t.B,t.C)))<ContactSkin*0.25f) accept(f,p);
        }
        // Minkowski boundary consists of two triangle faces, three finite
        // cylinders and three vertex spheres. Analytic roots avoid tunnelling.
        const std::array<V,3> vertices{t.A,t.B,t.C};
        for (size_t i=0;i<3;++i) {
            const V a=vertices[i],edge=Sub(vertices[(i+1)%3],a),o=Sub(from,a);
            const float od=Dot(o,delta),oo=Dot(o,o),disc=od*od-dd*(oo-radius*radius);
            if (disc>=0) accept((-od-std::sqrt(disc))/dd,a);
            const float ee=Dot(edge,edge),ed=Dot(edge,delta),eo=Dot(edge,o);
            const float qa=ee*dd-ed*ed,qb=ee*od-eo*ed,qc=ee*(oo-radius*radius)-eo*eo;
            const float determinant=qb*qb-qa*qc;
            if (qa>1e-10f && determinant>=0) {
                const float f=(-qb-std::sqrt(determinant))/qa,y=eo+f*ed;
                if (y>=0 && y<=ee) accept(f,Add(a,Mul(edge,y/ee)));
            }
        }
        return false;
    });
    if (found) { fraction=best; if (contactNormal) *contactNormal=bestNormal; }
    return found;
}
size_t RealtimeGameplayWorld::TriangleCount() const { return m_Impl->SourceWorld ? m_Impl->SourceTriangles:m_Impl->Triangles.size(); }
size_t RealtimeGameplayWorld::SphereCount() const { return m_Impl->Spheres.size(); }
size_t RealtimeGameplayWorld::BoxCount() const { return m_Impl->Boxes.size(); }
size_t RealtimeGameplayWorld::CollapsedTriangleCount() const { return m_Impl->CollapsedTriangles; }
uint64_t RealtimeGameplayWorld::TriangleTests() const { return m_Impl->Tests; }
uint64_t RealtimeGameplayWorld::VolumeTests() const { return m_Impl->VolumeTests; }

struct RealtimeGameplay::Impl {
    enum class AirActivity { None, Jump, Fall };
    enum class PublishedActivity { Unsupported, Ground, Drive, JumpAir, FallAir, JumpLand, FallLand };
    static constexpr size_t JumpLandClip=4, FallLandClip=5;
    struct Clip {
        const char* Name;
        std::vector<WorldShotScene> Frames;
        float Duration=1.0f,Stride=1.0f;
    };
    std::array<Clip,6> Clips{{{"IDLE_stance",{}},{"WALK_civi",{}},{"run_player",{}},{"JUMP_glide",{}},
                              {"JUMP_land",{}},{"FALL_land",{}}}};
    WorldShotScene CarBind,Actors;
    CarPoseStats CarStats{};
    CarPoseMeasure Measure{};
    HandlingParams Handling{};
    NativeTransmission Transmission;
    NativeTransmission::State TransmissionState;
    double TransmissionTime=0;
    std::vector<V> WheelPivots;
    float BrakeDecel=0,SteeringLock=0,Traction=0;
    float CarHalfWidth=0,CarHalfLength=0,CarHeight=0;
    float CarVertical=0,CarPitch=0,CarRoll=0;
    float OrbitYaw=0,OrbitPitch=0.27f,Phase=0,PedSpeed=0;
    float IdlePhase=0,AirPhase=0,MoveBlend=0,RunBlend=0,AirBlend=0;
    size_t PedMeshes=0;
    bool Initialized=false;
    bool BasePlayer=false;
    IfpAnimStats PlayerStats{};
    RealtimeGameplayState State;
    NativePlayerActivitySnapshot Activity;
    RealtimeGameplayCamera Camera;
    AirActivity AirSource=AirActivity::None;
    PublishedActivity Published=PublishedActivity::Unsupported;
    uint64_t ActivityRevision=0;
    double LandingTime=0;
    float LandingSpeed=1;
    size_t LandingClip=FallLandClip;
    bool LandingFinished=false;

    void PublishActivity() {
        Activity.Authority=NativePlayerActivityAuthority::SourceBacked;
        Activity.Revision=++ActivityRevision;
    }

    void ActivityPedState(NativePlayerPedState state) {
        Activity.PedState=state;
        Activity.Alive=state!=NativePlayerPedState::Die && state!=NativePlayerPedState::Dead;
    }
    void ResetActivityTasks() {
        for (auto& chain:Activity.PrimaryTasks) chain.clear();
        for (auto& chain:Activity.SecondaryTasks) chain.clear();
        // This is only the controller-owned task projection. Typed events are
        // cleared by owner reset or an explicit source consumption, never here.
        Activity.PrimaryTasks[static_cast<size_t>(NativePlayerPrimarySlot::Default)]={NativePlayerTaskType::PlayerOnFoot};
        Activity.SecondaryTasks[static_cast<size_t>(NativePlayerSecondarySlot::Facial)]={NativePlayerTaskType::Facial};
        ActivityPedState(NativePlayerPedState::Idle);
        Activity.InAir=false; Activity.Landing=false;
    }
    void SpawnActivity() {
        Activity={};
        Activity.GamePlaying=true; Activity.CoopGame=false;
        for (auto& weapon:Activity.WeaponSlots) {
            weapon.Type=NativePlayerWeaponType::Unarmed;
            weapon.State=NativePlayerWeaponState::Ready;
            weapon.AmmoInClip=weapon.TotalAmmo=0;
        }
        Activity.ActiveWeaponSlot=0;
        AirSource=AirActivity::None;
        Published=PublishedActivity::Ground;
        LandingTime=0; LandingSpeed=1; LandingClip=FallLandClip; LandingFinished=false;
        ResetActivityTasks();
        PublishActivity();
    }
    void GroundActivity() {
        if (Published==PublishedActivity::Ground) return;
        ResetActivityTasks();
        AirSource=AirActivity::None;
        Published=PublishedActivity::Ground;
        LandingTime=0; LandingFinished=false;
        PublishActivity();
    }
    void DriveActivity() {
        if (Published==PublishedActivity::Drive) return;
        ResetActivityTasks();
        Activity.PrimaryTasks[static_cast<size_t>(NativePlayerPrimarySlot::Primary)]={NativePlayerTaskType::CarDrive};
        ActivityPedState(NativePlayerPedState::Driving);
        AirSource=AirActivity::None;
        Published=PublishedActivity::Drive;
        LandingTime=0; LandingFinished=false;
        PublishActivity();
    }
    void AirborneActivity() {
        const auto next=AirSource==AirActivity::Jump ? PublishedActivity::JumpAir:PublishedActivity::FallAir;
        if (Published==next) return;
        ResetActivityTasks();
        Activity.InAir=true;
        if (AirSource==AirActivity::Jump) {
            Activity.PrimaryTasks[static_cast<size_t>(NativePlayerPrimarySlot::Primary)]={
                NativePlayerTaskType::Jump,NativePlayerTaskType::InAirAndLand,NativePlayerTaskType::InAir
            };
        } else {
            Activity.PrimaryTasks[static_cast<size_t>(NativePlayerPrimarySlot::EventResponseTemp)]={
                NativePlayerTaskType::InAirAndLand,NativePlayerTaskType::InAir
            };
        }
        Published=next;
        PublishActivity();
    }
    void LandingActivity(bool jumpLand) {
        ResetActivityTasks();
        Activity.Landing=true;
        if (AirSource==AirActivity::Jump) {
            Activity.PrimaryTasks[static_cast<size_t>(NativePlayerPrimarySlot::Primary)]={
                NativePlayerTaskType::Jump,NativePlayerTaskType::InAirAndLand,NativePlayerTaskType::Land
            };
        } else {
            Activity.PrimaryTasks[static_cast<size_t>(NativePlayerPrimarySlot::EventResponseTemp)]={
                NativePlayerTaskType::InAirAndLand,NativePlayerTaskType::Land
            };
        }
        LandingClip=jumpLand ? JumpLandClip:FallLandClip;
        // The current native player profile has source-initial fat/muscle, so
        // STAT_MOD_2 is 1. Sprint JUMP_land has the source 2x speed override.
        LandingSpeed=jumpLand ? 2.0f:1.0f;
        LandingTime=0; LandingFinished=false;
        Published=AirSource==AirActivity::Jump ? PublishedActivity::JumpLand:PublishedActivity::FallLand;
        PublishActivity();
    }
    void AdvanceLanding(double dt) {
        if (LandingFinished) return;
        LandingTime=std::min(LandingTime+dt*LandingSpeed,static_cast<double>(Clips[LandingClip].Duration));
        LandingFinished=LandingTime>=Clips[LandingClip].Duration;
    }
    float LandingPhase() const {
        return std::clamp(static_cast<float>(LandingTime/Clips[LandingClip].Duration),0.0f,1.0f);
    }

    bool PedBlocked(const RealtimeGameplayWorld& world,V feet,bool car=true) const {
        for (float z:PedCenters) {
            if (world.SphereBlocked(Add(feet,{0,0,z}),PedRadius)) return true;
        }
        if (car && State.CarPresent) {
            const V d=Sub(feet,State.Car);
            if (feet.Z+1.75f>State.Car.Z-static_cast<float>(Measure.clearance) && feet.Z<State.Car.Z+CarHeight &&
                std::abs(Dot(d,Right(State.CarHeading)))<CarHalfWidth+PedRadius &&
                std::abs(Dot(d,Forward(State.CarHeading)))<CarHalfLength+PedRadius) return true;
        }
        return false;
    }
    bool PedPathBlocked(const RealtimeGameplayWorld& world,V from,V to,V* normal=nullptr) const {
        float hit,best=INFINITY;
        if (normal) *normal={};
        for (float z:PedCenters) {
            V n;
            if (world.SweepSphere(Add(from,{0,0,z}),Add(to,{0,0,z}),PedRadius,hit,false,INFINITY,&n)) {
                if (!normal) return true;
                if (hit<best) { best=hit; *normal=n; }
            }
        }
        // Car is a separate dynamic solid, absent from the static BVH. Small
        // substeps plus endpoint overlap cover it (max sprint step is 4.6 cm).
        return best<INFINITY || PedBlocked(world,to);
    }
    bool Support(const RealtimeGameplayWorld& world,V feet,float up,float down,float& height) const {
        const V from=Add(feet,{0,0,FootCenter+up}),to=Add(feet,{0,0,FootCenter-down});
        float fraction;
        // Bound the actual touched triangle height, not just the sphere centre:
        // otherwise a rounded edge could ratchet the body up an excessive ledge.
        if (!world.SweepSphere(from,to,PedRadius,fraction,true,feet.Z+std::max(up,StepUp))) return false;
        height=from.Z+(to.Z-from.Z)*fraction-PedRadius;
        return true;
    }
    bool CarBlocked(const RealtimeGameplayWorld& world,V origin,float heading) const {
        const float radius=CarHalfWidth*0.92f;
        // Overlapping sphere rows cover the body width and both bumpers. The
        // lower row sits above wheel contact, allowing small road irregularities.
        const float low=std::max(radius-static_cast<float>(Measure.clearance)+0.18f,0.15f);
        for (float y:{-CarHalfLength+radius,0.0f,CarHalfLength-radius}) {
            const V p=Add(origin,Mul(Forward(heading),y));
            if (world.SphereBlocked(Add(p,{0,0,low}),radius)) return true;
        }
        return false;
    }
    V Door(float side= -1.0f) const {
        return Add(State.Car,Add(Mul(Right(State.CarHeading),side*(CarHalfWidth+0.65f)),
                                Mul(Forward(State.CarHeading),static_cast<float>(Measure.frontY)*0.3f)));
    }
    void Interact(const RealtimeGameplayWorld& world) {
        if (Activity.Landing) return;
        if (!State.CarPresent) return;
        if (std::abs(State.Speed)>0.8f) return;
        if (!State.InVehicle) {
            const V door=Door();
            V delta=Sub(door,State.Ped); delta.Z=0;
            V hit;
            if (State.Grounded && Length(delta)<2.1f && std::abs(State.Ped.Z-(State.Car.Z-Measure.clearance))<1.0f &&
                !world.Raycast(Add(State.Ped,{0,0,0.9f}),Add(door,{0,0,0.3f}),hit)) {
                State.InVehicle=true; ++State.Entries; State.VerticalSpeed=0;
            }
            return;
        }
        for (float side:{-1.0f,1.0f}) {
            V p=Door(side);
            float ground;
            V hit;
            if (!world.Ground(p.X,p.Y,State.Car.Z+0.5f,State.Car.Z-2.5f,ground)) continue;
            p.Z=ground;
            if (PedBlocked(world,p) || world.Raycast(Add(State.Car,{0,0,0.5f}),Add(p,{0,0,0.9f}),hit)) continue;
            State.Ped=p; State.InVehicle=false; State.Grounded=true; State.VerticalSpeed=0;
            State.PedHeading=State.CarHeading; ++State.Exits; return;
        }
    }
    void PedStep(float h,const RealtimeGameplayInput& input,const RealtimeGameplayWorld& world) {
        V dir=Add(Mul(Forward(OrbitYaw),input.Forward),Mul(Right(OrbitYaw),input.Side));
        const float len=Length(dir);
        if (len>1.0f) dir=Mul(dir,1.0f/len);
        const float speed=input.Sprint ? 5.5f : 2.0f;
        const V before=State.Ped;
        const bool wasGrounded=State.Grounded;
        V collisionNormal{};
        auto move=[&](V offset,V& result) {
            result=Add(before,offset);
            float support;
            if (wasGrounded && Support(world,result,StepUp,StepDown,support)) {
                result.Z=support;
                if (!PedPathBlocked(world,before,result,&collisionNormal)) return true;
                // Step path: raise, advance, settle onto a real footprint hit.
                // Every leg is swept, including the head; never bypass a riser.
                V raised=before; raised.Z=std::max(before.Z,result.Z);
                V across=result; across.Z=raised.Z;
                return !PedPathBlocked(world,before,raised) && !PedPathBlocked(world,raised,across) &&
                       !PedPathBlocked(world,across,result);
            }
            return !PedPathBlocked(world,before,result,&collisionNormal);
        };
        const V offset=Mul(dir,speed*h);
        V next;
        if (!move(offset,next)) {
            ++State.BlockedSteps;
            next=before;
            std::array<V,4> planes{};
            for (size_t count=0;count<planes.size();++count) {
                // Horizontal contact constraints cannot inject upward velocity.
                // Step/ramp support still comes exclusively from real geometry.
                collisionNormal.Z=0;
                const float length=Length(collisionNormal);
                if (length<0.01f) {
                    // Dynamic car overlap has no static triangle normal. Retain
                    // its clearance-checked axis fallback, never after acquiring
                    // a world plane (which would discard a corner constraint).
                    if (count==0) {
                        V x,y;
                        const bool xOk=move({offset.X,0,0},x),yOk=move({0,offset.Y,0},y);
                        if (xOk && Length(Sub(x,before))>Length(Sub(next,before))) next=x;
                        if (yOk && Length(Sub(y,before))>Length(Sub(next,before))) next=y;
                    }
                    break;
                }
                planes[count]=Mul(collisionNormal,1/length);
                V slide{}; float bestError=Dot(offset,offset);
                for (size_t i=0;i<=count;++i) {
                    const V candidate=Sub(offset,Mul(planes[i],std::min(0.0f,Dot(offset,planes[i]))));
                    bool feasible=true;
                    for (size_t j=0;j<=count;++j) feasible &= Dot(candidate,planes[j])>=-1e-7f;
                    const float error=Dot(Sub(candidate,offset),Sub(candidate,offset));
                    if (feasible && error<bestError) { slide=candidate; bestError=error; }
                }
                if (Length(slide)<1e-6f) break;
                V candidate;
                // Recheck the WHOLE body and step path after every projection.
                // A second wall adds a constraint instead of undoing the first.
                if (move(slide,candidate)) { next=candidate; break; }
            }
        }
        const float distance=std::hypot(next.X-before.X,next.Y-before.Y);
        State.WalkDistance+=distance;
        if (distance>1e-5f) {
            const float desired=std::atan2(next.Y-before.Y,next.X-before.X);
            State.PedHeading=Wrap(State.PedHeading+std::clamp(Wrap(desired-State.PedHeading),-6*h,6*h));
        }
        State.Grounded=false;
        float ground;
        if (wasGrounded && Support(world,next,0.015f,0.03f,ground) &&
            !PedPathBlocked(world,next,{next.X,next.Y,ground})) {
            next.Z=ground; State.VerticalSpeed=0; State.Grounded=true;
        } else {
            State.VerticalSpeed-=Gravity*h;
            const float z=next.Z+State.VerticalSpeed*h;
            if (State.VerticalSpeed<=0 && Support(world,next,0.01f,next.Z-z+0.01f,ground) &&
                !PedPathBlocked(world,next,{next.X,next.Y,ground})) {
                next.Z=ground; State.VerticalSpeed=0; State.Grounded=true; ++State.Landings;
            } else {
                if (PedPathBlocked(world,next,{next.X,next.Y,z})) {
                    State.VerticalSpeed=0; ++State.BlockedSteps;
                } else next.Z=z;
            }
        }
        State.Ped=next;
    }
    void CarStep(float h,const RealtimeGameplayInput& input,const RealtimeGameplayWorld& world) {
        // Original inertia smoothing and wheel friction are per CALL, not per
        // second. Use the original default 30 FPS limiter cadence (app/app.h),
        // CTimer timestep=50/30, independently of render/collision substeps.
        TransmissionTime+=h;
        constexpr double engineStep=NativeTransmission::SecondsPerUpdate;
        constexpr float timeStep=NativeTransmission::TimeStep;
        while (TransmissionTime+1e-9>=engineStep) {
            TransmissionTime-=engineStep;
            const float throttle=State.InVehicle ? input.Forward:0.0f;
            const bool braking=!State.InVehicle || input.Brake || input.Handbrake || throttle*State.Speed< -0.1f;
            if (State.InVehicle) {
                const float gas=braking ? 0.0f:throttle;
                const float acceleration=Transmission.DriveAcceleration(gas,TransmissionState,State.Speed/50.0f,timeStep,CarVertical==0);
                // ProcessCarWheelPair -> ProcessWheel -> ApplyMoveForce applies
                // this delta once per contacting driven wheel, not once per car.
                // Current native body has no individual suspension/tire solver:
                // flat no-slip all-wheel contact aggregation only; no air thrust.
                if (CarVertical==0) State.Speed+=acceleration*static_cast<float>(Transmission.DrivenWheels())*50.0f;
            }
            if (braking) {
                // Existing scalar brake/handbrake controller. NOT the original
                // per-wheel adhesion, brake bias, lockup or lateral slip solver.
                State.Speed=Approach(State.Speed,0.0f,BrakeDecel*(input.Handbrake ? 1.5f:1.0f)*static_cast<float>(engineStep));
            } else if (std::abs(throttle)<0.01f && CarVertical==0) {
                // ProcessWheel's non-driving friction: 0.9 / mass per contact
                // wheel per call (four supported wheels), capped at rest.
                State.Speed=Approach(State.Speed,0.0f,4.0f*0.9f/static_cast<float>(Handling.mass)*50.0f);
            }
            State.Speed=Transmission.AirResistance(State.Speed,timeStep);
            State.Gear=TransmissionState.CurrentGear;
        }
        const float steering=State.InVehicle ? -input.Side*SteeringLock/(1.0f+std::abs(State.Speed)*0.04f) : 0.0f;
        State.Steer=Approach(State.Steer,steering,h*2.0f);
        float yawRate=State.Speed/static_cast<float>(Measure.wheelbase)*std::tan(State.Steer);
        const float maxYaw=Traction*Gravity/std::max(1.0f,std::abs(State.Speed));
        yawRate=std::clamp(yawRate,-maxYaw,maxYaw);
        const float heading=Wrap(State.CarHeading+yawRate*h);
        V next=Add(State.Car,Mul(Forward(State.CarHeading+yawRate*h*0.5f),State.Speed*h));
        float ground;
        if (world.Ground(next.X,next.Y,State.Car.Z-static_cast<float>(Measure.clearance)+0.35f,
                         State.Car.Z-static_cast<float>(Measure.clearance)-0.6f,ground)) {
            next.Z=ground+static_cast<float>(Measure.clearance); CarVertical=0;
        } else {
            CarVertical-=Gravity*h;
            next.Z+=CarVertical*h;
            if (world.Ground(next.X,next.Y,State.Car.Z-static_cast<float>(Measure.clearance)+0.02f,
                             next.Z-static_cast<float>(Measure.clearance),ground)) {
                next.Z=ground+static_cast<float>(Measure.clearance); CarVertical=0;
            }
        }
        if (CarBlocked(world,next,heading)) { State.Speed=0; ++State.BlockedSteps; }
        else {
            const float distance=std::hypot(next.X-State.Car.X,next.Y-State.Car.Y);
            State.DriveDistance+=distance;
            State.WheelSpin=Wrap(State.WheelSpin-std::copysign(distance,State.Speed)/static_cast<float>(Measure.wheelR));
            State.Car=next; State.CarHeading=heading;
        }
        auto contact=[&](V direction,float offset,float& z) {
            V p=Add(State.Car,Mul(direction,offset));
            return world.Ground(p.X,p.Y,State.Car.Z+0.4f,State.Car.Z-static_cast<float>(Measure.clearance)-1.0f,z);
        };
        float a,b;
        if (contact(Forward(State.CarHeading),static_cast<float>(Measure.frontY),a) &&
            contact(Forward(State.CarHeading),static_cast<float>(Measure.rearY),b))
            CarPitch=Approach(CarPitch,std::clamp(std::atan2(a-b,static_cast<float>(Measure.wheelbase)),-0.45f,0.45f),h*2);
        if (contact(Right(State.CarHeading),CarHalfWidth,a) && contact(Right(State.CarHeading),-CarHalfWidth,b))
            CarRoll=Approach(CarRoll,std::clamp(-std::atan2(a-b,2*CarHalfWidth),-0.4f,0.4f),h*2);
        if (State.InVehicle) { State.Ped=State.Car; State.Grounded=CarVertical==0; }
    }
    void Pose(float dt) {
        State.PedRoot=Add(State.Ped,{0,0,1.0f});
        State.PedCurrentRotation=State.PedHeading-Pi*0.5f;
        const float moveTarget=std::clamp(PedSpeed/2.0f,0.0f,1.0f);
        MoveBlend+=(moveTarget-MoveBlend)*(-std::expm1(-12.0f*dt));
        if (moveTarget==0 && MoveBlend<0.001f) MoveBlend=0;
        RunBlend=Approach(RunBlend,std::clamp((PedSpeed-2.0f)/3.5f,0.0f,1.0f),dt*5);
        AirBlend=Approach(AirBlend,State.Grounded ? 0.0f:1.0f,dt*7);
        IdlePhase=std::fmod(IdlePhase+dt/Clips[0].Duration,1.0f);
        // Glide is a one-shot pose, not a foot cycle. Hold its endpoint on a
        // long fall and rewind only after the grounded crossfade has completed.
        if (!State.Grounded) AirPhase=std::min(AirPhase+dt/Clips[3].Duration,1.0f);
        else if (AirBlend==0) AirPhase=0;
        State.Animation=Activity.Landing ? Clips[LandingClip].Name:
            Clips[AirBlend>0.5f ? 3:MoveBlend<0.5f ? 0:RunBlend>0.5f ? 2:1].Name;
        State.LocomotionPhase=Phase; State.LocomotionBlend=MoveBlend;
        State.RunBlend=RunBlend; State.AirBlend=AirBlend;
        std::array<float,6> weights{},phases{IdlePhase,Phase,Phase,AirPhase,0,0};
        if (Activity.Landing) {
            weights[LandingClip]=1;
            phases[LandingClip]=LandingPhase();
        } else {
            weights={ (1-AirBlend)*(1-MoveBlend),(1-AirBlend)*MoveBlend*(1-RunBlend),
                      (1-AirBlend)*MoveBlend*RunBlend,AirBlend,0,0 };
        }
        std::array<size_t,6> frames{}; std::array<float,6> blends{};
        for (size_t k=0;k<Clips.size();++k) {
            const float sample=phases[k]*static_cast<float>(Clips[k].Frames.size()-1);
            frames[k]=static_cast<size_t>(sample); blends[k]=sample-static_cast<float>(frames[k]);
        }
        for (size_t i=0;i<PedMeshes;++i) {
            auto& out=Actors.meshes[i]; const auto& ma=Clips[0].Frames[0].meshes[i];
            out.tris=State.InVehicle ? 0 : ma.tris;
            // Keep arrays consistent with tris even for renderers that iterate
            // the soup instead of tris. Capacity survives entry/exit.
            out.pos.resize(State.InVehicle ? 0 : ma.pos.size()); out.nrm.resize(out.pos.size());
            for (size_t v=0;v<out.pos.size();v+=3) {
                V p{},n{};
                for (size_t k=0;k<Clips.size();++k) {
                    if (weights[k]<=0) continue;
                    const auto& a=Clips[k].Frames[frames[k]].meshes[i];
                    const auto& b=Clips[k].Frames[std::min(frames[k]+1,Clips[k].Frames.size()-1)].meshes[i];
                    const float wa=weights[k]*(1-blends[k]),wb=weights[k]*blends[k];
                    p=Add(p,Add(Mul(Load(a.pos,v),wa),Mul(Load(b.pos,v),wb)));
                    n=Add(n,Add(Mul(Load(a.nrm,v),wa),Mul(Load(b.nrm,v),wb)));
                }
                Store(out.pos,v,Add(State.Ped,RotateZ(p,State.PedHeading-Pi*0.5f)));
                const float length=Length(n);
                Store(out.nrm,v,RotateZ(Mul(n,1.0f/std::max(length,1e-6f)),State.PedHeading-Pi*0.5f));
            }
        }
        const size_t kits=(CarBind.meshes.size()-static_cast<size_t>(CarStats.geoms))/4;
        for (size_t i=0;i<CarBind.meshes.size();++i) {
            auto& out=Actors.meshes[PedMeshes+i]; const auto& bind=CarBind.meshes[i];
            out.tris=State.CarPresent ? bind.tris:0;
            out.pos.resize(State.CarPresent ? bind.pos.size():0); out.nrm.resize(out.pos.size());
            for (size_t v=0;v<out.pos.size();v+=3) {
                V p=Load(bind.pos,v),n=Load(bind.nrm,v);
                if (i>=static_cast<size_t>(CarStats.geoms)) {
                    const size_t wheel=(i-CarStats.geoms)/kits;
                    const V pivot=WheelPivots[wheel];
                    p=RotateX(Sub(p,pivot),State.WheelSpin); n=RotateX(n,State.WheelSpin);
                    const std::string name=CarStats.wheelNames[wheel];
                    if (name.find("lf")!=std::string::npos || name.find("rf")!=std::string::npos) {
                        p=RotateZ(p,State.Steer); n=RotateZ(n,State.Steer);
                    }
                    p=Add(p,pivot);
                }
                p=RotateZ(RotateY(RotateX(p,CarPitch),CarRoll),State.CarHeading-Pi*0.5f);
                n=RotateZ(RotateY(RotateX(n,CarPitch),CarRoll),State.CarHeading-Pi*0.5f);
                Store(out.pos,v,Add(p,State.Car)); Store(out.nrm,v,n);
            }
        }
        V lo{INFINITY,INFINITY,INFINITY},hi{-INFINITY,-INFINITY,-INFINITY};
        Actors.stats.triangles=0;
        for (const auto& m:Actors.meshes) {
            Actors.stats.triangles+=m.tris;
            for (size_t i=0;i<m.pos.size();i+=3) { const V p=Load(m.pos,i); lo=Min(lo,p); hi=Max(hi,p); }
        }
        Actors.stats.vertices=Actors.stats.triangles*3;
        Actors.bboxMin[0]=lo.X; Actors.bboxMin[1]=lo.Y; Actors.bboxMin[2]=lo.Z;
        Actors.bboxMax[0]=hi.X; Actors.bboxMax[1]=hi.Y; Actors.bboxMax[2]=hi.Z;
    }
    void UpdateCamera(float dt,const RealtimeGameplayWorld& world) {
        if (State.InVehicle && std::abs(State.Speed)>1.0f) OrbitYaw=Wrap(OrbitYaw+Wrap(State.CarHeading-OrbitYaw)*std::min(dt*1.8f,1.0f));
        Camera.Target=Add(State.InVehicle ? State.Car:State.Ped,{0,0,State.InVehicle ? 0.8f:1.15f});
        const float distance=State.InVehicle ? 7.5f:4.6f;
        V desired=Add(Camera.Target,Add(Mul(Forward(OrbitYaw),-distance*std::cos(OrbitPitch)),{0,0,distance*std::sin(OrbitPitch)}));
        V hit;
        if (world.Raycast(Camera.Target,desired,hit)) {
            V delta=Sub(hit,Camera.Target); const float len=Length(delta);
            desired=Add(Camera.Target,Mul(delta,std::max(0.0f,len-0.25f)/std::max(len,1e-6f)));
        }
        Camera.Position=desired;
        const V direction=Sub(Camera.Target,desired);
        Camera.Yaw=std::atan2(direction.Y,direction.X);
        Camera.Pitch=std::atan2(direction.Z,std::hypot(direction.X,direction.Y));
    }
};

RealtimeGameplay::RealtimeGameplay():m_Impl(std::make_unique<Impl>()) {}
RealtimeGameplay::~RealtimeGameplay()=default;
bool RealtimeGameplay::Initialize(const char* gameDir,std::string& error) {
    return Initialize(gameDir,error,nullptr);
}
bool RealtimeGameplay::Initialize(const char* gameDir,std::string& error,const NativePlayerClothes* player) {
    return InitializeModel(gameDir,error,player,RealtimeGameplayModel::Andre);
}
bool RealtimeGameplay::Initialize(const char* gameDir,std::string& error,RealtimeGameplayModel model) {
    return InitializeModel(gameDir,error,nullptr,model);
}
bool RealtimeGameplay::InitializeModel(const char* gameDir,std::string& error,const NativePlayerClothes* player,RealtimeGameplayModel model) {
    auto next=std::make_unique<Impl>();
    next->BasePlayer=model==RealtimeGameplayModel::BasePlayer;
    char err[512]={};
    // The parse helpers keep their own TXDs but borrow the global engine. Their
    // shutdown functions destroy only their dictionaries; restore current before
    // and after cleanup so pager dictionaries cannot become dangling/current.
    struct DictionaryScope {
        rw::TexDictionary* Saved=rw::Engine::state!=rw::Engine::Dead ? rw::TexDictionary::getCurrent():nullptr;
        ~DictionaryScope() {
            if (rw::Engine::state==rw::Engine::Dead) return;
            rw::TexDictionary::setCurrent(Saved);
            IfpAnim_Shutdown(); CarPose_Shutdown();
            rw::TexDictionary::setCurrent(Saved);
        }
    } scope;
    if (!Handling_Load(gameDir,"landstal",next->Handling,err,sizeof(err))) { error=err; return false; }
    uint32_t handlingFlags=0;
    if (!ExtraHandling(next->BrakeDecel,next->SteeringLock,next->Traction,handlingFlags,error)) return false;
    const auto& handling=next->Handling;
    if (handling.gears>5 || !std::isfinite(handling.mass) || !std::isfinite(handling.drag) || handling.drag<0 ||
        !std::isfinite(handling.vmaxFileKmh) || !std::isfinite(handling.accelFile) ||
        !std::isfinite(handling.inertia) || handling.inertia<=0 ||
        (handling.driveType!='4' && handling.driveType!='F' && handling.driveType!='R')) {
        error="unsupported/invalid native automobile transmission"; return false;
    }
    next->Transmission.Initialize({static_cast<float>(handling.vmaxFileKmh),static_cast<float>(handling.accelFile),
        static_cast<float>(handling.inertia),static_cast<float>(handling.drag),static_cast<uint8_t>(handling.gears),
        handling.driveType,handlingFlags});
    NativePlayerAssets playerAssets;
    IfpAnimPlayerBank playerBank;
    if (player) {
        if (!NativePlayerAssets_Load(*player,playerAssets,error)) return false;
        if (!playerBank.Load(gameDir,"ped",err,sizeof(err))) { error=err; return false; }
    }
    for (auto& clip:next->Clips) {
        std::vector<IfpAnimSeqFrame> seq;
        if (player) {
            seq.resize(33);
            for (int i=0;i<33;++i) {
                auto& frame=seq[i];
                if (!IfpAnim_InitPlayer(playerAssets,playerBank,clip.Name,IfpAnim_SeqTimeFrac(i,33),
                    frame.scene,frame.stats,err,sizeof(err),next->Actors.images.empty() && i==0)) {
                    error=err; return false;
                }
                const bool partial=std::string(clip.Name)=="JUMP_glide" || std::string(clip.Name)=="JUMP_land" ||
                                   std::string(clip.Name)=="FALL_land";
                const int tracks=partial ? 26:32;
                if (frame.stats.bones!=32 || frame.stats.mapped!=tracks) { error="CJ clip has unexpected bone coverage"; return false; }
            }
        } else if (!IfpAnim_Seq(gameDir,next->BasePlayer ? "player":"andre",clip.Name,33,seq,err,sizeof(err))) { error=err; return false; }
        if (next->BasePlayer) {
            for (const auto& frame:seq) {
                const auto& s=frame.stats;
                const bool partial=std::string(clip.Name)=="JUMP_glide" || std::string(clip.Name)=="JUMP_land" ||
                                   std::string(clip.Name)=="FALL_land";
                const int tracks=partial ? 26:32;
                if (std::string(s.model)!="player" || std::string(s.src)!="gta3.img:player.dff" || s.tried ||
                    s.bones!=32 || s.mapped!=tracks || s.verts!=6 || s.tris!=2 || std::abs(s.wsum-1.0)>0.001) {
                    error="MODEL_PLAYER must be direct gta3.img:player.dff (6 vertices, 2 triangles, 32 bones), no fallback"; return false;
                }
            }
        }
        if (&clip==&next->Clips[0]) next->PlayerStats=seq.front().stats;
        clip.Duration=static_cast<float>(seq.front().stats.animTotal);
        const auto& first=seq.front().stats; const auto& last=seq.back().stats;
        clip.Stride=std::hypot(last.rootWorld[0]-first.rootWorld[0],last.rootWorld[1]-first.rootWorld[1]);
        if (!(clip.Duration>0)) { error="empty real IFP animation"; return false; }
        if (clip.Stride<0.01f) clip.Stride=1.0f; // idle/glide have no locomotion; never used as speed
        for (auto& frame:seq) {
            // Base MODEL_PLAYER retains source model-space Z at the entity
            // origin; the six-vertex placeholder is not a feet-height measure.
            const V offset{frame.stats.rootWorld[0],frame.stats.rootWorld[1],next->BasePlayer ? -1.0f:first.animMin[2]};
            for (auto& mesh:frame.scene.meshes) {
                for (size_t v=0;v<mesh.pos.size();v+=3) Store(mesh.pos,v,Sub(Load(mesh.pos,v),offset));
            }
            if (next->Actors.images.empty()) next->Actors.images=frame.scene.images;
            frame.scene.images.clear();
            clip.Frames.push_back(std::move(frame.scene));
        }
    }
    CarPoseAudit audit{};
    if (!CarPose_Measure(gameDir,"landstal",next->Measure,err,sizeof(err)) ||
        !CarPose_Init(gameDir,"landstal",0,0,next->CarBind,next->CarStats,audit,err,sizeof(err),CarPoseTextures::RealtimeVehicle)) { error=err; return false; }
    if (next->CarStats.wheels!=4 || next->Measure.wheelbase<=0 || next->Measure.wheelR<=0 ||
        (next->CarBind.meshes.size()-next->CarStats.geoms)%4) { error="invalid car wheel layout"; return false; }
    WorldShotScene spin,steer;
    CarPoseStats stats{};
    if (!CarPose_Init(gameDir,"landstal",0,180,spin,stats,audit,err,sizeof(err),CarPoseTextures::RealtimeVehicle) ||
        !CarPose_Init(gameDir,"landstal",180,0,steer,stats,audit,err,sizeof(err),CarPoseTextures::RealtimeVehicle)) { error=err; return false; }
    if (spin.meshes.size()!=next->CarBind.meshes.size() || steer.meshes.size()!=next->CarBind.meshes.size()) {
        error="car pose cache component layouts differ"; return false;
    }
    const size_t kits=(next->CarBind.meshes.size()-next->CarStats.geoms)/4;
    if (!kits) { error="car has no wheel geometry"; return false; }
    for (size_t w=0;w<4;++w) {
        const size_t m=next->CarStats.geoms+w*kits;
        const V p=Load(next->CarBind.meshes[m].pos,0),s=Load(spin.meshes[m].pos,0),t=Load(steer.meshes[m].pos,0);
        // R180 around each DFF dummy reveals the exact Y/Z spin pivot and
        // front X steering pivot. Rear X is immaterial (no rear steering).
        next->WheelPivots.push_back({(p.X+t.X)*0.5f,(p.Y+s.Y)*0.5f,(p.Z+s.Z)*0.5f});
    }
    next->CarHalfWidth=std::max(std::abs(next->CarBind.bboxMin[0]),std::abs(next->CarBind.bboxMax[0]));
    next->CarHalfLength=std::max(std::abs(next->CarBind.bboxMin[1]),std::abs(next->CarBind.bboxMax[1]));
    next->CarHeight=next->CarBind.bboxMax[2];
    next->Actors.meshes=next->Clips[0].Frames[0].meshes;
    next->PedMeshes=next->Actors.meshes.size();
    const int imageBase=static_cast<int>(next->Actors.images.size());
    next->Actors.images.insert(next->Actors.images.end(),next->CarBind.images.begin(),next->CarBind.images.end());
    for (const auto& mesh:next->CarBind.meshes) {
        next->Actors.meshes.push_back(mesh);
        for (auto& image:next->Actors.meshes.back().triImg) if (image>=0) image+=imageBase;
    }
    next->Actors.stats.textures=static_cast<int>(next->Actors.images.size());
    next->Actors.stats.atomics=static_cast<int>(next->Actors.meshes.size());
    next->Initialized=true;
    m_Impl=std::move(next); error.clear(); return true;
}
bool RealtimeGameplay::Spawn(const RealtimeGameplayWorld& world,float x,float y,float rayTop,float heading,std::string& error) {
    auto& p=*m_Impl;
    if (!p.Initialized) { error="gameplay not initialized"; return false; }
    float ground;
    if (!world.Ground(x,y,rayTop,rayTop-150.0f,ground)) { error="no real ground at spawn"; return false; }
    V ped{x,y,ground};
    if (p.Support(world,ped,StepUp,0.05f,ground)) ped.Z=ground;
    if (p.PedBlocked(world,ped,false)) { error="spawn body intersects world"; return false; }
    V car;
    bool found=false;
    for (float distance:{3.5f,5.0f,7.0f,9.0f}) {
        for (float side:{1.0f,-1.0f}) {
            car=Add(ped,Mul(Right(heading),distance*side));
            if (!world.Ground(car.X,car.Y,ground+1.0f,ground-1.0f,car.Z)) continue;
            car.Z+=static_cast<float>(p.Measure.clearance);
            if (!p.CarBlocked(world,car,heading)) { found=true; break; }
        }
        if (found) break;
    }
    if (!found) { error="no nearby real-ground car clearance at spawn"; return false; }
    p.State={}; p.State.Ped=ped; p.State.Car=car;
    p.State.PedHeading=p.State.CarHeading=p.OrbitYaw=heading;
    p.State.Ready=p.State.Grounded=true;
    p.SpawnActivity();
    p.CarVertical=p.CarPitch=p.CarRoll=p.Phase=p.PedSpeed=0;
    p.TransmissionState={}; p.TransmissionTime=0;
    p.IdlePhase=p.AirPhase=p.MoveBlend=p.RunBlend=p.AirBlend=0;
    p.Pose(0); p.UpdateCamera(0,world); error.clear(); return true;
}
const IfpAnimStats& RealtimeGameplay::PlayerModelStats() const { return m_Impl->PlayerStats; }
bool RealtimeGameplay::SpawnScriptPlayer(const RealtimeGameplayWorld& world,V authoredBase,std::string& error) {
    auto& p=*m_Impl;
    if (!p.Initialized || !p.BasePlayer) { error="0053 requires preloaded base MODEL_PLAYER"; return false; }
    if (!std::isfinite(authoredBase.X) || !std::isfinite(authoredBase.Y) || !std::isfinite(authoredBase.Z)) {
        error="nonfinite script player position"; return false;
    }
    // MAP_Z_LOW_LIMIT auto-ground is a separate source operation; this slice
    // accepts the real first-pass authored position only.
    if (authoredBase.Z<=-100.0f) { error="0053 auto-ground sentinel not implemented"; return false; }
    float ground;
    if (!world.Ground(authoredBase.X,authoredBase.Y,authoredBase.Z+1.0f,authoredBase.Z-150.0f,ground)) {
        error="no resident real triangle ground at script player"; return false;
    }
    if (p.PedBlocked(world,authoredBase,false)) { error="script player body intersects resident world"; return false; }
    p.State={}; p.State.Ped=authoredBase; p.State.CarPresent=false;
    p.State.PedHeading=p.OrbitYaw=Pi*0.5f; // SetupPlayerPed orientation(0,0,0): forward +Y
    p.State.Ready=p.State.MissionCreated=p.State.PlayerOnFootTask=true;
    p.State.Grounded=std::abs(authoredBase.Z-ground)<0.05f;
    p.SpawnActivity(); // Exact 0053 task/event/ped/weapon initial state.
    p.CarVertical=p.CarPitch=p.CarRoll=p.Phase=p.PedSpeed=0;
    p.IdlePhase=p.AirPhase=p.MoveBlend=p.RunBlend=p.AirBlend=0;
    p.Pose(0); p.UpdateCamera(0,world); error.clear(); return true;
}
bool RealtimeGameplay::SetScriptHeading(float radians,std::string& error) {
    auto& p=*m_Impl;
    if (!p.State.Ready || !std::isfinite(radians)) { error="heading requires live player and finite angle"; return false; }
    if (!p.State.InVehicle) {
        p.State.PedHeading=radians+Pi*0.5f;
        p.State.PedAimingRotation=radians;
        p.Pose(0); // actual entity transform and persistent CPU-skinned RW mesh
        p.State.PedCurrentRotation=radians;
    }
    error.clear(); return true;
}
bool RealtimeGameplay::SetScriptCameraBehind(const RealtimeGameplayWorld& world,std::string& error) {
    auto& p=*m_Impl;
    if (!p.State.Ready) { error="camera requires live player"; return false; }
    p.OrbitYaw=p.State.PedHeading;
    p.Camera.ScriptDirectlyBehind=true;
    p.Camera.ScriptPedOrientation=std::fmod(p.State.PedHeading,2*Pi);
    if (p.Camera.ScriptPedOrientation<0) p.Camera.ScriptPedOrientation+=2*Pi;
    p.UpdateCamera(0,world); error.clear(); return true;
}
void RealtimeGameplay::Tick(double dt,const RealtimeGameplayInput& input,const RealtimeGameplayWorld& world) {
    auto& p=*m_Impl;
    if (!p.State.Ready || !std::isfinite(dt) || dt<=0) return;
    // FinishAnimCB marks SIMPLE_LAND finished; the following ProcessPed clears
    // bIsLanding and completes the task.
    if (p.Activity.Landing && p.LandingFinished && p.State.Grounded && !p.State.InVehicle) p.GroundActivity();
    RealtimeGameplayInput controls=input;
    auto axis=[](float v) { return std::isfinite(v) ? std::clamp(v,-1.0f,1.0f):0.0f; };
    controls.Forward=axis(controls.Forward); controls.Side=axis(controls.Side);
    p.OrbitYaw=Wrap(p.OrbitYaw+(std::isfinite(input.LookYaw) ? input.LookYaw:0.0f));
    p.OrbitPitch=std::clamp(p.OrbitPitch+(std::isfinite(input.LookPitch) ? input.LookPitch:0.0f),-0.1f,1.0f);
    const double bounded=std::min(dt,0.1);
    p.State.DroppedSeconds+=dt-bounded;
    const auto landingsBefore=p.State.Landings;
    if (controls.Interact) p.Interact(world);
    if (controls.Jump && !p.State.InVehicle && p.State.Grounded && !p.Activity.Landing) {
        p.State.VerticalSpeed=5.0f; p.State.Grounded=false; ++p.State.Jumps;
        p.AirSource=Impl::AirActivity::Jump;
    }
    const int steps=std::max(1,static_cast<int>(std::ceil(bounded*120.0)));
    const float h=static_cast<float>(bounded/steps);
    const double walkBefore=p.State.WalkDistance;
    for (int i=0;i<steps;++i) {
        if (p.State.CarPresent) p.CarStep(h,controls,world);
        if (!p.State.InVehicle) p.PedStep(h,controls,world);
    }
    // Average all accepted substeps BEFORE filtering. A rate limiter applied
    // separately to alternating short/long slides biases speed toward their
    // median and made a moving ped look idle. Exponential filtering preserves
    // the mean and is stable across the 2/3-substep boundary near 60 Hz.
    const float distance=static_cast<float>(p.State.WalkDistance-walkBefore);
    const float actualSpeed=distance/static_cast<float>(bounded);
    p.PedSpeed+=(actualSpeed-p.PedSpeed)*(-std::expm1(-18.0f*static_cast<float>(bounded)));
    if (actualSpeed==0 && p.PedSpeed<0.001f) p.PedSpeed=0;
    const float run=std::clamp((p.PedSpeed-2.0f)/3.5f,0.0f,1.0f);
    p.Phase+=distance/(p.Clips[1].Stride*(1-run)+p.Clips[2].Stride*run);
    p.Phase-=std::floor(p.Phase);
    if (p.State.InVehicle) p.DriveActivity();
    else if (p.State.Landings!=landingsBefore) {
        const bool jumpLand=controls.Sprint && (std::abs(controls.Forward)>0.0f || std::abs(controls.Side)>0.0f);
        p.LandingActivity(jumpLand);
    }
    else if (p.Activity.Landing) p.AdvanceLanding(bounded);
    else if (!p.State.Grounded) {
        if (p.AirSource==Impl::AirActivity::None) p.AirSource=Impl::AirActivity::Fall;
        p.AirborneActivity();
    } else p.GroundActivity();
    ++p.State.Ticks; p.State.SimulatedSeconds+=bounded;
    p.Pose(static_cast<float>(bounded)); p.UpdateCamera(static_cast<float>(bounded),world);
}
const RealtimeGameplayState& RealtimeGameplay::State() const { return m_Impl->State; }
const NativePlayerActivitySnapshot& RealtimeGameplay::Activity() const { return m_Impl->Activity; }
const RealtimeGameplayCamera& RealtimeGameplay::Camera() const { return m_Impl->Camera; }
const WorldShotScene& RealtimeGameplay::Actors() const { return m_Impl->Actors; }
