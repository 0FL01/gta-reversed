#include "NativeSourceGround.h"
#include "NativeWorldEntityInfo.h"
#include <algorithm>

namespace {
using V = NativeCollisionVector;
using Result = NativeSourceGroundResult;
using Status = NativeSourceGroundStatus;
using Reason = NativeSourceGroundReason;
static V Add(V a, V b) { return {a[0]+b[0], a[1]+b[1], a[2]+b[2]}; }
static V Sub(V a, V b) { return {a[0]-b[0], a[1]-b[1], a[2]-b[2]}; }
static V Mul(V a, float b) { return {a[0]*b, a[1]*b, a[2]*b}; }
// Vector.cpp:196: DotProduct sums Z, then Y, then X. Float rounding is
// observable in compressed-plane intersection fractions.
static float Dot(V a, V b) { return a[2]*b[2]+a[1]*b[1]+a[0]*b[0]; }
static V Cross(V a, V b) { return {a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0]}; }
static V Normalized(V a) {
    // NormaliseAndMag deliberately uses a different, explicit X/Y/Z sum.
    const auto dot = a[0]*a[0]+a[1]*a[1]+a[2]*a[2];
    if (dot <= 0.0f) { a[0] = 1.0f; return a; } // Vector.cpp:70-84
    // Source uses unqualified sqrt (double), auto reciprocal and compound
    // assignment back to each float. std::sqrt(float) changes sphere normals.
    const auto reciprocal = 1.0f / std::sqrt(static_cast<double>(dot));
    for (auto& component : a) component *= reciprocal;
    return a;
}
static bool Finite(V a) { return std::ranges::all_of(a, [](float f) { return std::isfinite(f); }); }
static bool Bounds(V a, V b) { return Finite(a) && Finite(b) && a[0]<=b[0] && a[1]<=b[1] && a[2]<=b[2]; }
static V Vector(const NativeSourceGroundTransform& m, V p) {
    return Add(Add(Mul(m.Basis[0], p[0]), Mul(m.Basis[1], p[1])), Mul(m.Basis[2], p[2]));
}
static V Point(const NativeSourceGroundTransform& m, V p) { return Add(Vector(m, p), m.Position); }
static NativeSourceGroundTransform Invert(const NativeSourceGroundTransform& m) {
    NativeSourceGroundTransform inverse;
    for (size_t i = 0; i < 3; ++i) for (size_t j = 0; j < 3; ++j) inverse.Basis[i][j] = m.Basis[j][i];
    inverse.Position = Mul(Vector(inverse, m.Position), -1.0f);
    return inverse;
}
static Result Unsupported(Reason reason) { Result r; r.Reason = reason; return r; }
static bool Pack(float value, float scale, int16_t& out) {
    const auto scaled = std::trunc(value * scale);
    if (!std::isfinite(scaled) || scaled < -32768.0f || scaled > 32767.0f) return false;
    out = static_cast<int16_t>(scaled);
    return true;
}
static bool Vertex(V in, V& out) {
    for (size_t j = 0; j < 3; ++j) {
        int16_t packed;
        if (!Pack(in[j], 128.0f, packed)) return false;
        out[j] = static_cast<float>(packed) / 128.0f;
    }
    return true;
}
static bool Inside(V p, V min, V max) {
    return p[0]>=min[0] && p[1]>=min[1] && p[2]>=min[2] && p[0]<=max[0] && p[1]<=max[1] && p[2]<=max[2];
}
// ProcessLineBox 0x413100, reduced only by the already-forced local XY.
// minX/maxX/minY/maxY products are never negative for a vertical line.
static bool Box(V a, V b, V min, V max, float& depth, V& p, V& n) {
    float nearest = 1.0f;
    for (int side = 0; side < 2; ++side) {
        const auto z = side ? max[2] : min[2];
        const auto crossing = side ? (a[2]-z)*(b[2]-z) : (z-a[2])*(z-b[2]);
        if (crossing >= 0.0f) continue;
        const auto t = side ? (a[2]-z)/(a[2]-b[2]) : (z-a[2])/(b[2]-a[2]);
        const auto x = a[0]+(b[0]-a[0])*t;
        const auto y = a[1]+(b[1]-a[1])*t;
        if (x>min[0] && x<max[0] && y>min[1] && y<max[1] && t<nearest) {
            nearest = t; p = {x,y,z}; n = {0,0,side ? 1.0f : -1.0f};
        }
    }
    if (nearest >= depth) return false;
    depth = nearest;
    return true;
}
static bool TestLineBox(V a, V b, V min, V max) {
    // TestLineBox_DW 0x412C70: inclusive endpoint test then strict faces.
    if (Inside(a,min,max) || Inside(b,min,max)) return true;
    float t = 1.0f; V p{}, n{};
    return Box(a,b,min,max,t,p,n);
}
static bool Sphere(V a, V b, const NativeCollisionModel::Sphere& sphere, float& depth, V& p, V& n) {
    const auto d = Sub(b,a), m = Sub(a,sphere.Center);
    const auto c = Dot(m,m)-sphere.Radius*sphere.Radius;
    const auto quadraticA = Dot(d,d);
    if (c <= 0.0f) return false; // Default AllowLineOriginInsideSphere=false.
    const auto quadraticB = Dot(m,d);
    if (quadraticB > 0.0f) return false;
    const auto discriminant = quadraticB*quadraticB-quadraticA*c;
    if (discriminant < 0.0f || quadraticA <= 0.0f) return false;
    const auto t = (-quadraticB-std::sqrt(discriminant))/quadraticA;
    if (t > 1.0f || t >= depth) return false;
    depth = t; p = Add(a,Mul(d,t)); n = Normalized(Sub(p,sphere.Center));
    return true;
}
static bool Triangle(V a, V b, V va, V vb, V vc, const NativeSourceGroundPlane& plane, float& depth, V& p, V& n) {
    if (a[0]<std::min({va[0],vb[0],vc[0]}) || a[0]>std::max({va[0],vb[0],vc[0]}) ||
        a[1]<std::min({va[1],vb[1],vc[1]}) || a[1]>std::max({va[1],vb[1],vc[1]})) return false;
    for (size_t j = 0; j < 3; ++j) n[j] = static_cast<float>(plane.Normal[j])/4096.0f;
    const auto offset = static_cast<float>(plane.Distance)/128.0f;
    const auto origin = Dot(a,n)-offset;
    if (std::signbit(origin) == std::signbit(Dot(b,n)-offset)) return false;
    const auto magnitude = -Dot(Sub(b,a),n);
    // Nonfinite/parallel arithmetic is outside the verified valid domain, not
    // assigned an invented epsilon hit. Caller detects a nonfinite result.
    const auto t = origin/magnitude;
    if (t >= depth) return false;
    // common.h:196: NOT a+(b-a)*t. This grouping is observable in float32.
    p = Add(Mul(b,t),Mul(a,1.0f-t));
    const auto direction = static_cast<unsigned>(plane.Direction);
    const auto axis = direction/2;
    const auto u = (axis+1)%3, v = (axis+2)%3;
    if (direction%2 == 0) std::swap(vb,vc);
    const auto edge = [u,v](V x, V y, V relative) {
        return (y[u]-x[u])*relative[v]-(y[v]-x[v])*relative[u];
    };
    const auto pa = Sub(p,va);
    if (edge(va,vb,pa)>=0.0f && edge(va,vc,pa)<=0.0f && edge(vb,vc,Sub(p,vb))>=0.0f) {
        depth = t; return true;
    }
    return false;
}
}

NativeSourceGroundRequest NativeSourceGroundRequest::Generator(V stored) {
    NativeSourceGroundRequest request;
    request.Start = {stored[0],stored[1],stored[2]>-100.0f ? stored[2]+1.0f : 1000.0f};
    request.End = {stored[0],stored[1],-1000.0f};
    return request;
}

NativeSourceGroundTarget NativeSourceGround::BindInitialMetadata(const NativeCollisionInstance& instance,
    const NativeWorldEntityMetadata& metadata, bool initialClassStillEffective) {
    NativeSourceGroundTarget target;
    target.Identity = NativePlacementIdentity::From(instance.Placement);
    target.Model = instance.Model;
    target.Transform = {instance.Placement.Position,instance.Basis};
    if (metadata.Status!=NativeWorldInfoStatus::Ready || !metadata.Model || !metadata.Placement ||
        !(metadata.Placement->Identity==target.Identity) || !metadata.Placement->SourceInstanceType || !metadata.Placement->Area ||
        *metadata.Placement->Area!=(*metadata.Placement->SourceInstanceType&255u)) return target;
    const auto known = [](NativeWorldKnownBool b) {
        return b==NativeWorldKnownBool::True ? NativeSourceGroundKnown::Yes :
            b==NativeWorldKnownBool::False ? NativeSourceGroundKnown::No : NativeSourceGroundKnown::Unknown;
    };
    target.InWorld = known(metadata.Placement->InWorld);
    target.UsesCollision = known(metadata.Placement->UsesCollision);
    target.NormalSector = known(metadata.Placement->InNormalBuildingSector);
    target.BigBuilding = known(metadata.Placement->IsBigBuilding);
    if (!initialClassStillEffective || metadata.Model->ModelId!=instance.Placement.ModelId) return target;
    const auto classification = metadata.Model->InitialClass;
    if ((classification==NativeWorldInitialClass::Building || classification==NativeWorldInitialClass::AnimatedBuilding) &&
        metadata.Model->ObjectInfo==NativeWorldObjectAssignment::Unassigned) target.EffectiveClass = NativeSourceGroundClass::Building;
    if (classification==NativeWorldInitialClass::DummyObject && metadata.Model->ObjectInfo==NativeWorldObjectAssignment::Assigned)
        target.EffectiveClass = NativeSourceGroundClass::Other;
    target.VerifiedClassification = target.EffectiveClass!=NativeSourceGroundClass::Unknown;
    return target;
}

bool NativeSourceGround::CalculatePlane(V a, V b, V c, NativeSourceGroundPlane& out) {
    const auto normal = Normalized(Cross(Sub(c,a),Sub(b,a)));
    for (size_t j = 0; j < 3; ++j) if (!Pack(normal[j],4096.0f,out.Normal[j])) return false;
    if (!Pack(Dot(a,normal),128.0f,out.Distance)) return false;
    const auto nx = std::abs(normal[0]), ny = std::abs(normal[1]), nz = std::abs(normal[2]);
    const auto axis = nx>ny && nx>nz ? 0u : ny>nz ? 1u : 2u;
    out.Direction = static_cast<NativeSourceGroundPlane::Orientation>(axis*2+(normal[axis]<=0.0f ? 1u : 0u));
    return true;
}

NativeSourceGroundResult NativeSourceGround::ProcessVerticalLine(
    const NativeCollisionModel& model, const NativeSourceGroundTransform& transform, const NativeSourceGroundRequest& request) {
    if (!Finite(request.Start) || !Finite(request.End) || !std::isfinite(request.MaxFraction) ||
        request.MaxFraction<0.0f || request.MaxFraction>1.0f || !Finite(transform.Position) ||
        !std::ranges::all_of(transform.Basis,Finite)) return Unsupported(Reason::InvalidInput);
    if (!model.Unsupported.empty() || model.Version<1 || model.Version>4 || !Bounds(model.Min,model.Max) ||
        model.Spheres.size()>65535 || model.Boxes.size()>65535 || model.Faces.size()>65535)
        return Unsupported(Reason::UnsupportedModel);
    if (request.SeeThroughCheck && !request.VerifiedSeeThroughMaterials) return Unsupported(Reason::UnknownSurfaceTable);
    const auto inverse = Invert(transform);
    const auto a = Point(inverse,request.Start);
    auto b = Point(inverse,request.End);
    b[0] = a[0]; b[1] = a[1];
    if (!Finite(a) || !Finite(b)) return Unsupported(Reason::InvalidInput);
    Result result; result.Status = Status::Miss; result.Fraction = request.MaxFraction;
    if (!TestLineBox(a,b,model.Min,model.Max)) return result;
    const auto eligible = [&](NativeCollisionSurface surface) {
        return !request.SeeThroughCheck || (*request.VerifiedSeeThroughMaterials)[surface.Material];
    };
    const auto accept = [&](NativeCollisionPrimitive primitive, size_t index, NativeCollisionSurface surface, V p, V n) {
        result.Status = Status::Hit; result.Primitive = primitive; result.PrimitiveIndex = static_cast<uint32_t>(index);
        result.Point = p; result.Normal = n; result.Surface = surface; result.MaterialB = surface.Material;
        // V1 conversions use TSurface.light. V2+ sphere/box arrays are copied
        // directly as CColSurface: m_nLighting is byte2, not file byte3.
        // ColHelpers.h:74-139; FileLoader.cpp:639-685,730-763; ColSurface.h.
        result.LightingB = primitive==NativeCollisionPrimitive::Triangle || model.Version==1 ? surface.Light : surface.Brightness;
    };
    for (size_t i = 0; i < model.Spheres.size(); ++i) {
        const auto& sphere = model.Spheres[i]; V p{}, n{};
        if (!Finite(sphere.Center) || !std::isfinite(sphere.Radius) || sphere.Radius<0.0f) return Unsupported(Reason::UnsupportedModel);
        if (eligible(sphere.Surface) && Sphere(a,b,sphere,result.Fraction,p,n)) accept(NativeCollisionPrimitive::Sphere,i,sphere.Surface,p,n);
    }
    for (size_t i = 0; i < model.Boxes.size(); ++i) {
        const auto& box = model.Boxes[i]; V p{}, n{};
        if (!Bounds(box.Min,box.Max)) return Unsupported(Reason::UnsupportedModel);
        if (eligible(box.Surface) && Box(a,b,box.Min,box.Max,result.Fraction,p,n)) accept(NativeCollisionPrimitive::Box,i,box.Surface,p,n);
    }
    for (size_t i = 0; i < model.Faces.size(); ++i) {
        const auto& face = model.Faces[i]; V vertices[3], p{}, n{}; NativeSourceGroundPlane plane;
        for (size_t j = 0; j < 3; ++j) {
            if (face.Vertices[j]>65535 || face.Vertices[j]>=model.Vertices.size() || !Vertex(model.Vertices[face.Vertices[j]],vertices[j]))
                return Unsupported(Reason::UnsupportedModel);
        }
        if (!CalculatePlane(vertices[0],vertices[1],vertices[2],plane)) return Unsupported(Reason::UnrepresentablePlane);
        if (eligible(face.Surface) && Triangle(a,b,vertices[0],vertices[1],vertices[2],plane,result.Fraction,p,n)) {
            accept(NativeCollisionPrimitive::Triangle,i,face.Surface,p,n); result.Plane = plane;
        }
    }
    if (result.Status == Status::Hit) {
        result.Point = Point(transform,result.Point); result.Normal = Vector(transform,result.Normal);
        if (!Finite(result.Point) || !Finite(result.Normal) || !std::isfinite(result.Fraction)) return Unsupported(Reason::InvalidInput);
    }
    return result;
}

NativeSourceGroundResult NativeSourceGround::QueryBuildings(const NativeSourceGroundSnapshot& snapshot,
    V stored, uint64_t generation, uint64_t revision) {
    const auto stamp = [&](Result r) { r.WorldGeneration = snapshot.WorldGeneration; r.MetadataRevision = snapshot.MetadataRevision; return r; };
    if (generation!=snapshot.WorldGeneration || revision!=snapshot.MetadataRevision) return stamp(Unsupported(Reason::StaleWorld));
    if (!snapshot.CompleteNormalSector || !snapshot.MembershipAndOverridesVerified || !snapshot.Deduplicated)
        return stamp(Unsupported(Reason::UnknownCoverage));
    if (!Finite(stored)) return stamp(Unsupported(Reason::InvalidInput));
    for (size_t j = 0; j < 2; ++j) {
        if (!std::isfinite(snapshot.MinXY[j]) || !std::isfinite(snapshot.MaxXY[j]) || snapshot.MinXY[j]>=snapshot.MaxXY[j])
            return stamp(Unsupported(Reason::UnknownCoverage));
        if (stored[j]<snapshot.MinXY[j] || stored[j]>=snapshot.MaxXY[j]) return stamp(Unsupported(Reason::OutsideCoverage));
    }
    using Known = NativeSourceGroundKnown;
    Result best; best.Status = Status::Miss;
    bool ambiguous = false;
    const auto request = NativeSourceGroundRequest::Generator(stored);
    for (size_t i = 0; i < snapshot.Targets.size(); ++i) {
        const auto& target = snapshot.Targets[i];
        if (!target.VerifiedClassification || target.EffectiveClass==NativeSourceGroundClass::Unknown)
            return stamp(Unsupported(Reason::UnknownEntityMetadata));
        if (target.EffectiveClass!=NativeSourceGroundClass::Building) continue;
        if (target.InWorld==Known::No || target.UsesCollision==Known::No || target.NormalSector==Known::No ||
            target.BigBuilding==Known::Yes || target.Ignored==Known::Yes) continue;
        if (target.InWorld==Known::Unknown || target.UsesCollision==Known::Unknown || target.NormalSector==Known::Unknown ||
            target.BigBuilding==Known::Unknown || target.Ignored==Known::Unknown) return stamp(Unsupported(Reason::UnknownEntityMetadata));
        if (!target.SourceEffectiveTransformKnown) return stamp(Unsupported(Reason::UnknownTransform));
        if (!target.CollisionModelKnown) return stamp(Unsupported(Reason::UnknownCollisionModel));
        if (!target.Model) continue; // Only an authority-proven null model.
        auto hit = ProcessVerticalLine(*target.Model,target.Transform,request);
        if (hit.Status==Status::Unsupported) return stamp(hit);
        if (hit.Status!=Status::Hit || hit.Fraction>best.Fraction) continue;
        hit.TargetIndex = i;
        if (best.Status!=Status::Hit || hit.Fraction<best.Fraction) { best = hit; ambiguous = false; continue; }
        const auto oldOrder = snapshot.Targets[best.TargetIndex].SourceListOrdinal;
        if (!oldOrder || !target.SourceListOrdinal || oldOrder==target.SourceListOrdinal) ambiguous = true;
        else if (*target.SourceListOrdinal<*oldOrder) best = hit;
    }
    if (ambiguous) return stamp(Unsupported(Reason::UnknownSourceOrder));
    return stamp(best);
}
