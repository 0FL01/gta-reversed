#include "NativeSourceModelContact.h"

#include <cstdio>
#include <cstdlib>
#include <limits>

namespace {
std::size_t s_Checks{};
void Check(bool ok, const char* reason) {
    ++s_Checks;
    if (!ok) { std::fprintf(stderr, "source-model-contact FAIL: %s\n", reason); std::exit(1); }
}
NativeCollisionModel Model() {
    NativeCollisionModel model;
    model.Version = 2; model.Flags = 2; model.Min = {-2,-2,-2}; model.Max = {2,2,2};
    model.BoundRadius = 3; return model;
}
NativeCollisionModel Floor() {
    auto model = Model(); model.Vertices = {{-2,-2,0},{2,-2,0},{2,2,0},{-2,2,0}};
    model.Faces = {{{0,1,2},{7,0,0,11}},{{0,2,3},{8,0,0,12}}}; return model;
}
}
int main() {
    using Status = NativeSourceModelStatus;
    auto a = Model(); a.Spheres.push_back({{0,0,0},.5f,{63,1,13,14}});
    auto b = Floor();
    NativeSourceGroundTransform transformA; transformA.Position[2] = .25f;
    NativeSourceModelContacts out;
    Check(NativeSourceProcessModels(a,transformA,{},0,b,{},false,out)==Status::Ok,"sphere/triangle query");
    Check(out.SphereCount==1&&out.Spheres[0].SurfaceA==NativeSourceContactSurface{63,1,13}&&
        out.Spheres[0].SurfaceB.Material==7&&out.Spheres[0].Normal[2]>0,"forward surface and normal provenance");
    const auto forward=out;
    transformA.Position[2]=3;
    Check(NativeSourceProcessModels(a,transformA,{},0,b,{},false,out)==Status::Ok&&out.SphereCount==0,
        "successful miss clears counts");
    out=forward; transformA.Position[2]=.75f;
    const std::array lines{NativeSourceModelLine{{0,0,0},{0,0,-1}}};
    Check(NativeSourceProcessModels(a,transformA,lines,0,b,{},false,out)==Status::Ok&&out.LineCount==1&&
        out.LineHits[0]&&out.LineFractions[0]==.75f&&out.Lines[0].Point==NativeCollisionVector{0,0,0},
        "general source line shares candidates and preserves fraction");
    auto reverseA=Floor(), reverseB=Model(); reverseB.Spheres.push_back({{0,0,.25f},.5f,{9,6,22,23}});
    out={};
    Check(NativeSourceProcessModels(reverseA,{}, {},0,reverseB,{},false,out)==Status::Ok&&out.SphereCount==1&&
        out.Spheres[0].SurfaceA.Material==7&&out.Spheres[0].SurfaceB.Material==9&&out.Spheres[0].Normal[2]<0,
        "reverse B sphere against A triangle uses source swap and normal");
    auto reverseBoxA=Model(); reverseBoxA.Boxes.push_back({{-1,-1,-.1f},{1,1,.1f},{4,3,17,18}}); out={};
    Check(NativeSourceProcessModels(reverseBoxA,{}, {},0,reverseB,{},false,out)==Status::Ok&&out.SphereCount==1&&
        out.Spheres[0].SurfaceA==NativeSourceContactSurface{9,6,22}&&
        out.Spheres[0].SurfaceB==NativeSourceContactSurface{4,3,17}&&out.Spheres[0].Normal[2]<0,
        "reverse B sphere against A box preserves source asymmetric surface swap");
    auto boxes=Model(); boxes.Boxes.resize(65,{{-.1f,-.1f,-.1f},{.1f,.1f,.1f},{4,0,0,1}}); out={};
    Check(NativeSourceProcessModels(a,{}, {},0,boxes,{},true,out)==Status::Ok&&out.CandidateBoxesB==64&&out.SphereCount==31,
        "source box64/contact31 limits");
    auto grouped=Floor(); grouped.Flags|=8; grouped.FaceGroups={{{-2,-2,-1},{2,2,1},0,0},{{-2,-2,-1},{2,2,1},1,1}};
    out={}; transformA.Position[2]=.25f;
    Check(NativeSourceProcessModels(a,transformA,{},0,grouped,{},false,out)==Status::Ok&&out.CandidateTrianglesB==2,
        "authored face-group traversal");
    const auto retained=out; grouped.FaceGroups[0].Last=99;
    Check(NativeSourceProcessModels(a,transformA,{},0,grouped,{},false,out)==Status::InvalidInput&&out==retained,
        "invalid grouped model retains complete output");
    transformA.Position[0]=std::numeric_limits<float>::max(); NativeSourceGroundTransform target;
    target.Basis[0][0]=std::numeric_limits<float>::max();
    Check(NativeSourceProcessModels(a,transformA,{},0,b,target,false,out)==Status::Overflow&&out==retained,
        "transform overflow cannot become collision miss");
    Check(NativeSourceProcessModels(a,{}, {},a.BoundRadius-.1f,b,{},false,out)==Status::InvalidInput&&out==retained,
        "effective bound cannot shrink authored collision");
    std::printf("source-model-contact-ok checks=%zu two-sided sphere-box-triangle-line limits=128,64,600,31\n",s_Checks);
}
