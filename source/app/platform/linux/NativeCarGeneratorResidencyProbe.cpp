// Actual owned assets plus explicit metadata-only transaction/lifetime fixtures.
#include "app/platform/linux/NativeCarGeneratorResidency.h"
#include "app/platform/linux/StreamPager.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <type_traits>

using int32 = std::int32_t;
using int64 = std::int64_t;
using uint32 = std::uint32_t;
using uint64 = std::uint64_t;
#ifndef __stdcall
#define __stdcall
#endif
#include "oswrapper/oswrapper.h"

int32 OS_FileOpen(OSFileDataArea, void** output, const char* path, OSFileAccessType access) {
    if (access != FILE_ACCESS_READ) return 1;
    *output = std::fopen(path, "rb");
    return *output ? 0 : 1;
}
int32 OS_FileClose(void* file) { return std::fclose(static_cast<FILE*>(file)); }
int32 OS_FileSize(void* file) {
    const auto position = std::ftell(static_cast<FILE*>(file));
    std::fseek(static_cast<FILE*>(file), 0, SEEK_END);
    const auto size = std::ftell(static_cast<FILE*>(file));
    std::fseek(static_cast<FILE*>(file), position, SEEK_SET);
    return int32(size);
}
int32 OS_FileRead(void* file, void* destination, int32 size) {
    return std::fread(destination, 1, std::size_t(size), static_cast<FILE*>(file)) == std::size_t(size) ? 0 : 3;
}
int32 OS_FileGetPosition(void* file) { return int32(std::ftell(static_cast<FILE*>(file))); }
void OS_FileSetPosition(void* file, int32 position) { std::fseek(static_cast<FILE*>(file), position, SEEK_SET); }
void OS_SetFilePathOffset(const char*) {}

namespace {
using Status = NativeCarGeneratorResidencyStatus;
static_assert(std::is_copy_constructible_v<NativeCarGenerators>);
static_assert(!std::is_copy_assignable_v<NativeCarGenerators>);
static_assert(!std::is_move_assignable_v<NativeCarGenerators>);
static_assert(noexcept(std::declval<NativeCarGenerators&>().Swap(std::declval<NativeCarGenerators&>())));
void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
std::string Key(std::string_view source) {
    std::string key, error;
    Require(NativeCarGeneratorResidency::CanonicalKey(source, key, error), error);
    return key;
}
std::string State(const NativeCarGenerators& registry) {
    std::ostringstream out;
    out << registry.Revision() << ',' << registry.Census().Registered << ',' << registry.Events().size()
        << ',' << int(registry.ProcessCounter()) << ',' << int(registry.GenerateCloseCounter());
    for (const auto& e : registry.Entries()) {
        out << ';' << e.Used << ',' << e.ModelId << ',' << int(e.PrimaryColor) << ',' << int(e.SecondaryColor)
            << ',' << e.CompressedPosition[0] << ',' << e.CompressedPosition[1] << ',' << e.CompressedPosition[2]
            << ',' << int(e.Angle) << ',' << int(e.AlarmChance) << ',' << int(e.DoorLockChance)
            << ',' << e.WaitUntilFarFromPlayer << e.HighPriority << e.ActiveFlag << e.PlayerHasAlreadyOwnedCar
            << e.IgnorePopulationLimit << ',' << e.MinDelay << ',' << e.MaxDelay << ',' << e.NextGenerationTime
            << ',' << e.Vehicle.Value << ',' << e.GenerateCount << ',' << int(e.IplId)
            << ',' << int(e.Provenance.Kind) << ',' << e.Provenance.Source << ',' << e.Provenance.Line
            << ',' << e.Provenance.Record << ',' << e.Provenance.ContainerByteOffset
            << ',' << e.Provenance.ByteOffset << ',' << e.Provenance.ByteSize;
    }
    for (const auto& a : registry.AssetRecords()) {
        out << ';' << a.RegistryIndex << ',' << int(a.RegistrationStatus) << ',' << int(a.RegistrationIplId)
            << ',' << a.RegistrationDetail;
    }
    for (const auto& e : registry.Events()) {
        const auto& c = e.Create;
        out << ';' << e.Sequence << ',' << int(e.Kind) << ',' << e.Id.Session << ',' << e.Id.Instruction << ',' << e.Id.IP
            << ',' << e.Generator.Value << ',' << e.Count << ',' << int(e.Result.Status) << ',' << e.Result.Message
            << ',' << c.Id.Session << ',' << c.Id.Instruction << ',' << c.Id.IP
            << ',' << c.Position.X << ',' << c.Position.Y << ',' << c.Position.Z << ',' << c.AngleDegrees
            << ',' << c.ModelId << ',' << c.PrimaryColor << ',' << c.SecondaryColor << ',' << c.ForceSpawn
            << ',' << c.AlarmChance << ',' << c.DoorLockChance << ',' << c.MinDelay << ',' << c.MaxDelay
            << ',' << int(c.IplId) << ',' << c.IgnorePopulationLimit;
    }
    return out.str();
}

// TEST ONLY: fixtures never acquired model residency or real world vehicles.
// Only explicit metadata-fixture Release can complete the attached reference.
// This is not evidence of production vehicle/model cleanup or runtime readiness.
struct TestOnlyFixtureCleanup final : NativeCarGeneratorResidencyCleanup {
    NativeVehicleRef Released;
    mutable std::size_t Calls{};
    bool Complete(const NativeCarGeneratorRemovalObligation& obligation) const noexcept override {
        ++Calls;
        return obligation.Vehicle.Value == -1 || obligation.Vehicle == Released;
    }
};

void CopyChecks(NativeCarGenerators& registry, NativeCarGeneratorRef script, const char* gameDir) {
    const auto before = State(registry);
    NativeCarGenerators copy(registry);
    Require(State(copy) == before, "public copy preserves complete visible registry and journal");
    Require(copy.Entries().data() != registry.Entries().data() &&
        copy.AssetRecords().data() != registry.AssetRecords().data() &&
        copy.ModelDefinitions().data() != registry.ModelDefinitions().data() &&
        copy.Events().data() != registry.Events().data(), "all owned containers copied independently");
    copy.Resolve(script)->Provenance.Source = "TEST ONLY copy mutation";
    auto& model = const_cast<NativeCarGeneratorModelDefinition&>(copy.ModelDefinitions().front());
    const auto originalModelName = registry.ModelDefinitions().front().ModelName;
    model.ModelName = "TEST ONLY model copy mutation";
    auto& event = const_cast<NativeCarGeneratorEvent&>(copy.Events().front());
    event.Result.Message = "TEST ONLY journal copy mutation";
    auto& asset = const_cast<NativeCarGeneratorAssetRecord&>(copy.AssetRecords().front());
    asset.Provenance.Source = "TEST ONLY asset copy mutation";
    copy.RemoveIpl(std::ranges::find_if(copy.Entries(), [](const auto& e) { return e.Used && e.IplId; })->IplId);
    copy.ActivateGenerateEvenIfPlayerIsClose(3);
    Require(State(registry) == before && registry.ModelDefinitions().front().ModelName == originalModelName,
        "copy mutation leaves original containers strings allocations and counters independent");
    std::string error;
    NativeCarGenerators sealedEmpty;
    sealedEmpty.SealStartup();
    NativeCarGenerators sealedCopy(sealedEmpty);
    Require(!sealedCopy.LoadBeforeWorker(gameDir, 0, error), "sealed-only startup state survives copy");
    const auto changed = State(copy);
    registry.Swap(copy);
    Require(State(registry) == changed && State(copy) == before, "public Swap exchanges exact owned state");
    registry.Swap(registry);
    Require(State(registry) == changed, "self Swap is inert");
    registry.Swap(copy);
    Require(State(registry) == before, "public Swap restores original state");
}

void CatalogChecks(const NativeCarGenerators& registry, NativeCollisionPopulation population,
    const NativeCarGeneratorResidency& residency) {
    std::string error;
    Require(Key("C:\\Game\\MODELS\\GTA3.IMG:LAE_STREAM3.IPL") == "gta3.img:lae_stream3.ipl", "canonical absolute key");
    Require(Key("models/gta3.img:lae_stream3.ipl") == Key("/game/models/GTA3.IMG:LAE_STREAM3.IPL"), "relative/absolute keys");
    std::string key;
    Require(!NativeCarGeneratorResidency::CanonicalKey("gta3.img:dir/a.ipl", key, error), "reject malformed entry");
    std::ranges::reverse(population.Instances);
    NativeCarGeneratorResidency reversed;
    Require(reversed.Initialize(registry, population, error), error);
    Require(std::ranges::equal(reversed.Catalog(), residency.Catalog()), "stable IDs independent of placement order");
    std::set<std::uint8_t> ids;
    std::string previous;
    for (const auto& source : residency.Catalog()) {
        Require(source.IplId == ids.size() + 1 && ids.insert(source.IplId).second && previous < source.Key,
            "canonical lexical catalog assigns consecutive nonzero IDs");
        previous = source.Key;
    }
    auto duplicate = *std::ranges::find_if(population.Instances, [](const auto& p) { return p.Binary; });
    duplicate.Ipl = "/different/" + Key(duplicate.Ipl);
    population.Instances.push_back(duplicate);
    NativeCarGeneratorResidency collided;
    Require(!collided.Initialize(registry, population, error) && collided.Catalog().empty(), "reject basename collision atomically");
    population.Instances.clear();
    for (unsigned i = 0; i < 256; ++i) {
        NativeCollisionPlacement p;
        p.Binary = true;
        p.Ipl = "models/gta3.img:fixture" + std::to_string(i) + ".ipl";
        population.Instances.push_back(std::move(p));
    }
    NativeCarGeneratorResidency exhausted;
    Require(!exhausted.Initialize(registry, population, error) && exhausted.Catalog().empty(), "reject uint8 ID exhaustion");
}

void Transactions(NativeCarGenerators& registry, NativeCarGeneratorResidency& residency,
    std::shared_ptr<const NativeCollisionSnapshot> initial, const NativeCollisionContext& context, const char* gameDir) {
    std::string error;
    const auto before = State(registry);
    const auto replay = residency.Reconcile(registry, 1, initial, 999);
    Require(replay.Status == Status::Ready && replay.Replay && State(registry) == before, "idempotent committed replay");
    Require(residency.Reconcile(registry, 1, std::make_shared<NativeCollisionSnapshot>(*initial), 10).Status == Status::Error,
        "changed generation replay rejected");
    Require(State(registry) == before, "changed replay atomic");
    const auto originalActive = std::vector(residency.Active().begin(), residency.Active().end());
    NativeCarGeneratorCreateRequest script;
    script.Id = {8001, 1, 100};
    script.ModelId = 411;
    const auto scriptRef = registry.Create(script, 10).Reference;
    Require(scriptRef.Value >= 0, "SCM definition fixture");
    NativeCarGeneratorSwitchRequest scriptSwitch{{8001, 2, 101}, scriptRef, -1};
    Require(registry.Switch(scriptSwitch, 11).Status == NativeScriptServiceStatus::Ready, "SCM switch journal fixture");
    const auto scriptBefore = *registry.Resolve(scriptRef);
    registry.ActivateGenerateEvenIfPlayerIsClose(17);
    registry.SealStartup();
    CopyChecks(registry, scriptRef, gameDir);

    auto moved = std::make_shared<NativeCollisionSnapshot>();
    Require(context.Snapshot(-2000.0f, 500.0f, *moved, error), error);
    const auto preMove = State(registry);
    auto pending = residency.Reconcile(registry, 2, moved, 20);
    Require(pending.Status == Status::PendingCleanup && !pending.Removals.empty(), pending.Detail);
    Require(State(registry) == preMove && residency.Snapshot() == initial && residency.Generation() == 1, "pending move atomic");
    Require(pending.GeneratorSources == 25 && pending.Records == 262, "actual moved source-COL census 25/262");
    Require(residency.Reconcile(registry, 2, std::make_shared<NativeCollisionSnapshot>(*moved), 20).Status == Status::Error &&
        State(registry) == preMove && residency.Snapshot() == initial, "changed pending sourceSnapshot identity rejected atomically");
    const auto repeatedPending = residency.Reconcile(registry, 2, moved, 20);
    Require(repeatedPending.Status == Status::PendingCleanup && repeatedPending.Removals == pending.Removals &&
        State(registry) == preMove, "same pending attempt preserves exact cleanup obligations without publication");
    Require(residency.Reconcile(registry, 1, initial, 20).Status == Status::Error, "stale generation rejected");
    TestOnlyFixtureCleanup cleanup;
    auto committed = residency.Reconcile(registry, 2, moved, 20, &cleanup);
    Require(committed.Status == Status::Ready, committed.Detail);
    Require(registry.Resolve(scriptRef)->Provenance == scriptBefore.Provenance &&
        registry.Resolve(scriptRef)->NextGenerationTime == scriptBefore.NextGenerationTime &&
        registry.Events().size() == 2 && registry.GenerateCloseCounter() == 17, "SCM journal/static slots and counters preserved by copy");
    std::printf("move sourceCOL=%zu binarySources=%zu cargenSources=%zu records=%zu added=%zu removed=%zu obligations=%zu\n",
        moved->Instances.size(), committed.BinarySources, committed.GeneratorSources, committed.Records,
        committed.AddedSources, committed.RemovedSources, committed.Removals.size());
    auto retained = residency.Snapshot();
    auto returned = residency.Reconcile(registry, 3, initial, 30, &cleanup);
    Require(returned.Status == Status::Ready && std::ranges::equal(originalActive, residency.Active()), "return/reuse retains deterministic IDs");
    Require(retained == moved && !retained->Instances.empty(), "retained old immutable snapshot survives swap");
    const auto beforeJournalReplay = State(registry);
    Require(registry.Create(script, 100).Reference == scriptRef &&
        registry.Switch(scriptSwitch, 100).Status == NativeScriptServiceStatus::Ready &&
        State(registry) == beforeJournalReplay, "SCM create/switch journal exact replay survives swap");

    // Real source provenance with a metadata-only pool binding: this checks
    // cleanup requirements, not real vehicle construction/destruction parity.
    NativeCarGeneratorRef attached;
    for (std::size_t i = 0; i < registry.Entries().size(); ++i) {
        const auto& e = registry.Entries()[i];
        if (e.Used && e.IplId && e.ModelId >= 400) { attached = {std::int32_t(i)}; break; }
    }
    Require(attached.Value >= 0, "fixed model resident fixture");
    NativeVehiclePool pool;
    Require(pool.BindProducer(NativeVehicleProducer::CarGenerator, error) && pool.SealProducerExtent(error), error);
    NativeVehicleCreateRequest vehicle;
    vehicle.Producer = NativeVehicleProducer::CarGenerator;
    vehicle.ProducerIndex = attached.Value;
    vehicle.State.ModelId = registry.Resolve(attached)->ModelId;
    vehicle.State.Type = registry.FindModel(vehicle.State.ModelId)->Type;
    vehicle.State.SubType = std::int32_t(vehicle.State.Type);
    vehicle.State.Status = NativeVehicleStatus::Abandoned;
    vehicle.State.CreatedBy = NativeVehicleCreatedBy::Parked;
    const auto allocated = pool.Allocate(vehicle);
    Require(allocated.Result.Status == NativeScriptServiceStatus::Ready && allocated.Reference.Value >= 0, allocated.Result.Message);
    Require(registry.AttachSpawnedVehicle(attached, allocated.Reference, vehicle.State.ModelId, -1, -1, 31, pool, error), error);
    auto empty = std::make_shared<NativeCollisionSnapshot>();
    const auto preRemoval = State(registry);
    pending = residency.Reconcile(registry, 4, empty, 40, &cleanup, &pool);
    Require(pending.Status == Status::PendingCleanup && State(registry) == preRemoval, "attached vehicle removal emits pending obligation");
    Require(std::ranges::any_of(pending.Removals, [&](const auto& o) {
        return o.Generator == attached && o.Vehicle == allocated.Reference && o.ModelId == vehicle.State.ModelId && o.ReleaseModelOwnership;
    }), "exact vehicle/model cleanup obligation");
    cleanup.Released = allocated.Reference;
    Require(residency.Reconcile(registry, 4, empty, 40, &cleanup, &pool).Status == Status::PendingCleanup,
        "cleanup acknowledgment cannot bypass live pool reference");
    Require(pool.Resolve(allocated.Reference) && pool.Census().ReleasedEvents == 0 && State(registry) == preRemoval,
        "helper never releases live vehicle references or publishes partial removals");
    Require(residency.Reconcile(registry, 4, empty, 40, &cleanup).Status == Status::PendingCleanup,
        "cleanup acknowledgment without pool authority remains pending");
    Require(pool.Release(allocated.Reference, error), error); // metadata fixture only
    Require(residency.Reconcile(registry, 4, empty, 40, &cleanup, &pool).Status == Status::Ready, "completed fixture cleanup commits removal");
    Require(registry.Census().Registered == 1 && registry.Resolve(scriptRef), "all binary removed; SCM unaffected");
    Require(residency.Reconcile(registry, 5, initial, 50).Status == Status::Ready, "reactivation reuses freed slots");

    // Fault injection changes a definition only in this probe-owned registry.
    // Changing a catalogued accepted record into a source rejection after
    // Initialize must still reject the unexpected staged partial allocation.
    const auto extraSource = *std::ranges::find_if(residency.Catalog(), [&](const auto& s) {
        return s.Records > 1 && std::ranges::find(residency.Active(), s.Key, &NativeCarGeneratorResidentSource::Key) == residency.Active().end();
    });
    auto expanded = std::make_shared<NativeCollisionSnapshot>(*initial);
    NativeCollisionInstance extra;
    extra.Placement = *std::ranges::find_if(context.Population.Instances, [&](const auto& p) {
        return p.Binary && Key(p.Ipl) == extraSource.Key;
    });
    expanded->Instances.push_back(extra);
    NativeCarGeneratorAssetRecord* fault = nullptr;
    for (const auto& asset : registry.AssetRecords()) {
        if (asset.Provenance.Source == extraSource.AssetSource) fault = &const_cast<NativeCarGeneratorAssetRecord&>(asset);
    }
    Require(fault, "owned fault-injection record");
    const auto model = fault->Authored.ModelId;
    fault->Authored.ModelId = 1;
    const auto preFailure = State(registry);
    {
        NativeCarGenerators partial(registry);
        Require(partial.ActivateStreamedIpl(extraSource.AssetSource, extraSource.IplId, 60, error),
            "source activation explicitly completes a model-range rejection");
        const auto allocations = std::ranges::count_if(partial.Entries(), [&](const auto& e) {
            return e.Used && e.IplId == extraSource.IplId;
        });
        Require(allocations > 0 && std::size_t(allocations) < extraSource.Records && State(registry) == preFailure,
            "fault provably causes late partial activation only in independent staged registry");
    }
    const auto failed = residency.Reconcile(registry, 6, expanded, 60);
    Require(failed.Status == Status::Error && failed.Detail.find("incomplete staged") != std::string::npos &&
        State(registry) == preFailure && residency.Generation() == 5, "late activation failure atomic");
    Require(residency.Reconcile(registry, 6, std::make_shared<NativeCollisionSnapshot>(*expanded), 60).Status == Status::Error &&
        State(registry) == preFailure && residency.Snapshot() == initial, "changed failed-attempt replay rejected atomically");
    fault->Authored.ModelId = model;

    // A conversion error after earlier successful rows must never be excused
    // as a source model-range rejection or invoke cleanup on the real registry.
    const auto position = fault->Authored.Position;
    fault->Authored.Position.X = 5000.0f;
    const auto badCoordinates = State(registry);
    const auto cleanupCalls = cleanup.Calls;
    const auto conversion = residency.Reconcile(registry, 6, expanded, 60, &cleanup);
    Require(conversion.Status == Status::Error && conversion.Detail.find("fixed-point range") != std::string::npos &&
        State(registry) == badCoordinates && residency.Snapshot() == initial && cleanup.Calls == cleanupCalls,
        "late coordinate failure atomic before cleanup");
    {
        NativeCarGenerators partial(registry);
        Require(!partial.ActivateStreamedIpl(extraSource.AssetSource, extraSource.IplId, 60, error),
            "activation reports late conversion failure");
        Require(std::ranges::any_of(partial.AssetRecords(), [&](const auto& a) {
            return a.Provenance == fault->Provenance &&
                a.RegistrationStatus == NativeCarGeneratorRegistrationStatus::InvalidMetadata;
        }), "conversion failure retains precise typed provenance in discarded stage");
    }
    fault->Authored.Position = position;

    for (std::uint64_t sequence = 2; registry.Census().Registered < NativeCarGenerators::Capacity; ++sequence) {
        script.Id.Instruction = sequence;
        Require(registry.Create(script, 61).Reference.Value >= 0, "fill 500 slots");
    }
    const auto full = State(registry);
    const auto capacity = residency.Reconcile(registry, 6, expanded, 62, &cleanup);
    Require(capacity.Status == Status::Error && capacity.Detail.find("capacity") != std::string::npos &&
        State(registry) == full && residency.Snapshot() == initial && cleanup.Calls == cleanupCalls,
        "capacity failure preserves prior registry and snapshot before cleanup");

    auto* foreign = registry.Resolve(scriptRef);
    foreign->IplId = extraSource.IplId;
    const auto collisionState = State(registry);
    Require(residency.Reconcile(registry, 7, initial, 70).Status == Status::Error && State(registry) == collisionState,
        "SCM IPL-ID collision cannot remove foreign entry");
    foreign->IplId = 0;
    auto unknown = std::make_shared<NativeCollisionSnapshot>(*initial);
    unknown->Instances.front().Placement.Binary = true;
    unknown->Instances.front().Placement.Ipl = "models/gta3.img:unknown_source.ipl";
    const auto preUnknown = State(registry);
    Require(residency.Reconcile(registry, 8, unknown, 80).Status == Status::Error &&
        State(registry) == preUnknown && residency.Snapshot() == initial, "unknown source cannot alter committed registry");
    std::printf("transactions PASS production-public-API no-overlay move25/262 stable190ids capacity500 late-partial-rollback strict-generation sourceSnapshot-identity SCM-create/switch-copy retained-snapshot live-refs-not-released cleanup=TEST-ONLY-metadata-oracle\n");
}

void OriginChecks(const NativeCarGenerators& loaded, const NativeCollisionContext& context,
    std::shared_ptr<const NativeCollisionSnapshot> initial) {
    std::string error;
    NativeCarGenerators registry(loaded);
    NativeCarGeneratorResidency residency;
    Require(residency.Initialize(registry, context.Population, error), error);
    Require(residency.Reconcile(registry, 1, initial, 0).Status == Status::Ready, "origin initial commit");
    auto origin = std::make_shared<NativeCollisionSnapshot>();
    Require(context.Snapshot(0.0f, 0.0f, *origin, error), error);
    const auto before = State(registry);
    const auto result = residency.Reconcile(registry, 2, origin, 10);
    Require(result.Status == Status::PendingCleanup && result.GeneratorSources == 10 && result.Records == 102 &&
        result.AcceptedRecords == 97 && result.RejectedRecords == 5 && result.Removals.size() == 73 &&
        State(registry) == before && residency.Snapshot() == initial, "origin 102=97 accepted+5 rejected; pending atomic");
    TestOnlyFixtureCleanup cleanup;
    const auto committed = residency.Reconcile(registry, 2, origin, 10, &cleanup);
    Require(committed.Status == Status::Ready && registry.Census().Registered == 97 &&
        registry.Census().RegisteredAssetRecords == 97 && registry.Census().RejectedAssetRecords == 5 &&
        residency.Snapshot() == origin, committed.Detail);
    const auto committedState = State(registry);
    Require(residency.Reconcile(registry, 2, origin, 99).Replay && State(registry) == committedState,
        "source-rejected registration replays without allocation or revision");
    std::printf("origin PASS sources=%zu authored=%zu accepted=%zu rejected=%zu removals=%zu pending-atomic then fixture-cleanup commit\n",
        committed.GeneratorSources, committed.Records, committed.AcceptedRecords, committed.RejectedRecords, committed.Removals.size());
    NativeCarGenerators staged(registry);
    for (const auto& source : residency.Active()) staged.RemoveIpl(source.IplId);
    std::set<std::string> keys;
    for (const auto& instance : origin->Instances) if (instance.Placement.Binary) keys.insert(Key(instance.Placement.Ipl));
    for (const auto& source : residency.Catalog()) {
        if (!source.Records || !keys.contains(source.Key)) continue;
        Require(staged.ActivateStreamedIpl(source.AssetSource, source.IplId, 10, error), error);
        for (const auto& asset : staged.AssetRecords()) {
            if (asset.Provenance.Source != source.AssetSource || asset.RegistryIndex >= 0) continue;
            const auto& a = asset.Authored;
            Require(asset.RegistrationStatus == NativeCarGeneratorRegistrationStatus::RejectedModelRange &&
                asset.RegistrationIplId == source.IplId && asset.Provenance.Source == "models/gta3.img:vegass_stream0.ipl" &&
                (a.ModelId == 2 || a.ModelId == 4), "real rejected registration retains typed source provenance");
            NativeCarGeneratorCreateRequest request;
            request.Position = a.Position;
            request.AngleDegrees = a.AngleRadians * (180.0f / 3.14159265358979323846f);
            request.ModelId = a.ModelId;
            NativeCarGenerators isolated;
            const auto rejected = isolated.Create(request, 10);
            std::printf("rejected source=%s record=%u container=%llu offset=%llu size=%u model=%d xyz=%.9g,%.9g,%.9g angle=%.9g colors=%d,%d flags=%u alarm=%d lock=%d delays=%d,%d IDE=%d reason=%s\n",
                source.AssetSource.c_str(), asset.Provenance.Record,
                (unsigned long long)asset.Provenance.ContainerByteOffset, (unsigned long long)asset.Provenance.ByteOffset,
                asset.Provenance.ByteSize, a.ModelId, a.Position.X, a.Position.Y, a.Position.Z, a.AngleRadians,
                a.PrimaryColor, a.SecondaryColor, a.Flags, a.AlarmChance, a.DoorLockChance, a.MinDelay, a.MaxDelay,
                asset.ModelDefinitionResolved, rejected.Result.Message.c_str());
        }
    }
    Require(residency.Reconcile(registry, 3, initial, 20, &cleanup).Status == Status::Ready &&
        registry.Census().Registered == 88 && registry.Census().RejectedAssetRecords == 0,
        "unload resets rejected registrations without fabricated allocation/removal");
    Require(residency.Reconcile(registry, 4, origin, 30, &cleanup).Status == Status::Ready &&
        registry.Census().RejectedAssetRecords == 5, "rejected provenance reactivates deterministically");

    std::size_t authored = 0, accepted = 0, rejected = 0;
    NativeCarGenerators census(loaded);
    for (const auto& source : residency.Catalog()) {
        if (!source.Records) continue;
        Require(census.ActivateStreamedIpl(source.AssetSource, source.IplId, 0, error), error);
        authored += source.Records;
        accepted += census.Census().RegisteredAssetRecords;
        rejected += census.Census().RejectedAssetRecords;
        if (census.Census().RejectedAssetRecords) {
            std::printf("catalog-rejections source=%s authored=%zu accepted=%zu rejected=%zu\n", source.AssetSource.c_str(),
                source.Records, census.Census().RegisteredAssetRecords, census.Census().RejectedAssetRecords);
        }
        Require(census.RemoveIpl(source.IplId) == source.AllocatableRecords(), "full source census remove allocations only");
    }
    std::printf("complete source catalog=190 authored=%zu accepted=%zu rejected-model-range=%zu (per-source activation, not simultaneous)\n",
        authored, accepted, rejected);
    Require(authored == 1045 && accepted == 1037 && rejected == 8, "complete real source acceptance census");
}

void RejectionCounterexamples(const NativeCarGenerators& loaded, const NativeCollisionContext& context) {
    std::string error;
    NativeCarGenerators registry(loaded);
    std::map<std::string, std::size_t> counts;
    for (const auto& asset : registry.AssetRecords()) ++counts[asset.Provenance.Source];
    const auto mixed = std::ranges::find_if(counts, [](const auto& item) { return item.second > 2; })->first;
    const auto rejectedOnly = std::ranges::find_if(counts, [&](const auto& item) {
        return item.first != mixed && item.second > 1;
    })->first;
    bool first = true;
    for (const auto& asset : registry.AssetRecords()) {
        if (asset.Provenance.Source != mixed && asset.Provenance.Source != rejectedOnly) continue;
        auto& fixture = const_cast<NativeCarGeneratorAssetRecord&>(asset);
        fixture.Authored.ModelId = asset.Provenance.Source == mixed && first ? 630 : 2;
        if (asset.Provenance.Source == mixed) first = false;
        fixture.ModelDefinitionResolved = false;
    }
    // 630 passes original Create's range despite not being a vehicles.ide model.
    // Neither missing IDE nor a rejected neighbour may silently erase its slot.
    Require(!registry.FindModel(630), "source upper bound has no vehicle IDE definition");
    NativeCarGeneratorResidency residency;
    Require(residency.Initialize(registry, context.Population, error), error);
    auto snapshot = std::make_shared<NativeCollisionSnapshot>();
    const auto addSource = [&](NativeCollisionSnapshot& target, const std::string& source) {
        NativeCollisionInstance instance;
        instance.Placement = *std::ranges::find_if(context.Population.Instances, [&](const auto& p) {
            return p.Binary && Key(p.Ipl) == Key(source);
        });
        target.Instances.push_back(std::move(instance));
    };
    addSource(*snapshot, mixed);
    NativeCarGeneratorCreateRequest request;
    request.ModelId = 411;
    for (std::size_t i = 0; i < 499; ++i) {
        request.Id = {9901, i + 1, 1};
        Require(registry.Create(request, 0).Reference.Value >= 0, "fill 499 counterexample slots");
    }
    const auto result = residency.Reconcile(registry, 1, snapshot, 1);
    Require(result.Status == Status::Ready && result.AcceptedRecords == 1 &&
        result.RejectedRecords == counts[mixed] - 1 && registry.Census().Registered == 500 &&
        registry.Resolve({499})->ModelId == 630 && registry.Events().size() == 499,
        "one capacity slot accepts unknown-IDE 630 and records source-invalid neighbours explicitly");
    auto expanded = std::make_shared<NativeCollisionSnapshot>(*snapshot);
    addSource(*expanded, rejectedOnly);
    const auto full = residency.Reconcile(registry, 2, expanded, 2);
    Require(full.Status == Status::Ready && full.AcceptedRecords == 1 &&
        full.RejectedRecords == counts[mixed] + counts[rejectedOnly] - 1 && registry.Census().Registered == 500,
        "source model rejection precedes capacity test even at all 500 slots occupied");
    const auto removedRejects = residency.Reconcile(registry, 3, snapshot, 3);
    Require(removedRejects.Status == Status::Ready && removedRejects.Removals.empty() &&
        registry.Census().Registered == 500 && registry.Census().RejectedAssetRecords == counts[mixed] - 1,
        "rejected-only IPL unload needs no fake vehicle cleanup and resets typed registration");
    // A forged rejection for accepted 630 is not a legal way around allocation validation.
    auto& forged = const_cast<NativeCarGeneratorAssetRecord&>(*std::ranges::find_if(registry.AssetRecords(),
        [](const auto& a) { return a.RegistryIndex == 499; }));
    forged.RegistrationStatus = NativeCarGeneratorRegistrationStatus::RejectedModelRange;
    const auto before = State(registry);
    Require(residency.Reconcile(registry, 4, snapshot, 4).Status == Status::Error && State(registry) == before &&
        residency.Snapshot() == snapshot && residency.Generation() == 3, "typed rejection cannot hide valid foreign/live allocation");
    std::printf("counterexamples PASS 630-missing-IDE-allocated mixed-rejections capacity500 rejected-only-unload forged-rejection-atomic\n");
}
} // namespace

int main(int argc, char** argv) try {
    Require(argc == 2, "usage: NativeCarGeneratorResidencyProbe GAME_DIR");
    struct Shutdown { ~Shutdown() { StreamPager_Shutdown(); } } shutdown;
    char buffer[512]{};
    E2ELoadInfo load;
    Require(StreamPager_Init(argv[1], load, buffer, sizeof(buffer),
        {.includeStreamed = true, .radius = 900.0f, .maxInstances = 4096}), buffer);
    std::string error;
    NativeCarGenerators registry;
    Require(registry.LoadBeforeWorker(argv[1], 0, error), error);
    auto context = NativeCollisionContext::LoadBeforeWorker(argv[1], 900.0f, error);
    Require(bool(context), error);
    NativeCarGeneratorResidency residency;
    Require(residency.Initialize(registry, context->Population, error), error);
    CatalogChecks(registry, context->Population, residency);
    constexpr NativeScriptPosition center{2488.562255859375f, -1666.864501953125f, 13.3757f};
    auto initial = std::make_shared<NativeCollisionSnapshot>();
    Require(context->Snapshot(center.X, center.Y, *initial, error), error);
    OriginChecks(registry, *context, initial);
    RejectionCounterexamples(registry, *context);
    NativeCarGenerators wrongOwner(registry);
    Require(residency.Reconcile(wrongOwner, 1, initial, 0).Status == Status::Error && !residency.Snapshot(),
        "foreign registry rejected before publication");
    Require(residency.Reconcile(registry, 0, initial, 0).Status == Status::Error &&
        residency.Reconcile(registry, 1, nullptr, 0).Status == Status::Error && registry.Census().Registered == 0,
        "zero generation and missing sourceSnapshot rejected atomically");
    WorldShotScene scene;
    E2EPagerFrame frame;
    std::vector<NativePlacementIdentity> rendered;
    Require(StreamPager_Update(center.X, center.Y, center.Z, scene, frame, buffer, sizeof(buffer), {}, &rendered), buffer);
    std::set<std::string> renderSources;
    for (const auto& p : rendered) if (p.Binary) renderSources.insert(Key(p.Ipl));
    std::size_t renderGeneratorSources = 0, renderRecords = 0;
    for (const auto& source : residency.Catalog()) {
        if (source.Records && renderSources.contains(source.Key)) { ++renderGeneratorSources; renderRecords += source.Records; }
    }
    const auto initialResult = residency.Reconcile(registry, 1, initial, 0);
    Require(residency.Catalog().size() == 190 && registry.Census().DeferredAssetRecords == 1045 &&
        registry.Census().TextIplFiles == 52 && registry.Census().StartupAssetRecords == 0, "actual catalog census");
    Require(initialResult.BinarySources == 35 && initialResult.GeneratorSources == 22 && initialResult.Records == 88 &&
        renderGeneratorSources == 15 && renderRecords == 63 && frame.instances == 4096, "actual initial source-COL vs render counts");
    std::printf("actual catalog=190 deferred=1045 textIpls=52 static=0 render=%d renderSources=%zu/%zu sourceCOL=%zu binarySources=%zu cargenSources=%zu records=%zu\n",
        frame.instances, renderGeneratorSources, renderRecords, initial->Instances.size(), initialResult.BinarySources,
        initialResult.GeneratorSources, initialResult.Records);
    Require(initialResult.Status == Status::Ready && registry.Census().Registered == 88, initialResult.Detail);
    for (const auto& source : residency.Active()) std::printf("active id=%u key=%s records=%zu\n", unsigned(source.IplId), source.Key.c_str(), source.Records);
    Transactions(registry, residency, initial, *context, argv[1]);
    std::weak_ptr<const NativeCollisionSnapshot> lifetime;
    {
        NativeCarGenerators owned;
        Require(owned.LoadBeforeWorker(argv[1], 0, error), error);
        NativeCarGeneratorResidency owner;
        Require(owner.Initialize(owned, context->Population, error), error);
        auto snapshot = std::make_shared<NativeCollisionSnapshot>(*initial);
        lifetime = snapshot;
        Require(owner.Reconcile(owned, 1, snapshot, 0).Status == Status::Ready, "lifetime commit");
        snapshot.reset();
        Require(!lifetime.expired() && owner.Snapshot()->Instances.size() == initial->Instances.size(), "owner retains immutable source-COL");
    }
    Require(lifetime.expired(), "snapshot released with residency owner");
    std::printf("NativeCarGeneratorResidencyProbe PASS production-public-API no-overlay native-source-COL-approximation; real runtime cleanup remains required\n");
    return 0;
} catch (const std::exception& exception) {
    std::fprintf(stderr, "NativeCarGeneratorResidencyProbe FAIL %s\n", exception.what());
    return 2;
}
