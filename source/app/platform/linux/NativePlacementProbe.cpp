// Real-asset initial placement replacement and publication/lifetime regression.
#include "app/platform/linux/RealtimeScriptHost.h"
#include "app/platform/linux/RealtimeStreaming.h"
#include <algorithm>
#include <atomic>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <set>

namespace {
std::atomic<bool> s_WorkerOwnsParsers{false};
std::atomic<size_t> s_WorkerParserCalls{0};
std::thread::id s_MainThread;
void Require(bool ok, const std::string& message) {
    if (!ok) { std::fprintf(stderr, "placement probe: %s\n", message.c_str()); std::exit(2); }
}
uint64_t Hash(const WorldShotScene& scene) {
    uint64_t hash = 14695981039346656037ull;
    const auto bytes = [&](const auto& values) {
        for (const auto byte : std::as_bytes(std::span(values))) {
            hash ^= std::to_integer<uint8_t>(byte); hash *= 1099511628211ull;
        }
    };
    for (const auto& mesh : scene.meshes) {
        bytes(mesh.pos); bytes(mesh.nrm); bytes(mesh.uv); bytes(mesh.triImg); bytes(mesh.triCol);
        bytes(mesh.dayColors); bytes(mesh.nightColors);
    }
    for (const auto& image : scene.images) bytes(image.rgba);
    return hash;
}
size_t Index(const std::vector<NativePlacementIdentity>& ids, const NativePlacementIdentity& id) {
    const auto it = std::ranges::find(ids, id);
    Require(it != ids.end() && std::ranges::count(ids, id) == 1, "exactly one render mesh for source identity");
    return static_cast<size_t>(it - ids.begin());
}
NativeCollisionVector Transform(const NativeGarageMatrix& pose, NativeCollisionVector local) {
    auto result = pose.Position;
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) result[i] += pose.Basis[j][i] * local[j];
    return result;
}
void Geometry(const WorldShotMesh& authored, const WorldShotMesh& placed, const NativeGarageDoor& door) {
    Require(authored.pos.size() == placed.pos.size(), "replacement preserves DFF topology");
    for (size_t i = 0; i < authored.pos.size(); i += 3) {
        NativeCollisionVector local{};
        for (int j = 0; j < 3; ++j) for (int k = 0; k < 3; ++k)
            local[j] += door.Authored.Basis[j][k] * (authored.pos[i+k] - door.Authored.Position[k]);
        const auto expected = Transform(door.SourcePose, local);
        for (int k = 0; k < 3; ++k) Require(std::abs(expected[k] - placed.pos[i+k]) < .002f, "actual requested DFF vertex pose");
        local = {};
        for (int j = 0; j < 3; ++j) for (int k = 0; k < 3; ++k) local[j] += door.Authored.Basis[j][k] * authored.nrm[i+k];
        NativeCollisionVector normal{};
        for (int j = 0; j < 3; ++j) for (int k = 0; k < 3; ++k) normal[k] += door.SourcePose.Basis[j][k] * local[j];
        for (int k = 0; k < 3; ++k) Require(std::abs(normal[k] - placed.nrm[i+k]) < .002f, "actual requested DFF normals");
    }
}
}

// The isolated runner wraps actual pager and TexSample parse entry points.
void NativePlacementProbe_ParserThread() {
    if (!s_WorkerOwnsParsers.load()) return;
    Require(std::this_thread::get_id() != s_MainThread, "main thread entered pager/RW parser after SealStartup");
    ++s_WorkerParserCalls;
}

int main(int argc, char** argv) {
    s_MainThread = std::this_thread::get_id();
    Require(argc >= 2, "game directory argument required");
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const bool offline = argc > 2 && std::string(argv[2]) == "--offline";
    char message[512]{}; E2ELoadInfo info;
    Require(StreamPager_Init(argv[1], info, message, sizeof(message),
        offline ? StreamPagerOptions{} : StreamPagerOptions{true, 120.0f, 1200}), message);
    if (offline) {
        for (const auto center : {realtime_streaming::Center{2495,-1687,15}, realtime_streaming::Center{-2026,156,30}}) {
            WorldShotScene scene; E2EPagerFrame frame;
            Require(StreamPager_Update(center.X, center.Y, center.Z, scene, frame, message, sizeof(message)), message);
            std::printf("offline-placement-hash %016llx instances=%d tris=%d\n", (unsigned long long)Hash(scene), frame.instances, frame.tris);
        }
        StreamPager_Shutdown(); return 0;
    }
    std::string error;
    auto context = NativeCollisionContext::LoadBeforeWorker(argv[1], 900, error); Require(bool(context), error);
    RealtimeGameplay gameplay; RealtimeScriptHost host(gameplay);
    Require(!host.PrepareInitialGarageWorldBeforeWorker(error) && host.WorldRevision() == 0, "preparation requires initialized player/world");
    Require(host.InitializeBeforeWorker(argv[1], error, context), error);
    auto overrides = host.InitialPlacementOverrides();
    Require(host.Garages().Doors().size() == 53 && overrides && overrides->Entries().size() == 14, "53 real doors / 14 initial overrides");
    Require(std::ranges::all_of(overrides->Entries(), [](const auto& p) { return p.CollisionEnabled; }), "constructor overrides all collide before explicit preparation");
    Require(!host.PrepareInitialGarageWorldBeforeWorker(error) && host.InitialPlacementOverrides() == overrides && host.WorldRevision() == 0,
        "pre-player preparation rejected without publication");
    Require(std::ranges::count_if(host.Garages().Doors(), [](const auto& d) { return d.RequiresDynamicPublication && d.Collision->Empty; }) == 2,
        "two overridden doors have genuine source-empty COL");
    NativeCollisionSnapshot original, actual, enabled;
    Require(context->Assets.Snapshot(context->Population, 0, 0, 20000, original, error), error);
    Require(context->Assets.Snapshot(context->Population, 0, 0, 20000, actual, error, 0, overrides), error);
    auto entries = std::vector<NativePlacementOverride>(overrides->Entries().begin(), overrides->Entries().end());
    for (auto& entry : entries) entry.CollisionEnabled = true;
    auto allEnabled = std::make_shared<const NativePlacementOverrides>(entries);
    Require(context->Assets.Snapshot(context->Population, 0, 0, 20000, enabled, error, 0, allEnabled), error);
    for (auto& entry : entries) entry.CollisionEnabled = false;
    auto allDisabled = std::make_shared<const NativePlacementOverrides>(entries);
    NativeCollisionSnapshot disabled;
    Require(context->Assets.Snapshot(context->Population, 0, 0, 20000, disabled, error, 0, allDisabled), error);
    size_t suppressed = 0, geometry = 0, unchanged = 0, sameModel = 0;
    for (const auto& source : original.Instances) {
        const auto id = NativePlacementIdentity::From(source.Placement);
        const auto match = [&](const auto& i) { return id.Matches(i.Placement); };
        const auto* replacement = overrides->Find(source.Placement);
        const auto count = std::ranges::count_if(actual.Instances, match);
        Require(count == (replacement && !replacement->CollisionEnabled ? 0 : 1), "COL exact replacement/suppression, no authored duplicate");
        if (replacement && !replacement->CollisionEnabled) ++suppressed;
        const auto moved = std::ranges::find_if(enabled.Instances, match);
        Require(moved != enabled.Instances.end() && moved->Model == source.Model, "owned exact source COL model retained");
        Require(moved->Placement.Position == (replacement ? replacement->Position : source.Placement.Position) &&
                moved->Basis == (replacement ? replacement->Basis : source.Basis), "matching COL transform");
        if (!replacement) {
            ++unchanged;
            if (std::ranges::any_of(overrides->Entries(), [&](const auto& o) { return o.Identity.ModelId == id.ModelId; })) ++sameModel;
            Require(moved->Min == source.Min && moved->Max == source.Max, "unrelated COL bounds unchanged");
        } else {
            // Check the broadphase bound really encloses the transformed source geometry.
            for (const auto& vertex : source.Model->Vertices) {
                const auto point = Transform({replacement->Position, replacement->Basis}, vertex);
                for (int k = 0; k < 3; ++k) Require(point[k] >= moved->Min[k] - .01f && point[k] <= moved->Max[k] + .01f, "moved COL source vertex bounds");
            }
        }
    }
    Require(enabled.Instances.size() == original.Instances.size() && actual.Instances.size()+suppressed == original.Instances.size(), "complete COL census / no double collision");
    Require(disabled.Instances.size() + 12 == original.Instances.size() && disabled.Overrides == allDisabled, "flag snapshot suppresses all 12 nonempty COL rows independently of render");
    for (const auto& instance : disabled.Instances) Require(!overrides->Find(instance.Placement), "disabled source identities absent, no static COL left behind");
    NativeCollisionSnapshot doorWorld, sentinelWorld;
    size_t triangles = 0, spheres = 0, boxes = 0;
    for (const auto& instance : actual.Instances) {
        if (!overrides->Find(instance.Placement)) continue;
        doorWorld.Instances.push_back(instance);
        triangles += instance.Model->Faces.size(); spheres += instance.Model->Spheres.size(); boxes += instance.Model->Boxes.size();
    }
    sentinelWorld.Instances.push_back(disabled.Instances.front());
    doorWorld.Instances.push_back(sentinelWorld.Instances.front());
    RealtimeGameplayWorld queryWorld, sentinel;
    Require(queryWorld.Rebuild(doorWorld, error) && sentinel.Rebuild(sentinelWorld, error), error);
    Require(queryWorld.TriangleCount() == sentinel.TriangleCount()+triangles && queryWorld.SphereCount() == sentinel.SphereCount()+spheres &&
        queryWorld.BoxCount() == sentinel.BoxCount()+boxes, "runtime query world has exactly one set of transformed door primitives");
    // Each identity component must exclude an otherwise identical source row.
    const auto& firstDoor = *std::ranges::find_if(host.Garages().Doors(), [](const auto& d) { return d.RequiresDynamicPublication; });
    for (int field = 0; field < 5; ++field) {
        auto wrong = firstDoor.Placement;
        if (field == 0) wrong.Ipl += "-different";
        if (field == 1) ++wrong.Record;
        if (field == 2) wrong.Binary = !wrong.Binary;
        if (field == 3) ++wrong.ModelId;
        if (field == 4) wrong.Model += "-different";
        Require(!overrides->Find(wrong), "full source identity required");
    }
    // This install's changed door models have no unrelated outdoor COL rows.
    // Reuse the real COL with distinct provenance to exercise that case too.
    NativeCollisionPopulation identities;
    identities.Models = context->Population.Models; identities.IncludesStreamed = true;
    for (int field = 0; field < 4; ++field) {
        auto source = firstDoor.Placement;
        if (field == 1) source.Ipl += "-different";
        if (field == 2) source.Record += 100000;
        if (field == 3) source.Binary = !source.Binary;
        identities.Instances.push_back(source);
    }
    NativeCollisionSnapshot identitySnapshot;
    Require(context->Assets.Snapshot(identities, 0, 0, 20000, identitySnapshot, error, 0, allEnabled), error);
    Require(identitySnapshot.Instances.size() == 4, "same-model provenance fixture keeps four distinct COL rows");
    for (size_t i = 1; i < 4; ++i) Require(identitySnapshot.Instances[i].Placement.Position == firstDoor.Authored.Position &&
        identitySnapshot.Instances[i].Basis == firstDoor.Authored.Basis, "same-model other IPL/record/binary COL untouched");
    for (const auto& door : host.Garages().Doors()) {
        if (!door.RequiresDynamicPublication) continue;
        const auto id = NativePlacementIdentity::From(door.Placement);
        WorldShotScene before, after, restored; E2EPagerFrame frame;
        std::vector<NativePlacementIdentity> beforeIds, afterIds;
        const auto p = door.SourcePose.Position;
        Require(StreamPager_Update(p[0], p[1], p[2], before, frame, message, sizeof(message), {}, &beforeIds), message);
        Require(StreamPager_Update(p[0], p[1], p[2], after, frame, message, sizeof(message), overrides, &afterIds), message);
        Geometry(before.meshes[Index(beforeIds,id)], after.meshes[Index(afterIds,id)], door); ++geometry;
        for (size_t i = 0; i < beforeIds.size(); ++i) {
            if (std::ranges::any_of(overrides->Entries(), [&](const auto& o) { return o.Identity == beforeIds[i]; })) continue;
            const auto it = std::ranges::find(afterIds, beforeIds[i]);
            if (it != afterIds.end()) Require(before.meshes[i].pos == after.meshes[it-afterIds.begin()].pos, "unaffected render identity unchanged");
        }
        Require(StreamPager_Update(p[0], p[1], p[2], restored, frame, message, sizeof(message)), message);
        Require(Hash(before) == Hash(restored), "source pager not mutated by override build");
        Require(StreamPager_Update(p[0], p[1], p[2], restored, frame, message, sizeof(message), allDisabled), message);
        Require(Hash(after) == Hash(restored), "COL suppression never removes/reverts render replacement");
        std::printf("placement-door id=%d record=%u binary=%d collision=%d vertices=%zu ipl=%s\n", id.ModelId, id.Record, id.Binary,
            overrides->Find(door.Placement)->CollisionEnabled, after.meshes[Index(afterIds,id)].pos.size()/3, id.Ipl.c_str());
    }
    // Real initial VM LOAD_SCENE uses the same snapshot, before worker ownership.
    Require(host.RunPass(4096).Status == NativeScriptStatus::Waiting && host.Session().Threads()[0].Commands == 53, "actual main53 startup boundary");
    const auto oldPublication = host.Publication();
    Require(oldPublication.Scene && oldPublication.Overrides == overrides && oldPublication.SourceCollision &&
        oldPublication.SourceCollision->Overrides == overrides && host.WorldRevision() == 2, "initial host LOAD_SCENE snapshot / original two world publications");
    const auto oldHash = Hash(*oldPublication.Scene);
    const auto events = host.Events().size();
    const auto sourceEvents = std::ranges::count_if(host.Events(), [](const auto& e) { return e.Opcode == 0x04E4 || e.Opcode == 0x03CB; });
    Require(sourceEvents == 2, "actual original collision/scene source events");
    std::vector<std::uint8_t> flags;
    for (const auto& g : host.Garages().Entries()) flags.push_back(g.Flags);
    // TEST-INPUT: alter one owned fixture pose, then restore it. Preparation
    // must reject movement without modifying the already published scene/COL.
    auto* fixture = const_cast<NativeGarageEntry*>(host.Garages().Resolve(*firstDoor.Garage));
    const auto saved = *fixture;
    fixture->DoorPosition += .1f;
    Require(!host.PrepareInitialGarageWorldBeforeWorker(error) && host.WorldRevision() == 2 &&
        host.Publication().SourceCollision == oldPublication.SourceCollision && host.InitialPlacementOverrides() == overrides &&
        Hash(*host.Publication().Scene) == oldHash, "pose-change preparation fails atomically");
    *fixture = saved;
    Require(host.PrepareInitialGarageWorldBeforeWorker(error), error);
    const auto prepared = host.Publication();
    const auto constructorOverrides = overrides;
    overrides = host.InitialPlacementOverrides();
    Require(prepared.Scene == oldPublication.Scene && Hash(*prepared.Scene) == oldHash && prepared.Collision != oldPublication.Collision &&
        prepared.SourceCollision != oldPublication.SourceCollision && prepared.SourceCollision->Overrides == overrides && prepared.Overrides == overrides &&
        host.World() == prepared.Collision.get() && host.WorldRevision() == 3 && host.Events().size() == events, "prepared atomic owned COL handoff / unchanged scene / no source event invented");
    Require(overrides->Entries().size() == 14 && std::ranges::count_if(overrides->Entries(), [](const auto& p) { return !p.CollisionEnabled; }) == 13,
        "prepared first common update disables exactly 13 of 14");
    for (size_t i = 0; i < flags.size(); ++i) Require(host.Garages().Entries()[i].Flags == flags[i], "preparation does not commit garage flags");
    Require(host.Garages().Frame().Updates.empty() && gameplay.State().Ticks == 0, "preparation is not a garage Tick or physics step");
    Require(host.PrepareInitialGarageWorldBeforeWorker(error) && host.WorldRevision() == 3 && host.Publication().SourceCollision == prepared.SourceCollision &&
        host.InitialPlacementOverrides() == overrides, "preworker preparation idempotent / no new allocations published");
    fixture->DoorState = 0;
    Require(!host.PrepareInitialGarageWorldBeforeWorker(error) && host.WorldRevision() == 3 &&
        host.Publication().SourceCollision == prepared.SourceCollision && host.InitialPlacementOverrides() == overrides,
        "preparation is single-use when expected collision state changes");
    *fixture = saved;
    size_t preparedResidentSuppressed = 0;
    for (const auto& source : oldPublication.SourceCollision->Instances) {
        const auto* replacement = overrides->Find(source.Placement);
        const bool removed = replacement && !replacement->CollisionEnabled;
        const auto count = std::ranges::count_if(prepared.SourceCollision->Instances, [&](const auto& p) { return NativePlacementIdentity::From(source.Placement).Matches(p.Placement); });
        Require(count == (removed ? 0 : 1), "prepared resident exact COL replacement/suppression without duplicates");
        preparedResidentSuppressed += removed;
    }
    Require(prepared.SourceCollision->Instances.size() + preparedResidentSuppressed == oldPublication.SourceCollision->Instances.size(), "prepared resident COL census");
    NativeCollisionSnapshot preparedGlobal;
    Require(context->Assets.Snapshot(context->Population, 0, 0, 20000, preparedGlobal, error, 0, overrides), error);
    Require(preparedGlobal.Instances.size() + 11 == original.Instances.size() && preparedResidentSuppressed == 2,
        "prepared real COL removes 11 globally / 2 resident, two disabled source-empty doors stay empty");
    for (const auto& door : host.Garages().Doors()) {
        if (!door.RequiresDynamicPublication) continue;
        const auto* initial = constructorOverrides->Find(door.Placement);
        const auto* next = overrides->Find(door.Placement);
        Require(next && initial && next->Position == initial->Position && next->Basis == initial->Basis, "all 14 prepared poses remain exact");
        WorldShotScene before, after; E2EPagerFrame frame; std::vector<NativePlacementIdentity> ids;
        const auto p = door.SourcePose.Position;
        Require(StreamPager_Update(p[0], p[1], p[2], before, frame, message, sizeof(message), constructorOverrides), message);
        Require(StreamPager_Update(p[0], p[1], p[2], after, frame, message, sizeof(message), overrides, &ids), message);
        Index(ids, next->Identity);
        Require(Hash(before) == Hash(after), "all 14 actual prepared render scenes unchanged");
    }
    std::printf("placement-preparation PASS revision=3 sourceEvents=%zu overrides=14 disabled=13 residentSuppressed=%zu globalSuppressed=%zu oldSourceCOL=%zu preparedSourceCOL=%zu geometryUnchanged=14 flagsUncommitted=1 idempotent=1 rollback=1\n",
        size_t(sourceEvents), preparedResidentSuppressed, original.Instances.size()-preparedGlobal.Instances.size(), oldPublication.SourceCollision->Instances.size(), prepared.SourceCollision->Instances.size());
    s_WorkerOwnsParsers = true;
    host.SealStartup();
    Require(!host.PrepareInitialGarageWorldBeforeWorker(error) && host.WorldRevision() == 3 && host.Publication().SourceCollision == prepared.SourceCollision,
        "even repeated preparation rejected after SealStartup without parser entry");
    const auto revision = host.WorldRevision();
    host.SetLiveWorldLoader([&](const auto&, auto& publication) {
        publication = oldPublication; publication.Overrides.reset();
        return NativeScriptServiceResult{NativeScriptServiceStatus::Ready,{}};
    }, [](const auto&) {});
    Require(host.RequestCollision({{90002,1,1},oldPublication.Center.X,oldPublication.Center.Y}).Status == NativeScriptServiceStatus::Error &&
        host.WorldRevision() == revision && host.Publication().Scene == oldPublication.Scene, "mixed override/render publication rejected atomically");
    realtime_streaming::Worker worker(true, context, overrides);
    const auto center = oldPublication.Center;
    worker.Request({center.X,center.Y,center.Z}, true);
    std::unique_ptr<realtime_streaming::CpuWorld> first;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
    size_t concurrent = 0;
    while (!(first = worker.TakeReady())) {
        Require(std::chrono::steady_clock::now() < deadline, "worker timeout");
        // Main thread only touches the immutable COL catalog while RW is worker-owned.
        NativeCollisionSnapshot scratch;
        Require(context->Snapshot(center.X, center.Y, scratch, error, overrides), error); ++concurrent;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    Require(first->Error.empty(), first->Error);
    Require(first->Overrides == overrides && first->SourceCollision->Overrides == overrides && Hash(first->Scene) == oldHash, "startup/worker identical render and COL snapshot");
    Require(first->Collision.TriangleCount() == prepared.Collision->TriangleCount() && first->Collision.SphereCount() == prepared.Collision->SphereCount() &&
        first->Collision.BoxCount() == prepared.Collision->BoxCount() && first->SourceCollision->Instances.size() == prepared.SourceCollision->Instances.size(), "prepared startup/worker identical source primitive counts");
    for (size_t i = 0; i < prepared.SourceCollision->Instances.size(); ++i) {
        const auto& startup = prepared.SourceCollision->Instances[i];
        const auto& streamed = first->SourceCollision->Instances[i];
        Require(NativePlacementIdentity::From(startup.Placement).Matches(streamed.Placement) && startup.Model == streamed.Model &&
            startup.Placement.Position == streamed.Placement.Position && startup.Basis == streamed.Basis && startup.Min == streamed.Min && startup.Max == streamed.Max,
            "prepared startup/worker exact source identity, geometry and bounds");
    }
    worker.Release();
    const auto p = firstDoor.SourcePose.Position;
    bool requested = false;
    host.SetLiveWorldLoader([&](const NativeScriptSceneRequest& request, RealtimeScriptWorldPublication& publication) {
        if (!requested) { worker.Request({request.Position.X, request.Position.Y, request.Position.Z}, true); requested = true; }
        auto ready = worker.TakeReady();
        if (!ready) return NativeScriptServiceResult{NativeScriptServiceStatus::Pending,{}};
        Require(ready->Error.empty(), ready->Error);
        publication.Scene = std::make_shared<const WorldShotScene>(std::move(ready->Scene));
        publication.Center = request.Position; publication.Frame = ready->Frame; publication.Overrides = ready->Overrides;
        publication.SourceCollision = ready->SourceCollision;
        worker.Release();
        return NativeScriptServiceResult{NativeScriptServiceStatus::Ready,{}};
    }, [&](const auto&) { worker.Request({}, false); });
    NativeScriptServiceResult result;
    do {
        result = host.RequestCollision({{90001,1,1},p[0],p[1]});
        Require(std::chrono::steady_clock::now() < deadline, "live world timeout");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (result.Status == NativeScriptServiceStatus::Pending);
    Require(result.Status == NativeScriptServiceStatus::Ready, result.Message);
    Require(host.Publication().Overrides == overrides && Hash(*oldPublication.Scene) == oldHash && Hash(first->Scene) == oldHash, "later publication and retained older world lifetime");
    Require(oldPublication.Overrides == constructorOverrides && oldPublication.SourceCollision->Overrides == constructorOverrides &&
        std::ranges::all_of(constructorOverrides->Entries(), [](const auto& p) { return p.CollisionEnabled; }) &&
        prepared.SourceCollision->Overrides == overrides && host.Publication().SourceCollision->Overrides == overrides,
        "constructor/prepared/later source packets independently retained");
    worker.Stop(std::move(first), {});
    s_WorkerOwnsParsers = false;
    Require(s_WorkerParserCalls > 0, "wrapped real parser calls exercised on worker only");
    Require(geometry == 14 && concurrent > 0, "all real poses / concurrent owned reads");
    // After joining, reconfigure the pager to a deliberately tiny render cap.
    // The retained publications/catalog/override ownership remain independent.
    StreamPager_Shutdown();
    Require(StreamPager_Init(argv[1], info, message, sizeof(message), {true,120,8}), message);
    realtime_streaming::CpuWorld capped;
    capped.Position = {center.X,center.Y,center.Z}; capped.Build(true,context,overrides);
    Require(capped.Error.empty(), capped.Error);
    NativeCollisionSnapshot expected;
    Require(context->Snapshot(center.X,center.Y,expected,error,overrides),error);
    Require(capped.Frame.instances <= 8 && capped.SourceCollision->Instances.size() == expected.Instances.size() &&
        capped.Overrides->Entries().size() == 14 && Hash(*oldPublication.Scene) == oldHash, "render cap/residency and pager lifetime do not control overrides/COL");
    // Force one genuine door to a different grid cell: culling must use the
    // replacement position while the authored catalog/grid remain unchanged.
    auto relocatedEntry = constructorOverrides->Entries().front(); relocatedEntry.Position[0] += 2000;
    auto relocated = std::make_shared<const NativePlacementOverrides>(std::vector{relocatedEntry});
    WorldShotScene relocatedScene; E2EPagerFrame relocatedFrame; std::vector<NativePlacementIdentity> relocatedIds;
    const auto destination = relocatedEntry.Position;
    Require(StreamPager_Update(destination[0], destination[1], destination[2], relocatedScene, relocatedFrame, message, sizeof(message), relocated, &relocatedIds),message);
    Index(relocatedIds,relocatedEntry.Identity);
    Require(context->Snapshot(destination[0],destination[1],expected,error,relocated),error);
    Require(std::ranges::count_if(expected.Instances,[&](const auto& i) { return relocatedEntry.Identity.Matches(i.Placement); }) == 1, "replacement source COL follows cross-cell residency");
    Require(context->Snapshot(firstDoor.Authored.Position[0],firstDoor.Authored.Position[1],expected,error,relocated),error);
    Require(std::ranges::none_of(expected.Instances,[&](const auto& i) { return relocatedEntry.Identity.Matches(i.Placement); }), "authored COL absent from old cell after relocation");
    std::printf("placement-parser-ownership PASS workerCalls=%zu mainCallsAfterSeal=0\n", s_WorkerParserCalls.load());
    std::printf("placement-probe PASS doors=53 overrides=%zu geometry=%zu sourceCOL=%zu suppressed=%zu remainingCOL=%zu unchangedCOL=%zu sameModelUntouched=%zu concurrentReads=%zu suppressionFixture=12 sameModelFixtures=3 emptyOverrideCOL=2\n",
        overrides->Entries().size(),geometry,original.Instances.size(),suppressed,actual.Instances.size(),unchanged,sameModel,concurrent);
    StreamPager_Shutdown();
}
