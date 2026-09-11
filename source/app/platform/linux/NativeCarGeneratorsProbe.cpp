// Isolated source registry/process oracle. No VM wiring or vehicle-spawn claim.
#include "app/platform/linux/NativeCarGenerators.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <string>
#include <vector>

using int32 = std::int32_t;
using int64 = std::int64_t;
using uint32 = std::uint32_t;
using uint64 = std::uint64_t;
#ifndef __stdcall
#define __stdcall
#endif
#include "oswrapper/oswrapper.h"

int32 OS_FileOpen(OSFileDataArea, void** output, const char* path, OSFileAccessType access) {
    *output = std::fopen(path, access == FILE_ACCESS_READ ? "rb" : access == FILE_ACCESS_WRITE ? "wb" : "rb+");
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
void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

template<typename T>
T Read(std::span<const std::uint8_t> bytes, std::size_t& position, std::uint8_t tag) {
    const auto size = tag == 4 ? 1u : tag == 5 ? 2u : 4u;
    Require(position + size <= bytes.size(), "actual 014B operand extent");
    std::uint32_t value = 0;
    for (unsigned i = 0; i < size; ++i) value |= std::uint32_t(bytes[position++]) << (8 * i);
    if constexpr (std::is_same_v<T, float>) return std::bit_cast<float>(value);
    if (tag == 4) return T(std::int8_t(value));
    if (tag == 5) return T(std::int16_t(value));
    return T(std::bit_cast<std::int32_t>(value));
}

struct Actual014B {
    NativeCarGeneratorCreateRequest Request;
    std::uint16_t Output = 0;
};

Actual014B ActualMission014B(const char* gameDir) {
    const std::string path = std::string(gameDir) + "/data/script/main.scm";
    std::ifstream file(path, std::ios::binary);
    Require(bool(file), "open actual main.scm");
    // Retail mission 0 starts at file 194125; VM mission IP 207007 is file
    // 201132. These are probe-only numeric assertions, not production dispatch.
    file.seekg(201132);
    std::vector<std::uint8_t> bytes(43);
    Require(bool(file.read(reinterpret_cast<char*>(bytes.data()), bytes.size())), "read actual first 014B");
    std::size_t p = 0;
    const auto u16 = [&] { const auto v = std::uint16_t(bytes[p] | std::uint16_t(bytes[p + 1]) << 8); p += 2; return v; };
    Require(u16() == 0x014b, "actual mission opcode 014B at VM IP 207007");
    Actual014B actual;
    actual.Request.Id = {7001, 680, 207007};
    float* floats[]{&actual.Request.Position.X, &actual.Request.Position.Y, &actual.Request.Position.Z,
        &actual.Request.AngleDegrees};
    for (auto* value : floats) {
        Require(bytes[p++] == 6, "actual 014B float tag");
        *value = Read<float>(bytes, p, 6);
    }
    int* integers[]{&actual.Request.ModelId, &actual.Request.PrimaryColor, &actual.Request.SecondaryColor,
        &actual.Request.ForceSpawn, &actual.Request.AlarmChance, &actual.Request.DoorLockChance,
        &actual.Request.MinDelay, &actual.Request.MaxDelay};
    for (auto* value : integers) {
        const auto tag = bytes[p++];
        Require(tag == 4 || tag == 5, "actual 014B compact integer tag");
        *value = Read<int>(bytes, p, tag);
    }
    Require(bytes[p++] == 2, "actual 014B output global tag");
    actual.Output = u16();
    Require(p == bytes.size(), "actual 014B exact 43-byte extent");
    return actual;
}

NativeCarGeneratorCreateRequest CreateRequest(std::uint64_t instruction, std::int32_t model = 400,
    NativeScriptPosition position = {}) {
    NativeCarGeneratorCreateRequest request;
    request.Id = {8000, instruction, std::uint32_t(1000 + instruction)};
    request.Position = position;
    request.ModelId = model;
    return request;
}

NativeCarGeneratorObservation Observation(NativeCarGeneratorRef generator,
    NativeCarGeneratorVisibility visibility = NativeCarGeneratorVisibility::HiddenOrOccluded,
    NativeCarGeneratorBlockage blockage = NativeCarGeneratorBlockage::Clear,
    NativeCarGeneratorGround ground = NativeCarGeneratorGround::Hit) {
    return {generator, visibility, blockage, ground, 0.0f};
}

NativeCarGeneratorProcessInput Input(const NativeVehiclePool* pool = nullptr) {
    NativeCarGeneratorProcessInput input;
    input.TimeMs = 4;
    input.PlayerCenter = {150.0f, 0.0f, 0.0f};
    input.Camera = input.PlayerCenter;
    input.Vehicles = pool;
    return input;
}

const NativeCarGeneratorAction& Find(const NativeCarGeneratorProcessFrame& frame, NativeCarGeneratorRef reference) {
    const auto found = std::ranges::find_if(frame.Actions, [&](const auto& action) { return action.Generator == reference; });
    Require(found != frame.Actions.end(), "processed generator action");
    return *found;
}

NativeVehicleState VehicleState() {
    NativeVehicleState state;
    state.ModelId = 430;
    state.Type = NativeVehicleType::Boat;
    state.SubType = 5;
    state.Status = NativeVehicleStatus::Abandoned;
    state.CreatedBy = NativeVehicleCreatedBy::Parked;
    return state;
}

void ParserBoundaries() {
    NativeCarGeneratorFileRecord record;
    std::string error;
    Require(NativeCarGenerators::ParseTextRecord("1, 2, -100, 3.1415927, 430, -1, 7, 3, 100, 25, 0, 10000", record, error), error);
    Require(record.Position.Z == -100 && record.ModelId == 430 && record.Flags == 3 && record.MaxDelay == 10000,
        "text IPL all twelve fields");
    const auto before = record;
    Require(!NativeCarGenerators::ParseTextRecord("1 2 3", record, error) && record == before,
        "text parser failure atomic");

    std::vector<std::uint8_t> binary(0x4c + 0x30);
    std::memcpy(binary.data(), "bnry", 4);
    const auto put = [&](std::size_t offset, std::uint32_t value) {
        for (unsigned i = 0; i < 4; ++i) binary[offset + i] = std::uint8_t(value >> (8 * i));
    };
    put(20, 1); put(60, 0x4c);
    const float values[]{1, 2, -100, 3.1415927f};
    for (unsigned i = 0; i < 4; ++i) put(0x4c + i * 4, std::bit_cast<std::uint32_t>(values[i]));
    put(0x4c + 16, 430); put(0x4c + 20, std::uint32_t(-1)); put(0x4c + 24, 7);
    put(0x4c + 28, 3); put(0x4c + 32, 100); put(0x4c + 36, 25); put(0x4c + 44, 10000);
    std::vector<NativeCarGeneratorAssetRecord> parsed;
    Require(NativeCarGenerators::ParseBinaryIpl(binary, "fixture.img:one.ipl", 4096, parsed, error), error);
    Require(parsed.size() == 1 && parsed[0].Authored == record && parsed[0].Provenance.ContainerByteOffset == 4096 &&
        parsed[0].Provenance.ByteOffset == 0x4c && parsed[0].Provenance.ByteSize == 0x30,
        "binary IPL explicit record/provenance boundaries");
    binary.pop_back();
    Require(!NativeCarGenerators::ParseBinaryIpl(binary, "bad", 0, parsed, error) && parsed.size() == 1,
        "binary IPL undersized car section failure atomic");
}

void AllocationReplayAndCounters() {
    NativeCarGenerators registry;
    Require(registry.Census().Registered == 0 && registry.ProcessCounter() == 0 &&
        registry.GenerateCloseCounter() == 0 && registry.Revision() == 0,
        "source registry/counter initial conditions");
    const auto firstRequest = CreateRequest(1);
    const auto first = registry.Create(firstRequest, 10);
    Require(first.Result.Status == NativeScriptServiceStatus::Ready && first.Reference.Value == 0,
        first.Result.Message);
    const auto revision = registry.Revision();
    Require(registry.Create(firstRequest, 999).Reference == first.Reference && registry.Revision() == revision,
        "014B request replay does not rerun timer/allocation");
    auto changed = firstRequest; changed.ModelId = 401;
    Require(registry.Create(changed, 10).Result.Status == NativeScriptServiceStatus::Error &&
        registry.Census().Registered == 1, "014B changed replay rejected");
    Require(registry.Switch({{8001, 1, 1}, first.Reference, 100}, 20).Status == NativeScriptServiceStatus::Ready &&
        registry.Resolve(first.Reference)->GenerateCount == 100 &&
        registry.Resolve(first.Reference)->NextGenerationTime == 24,
        "014C count at finite source boundary and next-time four");
    Require(registry.Switch({{8001, 2, 2}, first.Reference, 101}, 20).Status == NativeScriptServiceStatus::Ready &&
        registry.Resolve(first.Reference)->GenerateCount == std::numeric_limits<std::uint16_t>::max(),
        "014C count above 100 means source infinite");
    Require(registry.Switch({{8001, 3, 3}, first.Reference, 0}, 20).Status == NativeScriptServiceStatus::Ready &&
        registry.Resolve(first.Reference)->GenerateCount == 0,
        "014C zero switches source generator off");
    auto invalid = CreateRequest(2, 399);
    Require(registry.Create(invalid, 10).Result.Status == NativeScriptServiceStatus::Ready &&
        registry.Create(invalid, 10).Reference.Value == -1, "source invalid model returns generator -1");

    NativeCarGenerators capacity;
    for (std::size_t i = 0; i < NativeCarGenerators::Capacity; ++i) {
        const auto created = capacity.Create(CreateRequest(i + 1), 0);
        Require(created.Reference.Value == std::int32_t(i), "source first-free generator allocation");
    }
    const auto full = capacity.Create(CreateRequest(1000), 0);
    Require(full.Result.Status == NativeScriptServiceStatus::Ready && full.Reference.Value == -1 &&
        capacity.Census().Registered == NativeCarGenerators::Capacity, "source generator capacity 500");

    NativeCarGenerators timer;
    timer.Create(CreateRequest(1), 0);
    const auto timerRef = timer.Create(CreateRequest(2), 0).Reference;
    timer.Switch({{8050, 1, 1}, timerRef, 101}, 0);
    auto timerInput = Input(); timerInput.TimeMs = 3;
    Require(Find(timer.Process(timerInput), timerRef).Decision == NativeCarGeneratorDecision::Timer,
        "source next-generation time is inclusive and not reached at three");

    NativeCarGenerators counters;
    std::vector<NativeCarGeneratorRef> refs;
    for (unsigned i = 0; i < 4; ++i) {
        const auto created = counters.Create(CreateRequest(i + 1), 0);
        refs.push_back(created.Reference);
        Require(counters.Switch({{8100, i + 1, std::uint32_t(2000 + i)}, created.Reference, 101}, 0).Status ==
            NativeScriptServiceStatus::Ready, "activate counter fixture");
    }
    auto input = Input(); input.TimeMs = 3; input.PlayerInTrain = true;
    counters.ActivateGenerateEvenIfPlayerIsClose();
    auto frame = counters.Process(input);
    Require(frame.ProcessCounterAfter == 0 && frame.GenerateCloseCounterAfter == 20 && frame.Actions.empty(),
        "train gate freezes both source counters");
    input.PlayerInTrain = false;
    NativeCarGeneratorModelRuntime carModel{400, NativeVehicleType::Automobile, true, true};
    auto counterObservation = Observation(refs[0]);
    input.Models = std::span(&carModel, 1);
    input.Observations = std::span(&counterObservation, 1);
    for (unsigned expected = 1; expected <= 3; ++expected) {
        frame = counters.Process(input);
        Require(frame.ProcessCounterAfter == expected && frame.GenerateCloseCounterAfter == 20 - expected,
            "source quarter-pool counter timestep");
    }
    frame = counters.Process(input);
    Require(frame.ProcessCounterAfter == 0 && frame.GenerateCloseCounterAfter == 16 &&
        Find(frame, refs[0]).Requirement == NativeCarGeneratorRequirement::VehiclePool,
        "source counter wraps four to zero and bypasses the timer gate");
}

void RangeAndConsumerGates() {
    NativeCarGenerators hidden;
    hidden.Create(CreateRequest(1), 0);
    const auto boat = hidden.Create(CreateRequest(2, 430), 0).Reference;
    hidden.Switch({{8200, 1, 1}, boat, 101}, 0);
    NativeCarGeneratorModelRuntime boatModel{430, NativeVehicleType::Boat, true, true};
    auto input = Input(); input.PlayerCenter.X = 200; input.Camera = input.PlayerCenter;
    auto observation = Observation(boat, NativeCarGeneratorVisibility::HiddenOrOccluded);
    input.Models = std::span(&boatModel, 1); input.Observations = std::span(&observation, 1);
    auto frame = hidden.Process(input);
    Require(Find(frame, boat).Decision == NativeCarGeneratorDecision::TooFar,
        "hidden/occluded boat outside 160 source range rejected");

    NativeCarGenerators visible;
    visible.Create(CreateRequest(1), 0);
    const auto visibleBoat = visible.Create(CreateRequest(2, 430), 0).Reference;
    visible.Switch({{8201, 1, 1}, visibleBoat, 101}, 0);
    input = Input(); input.PlayerCenter.X = 200; input.Camera = input.PlayerCenter;
    observation = Observation(visibleBoat, NativeCarGeneratorVisibility::VisibleUnoccluded);
    input.Models = std::span(&boatModel, 1); input.Observations = std::span(&observation, 1);
    frame = visible.Process(input);
    Require(Find(frame, visibleBoat).Requirement == NativeCarGeneratorRequirement::VehiclePool &&
        Find(frame, visibleBoat).Result.Status == NativeScriptServiceStatus::Unsupported,
        "visible boat reaches typed owned-pool requirement, not a fake vehicle");

    NativeCarGenerators blocked;
    blocked.Create(CreateRequest(1), 0);
    const auto blockedBoat = blocked.Create(CreateRequest(2, 430), 0).Reference;
    blocked.Switch({{8202, 1, 1}, blockedBoat, 101}, 0);
    input = Input(); observation = Observation(blockedBoat, NativeCarGeneratorVisibility::HiddenOrOccluded,
        NativeCarGeneratorBlockage::Blocked);
    input.Models = std::span(&boatModel, 1); input.Observations = std::span(&observation, 1);
    frame = blocked.Process(input);
    Require(Find(frame, blockedBoat).Decision == NativeCarGeneratorDecision::Blocked &&
        blocked.Resolve(blockedBoat)->WaitUntilFarFromPlayer &&
        blocked.Resolve(blockedBoat)->NextGenerationTime == 8,
        "source blockage sets wait flag and adds exactly four milliseconds");

    NativeVehiclePool pool;
    std::string error;
    Require(pool.BindProducer(NativeVehicleProducer::CarGenerator, error), error);
    Require(pool.SealProducerExtent(error), error);
    for (std::size_t i = 0; i < NativeVehiclePool::Capacity; ++i) {
        const auto result = pool.Allocate({.Producer = NativeVehicleProducer::CarGenerator,
            .ScriptRequest = {}, .ProducerIndex = std::int32_t(i), .State = VehicleState()});
        Require(result.Result.Status == NativeScriptServiceStatus::Ready, result.Result.Message);
    }
    NativeCarGenerators full;
    full.Create(CreateRequest(1), 0);
    const auto fullBoat = full.Create(CreateRequest(2, 430), 0).Reference;
    full.Switch({{8203, 1, 1}, fullBoat, 101}, 0);
    input = Input(&pool); observation = Observation(fullBoat);
    input.Models = std::span(&boatModel, 1); input.Observations = std::span(&observation, 1);
    frame = full.Process(input);
    Require(Find(frame, fullBoat).Requirement == NativeCarGeneratorRequirement::VehiclePoolCapacity &&
        Find(frame, fullBoat).Result.Status == NativeScriptServiceStatus::Pending &&
        pool.Census().Alive == NativeVehiclePool::Capacity,
        "full source vehicle pool is typed Pending without allocation/discard");

    NativeVehiclePool lifecyclePool;
    Require(lifecyclePool.BindProducer(NativeVehicleProducer::CarGenerator, error), error);
    Require(lifecyclePool.SealProducerExtent(error), error);
    NativeCarGenerators lifecycle;
    lifecycle.Create(CreateRequest(1), 0);
    const auto lifecycleGenerator = lifecycle.Create(CreateRequest(2, 430), 0).Reference;
    const auto vehicle = lifecyclePool.Allocate({.Producer = NativeVehicleProducer::CarGenerator,
        .ScriptRequest = {}, .ProducerIndex = lifecycleGenerator.Value, .State = VehicleState()});
    Require(vehicle.Result.Status == NativeScriptServiceStatus::Ready &&
        lifecycle.AttachSpawnedVehicle(lifecycleGenerator, vehicle.Reference, 430, -1, -1, 10,
            lifecyclePool, error), error);
    input = Input(&lifecyclePool);
    frame = lifecycle.Process(input);
    Require(Find(frame, lifecycleGenerator).Decision == NativeCarGeneratorDecision::HasVehicle,
        "generator resolves its exact generation-bearing owned vehicle reference");
    auto playerVehicle = VehicleState(); playerVehicle.Status = NativeVehicleStatus::Player;
    Require(lifecyclePool.Update(vehicle.Reference, playerVehicle, error), error);
    for (unsigned i = 0; i < 4; ++i) frame = lifecycle.Process(input);
    Require(Find(frame, lifecycleGenerator).Requirement == NativeCarGeneratorRequirement::PlayerVehicleTransition &&
        lifecycle.CommitPlayerVehicleTransition(lifecycleGenerator, lifecyclePool, error) &&
        lifecycle.Resolve(lifecycleGenerator)->Vehicle.Value == -1 &&
        lifecycle.Resolve(lifecycleGenerator)->WaitUntilFarFromPlayer &&
        lifecycle.Resolve(lifecycleGenerator)->NextGenerationTime == 60014,
        "player claim is explicit before source reference detach/timer transition");

    NativeCarGenerators random;
    random.Create(CreateRequest(1), 0);
    const auto randomRef = random.Create(CreateRequest(2, -1), 0).Reference;
    random.Switch({{8204, 1, 1}, randomRef, 101}, 0);
    input = Input(); observation = Observation(randomRef);
    input.Observations = std::span(&observation, 1); input.Models = {};
    frame = random.Process(input);
    Require(Find(frame, randomRef).Requirement == NativeCarGeneratorRequirement::RandomPopulationSelection &&
        !random.CommitRandomPopulationSelection(randomRef, 430, NativeVehicleType::Boat, 7.0f, error) &&
        !random.CommitRandomPopulationSelection(randomRef, 400, NativeVehicleType::Automobile, 8.01f, error) &&
        random.CommitRandomPopulationSelection(randomRef, 400, NativeVehicleType::Automobile, 8.0f, error),
        "random model uses explicit source RNG boundary and exact boat/length filters");
    NativeCarGeneratorModelRuntime cachedModel{400, NativeVehicleType::Automobile, false, true};
    input.Models = std::span(&cachedModel, 1);
    for (unsigned i = 0; i < 4; ++i) frame = random.Process(input);
    Require(random.Resolve(randomRef)->ModelId == -400 &&
        Find(frame, randomRef).Requirement != NativeCarGeneratorRequirement::ModelKeepInMemory,
        "resident cached negative model bypasses fixed-model RequestModel path");
}

void ActualAssetsAndMission(const char* gameDir) {
    NativeCarGenerators registry;
    std::string error;
    Require(registry.LoadBeforeWorker(gameDir, 0, error), error);
    const auto census = registry.Census();
    Require(census.StartupOrderProven && census.StartupAssetRecords == 0 && census.Registered == 0 &&
        census.StartupDisposition == NativeCarGeneratorStartupDisposition::ProvenEmptyTextIplsBinaryDeferred &&
        census.Definitions == 212 && census.TextIplFiles == 52 && census.BinaryIplFiles == 190 &&
        census.DeferredAssetRecords == 1045,
        "actual pre-script text IPL registry is proven empty, not inferred from a missing loader");
    const auto binaryAsset = std::ranges::find_if(registry.AssetRecords(), [](const auto& asset) {
        return asset.Phase == NativeCarGeneratorAssetPhase::DeferredStreaming;
    });
    Require(binaryAsset != registry.AssetRecords().end() && binaryAsset->RegistryIndex == -1 &&
        binaryAsset->ModelDefinitionResolved && binaryAsset->Provenance.ContainerByteOffset &&
        binaryAsset->Provenance.Record && binaryAsset->Provenance.ByteSize == 0x30,
        "actual binary car-generator remains deferred with archive/record boundaries");
    const auto* rustler = registry.FindModel(476);
    Require(rustler && rustler->ModelName == "rustler" && rustler->Type == NativeVehicleType::Plane,
        "actual vehicles.ide model 476 metadata");
    const auto streamedSource = binaryAsset->Provenance.Source;
    Require(registry.ActivateStreamedIpl(streamedSource, 7, 0, error), error);
    const auto streamedCount = registry.Census().Registered;
    Require(streamedCount && std::ranges::all_of(registry.Entries(), [](const auto& entry) {
        return !entry.Used || entry.IplId == 7;
    }), "actual binary IPL records activate only on explicit streamed load");
    Require(registry.RemoveIpl(7) == streamedCount && registry.Census().Registered == 0,
        "actual streamed IPL unload removes its generator records");

    const auto actual = ActualMission014B(gameDir);
    Require(actual.Request.Position.X == 325.1199951171875f && actual.Request.Position.Y == 2537.10009765625f &&
        actual.Request.Position.Z == 17.520000457763672f && actual.Request.AngleDegrees == 180.0f &&
        actual.Request.ModelId == 476 && actual.Request.PrimaryColor == -1 && actual.Request.SecondaryColor == -1 &&
        actual.Request.ForceSpawn == 1 && actual.Request.AlarmChance == 0 && actual.Request.DoorLockChance == 0 &&
        actual.Request.MinDelay == 0 && actual.Request.MaxDelay == 10000 && actual.Output == 7824,
        "actual mission first 014B typed operands/output at 207007");
    const auto before = registry.Census().Registered;
    const auto created = registry.Create(actual.Request, 0);
    Require(before == 0 && created.Result.Status == NativeScriptServiceStatus::Ready && created.Reference.Value == 0,
        created.Result.Message);
    const auto* state = registry.Resolve(created.Reference);
    Require(state && state->Position() == NativeScriptPosition{325.0f, 2537.0f, 17.5f} &&
        state->Angle == std::int8_t(-128) && state->HeadingRadians() == -std::numbers::pi_v<float> &&
        state->ModelId == 476 && state->PrimaryColor == -1 && state->SecondaryColor == -1 &&
        state->HighPriority && !state->PlayerHasAlreadyOwnedCar && !state->IgnorePopulationLimit &&
        state->AlarmChance == 0 && state->DoorLockChance == 0 && state->MinDelay == 0 &&
        state->MaxDelay == 10000 && state->NextGenerationTime == 1 && state->GenerateCount == 0 &&
        state->Vehicle.Value == -1 && state->IplId == 0 && state->Used,
        "source 014B constructor oracle stores every field without spawning");
    const auto revision = registry.Revision();
    Require(registry.Create(actual.Request, 500).Reference == created.Reference && registry.Revision() == revision,
        "actual 014B constructor request replay");
    const NativeCarGeneratorSwitchRequest first014C{{7001, 683, 207136}, created.Reference, 0};
    Require(registry.Switch(first014C, 0).Status == NativeScriptServiceStatus::Ready &&
        registry.Resolve(created.Reference)->GenerateCount == 0,
        "actual next 014C count-zero switch contract");
    std::printf("actual cargens definitions=%zu startup=proven-empty static=%zu registered=%zu binaryRecords=%zu textIpls=%zu binaryIpls=%zu first014B=%d output=%u\n",
        census.Definitions, census.StartupAssetRecords, census.Registered, census.DeferredAssetRecords,
        census.TextIplFiles, census.BinaryIplFiles, created.Reference.Value, actual.Output);
}
} // namespace

int main(int argc, char** argv) try {
    Require(argc == 2, "usage: NativeCarGeneratorsProbe GAME_DIR");
    ParserBoundaries();
    AllocationReplayAndCounters();
    RangeAndConsumerGates();
    ActualAssetsAndMission(argv[1]);
    std::printf("NativeCarGeneratorsProbe PASS registry500 processQuartered actual014B=constructor-oracle-not-VM-executed spawn=typed-unsupported rand=shared-source-required\n");
    return 0;
} catch (const std::exception& exception) {
    std::fprintf(stderr, "NativeCarGeneratorsProbe FAIL %s\n", exception.what());
    return 2;
}
