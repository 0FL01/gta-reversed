#include "NativeSourcePedResponse.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <string>

namespace {
using Status = NativeSourcePedResponseStatus;
std::size_t s_Checks{};
void Check(bool ok, const char* reason) {
    ++s_Checks;
    if (!ok) { std::fprintf(stderr, "source-ped-response FAIL: %s\n", reason); std::exit(1); }
}
NativeSourcePhysicalState Body(NativeCollisionVector position = {0, 0, 0.9f}) {
    NativeSourcePhysicalState body;
    body.Position = position; body.MoveSpeed = {0, 0, -0.1f}; body.Mass = 70; body.Elasticity = 0.05f;
    body.IsPed = body.DisableTurnForce = body.UsesCollision = body.ApplyGravity = body.Collidable = true;
    return body;
}
std::string Matrix() {
    std::string text;
    for (int row = 0; row < 6; ++row) {
        text += "ignored";
        for (int column = 0; column <= row; ++column) text += " " + std::to_string(1 + row * 2 + column);
        text += '\n';
    }
    return text;
}
std::string Material(const char* name, const char* group, int steep = 0) {
    std::string text = std::string(name) + " " + group + " 1 0 DEFAULT NONE";
    for (int field = 0; field < 29; ++field) text += " " + std::to_string(field == 7 ? steep : 0);
    return text + " NONE\n";
}
NativeSourceSurfaces Surfaces() {
    NativeSourceSurfaces surfaces; std::string error;
    const auto rows = Material("DEFAULT", "RUBBER") + Material("TARMAC", "HARD") +
        Material("STEEP_SLIDYGRASS", "LOOSE", 1) + Material("PED", "ROAD");
    Check(surfaces.LoadBytes(Matrix(), rows, error), error.c_str());
    return surfaces;
}
NativeSourcePedCollisionShape Shape(NativeSourcePedCollisionEntity other, bool above = false) {
    NativeSourcePedCollisionInput input;
    input.Other = other; input.UsesCollision = input.ModelIsStandardPed1 = true; input.CheckAboveHead = above;
    NativeSourcePedCollisionShape shape;
    Check(NativeSourcePreparePedCollision(input, shape) == NativeSourcePedCollisionStatus::Ok, "standard source ped shape");
    return shape;
}
NativeCollisionModel Floor(std::uint8_t material = 1) {
    NativeCollisionModel model;
    model.Version = 2; model.Flags = 2; model.Min = {-4, -4, -1}; model.Max = {4, 4, 1}; model.BoundRadius = 6;
    model.Vertices = {{-4, -4, 0}, {4, -4, 0}, {0, 4, 0}};
    model.Faces.push_back({{0, 1, 2}, {material, 0, 0, 11}});
    return model;
}
NativeCollisionModel Wall() {
    NativeCollisionModel model;
    model.Version = 2; model.Flags = 2; model.Min = {-0.1f, -2, -2}; model.Max = {0.1f, 2, 2};
    model.BoundRadius = 3;
    model.Boxes.push_back({model.Min, model.Max, {1, 0, 12, 13}});
    return model;
}
NativeCollisionModel PedModel() {
    NativeCollisionModel model;
    model.Version = 2; model.Flags = 2; model.Min = {-0.35f, -0.35f, -0.55f}; model.Max = {0.35f, 0.35f, 0.95f};
    model.BoundRadius = 1;
    for (const auto z : {-0.2f, 0.2f, 0.6f}) model.Spheres.push_back({{0, 0, z}, 0.35f, {62, 0, 0, 0}});
    return model;
}
NativeSourcePedModelContacts Query(const NativeSourcePedCollisionShape& shape, const NativeSourcePhysicalState& body,
    const NativeCollisionModel& model, NativeSourceGroundTransform other = {}) {
    NativeSourcePedModelContacts contacts;
    NativeSourceGroundTransform ped; ped.Position = body.Position;
    Check(NativeSourceProcessPedModel(shape, ped, model, other, contacts) == NativeSourcePedModelStatus::Ok,
          "owned model contact query");
    return contacts;
}
void Support() {
    auto surfaces = Surfaces(); auto shape = Shape(NativeSourcePedCollisionEntity::Building); auto floor = Floor();
    auto body = Body(); NativeSourcePedContactState state; NativeSourcePedEntityResponse response;
    auto contacts = Query(shape, body, floor);
    Check(contacts.LineHits[0] && contacts.LineFractions[0] == 0.9f, "source support-line witness");
    Check(NativeSourceResolvePedBuilding(body, state, shape, contacts, {}, surfaces, 1, response) == Status::Ok,
          "ordinary support response");
    Check(response.SupportAccepted && !response.BlockingCollision && response.ContactCount == 0 && response.SupportFraction == 0.9f,
          "support is not a blocking sphere contact");
    Check(body.Position[2] == 1 && body.MoveSpeed[2] == 0 && state.IsStanding && state.SurfaceTouched == 1,
          "source line correction and standing state");
    Check(state.ContactNormal == contacts.Lines[0].Normal && state.ContactOffset == contacts.Lines[0].Point,
          "support provenance retained");
    Check(NativeSourceBeginPedCollisionCheck(body, state) == Status::Ok && !state.IsStanding && state.WasStanding,
          "world scan clears current standing once");
    const auto second = Query(shape, body, floor);
    Check(NativeSourceResolvePedBuilding(body, state, shape, second, {}, surfaces, 1, response) == Status::Ok && !response.SupportAccepted,
          "strict .95 support range does not reacquire exact-height line");
    NativeSourcePedCollisionInput extendedInput; extendedInput.Other = NativeSourcePedCollisionEntity::Building;
    extendedInput.UsesCollision = extendedInput.ModelIsStandardPed1 = extendedInput.WasStanding = true; extendedInput.TimeStep = 1;
    NativeSourcePedCollisionShape extendedShape;
    Check(NativeSourcePreparePedCollision(extendedInput, extendedShape) == NativeSourcePedCollisionStatus::Ok,
          "prior-standing support-line extension");
    const auto extended = Query(extendedShape, body, floor);
    Check(NativeSourceResolvePedBuilding(body, state, extendedShape, extended, {}, surfaces, 1, response) == Status::Ok &&
          response.SupportAccepted && state.IsStanding, "equal-height support is accepted through the extended source line");
    body.Position[2] = 0.9f; body.MoveSpeed[2] = -0.1f; contacts = Query(shape, body, floor);
    Check(NativeSourceResolvePedBuilding(body, state, shape, contacts, {}, surfaces, 1, response) == Status::Ok &&
          response.SupportAccepted && state.IsStanding, "falling correction reacquires prior support");
    auto steep = Floor(17); body = Body(); state = {}; contacts = Query(shape, body, steep);
    Check(NativeSourceResolvePedBuilding(body, state, shape, contacts, {}, surfaces, 1, response) == Status::Ok &&
          state.HitSteepSlope, "source steep-surface flag reaches ped state");
    body = Body(); state.IsStanding = false; state.HitSteepSlope = true; contacts = Query(shape, body, floor);
    Check(NativeSourceResolvePedBuilding(body, state, shape, contacts, {}, surfaces, 1, response) == Status::Ok &&
          state.HitSteepSlope, "ordinary support does not clear an earlier steep-slope observation");
    NativeSourcePedModelContacts threshold; threshold.LineHits[0] = true; threshold.LineFractions[0] = 0.95f;
    threshold.Lines[0] = contacts.Lines[0]; body = Body(); state = {};
    Check(NativeSourceResolvePedBuilding(body, state, shape, threshold, {}, surfaces, 1, response) == Status::Ok &&
          !response.SupportAccepted && body.Position[2] == 0.9f, "support fraction threshold is strict");
}
void Walls() {
    auto surfaces = Surfaces(); auto shape = Shape(NativeSourcePedCollisionEntity::Building); auto wall = Wall();
    auto body = Body({0.3f, 0, 1}); body.MoveSpeed = {-1, 1, 0};
    const auto contacts = Query(shape, body, wall);
    Check(contacts.SphereCount == 3 && !contacts.LineHits[0], "source wall contacts without support");
    NativeSourcePedContactState state; NativeSourcePedEntityResponse response;
    Check(NativeSourceResolvePedBuilding(body, state, shape, contacts, {}, surfaces, 1, response) == Status::Ok,
          "wall response");
    Check(response.BlockingCollision && response.ContactCount == 3 && response.AppliedCount == 1 && response.ReportCount == 1,
          "wall reports the first effective source sphere response");
    Check(body.MoveSpeed[0] == 0 && body.MoveSpeed[1] == 1 && body.HasHitWall && response.FrictionCount > 0 && state.HasContacted,
          "wall impulse and source material friction accumulation");
    Check(body.FrictionMoveSpeed[1] < 0 && body.Position == NativeCollisionVector{0.3f, 0, 1},
          "response leaves matrix rollback to driver");
    const auto retainedBody = body; const auto retainedState = state; const auto retainedOut = response;
    auto bad = contacts; bad.Spheres[0].SurfaceB.Material = 255;
    Check(NativeSourceResolvePedBuilding(body, state, shape, bad, {}, surfaces, 1, response) == Status::SurfaceUnavailable &&
          body == retainedBody && state == retainedState && response == retainedOut, "unknown material rejects atomically");
    bad = contacts; bad.Spheres[0].SurfaceA.Piece = 3;
    Check(NativeSourceResolvePedBuilding(body, state, shape, bad, {}, surfaces, 1, response) == Status::InvalidInput &&
          body == retainedBody, "blocked-position pieces are outside ordinary slice");
    bad = contacts; bad.Spheres[0].Normal[2] = -0.9f;
    Check(NativeSourceResolvePedBuilding(body, state, shape, bad, {}, surfaces, 1, response) == Status::Unsupported &&
          body == retainedBody, "overhead sphere response is not silently flattened");
    auto falling = retainedBody; falling.Position[2] = 0.9f; falling.MoveSpeed[2] = -0.3f;
    const auto floorContacts = Query(shape, falling, Floor());
    Check(NativeSourceResolvePedBuilding(falling, state, shape, floorContacts, {}, surfaces, 1, response) == Status::Unsupported,
          "fall damage/task branch remains explicit");
    auto attached = retainedBody; attached.Attached = true;
    Check(NativeSourceResolvePedBuilding(attached, state, shape, contacts, {}, surfaces, 1, response) == Status::Unsupported,
          "attached support remains explicit");
    auto specialState = state; specialState.HeightLimit = 1;
    Check(NativeSourceResolvePedBuilding(body, specialState, shape, contacts, {}, surfaces, 1, response) == Status::Unsupported,
          "qualified height-limit correction is not silently omitted");
    specialState = state; specialState.HeadStuckInCollision = true;
    Check(NativeSourceResolvePedBuilding(body, specialState, shape, contacts, {}, surfaces, 1, response) == Status::Unsupported,
          "pre-existing head-stuck branch remains explicit");
    auto above = Shape(NativeSourcePedCollisionEntity::Building, true); auto overhead = contacts; overhead.LineHits[1] = true;
    overhead.LineFractions[1] = 0.5f; overhead.Lines[1] = contacts.Spheres[0];
    Check(NativeSourceResolvePedBuilding(body, state, above, overhead, {}, surfaces, 1, response) == Status::Unsupported,
          "above-head line remains explicit");
}
void Pair() {
    auto surfaces = Surfaces(); auto shape = Shape(NativeSourcePedCollisionEntity::Ped); auto model = PedModel();
    auto first = Body({0, 0, 0}); first.MoveSpeed = {1, 0.5f, 0}; first.PushOtherPeds = true;
    auto second = Body({0.5f, 0, 0}); second.MoveSpeed = {};
    NativeSourceGroundTransform other; other.Position = second.Position;
    const auto contacts = Query(shape, first, model, other);
    Check(shape.LineCount == 0 && contacts.SphereCount > 0, "dynamic ped query suppresses support lines");
    NativeSourcePedContactState state; NativeSourcePedEntityResponse response;
    Check(NativeSourceResolvePedPair(first, state, second, shape, contacts, surfaces, 1, response) == Status::Ok,
          "dynamic ped source response");
    Check(response.BlockingCollision && response.AppliedCount > 0 && response.ReportCount >= 2,
          "dynamic pair reports ordered two-body collision");
    Check(first.MoveSpeed[0] < 1 && second.MoveSpeed[0] > 0 && first.Position == NativeCollisionVector{0, 0, 0} &&
          second.Position == NativeCollisionVector{0.5f, 0, 0}, "pair changes velocity without hidden integration");
    const auto oldFirst = first, oldSecond = second; const auto oldState = state; const auto oldOut = response;
    second.SafePosition = true;
    Check(NativeSourceResolvePedPair(first, state, second, shape, contacts, surfaces, 1, response) == Status::Unsupported &&
          first == oldFirst && state == oldState && response == oldOut, "safe-position dynamic path is explicit");
    second = oldSecond; auto lines = contacts; lines.LineHits[0] = true;
    Check(NativeSourceResolvePedPair(first, state, second, shape, lines, surfaces, 1, response) == Status::Unsupported,
          "dynamic support line is not accepted");
}
void Atomic() {
    auto body = Body(); NativeSourcePedContactState state; state.IsStanding = true;
    const auto oldBody = body;
    const auto oldState = state;
    body.Mass = 0;
    const auto invalidBody = body;
    Check(NativeSourceBeginPedCollisionCheck(body, state) == Status::InvalidInput && body == invalidBody &&
          state == oldState, "invalid world-scan prefix retention");
    body = oldBody; body.Attached = true;
    Check(NativeSourceBeginPedCollisionCheck(body, state) == Status::Ok && state.IsStanding && !state.WasStanding,
          "attached scan does not clear standing");
}
void Real(const char* game) {
    NativeSourceSurfaces surfaces; std::string error;
    Check(surfaces.Load(game, error), error.c_str());
    NativeCollisionPopulation population; population.Models[3991] = {"gsfreeway7_lan", false}; population.IncludesStreamed = true;
    NativeCollisionPlacement placement; placement.ModelId = 3991; placement.Model = "gsfreeway7_lan"; placement.Ipl = "isolated-response-fixture";
    population.Instances.push_back(placement);
    NativeCollisionAssets assets; Check(assets.Load(game, population, error), error.c_str());
    const auto lookup = assets.LookupModel("gsfreeway7_lan");
    Check(lookup.Model && lookup.Model->Faces.size() == 122, "real grouped source model");
    auto shape = Shape(NativeSourcePedCollisionEntity::Building);
    bool supported = false;
    for (const auto& face : lookup.Model->Faces) {
        NativeSourceGroundTransform ped;
        for (const auto index : face.Vertices) for (std::size_t axis = 0; axis < 3; ++axis)
            ped.Position[axis] += lookup.Model->Vertices[index][axis] / 3;
        ped.Position[2] += 0.9f;
        auto body = Body(ped.Position); NativeSourcePedModelContacts contacts;
        Check(NativeSourceProcessPedModel(shape, ped, *lookup.Model, {}, contacts) == NativeSourcePedModelStatus::Ok,
              "real response candidate query");
        if (!contacts.LineHits[0] || contacts.SphereCount) continue;
        NativeSourcePedContactState state; NativeSourcePedEntityResponse response;
        const auto status = NativeSourceResolvePedBuilding(body, state, shape, contacts, {}, surfaces, 1, response);
        Check(status == Status::Ok, "real source support response status");
        if (response.SupportAccepted) {
            Check(state.SurfaceTouched == contacts.Lines[0].SurfaceB.Material && state.ContactNormal == contacts.Lines[0].Normal,
                  "real support material and normal provenance");
            supported = true; break;
        }
    }
    Check(supported, "real grouped COL reaches ordinary support response");
}
}

int main(int argc, char** argv) {
    if (argc > 2) return 2;
    Support(); Walls(); Pair(); Atomic();
    if (argc == 2) Real(argv[1]);
    std::printf("source-ped-response-ok checks=%zu support-wall-pair response-only-not-world-driver\n", s_Checks);
}
