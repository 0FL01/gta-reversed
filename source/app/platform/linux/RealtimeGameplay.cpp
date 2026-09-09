#include "app/platform/linux/RealtimeGameplay.h"

#include "app/platform/linux/CarPose.h"
#include "app/platform/linux/Collide.h"
#include "app/platform/linux/Handling.h"
#include "app/platform/linux/IfpAnim.h"

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
static bool ExtraHandling(float& brake, float& lock, float& traction, std::string& error) {
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
        if (t.size()<21) break;
        auto number=[&](size_t i, float& out) {
            char* end=nullptr; out=std::strtof(t[i].c_str(),&end);
            return end && !*end && std::isfinite(out) && out>0.0f;
        };
        if (!number(17,brake) || !number(20,lock) || !number(8,traction)) break;
        lock*=Pi/180.0f;
        return true;
    }
    error="missing/invalid LANDSTAL braking, traction or steering lock"; return false;
}
} // namespace

struct RealtimeGameplayWorld::Impl {
    struct Triangle { V A,B,C,Lo,Hi; float Up; };
    struct Node { V Lo,Hi; uint32_t Begin=0,Count=0,Left=0,Right=0; };
    std::vector<Triangle> Triangles;
    std::vector<uint32_t> Order;
    std::vector<Node> Nodes;
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
bool RealtimeGameplayWorld::Ground(float x,float y,float top,float bottom,float& height) const {
    if (m_Impl->Nodes.empty() || top<bottom) return false;
    float best=top-bottom;
    bool found=false;
    const float origin[3]={x,y,top},direction[3]={0,0,-1};
    m_Impl->Query(0,{x,y,bottom},{x,y,top},[&](const Impl::Triangle& t) {
        if (t.Up<WalkableUp) return false; // reject unwalkable walls, accept real ramps
        const float a[3]={t.A.X,t.A.Y,t.A.Z},b[3]={t.B.X,t.B.Y,t.B.Z},c[3]={t.C.X,t.C.Y,t.C.Z};
        float hit;
        if (Collide::RayTri(origin,direction,a,b,c,hit) && hit<=best) { best=hit; found=true; }
        return false;
    });
    if (found) height=top-best;
    return found;
}
bool RealtimeGameplayWorld::Raycast(V from,V to,V& hit) const {
    if (m_Impl->Nodes.empty()) return false;
    const V delta=Sub(to,from);
    float best=Length(delta);
    if (best<1e-6f) return false;
    const V d=Mul(delta,1.0f/best);
    const float origin[3]={from.X,from.Y,from.Z},direction[3]={d.X,d.Y,d.Z};
    bool found=false;
    m_Impl->Query(0,Min(from,to),Max(from,to),[&](const Impl::Triangle& t) {
        const float a[3]={t.A.X,t.A.Y,t.A.Z},b[3]={t.B.X,t.B.Y,t.B.Z},c[3]={t.C.X,t.C.Y,t.C.Z};
        float distance;
        if (Collide::RayTri(origin,direction,a,b,c,distance) && distance<=best) { best=distance; found=true; }
        return false;
    });
    if (found) hit=Add(from,Mul(d,best));
    return found;
}
bool RealtimeGameplayWorld::SphereBlocked(V center,float radius) const {
    if (m_Impl->Nodes.empty()) return false;
    const V r{radius,radius,radius};
    return m_Impl->Query(0,Sub(center,r),Add(center,r),[&](const Impl::Triangle& t) {
        const V d=Sub(center,Closest(center,t.A,t.B,t.C));
        return Dot(d,d)<radius*radius;
    });
}
bool RealtimeGameplayWorld::SweepSphere(V from,V to,float radius,float& fraction,bool walkableOnly,float maxContactHeight) const {
    if (m_Impl->Nodes.empty()) return false;
    const V delta=Sub(to,from),r{radius,radius,radius};
    const float dd=Dot(delta,delta);
    if (dd<1e-12f) return false;
    float best=1.0f; bool found=false;
    m_Impl->Query(0,Sub(Min(from,to),r),Add(Max(from,to),r),[&](const Impl::Triangle& t) {
        if (walkableOnly && t.Up<WalkableUp) return false;
        auto accept=[&](float f,V contact) {
            if (f<0 || f>best) return;
            const V normal=Sub(Add(from,Mul(delta,f)),contact);
            if (walkableOnly && (normal.Z<0.05f*radius || contact.Z>maxContactHeight+0.001f)) return;
            // Touching while travelling away/tangentially isn't an obstruction.
            if (Dot(normal,delta)>=-1e-8f) return;
            best=f; found=true;
        };
        const V closest=Closest(from,t.A,t.B,t.C);
        if (Dot(Sub(from,closest),Sub(from,closest))<radius*radius) accept(0,closest);
        V normal=Cross(Sub(t.B,t.A),Sub(t.C,t.A)); normal=Mul(normal,1/Length(normal));
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
    if (found) fraction=best;
    return found;
}
size_t RealtimeGameplayWorld::TriangleCount() const { return m_Impl->Triangles.size(); }
uint64_t RealtimeGameplayWorld::TriangleTests() const { return m_Impl->Tests; }

struct RealtimeGameplay::Impl {
    struct Clip {
        const char* Name;
        std::vector<WorldShotScene> Frames;
        float Duration=1.0f,Stride=1.0f;
    };
    std::array<Clip,4> Clips{{{"IDLE_stance",{}},{"WALK_civi",{}},{"run_player",{}},{"JUMP_glide",{}}}};
    WorldShotScene CarBind,Actors;
    CarPoseStats CarStats{};
    CarPoseMeasure Measure{};
    HandlingParams Handling{};
    std::vector<V> WheelPivots;
    float BrakeDecel=0,SteeringLock=0,Traction=0;
    float CarHalfWidth=0,CarHalfLength=0,CarHeight=0;
    float CarVertical=0,CarPitch=0,CarRoll=0;
    float OrbitYaw=0,OrbitPitch=0.27f,Phase=0,PedSpeed=0;
    float IdlePhase=0,AirPhase=0,MoveBlend=0,RunBlend=0,AirBlend=0;
    size_t PedMeshes=0;
    bool Initialized=false;
    RealtimeGameplayState State;
    RealtimeGameplayCamera Camera;

    bool PedBlocked(const RealtimeGameplayWorld& world,V feet,bool car=true) const {
        for (float z:PedCenters) {
            if (world.SphereBlocked(Add(feet,{0,0,z}),PedRadius)) return true;
        }
        if (car) {
            const V d=Sub(feet,State.Car);
            if (feet.Z+1.75f>State.Car.Z-static_cast<float>(Measure.clearance) && feet.Z<State.Car.Z+CarHeight &&
                std::abs(Dot(d,Right(State.CarHeading)))<CarHalfWidth+PedRadius &&
                std::abs(Dot(d,Forward(State.CarHeading)))<CarHalfLength+PedRadius) return true;
        }
        return false;
    }
    bool PedPathBlocked(const RealtimeGameplayWorld& world,V from,V to) const {
        float hit;
        for (float z:PedCenters) {
            if (world.SweepSphere(Add(from,{0,0,z}),Add(to,{0,0,z}),PedRadius,hit)) return true;
        }
        // Car is a separate dynamic solid, absent from the static BVH. Small
        // substeps plus endpoint overlap cover it (max sprint step is 4.6 cm).
        return PedBlocked(world,to);
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
        auto move=[&](V offset,V& result) {
            result=Add(before,offset);
            float support;
            if (wasGrounded && Support(world,result,StepUp,StepDown,support)) {
                result.Z=support;
                if (!PedPathBlocked(world,before,result)) return true;
                // Step path: raise, advance, settle onto a real footprint hit.
                // Every leg is swept, including the head; never bypass a riser.
                V raised=before; raised.Z=std::max(before.Z,result.Z);
                V across=result; across.Z=raised.Z;
                return !PedPathBlocked(world,before,raised) && !PedPathBlocked(world,raised,across) &&
                       !PedPathBlocked(world,across,result);
            }
            return !PedPathBlocked(world,before,result);
        };
        const V offset=Mul(dir,speed*h);
        V next;
        if (!move(offset,next)) {
            ++State.BlockedSteps;
            // Evaluate both axes: a zero-length first axis mustn't suppress a
            // valid second slide. Keep the candidate with greatest progress.
            V x,y; const bool xOk=move({offset.X,0,0},x),yOk=move({0,offset.Y,0},y);
            next=before;
            if (xOk && Length(Sub(x,before))>Length(Sub(next,before))) next=x;
            if (yOk && Length(Sub(y,before))>Length(Sub(next,before))) next=y;
        }
        const float distance=std::hypot(next.X-before.X,next.Y-before.Y);
        PedSpeed=Approach(PedSpeed,distance/h,h*18.0f);
        State.WalkDistance+=distance;
        // Preserve foot-cycle progress through idle, airborne and speed changes.
        const float run=std::clamp((PedSpeed-2.0f)/3.5f,0.0f,1.0f);
        Phase+=distance/(Clips[1].Stride*(1-run)+Clips[2].Stride*run);
        Phase-=std::floor(Phase);
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
        const float throttle=State.InVehicle ? input.Forward : 0.0f;
        const bool braking=!State.InVehicle || input.Brake || input.Handbrake || throttle*State.Speed< -0.1f;
        if (braking) State.Speed=Approach(State.Speed,0.0f,BrakeDecel*(input.Handbrake ? 1.5f:1.0f)*h);
        else {
            State.Speed+=throttle*static_cast<float>(Handling.accelSi)*h;
            const float resistance=0.12f+static_cast<float>(Handling.drag/Handling.mass)*State.Speed*State.Speed;
            State.Speed=Approach(State.Speed,0.0f,resistance*h);
        }
        State.Speed=std::clamp(State.Speed,-static_cast<float>(Handling.vmaxMs)*0.25f,static_cast<float>(Handling.vmaxMs));
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
        MoveBlend=Approach(MoveBlend,std::clamp(PedSpeed/2.0f,0.0f,1.0f),dt*6);
        RunBlend=Approach(RunBlend,std::clamp((PedSpeed-2.0f)/3.5f,0.0f,1.0f),dt*5);
        AirBlend=Approach(AirBlend,State.Grounded ? 0.0f:1.0f,dt*7);
        IdlePhase=std::fmod(IdlePhase+dt/Clips[0].Duration,1.0f);
        // Glide is a one-shot pose, not a foot cycle. Hold its endpoint on a
        // long fall and rewind only after the grounded crossfade has completed.
        if (!State.Grounded) AirPhase=std::min(AirPhase+dt/Clips[3].Duration,1.0f);
        else if (AirBlend==0) AirPhase=0;
        State.Animation=Clips[AirBlend>0.5f ? 3:MoveBlend<0.5f ? 0:RunBlend>0.5f ? 2:1].Name;
        State.LocomotionPhase=Phase; State.LocomotionBlend=MoveBlend;
        State.RunBlend=RunBlend; State.AirBlend=AirBlend;
        const std::array<float,4> weights{(1-AirBlend)*(1-MoveBlend),(1-AirBlend)*MoveBlend*(1-RunBlend),
                                          (1-AirBlend)*MoveBlend*RunBlend,AirBlend};
        const std::array<float,4> phases{IdlePhase,Phase,Phase,AirPhase};
        std::array<size_t,4> frames{}; std::array<float,4> blends{};
        for (size_t k=0;k<4;++k) {
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
                for (size_t k=0;k<4;++k) {
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
    auto next=std::make_unique<Impl>();
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
    if (!ExtraHandling(next->BrakeDecel,next->SteeringLock,next->Traction,error)) return false;
    for (auto& clip:next->Clips) {
        std::vector<IfpAnimSeqFrame> seq;
        if (!IfpAnim_Seq(gameDir,"andre",clip.Name,33,seq,err,sizeof(err))) { error=err; return false; }
        clip.Duration=static_cast<float>(seq.front().stats.animTotal);
        const auto& first=seq.front().stats; const auto& last=seq.back().stats;
        clip.Stride=std::hypot(last.rootWorld[0]-first.rootWorld[0],last.rootWorld[1]-first.rootWorld[1]);
        if (!(clip.Duration>0)) { error="empty real IFP animation"; return false; }
        if (clip.Stride<0.01f) clip.Stride=1.0f; // idle/glide have no locomotion; never used as speed
        for (auto& frame:seq) {
            const V offset{frame.stats.rootWorld[0],frame.stats.rootWorld[1],first.animMin[2]};
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
        !CarPose_Init(gameDir,"landstal",0,0,next->CarBind,next->CarStats,audit,err,sizeof(err))) { error=err; return false; }
    if (next->CarStats.wheels!=4 || next->Measure.wheelbase<=0 || next->Measure.wheelR<=0 ||
        (next->CarBind.meshes.size()-next->CarStats.geoms)%4) { error="invalid car wheel layout"; return false; }
    WorldShotScene spin,steer;
    CarPoseStats stats{};
    if (!CarPose_Init(gameDir,"landstal",0,180,spin,stats,audit,err,sizeof(err)) ||
        !CarPose_Init(gameDir,"landstal",180,0,steer,stats,audit,err,sizeof(err))) { error=err; return false; }
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
    p.CarVertical=p.CarPitch=p.CarRoll=p.Phase=p.PedSpeed=0;
    p.IdlePhase=p.AirPhase=p.MoveBlend=p.RunBlend=p.AirBlend=0;
    p.Pose(0); p.UpdateCamera(0,world); error.clear(); return true;
}
void RealtimeGameplay::Tick(double dt,const RealtimeGameplayInput& input,const RealtimeGameplayWorld& world) {
    auto& p=*m_Impl;
    if (!p.State.Ready || !std::isfinite(dt) || dt<=0) return;
    RealtimeGameplayInput controls=input;
    auto axis=[](float v) { return std::isfinite(v) ? std::clamp(v,-1.0f,1.0f):0.0f; };
    controls.Forward=axis(controls.Forward); controls.Side=axis(controls.Side);
    p.OrbitYaw=Wrap(p.OrbitYaw+(std::isfinite(input.LookYaw) ? input.LookYaw:0.0f));
    p.OrbitPitch=std::clamp(p.OrbitPitch+(std::isfinite(input.LookPitch) ? input.LookPitch:0.0f),-0.1f,1.0f);
    const double bounded=std::min(dt,0.1);
    p.State.DroppedSeconds+=dt-bounded;
    if (controls.Interact) p.Interact(world);
    if (controls.Jump && !p.State.InVehicle && p.State.Grounded) {
        p.State.VerticalSpeed=5.0f; p.State.Grounded=false; ++p.State.Jumps;
    }
    const int steps=std::max(1,static_cast<int>(std::ceil(bounded*120.0)));
    const float h=static_cast<float>(bounded/steps);
    for (int i=0;i<steps;++i) {
        p.CarStep(h,controls,world);
        if (!p.State.InVehicle) p.PedStep(h,controls,world);
    }
    ++p.State.Ticks; p.State.SimulatedSeconds+=bounded;
    p.Pose(static_cast<float>(bounded)); p.UpdateCamera(static_cast<float>(bounded),world);
}
const RealtimeGameplayState& RealtimeGameplay::State() const { return m_Impl->State; }
const RealtimeGameplayCamera& RealtimeGameplay::Camera() const { return m_Impl->Camera; }
const WorldShotScene& RealtimeGameplay::Actors() const { return m_Impl->Actors; }
