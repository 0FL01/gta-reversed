#include "NativeSourcePedCollision.h"
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace {
std::size_t s_Checks = 0;
void Check(bool ok, const char* message) {
    ++s_Checks;
    if (!ok) { std::fprintf(stderr, "source-ped-collision FAIL: %s\n", message); std::exit(1); }
}
}
int main() {
    using Status = NativeSourcePedCollisionStatus;
    using Entity = NativeSourcePedCollisionEntity;
    NativeSourcePedCollisionInput input;
    NativeSourcePedCollisionShape shape;
    Check(NativeSourcePreparePedCollision(input, shape) == Status::Unsupported, "unknown target cannot manufacture collision eligibility");
    input.Other = Entity::Building;
    Check(NativeSourcePreparePedCollision(input, shape) == Status::Unsupported, "model owner must verify standard Ped1 rather than defaulting every ped");
    input.ModelIsStandardPed1 = true;
    Check(NativeSourcePreparePedCollision(input, shape) == Status::Ok && !shape.QueryEnabled && !shape.LineCount, "disabled source collision early-out");
    input.UsesCollision = true; input.TimeStep = 1;
    Check(NativeSourcePreparePedCollision(input, shape) == Status::Ok && shape.QueryEnabled && shape.LineCount == 1 &&
        shape.SetHasContacted && shape.LineEnd == NativeCollisionVector{0, 0, -1} && shape.Max[2] == 1 && shape.BoundRadius == 1,
        "ordinary source ped support line and temporary broadphase bound");
    Check(shape.Spheres[0].Center[2] == -0.2f && shape.Spheres[1].Center[2] == 0.2f && shape.Spheres[2].Center[2] == 0.6f &&
        shape.Spheres[0].Radius == 0.35f && shape.Spheres[2].Surface.Material == 62 && shape.Spheres[2].Surface.Piece == 2,
        "source TempColModels Ped1 three-sphere identity, no diagnostic capsule");
    Check(shape.LegSphereTop == 0.6f + 0.35f, "source preparation uses third sphere top, not arbitrary standing height");
    input.WasStanding = true;
    Check(NativeSourcePreparePedCollision(input, shape) == Status::Ok && shape.LineEnd[2] == -1.0f + -0.15f &&
        shape.Min[2] == shape.LineEnd[2] && shape.Max[2] == -shape.LineEnd[2] && shape.BoundRadius == shape.Max[2],
        "previously standing extends line using source timestep");
    input.TimeStep = 2;
    Check(NativeSourcePreparePedCollision(input, shape) == Status::Ok && shape.LineEnd[2] == -1.3f, "source doubled normalized timestep extension");
    input.Other = Entity::Ped;
    Check(NativeSourcePreparePedCollision(input, shape) == Status::Ok && !shape.LineCount && shape.Max[2] == 0.95f && shape.LegSphereTop == 0.94f,
        "ped-ped pair excludes support lines and retains original bounds");
    input.Other = Entity::Building; input.SkipLineCollision = true;
    Check(NativeSourcePreparePedCollision(input, shape) == Status::Ok && !shape.LineCount && !shape.SetHasContacted, "source skip-line flag");
    input.SkipLineCollision = false; input.ForceHitReturnFalse = true; input.UsesCollision = false;
    Check(NativeSourcePreparePedCollision(input, shape) == Status::Ok && shape.QueryEnabled && !shape.LineCount, "force-hit flag enables sphere query but suppresses lines");
    input.ForceHitReturnFalse = false; input.UsesCollision = true; input.IsStuck = true;
    Check(NativeSourcePreparePedCollision(input, shape) == Status::Ok && shape.ReturnAllContacts, "stuck against building requests all source contacts");
    input.Other = Entity::Object;
    Check(NativeSourcePreparePedCollision(input, shape) == Status::Ok && !shape.ReturnAllContacts, "ordinary object does not imply all-contacts mode");
    input.OtherDisableCollisionForce = true;
    Check(NativeSourcePreparePedCollision(input, shape) == Status::Ok && shape.ReturnAllContacts, "object disabled-force qualifies all-contacts mode");
    const auto retained = shape.LineEnd;
    input.CheckAboveHead = true;
    Check(NativeSourcePreparePedCollision(input, shape) == Status::Ok && shape.LineCount == 2 && shape.LineEnd == retained &&
        shape.HeadLineStart[2] == (0.6f + 0.35f) - float(double(-0.2f) - double(0.35f) + 1) &&
        shape.HeadLineEnd[2] == float(double(-0.2f) - double(0.35f) + 1) * 2 && shape.BoundRadius == shape.HeadLineEnd[2],
        "retail above-head second line uses first sphere bottom plus one, preserves support line");
    input.CheckAboveHead = false; input.TimeStep = std::numeric_limits<float>::quiet_NaN();
    Check(NativeSourcePreparePedCollision(input, shape) == Status::InvalidInput && shape.LineEnd == retained, "invalid timestep retains owned output");
    std::printf("source-ped-collision-ok checks=%zu retail-preparation-only no-world-route\n", s_Checks);
}
