#include "NativeSourcePedModelContact.h"
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace {
using Status = NativeSourcePedModelStatus;
std::size_t s_Checks = 0;
void Check(bool ok, const char* message) {
    ++s_Checks;
    if (!ok) { std::fprintf(stderr, "source-ped-model FAIL: %s\n", message); std::exit(1); }
}
NativeSourcePedCollisionShape Ped() {
    NativeSourcePedCollisionInput input;
    input.Other = NativeSourcePedCollisionEntity::Building;
    input.UsesCollision = input.ModelIsStandardPed1 = true;
    NativeSourcePedCollisionShape shape;
    Check(NativeSourcePreparePedCollision(input, shape) == NativeSourcePedCollisionStatus::Ok, "owned source standard ped preparation");
    return shape;
}
NativeCollisionModel Floor() {
    NativeCollisionModel model;
    model.Version = 2; model.Flags = 2;
    model.Min = {-4, -4, -1}; model.Max = {4, 4, 1}; model.BoundRadius = 6;
    model.Vertices = {{-4, -4, 0}, {4, -4, 0}, {0, 4, 0}};
    model.Faces.push_back({{0, 1, 2}, {10, 0, 0, 11}});
    return model;
}
void LinesAndOrder() {
    auto ped = Ped();
    auto model = Floor();
    NativeSourceGroundTransform a, b;
    a.Position = {0, 0, 0.75f};
    NativeSourcePedModelContacts out;
    out.Lines[0].Depth = 17;
    Check(NativeSourceProcessPedModel(ped, a, model, b, out) == Status::Ok && out.SphereCount == 0 && out.LineHits[0] &&
        out.LineFractions[0] == 0.75f && out.Lines[0].Point == NativeCollisionVector{0, 0, 0} && out.Lines[0].Depth == 17 &&
        out.Lines[0].SurfaceB == NativeSourceContactSurface{10, 0, 11}, "support line uses full candidate path and retains caller fields");
    const auto point = out.Lines[0];
    Check(NativeSourceProcessPedModel(ped, a, model, b, out) == Status::Ok && !out.LineHits[0] && out.Lines[0] == point &&
        out.LineFractions[0] == 0.75f, "strict equal fraction preserves previous entity hit");
    model.Faces.push_back({{0, 1, 2}, {20, 0, 0, 21}});
    model.Flags |= 8;
    model.FaceGroups = {{model.Min, model.Max, 1, 1}, {model.Min, model.Max, 0, 0}};
    out = {};
    Check(NativeSourceProcessPedModel(ped, a, model, b, out) == Status::Ok && out.CandidateTriangles == 2 &&
        out.Lines[0].SurfaceB.Material == 20, "authored group order, not face index sort, wins exact tie");
    b.Position = {8, 9, 10}; a.Position = {8, 9, 10.75f};
    b.Basis = {{{0, 1, 0}, {-1, 0, 0}, {0, 0, 1}}};
    out = {};
    Check(NativeSourceProcessPedModel(ped, a, model, b, out) == Status::Ok && out.LineHits[0] &&
        out.Lines[0].Point == b.Position && out.Lines[0].Normal == NativeCollisionVector{0, 0, -1}, "source inverse/product and world point transform");
    // A slanted source line is not replaced by a local-vertical generator query.
    ped.LineStart = {-1, 0, 0}; ped.LineEnd = {1, 0, -1}; out = {};
    Check(NativeSourceProcessPedModel(ped, a, model, b, out) == Status::Ok && out.LineHits[0] &&
        out.Lines[0].Point == NativeCollisionVector{8.5f, 9, 10}, "general support line retains both transformed endpoints");
    const auto retained = out;
    model.FaceGroups[0].Last = 100;
    Check(NativeSourceProcessPedModel(ped, a, model, b, out) == Status::InvalidInput && out == retained, "bad group range rejects atomically");
    model = Floor(); a.Position[0] = std::numeric_limits<float>::max();
    b.Basis[0][0] = std::numeric_limits<float>::max();
    Check(NativeSourceProcessPedModel(ped, a, model, b, out) == Status::Overflow && out == retained, "matrix overflow cannot become empty collision");
    NativeSourcePedCollisionInput input;
    input.Other = NativeSourcePedCollisionEntity::Building;
    input.UsesCollision = input.ModelIsStandardPed1 = input.CheckAboveHead = true;
    Check(NativeSourcePreparePedCollision(input, ped) == NativeSourcePedCollisionStatus::Ok && ped.LineCount == 2, "source head-line preparation");
    a = {}; b = {}; b.Position[2] = 0.75f; model = Floor(); out = {};
    out.LineFractions[0] = 0.4f;
    Check(NativeSourceProcessPedModel(ped, a, model, b, out) == Status::Ok && !out.LineHits[0] && out.LineHits[1] &&
        out.LineFractions[0] == 0.4f && out.Lines[1].Point[2] == 0.75f, "support and above-head lines keep independent fractions and results");
}
void ContactsAndLimits() {
    auto ped = Ped(); ped.LineCount = 0;
    auto model = Floor(); model.Faces.clear(); model.Vertices.clear();
    model.Boxes.push_back({{-0.1f, -0.1f, -1}, {0.1f, 0.1f, 1}, {7, 9, 12, 13}});
    NativeSourcePedModelContacts out;
    Check(NativeSourceProcessPedModel(ped, {}, model, {}, out) == Status::Ok && out.SphereCount == 3 &&
        out.Spheres[0].SurfaceB.Material == 7 && out.Spheres[0].SurfaceB.Lighting == 12 && out.Spheres[3].Depth == -1,
        "nearest one contact per source sphere with v2 brightness and trailing sentinel");
    model.Version = 1; out = {};
    Check(NativeSourceProcessPedModel(ped, {}, model, {}, out) == Status::Ok && out.Spheres[0].SurfaceB.Lighting == 13,
        "v1 box lighting follows legacy surface field rather than v2 packed brightness");
    model.Version = 2;
    model.Boxes.push_back(model.Boxes[0]); ped.ReturnAllContacts = true; out = {};
    Check(NativeSourceProcessPedModel(ped, {}, model, {}, out) == Status::Ok && out.SphereCount == 6,
        "return-all emits boxes per low-piece sphere rather than nearest only");
    model.Boxes.resize(70, model.Boxes[0]); out = {};
    Check(NativeSourceProcessPedModel(ped, {}, model, {}, out) == Status::Ok && out.CandidateBoxes == 64 && out.SphereCount == 31,
        "source box64 and contact31 limits preserve in-bounds trailing slot");
    model.Boxes.clear(); model.Spheres.push_back({{0.5f, 0, -0.2f}, 0.35f, {9, 4, 23, 24}}); out = {};
    Check(NativeSourceProcessPedModel(ped, {}, model, {}, out) == Status::Ok && out.CandidateSpheres == 1 && out.SphereCount > 0 &&
        out.Spheres[0].SurfaceB == NativeSourceContactSurface{9, 4, 23}, "dynamic sphere target retains piece and v2 brightness");
    model = Floor(); model.Faces.resize(601, model.Faces.front()); out = {};
    Check(NativeSourceProcessPedModel(ped, {}, model, {}, out) == Status::Ok && out.CandidateTriangles == 600,
        "ungrouped source triangle candidate cap");
    model.Flags |= 8; model.FaceGroups = {{model.Min, model.Max, 0, 599}, {model.Min, model.Max, 600, 600}};
    const auto retained = out;
    Check(NativeSourceProcessPedModel(ped, {}, model, {}, out) == Status::Unsupported && out == retained,
        "grouped source overrun path rejected, not memory corruption or silent omission");
    model = Floor(); model.Version = 1; model.Vertices[0][0] = -4.001f; out = {};
    Check(NativeSourceProcessPedModel(ped, {}, model, {}, out) == Status::Ok, "v1 vertices use source fixed-point truncation at binding");
    model.Version = 2;
    const auto old = out;
    Check(NativeSourceProcessPedModel(ped, {}, model, {}, out) == Status::Unsupported && out == old, "unquantized v2 vertices are not silently rebound");
    model = Floor(); model.Faces[0].Vertices[2] = 100;
    Check(NativeSourceProcessPedModel(ped, {}, model, {}, out) == Status::InvalidInput && out == old, "bad vertex index preserves all caller contact seeds");
}
void Real(const char* game) {
    NativeCollisionPopulation population;
    population.Models[3991] = {"gsfreeway7_lan", false}; population.IncludesStreamed = true;
    NativeCollisionPlacement placement;
    placement.ModelId = 3991; placement.Model = "gsfreeway7_lan"; placement.Ipl = "isolated-model-contact-fixture";
    population.Instances.push_back(placement);
    NativeCollisionAssets assets; std::string error;
    Check(assets.Load(game, population, error), error.c_str());
    const auto lookup = assets.LookupModel("gsfreeway7_lan");
    Check(lookup.Model && lookup.Model->FaceGroups.size() == 4, "real owned grouped collision model");
    auto ped = Ped();
    NativeSourceGroundTransform a;
    bool hit = false;
    for (const auto& face : lookup.Model->Faces) {
        a.Position = {};
        for (auto index : face.Vertices) for (std::size_t axis = 0; axis < 3; ++axis) a.Position[axis] += lookup.Model->Vertices[index][axis] / 3;
        a.Position[2] += 0.75f;
        NativeSourcePedModelContacts out;
        Check(NativeSourceProcessPedModel(ped, a, *lookup.Model, {}, out) == Status::Ok, "real source candidate and contact path remains supported");
        if (out.LineHits[0]) { hit = true; break; }
    }
    Check(hit, "prepared ped support line hits real grouped COL through model-pair traversal");
}
}
int main(int argc, char** argv) {
    if (argc > 2) return 2;
    LinesAndOrder(); ContactsAndLimits();
    if (argc == 2) Real(argv[1]);
    std::printf("source-ped-model-ok checks=%zu isolated-model-query not-world-support-owner\n", s_Checks);
}
