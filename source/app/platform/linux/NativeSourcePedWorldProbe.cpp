#include "NativeSourcePedWorld.h"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {
using Status = NativeSourcePedWorldStatus;
std::size_t s_Checks{};
void Check(bool ok, const char* reason) {
    ++s_Checks;
    if (!ok) { std::fprintf(stderr, "source-ped-world FAIL: %s\n", reason); std::exit(1); }
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
std::string Material(const char* name, const char* group) {
    std::string text = std::string(name) + " " + group + " 1 0 DEFAULT NONE";
    for (int field = 0; field < 29; ++field) text += " 0";
    return text + " NONE\n";
}
NativeSourceSurfaces SyntheticSurfaces() {
    NativeSourceSurfaces surfaces; std::string error;
    Check(surfaces.LoadBytes(Matrix(), Material("TARMAC", "HARD") + Material("PED", "ROAD"), error), error.c_str());
    return surfaces;
}
std::shared_ptr<NativeCollisionModel> Floor(float minX, float maxX, float z) {
    auto model = std::make_shared<NativeCollisionModel>();
    model->Name = "controlled-floor"; model->Version = 2; model->Flags = 2;
    model->Min = {minX, -4, z - 0.125f}; model->Max = {maxX, 4, z + 0.125f}; model->BoundRadius = 6;
    model->Vertices = {{minX, -4, z}, {maxX, -4, z}, {maxX, 4, z}, {minX, 4, z}};
    model->Faces = {{{0, 1, 2}, {1, 0, 0, 10}}, {{0, 2, 3}, {1, 0, 0, 10}}};
    return model;
}
std::shared_ptr<NativeCollisionModel> Wall(float x) {
    auto model = std::make_shared<NativeCollisionModel>();
    model->Name = "controlled-wall"; model->Version = 2; model->Flags = 2;
    model->Min = {x - 0.125f, -4, -1}; model->Max = {x + 0.125f, 4, 3}; model->BoundRadius = 5;
    model->Boxes.push_back({model->Min, model->Max, {1, 0, 0, 10}});
    return model;
}
NativeSourceGroundTarget Target(std::shared_ptr<const NativeCollisionModel> model, std::uint64_t ordinal, std::uint32_t record) {
    NativeSourceGroundTarget target;
    target.Identity = {"controlled-world", model->Name, record, int(record), false};
    target.Model = std::move(model); target.EffectiveClass = NativeSourceGroundClass::Building;
    target.InWorld = target.UsesCollision = target.NormalSector = NativeSourceGroundKnown::Yes;
    target.BigBuilding = target.Ignored = NativeSourceGroundKnown::No;
    target.VerifiedClassification = target.SourceEffectiveTransformKnown = target.CollisionModelKnown = true;
    target.SourceListOrdinal = ordinal;
    return target;
}
NativeSourceGroundSnapshot Snapshot(std::vector<NativeSourceGroundTarget> targets) {
    NativeSourceGroundSnapshot snapshot;
    snapshot.Targets = std::move(targets); snapshot.WorldGeneration = 11; snapshot.MetadataRevision = 7;
    snapshot.CompleteNormalSector = snapshot.MembershipAndOverridesVerified = snapshot.Deduplicated = true;
    snapshot.MinXY = {-10, -10}; snapshot.MaxXY = {10, 10};
    return snapshot;
}
NativeSourcePedWorldPed Ped(std::uint64_t id, NativeCollisionVector position, NativeCollisionVector speed = {}) {
    NativeSourcePedWorldPed ped;
    ped.Identity = id; ped.HasPlayerData = id == 1; ped.Physical.Position = position; ped.Physical.MoveSpeed = speed;
    ped.Physical.Mass = 70; ped.Physical.Elasticity = 0.05f;
    ped.Physical.IsPed = ped.Physical.DisableTurnForce = ped.Physical.ApplyGravity = ped.Physical.UsesCollision = ped.Physical.Collidable = true;
    return ped;
}
void Ownership() {
    NativeSourcePedWorld world; std::string error;
    auto valid = Snapshot({Target(Floor(-4, 4, 0), 1, 1)});
    auto incomplete = valid; incomplete.CompleteNormalSector = false;
    Check(!world.Load(incomplete, error) && !error.empty() && world.StaticCount() == 0, "incomplete authority rejects before publication");
    auto unknown = valid; unknown.Targets[0].UsesCollision = NativeSourceGroundKnown::Unknown;
    Check(!world.Load(unknown, error) && world.StaticCount() == 0, "unknown target authority is not defaulted");
    auto unordered = valid; unordered.Targets.push_back(Target(Floor(-4, 4, 1), 1, 2));
    Check(!world.Load(unordered, error) && world.StaticCount() == 0, "source list order must be strict");
    Check(world.Load(valid, error) && error.empty() && world.StaticCount() == 1 && world.WorldGeneration() == 11,
          "qualified normal-sector publication");
    Check(world.AddPed(Ped(1, {0, 0, 0.9f})) == Status::Ok && world.PedCount() == 1, "owned player insertion");
    Check(world.AddPed(Ped(1, {0, 0, 0.9f})) == Status::DuplicatePed && world.PedCount() == 1, "duplicate player retained");
    auto bad = Ped(2, {0, 0, 1}); bad.Physical.Mass = 0;
    Check(world.AddPed(bad) == Status::InvalidInput && world.PedCount() == 1, "invalid dynamic body retained");
    NativeSourcePedWorldPed read;
    Check(world.Ped(999, read) == Status::PedNotFound && world.RemovePed(999) == Status::PedNotFound, "missing dynamic identity");
    const auto previous = valid; auto badReload = valid; badReload.MinXY[0] = badReload.MaxXY[0];
    Check(!world.Load(badReload, error) && world.StaticCount() == previous.Targets.size() && world.PedCount() == 1,
          "failed world reload retains statics and peds");
    Check(world.RemovePed(1) == Status::Ok && world.PedCount() == 0, "owned ped removal");
    Check(world.AddPed(Ped(1, {0, 0, 0.9f})) == Status::Ok, "control sequence fixture");
    NativeSourcePedWorldStep step;
    const auto surfaces = SyntheticSurfaces();
    Check(world.StepCollision(1, 1, surfaces, step) == Status::ControlRequired, "collision cannot skip source control prefix");
    Check(world.BeginControl(1, 1) == Status::Ok && world.BeginControl(1, 1) == Status::ControlOutstanding,
          "one control prefix per collision tick");
    Check(world.StepCollision(1, 1, surfaces, step) == Status::Ok && world.StepCollision(1, 1, surfaces, step) == Status::ControlRequired,
          "completed collision consumes control sequence");
}
void ControlledRoute() {
    auto surfaces = SyntheticSurfaces(); NativeSourcePedWorld world; std::string error;
    auto snapshot = Snapshot({Target(Floor(-4, 0, 0), 1, 1), Target(Floor(0, 4, 0.25f), 2, 2), Target(Wall(2), 3, 3)});
    Check(world.Load(snapshot, error), error.c_str());
    auto routePlayer = Ped(1, {-0.75f, 0, 0.9f}, {0.2f, 0, 0});
    routePlayer.Physical.PushOtherPeds = true;
    Check(world.AddPed(routePlayer) == Status::Ok, "route player");
    bool curb = false;
    for (int frame = 0; frame < 24 && !curb; ++frame) {
        Check(world.BeginControl(1, 1) == Status::Ok, "source control prefix before curb collision");
        NativeSourcePedWorldStep step;
        Check(world.StepCollision(1, 1, surfaces, step) == Status::Ok, "curb collision step");
        NativeSourcePedWorldPed player; Check(world.Ped(1, player) == Status::Ok, "curb player snapshot");
        curb = player.Physical.Position[0] > 0 && player.Physical.Position[2] == 1.25f && step.Supports > 0;
    }
    Check(curb, "controlled loaded curb raises ped through source support line");
    NativeSourcePedWorldPed player; Check(world.Ped(1, player) == Status::Ok, "post-curb snapshot");
    const float beforePairX = player.Physical.Position[0];
    Check(world.AddPed(Ped(2, {player.Physical.Position[0] + 0.65f, 0, 1.25f})) == Status::Ok, "dynamic route ped");
    bool pair = false;
    for (int frame = 0; frame < 8 && !pair; ++frame) {
        Check(world.BeginControl(1, 1) == Status::Ok, "source control prefix before pair");
        NativeSourcePedWorldStep step;
        Check(world.StepCollision(1, 1, surfaces, step) == Status::Ok, "dynamic collision step");
        pair = step.Blocked && step.DynamicQueries > 0 && step.Applied > 0 && step.Reports >= 2;
    }
    Check(pair, "controlled dynamic ped contact keeps pair response in world driver");
    NativeSourcePedWorldPed other; Check(world.Ped(2, other) == Status::Ok && other.Physical.MoveSpeed[0] > 0,
          "dynamic contact updates other owned velocity");
    Check(world.Ped(1, player) == Status::Ok && player.Physical.Position[0] >= beforePairX,
          "dynamic block rolls back only the attempted sample, not route history");
    Check(world.RemovePed(2) == Status::Ok, "dynamic route removal");
    bool wall = false;
    for (int frame = 0; frame < 80 && !wall; ++frame) {
        Check(world.BeginControl(1, 1) == Status::Ok, "source control prefix before wall");
        NativeSourcePedWorldStep step;
        Check(world.StepCollision(1, 1, surfaces, step) == Status::Ok, "wall collision step");
        wall = step.Blocked && step.StaticQueries > 0 && step.Contacts > 0 && step.Applied > 0;
    }
    Check(wall, "controlled loaded wall contact blocks and rolls back matrix sample");
    Check(world.Ped(1, player) == Status::Ok && player.Physical.Position[0] < 1.875f && player.Physical.MoveSpeed[0] == 0 &&
          player.Physical.HasHitWall,
          "wall cannot be tunneled through and incoming speed is removed");
    Check(!player.Contact.HasContacted && player.Physical.FrictionMoveSpeed[1] == 0,
          "zero-tangent wall contact does not invent contacted friction state");
}
void FailureAtomic() {
    auto surfaces = SyntheticSurfaces(); NativeSourcePedWorld world; std::string error;
    auto snapshot = Snapshot({Target(Floor(-4, 4, 0), 1, 1)}); snapshot.MinXY = {-2, -2}; snapshot.MaxXY = {2, 2};
    Check(world.Load(snapshot, error) && world.AddPed(Ped(1, {1.5f, 0, 0.9f}, {1, 0, 0})) == Status::Ok, "coverage fixture");
    NativeSourcePedWorldStep out; out.StaticQueries = 99; const auto oldOut = out;
    Check(world.BeginControl(1, 1) == Status::Ok, "coverage control prefix");
    NativeSourcePedWorldPed before; Check(world.Ped(1, before) == Status::Ok && before.ControlPrepared, "coverage prepared snapshot");
    Check(world.StepCollision(1, 1, surfaces, out) == Status::OutsideCoverage && out == oldOut,
          "outside complete sector rejects whole tick");
    NativeSourcePedWorldPed after; Check(world.Ped(1, after) == Status::Ok && after == before, "coverage rejection retains world");
    Check(world.StepCollision(1, -1, surfaces, out) == Status::InvalidInput && out == oldOut, "bad tick retains output");
    NativeSourceSurfaces unloaded;
    auto retryPed = before; retryPed.Physical.Position = {0, 0, 0.9f}; retryPed.ControlPrepared = false;
    NativeSourcePedWorld retry; Check(retry.Load(Snapshot({Target(Floor(-4, 4, 0), 1, 1)}), error) && retry.AddPed(retryPed) == Status::Ok,
          "surface failure fixture");
    Check(retry.BeginControl(1, 1) == Status::Ok, "surface failure control prefix");
    Check(retry.Ped(1, before) == Status::Ok && before.ControlPrepared, "surface failure prepared snapshot");
    Check(retry.StepCollision(1, 1, unloaded, out) == Status::SurfaceUnavailable && out == oldOut,
          "unloaded material table cannot produce contact");
    Check(retry.Ped(1, after) == Status::Ok && after == before, "surface failure retains complete ped owner");
}
void Real(const char* game) {
    NativeSourceSurfaces surfaces; std::string error;
    Check(surfaces.Load(game, error), error.c_str());
    NativeCollisionPopulation population; population.Models[3991] = {"gsfreeway7_lan", false}; population.IncludesStreamed = true;
    NativeCollisionPlacement placement; placement.ModelId = 3991; placement.Model = "gsfreeway7_lan"; placement.Ipl = "controlled-real-world";
    population.Instances.push_back(placement);
    NativeCollisionAssets assets; Check(assets.Load(game, population, error), error.c_str());
    const auto lookup = assets.LookupModel("gsfreeway7_lan");
    Check(lookup.Model && lookup.Model->Faces.size() == 122, "real world route model loaded");
    NativeCollisionVector witness{}; bool found = false;
    NativeSourcePedCollisionInput input; input.Other = NativeSourcePedCollisionEntity::Building;
    input.UsesCollision = input.ModelIsStandardPed1 = true;
    NativeSourcePedCollisionShape shape; Check(NativeSourcePreparePedCollision(input, shape) == NativeSourcePedCollisionStatus::Ok,
          "real world route ped shape");
    for (const auto& face : lookup.Model->Faces) {
        NativeSourceGroundTransform ped;
        for (const auto index : face.Vertices) for (std::size_t axis = 0; axis < 3; ++axis)
            ped.Position[axis] += lookup.Model->Vertices[index][axis] / 3;
        ped.Position[2] += 0.9f;
        NativeSourcePedModelContacts contacts;
        Check(NativeSourceProcessPedModel(shape, ped, *lookup.Model, {}, contacts) == NativeSourcePedModelStatus::Ok,
              "real loaded model witness scan");
        if (contacts.LineHits[0] && contacts.SphereCount == 0) { witness = ped.Position; found = true; break; }
    }
    Check(found, "real grouped COL has isolated support witness");
    auto target = Target(lookup.Model, 1, 3991); target.Identity = NativePlacementIdentity::From(placement);
    auto snapshot = Snapshot({std::move(target)}); snapshot.WorldGeneration = 31; snapshot.MetadataRevision = 23;
    snapshot.MinXY = {witness[0] - 10, witness[1] - 10}; snapshot.MaxXY = {witness[0] + 10, witness[1] + 10};
    NativeSourcePedWorld world; Check(world.Load(snapshot, error) && world.StaticCount() == 1, "real loaded collision publication");
    Check(world.AddPed(Ped(1, witness)) == Status::Ok && world.BeginControl(1, 1) == Status::Ok, "real route control prefix");
    NativeSourcePedWorldStep step; Check(world.StepCollision(1, 1, surfaces, step) == Status::Ok && step.Supports > 0 && !step.Blocked,
          "real loaded support survives substep driver");
    NativeSourcePedWorldPed player; Check(world.Ped(1, player) == Status::Ok && player.Contact.IsStanding &&
          player.Contact.SurfaceTouched < 179 && player.Physical.SafePosition, "real support publishes owned standing/material/safe state");
}
}

int main(int argc, char** argv) {
    if (argc > 2) return 2;
    Ownership(); ControlledRoute(); FailureAtomic();
    if (argc == 2) Real(argv[1]);
    std::printf("source-ped-world-ok checks=%zu controlled-curb-wall-dynamic real-loaded-support\n", s_Checks);
}
