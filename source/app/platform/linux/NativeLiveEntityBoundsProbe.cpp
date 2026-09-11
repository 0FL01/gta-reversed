// Independent arithmetic oracle plus actual native main53/player/COL ownership.
#include "NativeLiveEntityBounds.h"
#include "RealtimeScriptHost.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>

namespace {
void Require(bool ok, const std::string& detail) {
    if (!ok) throw std::runtime_error(detail);
}
using Status = NativeLiveBlockageStatus;
NativeLiveModelBounds Box(float radius = 1) {
    return {NativeLiveBoundsKnowledge::SourceCol, {}, {-1, -1, -1}, {1, 1, 1}, radius};
}
NativeLiveEntityBound Entity(float x, float y, float z) {
    NativeLiveEntityBound entry;
    entry.Transform.Position = {x, y, z};
    entry.Model = Box();
    entry.WorldCenter = entry.Transform.Position;
    return entry;
}
// Explicit source-list oracle: first eight XY, THEN Z. Kept independent of
// production helper and intentionally accepts a supplied fixture source order.
bool OrderedOracle(std::span<const NativeLiveEntityBound> entries) {
    std::vector<NativeLiveEntityBound> first;
    for (const auto& e : entries) {
        const auto x = e.WorldCenter[0], y = e.WorldCenter[1];
        if (std::sqrt(x*x + y*y) < e.Model.Radius + 1) first.push_back(e);
        if (first.size() == 8) break;
    }
    for (const auto& e : first) {
        if (e.Transform.Position[2] + e.Model.Max[2] + 1 > -1 &&
            e.Transform.Position[2] + e.Model.Min[2] - 1 < 1) return true;
    }
    return false;
}
void Fixtures() {
    auto e = Entity(2, 0, 0);
    const auto query = [&] { return NativeLiveCheckForBlockage({&e, 1}, {}, Box()); };
    Require(query().Status == Status::Clear, "strict XY tangent");
    e.WorldCenter[0] = std::nextafter(2.0f, 0.0f);
    Require(query().Status == Status::Blocked, "inside XY tangent");
    e = Entity(1.5f, 1.5f, 0);
    Require(query().Status == Status::Clear, "circle not square broadphase");
    e = Entity(0, 0, 3);
    Require(query().Status == Status::Clear, "strict lower Z tangent");
    e.Transform.Position[2] = std::nextafter(3.0f, 0.0f);
    Require(query().Status == Status::Blocked, "inside lower Z tangent");
    e = Entity(0, 0, -3);
    Require(query().Status == Status::Clear, "strict upper Z tangent");
    e.Transform.Position[2] = std::nextafter(-3.0f, 0.0f);
    Require(query().Status == Status::Blocked, "inside upper Z tangent");
    e = Entity(10, 0, 0);
    e.Model.Center = {0, 10, 50};
    e.Transform.Basis = {{{0, 1, 0}, {-1, 0, 0}, {0, 0, 1}}};
    e.WorldCenter = NativeLiveBoundCenter(e.Transform, e.Model.Center);
    Require(e.WorldCenter == NativeCollisionVector{0, 0, 50} && query().Status == Status::Blocked,
        "transformed COL center XY; Z uses origin not transformed center");
    e.Transform.Basis = {{{1, 0, 0}, {0, 0, 1}, {0, -1, 0}}};
    e.Model.Min = {-1, -100, -1}; e.Model.Max = {1, 100, 1};
    e.Transform.Position = {0, 0, 5}; e.WorldCenter = {};
    Require(query().Status == Status::Clear, "Z does not rotate bbox");
    e = Entity(0, 0, 0); e.Model = {};
    auto r = query();
    Require(r.Status == Status::Unsupported && r.PossibleUnknownCandidates == 1,
        "unknown entity retained as possible XY candidate");
    e = Entity(2.5f, 0, 0);
    NativeLiveModelBounds provenNull; provenNull.Knowledge = NativeLiveBoundsKnowledge::ProvenSourceNull;
    Require(NativeLiveCheckForBlockage({&e, 1}, {}, provenNull).Status == Status::Blocked,
        "fixture proven source NULL uses radius2 and Z+-1");
    Require(NativeLiveCheckForBlockage({&e, 1}, {}, {}).Status == Status::Unsupported,
        "missing packet never NULL fallback");
    std::vector<NativeLiveEntityBound> eight(8, Entity(0, 0, 20));
    for (std::size_t i = 0; i < eight.size(); ++i) eight[i].Reference = int(i);
    eight.back().Transform.Position[2] = 0;
    std::size_t permutations = 0;
    const auto less = [](const auto& a, const auto& b) { return a.Reference < b.Reference; };
    do {
        const auto actual = NativeLiveCheckForBlockage(eight, {}, Box());
        Require(OrderedOracle(eight) && actual.Status == Status::Blocked && actual.XYCandidates == 8,
            "all <=8 candidate permutations preserve source boolean");
        ++permutations;
    } while (std::next_permutation(eight.begin(), eight.end(), less));
    std::vector<NativeLiveEntityBound> nine(9, Entity(0, 0, 20));
    nine.back().Transform.Position[2] = 0;
    Require(!OrderedOracle(nine), "ninth blocker excluded BEFORE Z");
    auto overflow = NativeLiveCheckForBlockage(nine, {}, Box());
    Require(overflow.Status == Status::Unsupported && overflow.XYCandidates == 9 && !overflow.ZTests &&
        overflow.Reason == NativeLiveBlockageReason::UnknownSourceOrdering, "overflow cannot use any-blocker");
    std::swap(nine.front(), nine.back());
    Require(OrderedOracle(nine) && NativeLiveCheckForBlockage(nine, {}, Box()).Status == Status::Unsupported,
        "source order changes ninth boolean; unknown native order remains unsupported");
    std::printf("fixtures PASS permutations=%zu first8-before-Z=1 ninth-counterexample=1 strict-circle-Z=1 null-proof=1\n", permutations);
}
NativeVehicleCreateRequest Vehicle(const NativeGeneratedVehicleAsset& packet, NativeCollisionVector pos) {
    NativeVehicleCreateRequest request;
    request.Producer = NativeVehicleProducer::NativeGameplayController;
    request.State.ModelId = packet->Definition.ModelId;
    request.State.Type = packet->Definition.ModelId == 476 ? NativeVehicleType::Plane : NativeVehicleType::Automobile;
    request.State.SubType = int(request.State.Type);
    request.State.Status = NativeVehicleStatus::Abandoned;
    request.State.CreatedBy = NativeVehicleCreatedBy::Mission;
    request.State.InWorld = true;
    request.State.Matrix.Position = pos;
    request.State.Collision = packet->Collision;
    request.State.ModelCollision = NativeLiveVehicleModelCol(packet);
    return request;
}
} // namespace

int main(int argc, char** argv) try {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    Require(argc == 2, "usage: NativeLiveEntityBoundsProbe /game");
    Fixtures();
    std::string error;
    char message[512]{};
    E2ELoadInfo info{};
    Require(StreamPager_Init(argv[1], info, message, sizeof(message),
        {.includeStreamed = true, .radius = 300, .maxInstances = 1200}), message);
    {
        RealtimeGameplay gameplay;
        RealtimeScriptHost host(gameplay);
        Require(host.InitializeBeforeWorker(argv[1], error), error);
        Require(!host.PublishVehicles(0, error), "initialized pre-player preview guard");
        const auto pass = host.RunPass(1000);
        Require(pass.Status == NativeScriptStatus::Waiting && pass.Executed == 53, "actual main53");
        NativeScriptPedRef ped;
        for (const auto& event : host.Events()) if (event.Opcode == 0x01F5) ped.Value = event.Reference;
        Require(host.ResolvePed(ped) == &gameplay, "actual source ped reference");
        const auto source = host.Publication().SourceCollision;
        Require(bool(source), "actual scene COL publication");
        const auto generation = host.CarGeneratorResidency().Generation();
        Require(generation != 0 && host.CarGeneratorResidency().Snapshot() == source, "actual committed world generation");
        NativeGeneratedVehicleAsset landstal, rustler;
        NativeCollisionAssets embeddedCatalog;
        for (auto* output : {&landstal, &rustler}) {
            const auto id = output == &landstal ? 400 : 476;
            const auto* model = host.CarGenerators().FindModel(id);
            Require(model, "actual model definition");
            const auto load = NativeGeneratedVehicleAssets_Load(argv[1], *model, embeddedCatalog, *output);
            Require(bool(load), load.Detail);
            const auto& col = *(*output)->Collision;
            std::printf("actual-model id=%d COL=%s radius=%.9g center=%.9g,%.9g,%.9g bboxZ=%.9g,%.9g\n",
                id, col.Name.c_str(), col.BoundRadius, col.BoundCenter[0], col.BoundCenter[1], col.BoundCenter[2], col.Min[2], col.Max[2]);
        }
        auto pool = host.PublishVehicles(10, error);
        Require(bool(pool), error);
        auto proof = NativeLiveEntityBounds::Capture(host, ped, pool, error);
        Require(bool(proof), error);
        Require(proof->Entries().size() == 1 && proof->VehicleCensus().Alive == 0 &&
            proof->VehicleCensus().Producers.NativeHostComplete && !proof->SourceParityComplete,
            "real newgame native census: sole player, zero vehicles, no retail parity");
        auto root = proof->Entries().front().Transform.Position;
        Require(root[2] == gameplay.State().Ped.Z + 1 && proof->Entries().front().Model.Radius == 1,
            "actual GameplayPedRoot and source ped1");
        std::printf("actual-main53 entities=%zu vehicles=%zu pool-capacity=%zu root=%.9g,%.9g,%.9g world=%llu\n",
            proof->Entries().size(), proof->VehicleCensus().Alive, NativeVehiclePoolCapacity,
            root[0], root[1], root[2], (unsigned long long)proof->WorldGeneration());
        std::printf("query-only-begin\n");
        Require(proof->Query(host, 10, generation, root, landstal).Status == Status::Blocked, "actual ped blocks candidate");
        Require(proof->Query(host, 10, generation, root, {}).Reason == NativeLiveBlockageReason::UnknownCandidateModel,
            "missing actual packet explicit Unknown");
        Require(!proof->Matches(host, 11, generation) && !proof->Matches(host, 10, generation + 1), "frame/world exact");
        // Labelled in-memory state fault injection, never an observation of an
        // implemented death system. Alive/PedState are NOT world-membership tests.
        auto& activity = const_cast<NativePlayerActivitySnapshot&>(gameplay.Activity());
        const auto savedActivity = activity;
        activity.Alive = false; activity.PedState = NativePlayerPedState::Dead;
        Require(proof->Query(host, 10, generation, root, landstal).Status == Status::Blocked, "dead registered ped still blocks");
        activity = savedActivity;
        auto& state = const_cast<RealtimeGameplayState&>(gameplay.State());
        state.Ready = false;
        Require(!proof->Matches(host, 10, generation), "Ready/initialized membership required");
        state.Ready = true;
        state.PedRoot.X += 1;
        Require(!proof->Matches(host, 10, generation), "late player position even without revision invalidates");
        state.PedRoot.X -= 1;
        state.CarPresent = true;
        Require(!proof->Matches(host, 10, generation), "unregistered preview car invalidates complete census");
        state.CarPresent = false;
        auto& worldFixture = const_cast<NativeCollisionSnapshot&>(*source);
        const auto originalOverrides = worldFixture.Overrides;
        worldFixture.Overrides = std::make_shared<const NativePlacementOverrides>(std::vector<NativePlacementOverride>{});
        Require(!proof->Matches(host, 10, generation), "changed overrides identity invalidates");
        worldFixture.Overrides = originalOverrides;
        bool staticOverlap = false;
        for (const auto& instance : source->Instances) {
            const auto center = NativeLiveBoundCenter({instance.Placement.Position, instance.Basis}, instance.Model->BoundCenter);
            if (std::hypot(center[0] - root[0], center[1] - root[1]) < 40) continue;
            if (!host.World()->SphereBlocked({center[0], center[1], center[2]}, 1)) continue;
            Require(proof->Query(host, 10, generation, center, landstal).Status == Status::Clear,
                "actual static scene overlap is not vehicle/ped blockage");
            std::printf("actual-static-overlap PASS model=%s center=%.6f,%.6f,%.6f blockage=Clear\n",
                instance.Placement.Model.c_str(), center[0], center[1], center[2]);
            staticOverlap = true; break;
        }
        Require(staticOverlap, "actual static overlap witness");
        std::printf("query-only-end\n");

        // Controlled native-owner vehicle lifecycle fixtures using real packets.
        // They are not ambient traffic observations and never enter rendering.
        auto request = Vehicle(rustler, {root[0] + 100, root[1], root[2]});
        auto allocated = host.Vehicles().Allocate(request);
        Require(allocated.Result.Status == NativeScriptServiceStatus::Ready, allocated.Result.Message);
        Require(!proof->Matches(host, 10, generation), "late pool allocation invalidates");
        pool = host.PublishVehicles(11, error);
        proof = NativeLiveEntityBounds::Capture(host, ped, pool, error); Require(bool(proof), error);
        Require(proof->Entries().size() == 2 && proof->Entries()[1].ModelId == 476 &&
            proof->Entries()[1].Model == NativeLiveBoundsFromCol(rustler->Collision.get()), "typed actual vehicle COL ownership");
        Require(proof->Query(host, 11, generation, request.State.Matrix.Position, landstal).Status == Status::Blocked,
            "actual rustler COL native-owner fixture blocks");
        request.State.ModelCollision.reset(); // instance COL still exists; model-info owner unresolved
        Require(host.Vehicles().Update(allocated.Reference, request.State, error), error);
        pool = host.PublishVehicles(12, error);
        proof = NativeLiveEntityBounds::Capture(host, ped, pool, error); Require(bool(proof), error);
        const auto uncertain = proof->Query(host, 12, generation, root, landstal);
        Require(uncertain.Status == Status::Unsupported && uncertain.PossibleUnknownCandidates == 1,
            "unresolved registered vehicle cannot disappear or be inferred distant");
        Require(host.Vehicles().Release(allocated.Reference, error), error);
        Require(!proof->Matches(host, 12, generation), "late release invalidates");
        pool = host.PublishVehicles(13, error);
        proof = NativeLiveEntityBounds::Capture(host, ped, pool, error); Require(bool(proof), error);
        Require(host.PublishVehicles(14, error) && !proof->Matches(host, 13, generation), "later publication invalidates");
        NativeVehiclePool foreign;
        Require(foreign.BindProducer(NativeVehicleProducer::NativeScm, error) && foreign.SealProducerExtent(error), error);
        Require(!NativeLiveEntityBounds::Capture(host, ped, foreign.Publish(15, error), error), "foreign pool rejected");
        // Full source-sized census: dormant allocations are not world members;
        // the last (109th) slot is still included, regardless of wrecked status.
        request = Vehicle(landstal, {root[0] + 100, root[1], root[2]});
        std::vector<NativeVehicleRef> refs;
        for (std::size_t slot = 0; slot < NativeVehiclePoolCapacity; ++slot) {
            request.State.InWorld = slot == NativeVehiclePoolCapacity - 1;
            request.State.Status = NativeVehicleStatus::Wrecked;
            const auto vehicle = host.Vehicles().Allocate(request);
            Require(vehicle.Result.Status == NativeScriptServiceStatus::Ready, vehicle.Result.Message);
            refs.push_back(vehicle.Reference);
        }
        pool = host.PublishVehicles(15, error);
        proof = NativeLiveEntityBounds::Capture(host, ped, pool, error); Require(bool(proof), error);
        Require(proof->VehicleCensus().Alive == 110 && proof->Entries().size() == 2 &&
            proof->Entries()[1].Reference == refs.back().Value &&
            proof->Query(host, 15, generation, request.State.Matrix.Position, landstal).Status == Status::Blocked,
            "all110 slots enumerated; inWorld wrecked vehicle blocks; dormant109 do not");
        request.State.InWorld = true;
        const auto target = request.State.Matrix.Position;
        request.State.Matrix.Position[2] += 200;
        for (std::size_t i = 0; i < 8; ++i) Require(host.Vehicles().Update(refs[i], request.State, error), error);
        pool = host.PublishVehicles(16, error);
        proof = NativeLiveEntityBounds::Capture(host, ped, pool, error); Require(bool(proof), error);
        const auto crowded = proof->Query(host, 16, generation, target, landstal);
        Require(crowded.XYCandidates == 9 && crowded.Status == Status::Unsupported && crowded.ZTests == 0,
            "real owner nine XY candidates require unknown source ordering before any Z test");
        auto wrongModel = request.State;
        wrongModel.ModelCollision = NativeLiveVehicleModelCol(rustler);
        const auto revision = host.Vehicles().Revision();
        Require(!host.Vehicles().Update(refs.front(), wrongModel, error) && host.Vehicles().Revision() == revision,
            "typed model/COL mismatch rejected without mutation");
        for (const auto ref : refs) Require(host.Vehicles().Release(ref, error), error);
        std::printf("owner-fixtures PASS late-census=1 late-position=1 frame-generation=1 liveness-independent=1 unknown-vehicle=1\n");
        std::printf("pool-fixtures PASS capacity=110 last-slot-included=1 dormant-excluded=109 wrecked-blocks=1 overflow-before-Z=1 typed-mismatch=1\n");
    }
    StreamPager_Shutdown();
    std::printf("live-entity-bounds-probe PASS native-complete-only=1 spawn=0\n");
    return 0;
} catch (const std::exception& exception) {
    std::fprintf(stderr, "FAIL %s\n", exception.what());
    return 1;
}
