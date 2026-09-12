#include "app/platform/linux/NativeScriptPortableSave.h"

#include "app/platform/linux/NativeScriptSession.h"

#include <algorithm>
#include <bit>
#include <cerrno>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace {
using Bytes = std::vector<std::uint8_t>;
std::size_t s_Checks = 0;

void Check(bool condition, const char* message) {
    ++s_Checks;
    if (!condition) {
        std::fprintf(stderr, "native-script-portable-save FAIL: %s\n", message);
        std::exit(1);
    }
}

void Put(Bytes& bytes, std::uint32_t value, unsigned count) {
    for (unsigned i = 0; i < count; ++i) bytes.push_back(std::uint8_t(value >> (8 * i)));
}
void Patch(Bytes& bytes, std::size_t offset, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bytes.at(offset + i) = std::uint8_t(value >> (8 * i));
}
void Op(Bytes& bytes, std::uint16_t value) { Put(bytes, value, 2); }
void I8(Bytes& bytes, std::int8_t value) { bytes.push_back(4); Put(bytes, std::uint8_t(value), 1); }
void I32(Bytes& bytes, std::int32_t value) { bytes.push_back(1); Put(bytes, std::uint32_t(value), 4); }
void Local(Bytes& bytes, std::uint16_t value) { bytes.push_back(3); Put(bytes, value, 2); }
void Global(Bytes& bytes, std::uint16_t value) { bytes.push_back(2); Put(bytes, value, 2); }
void EndArguments(Bytes& bytes) { bytes.push_back(0); }

std::uint32_t CodeStart() { return 136; } // six fixed headers + one mission + one streamed definition

Bytes Fixture(const Bytes& code, const Bytes& mission, std::uint32_t streamSize) {
    Bytes result;
    auto chunk = [&](std::uint8_t index, const Bytes& payload) {
        const auto next = std::uint32_t(result.size() + payload.size() + 8);
        Op(result, 2); I32(result, std::int32_t(next)); result.push_back(index);
        result.insert(result.end(), payload.begin(), payload.end());
    };
    chunk(115, Bytes(16));
    chunk(0, Bytes(4));
    Bytes missions(20);
    const auto mainSize = CodeStart() + std::uint32_t(code.size());
    Patch(missions, 0, mainSize); Patch(missions, 4, std::uint32_t(mission.size()));
    Patch(missions, 8, 1); Patch(missions, 12, 1024); Patch(missions, 16, mainSize);
    chunk(1, missions);
    Bytes streams(36);
    Patch(streams, 0, streamSize); Patch(streams, 4, 1);
    const char name[] = "SAVE_STREAM";
    std::copy_n(reinterpret_cast<const std::uint8_t*>(name), sizeof(name) - 1, streams.begin() + 8);
    Patch(streams, 28, 0); Patch(streams, 32, streamSize);
    chunk(2, streams);
    chunk(3, Bytes(4));
    Bytes extra(8); Patch(extra, 0, 16); chunk(4, extra);
    Check(result.size() == CodeStart(), "generated portable header size");
    result.insert(result.end(), code.begin(), code.end());
    result.insert(result.end(), mission.begin(), mission.end());
    return result;
}

struct FixtureBytes { Bytes Main, External; };

FixtureBytes MakeFixture(bool changedSource = false) {
    Bytes external;
    Op(external, 0x0050); const auto externalTarget = external.size() + 1; I32(external, 0);
    Op(external, 0x0006); Local(external, 1); I32(external, 401);
    Op(external, 0x004E);
    const auto externalSub = std::uint32_t(external.size());
    Op(external, 0x0001); I8(external, 3); Op(external, 0x0051);
    Patch(external, externalTarget, std::uint32_t(-std::int32_t(externalSub)));

    Bytes mission;
    Op(mission, 0x0050); const auto missionTarget = mission.size() + 1; I32(mission, 0);
    Op(mission, 0x0006); Local(mission, 1); I32(mission, 301);
    Op(mission, 0x004E);
    const auto missionSub = std::uint32_t(mission.size());
    Op(mission, 0x0001); I8(mission, 4); Op(mission, 0x0051);
    Patch(mission, missionTarget, std::uint32_t(-std::int32_t(missionSub)));

    Bytes code;
    Op(code, 0x0050); const auto mainTarget = code.size() + 1; I32(code, 0);
    Op(code, 0x0004); Global(code, 12); I32(code, 123);
    Op(code, 0x0001); I8(code, 2);
    Op(code, 0x004E);
    const auto mainSub = std::uint32_t(code.size());
    Op(code, 0x0006); Local(code, 0); I32(code, changedSource ? 12 : 11);
    Op(code, 0x06C8); I8(code, 1);
    Op(code, 0x01F0); I8(code, 3);
    Op(code, 0x00C0); I8(code, 7); I8(code, 45);
    Op(code, 0x016A); I32(code, 1000); I8(code, 0);
    Op(code, 0x0629); I8(code, 42); I8(code, 17);
    Op(code, 0x0746); I8(code, 0); I8(code, 1); I8(code, 2);
    Op(code, 0x0913); I8(code, 0); Local(code, 0); EndArguments(code);
    Op(code, 0x0417); I8(code, 0);
    Op(code, 0x0001); I8(code, 10);
    Op(code, 0x0051);
    Patch(code, mainTarget, CodeStart() + mainSub);
    return {Fixture(code, mission, std::uint32_t(external.size())), external};
}

struct Services final : NativeScriptServices {
    NativeScriptServiceResult RequestCollision(const NativeScriptCollisionRequest&) override { return {}; }
    NativeScriptServiceResult LoadScene(const NativeScriptSceneRequest&) override { return {}; }
    NativeScriptServiceResult CreatePlayer(const NativeScriptPlayerRequest&) override { return {}; }
};

struct PublicGraph {
    NativeScriptState State;
    std::vector<NativeScriptThreadState> Threads;
    std::vector<NativeScriptStreamedState> Streamed;
    std::vector<std::int32_t> Globals;
    std::uint64_t Session = 0;
    bool Pass = false;
    bool operator==(const PublicGraph&) const = default;
};

PublicGraph Graph(const NativeScriptSession& session) {
    PublicGraph graph;
    graph.State = session.State();
    graph.Threads.assign(session.Threads().begin(), session.Threads().end());
    graph.Streamed.assign(session.StreamedScripts().begin(), session.StreamedScripts().end());
    for (std::uint32_t offset = 8; offset < 8 + session.Metadata().GlobalBytes; offset += 4) {
        std::int32_t value = 0;
        Check(session.ReadGlobal(std::uint16_t(offset), value), "read portable graph global");
        graph.Globals.push_back(value);
    }
    graph.Session = session.SessionId();
    graph.Pass = session.PassOutstanding();
    return graph;
}

Bytes Canonical(const NativeScriptSession& session) {
    Bytes bytes;
    NativeScriptPortableSaveInfo info;
    std::string error;
    Check(NativeScriptPortableSave::Encode(session, bytes, info, error) == NativeScriptPortableSaveStatus::Ok,
        "encode canonical public owner graph");
    return bytes;
}

void Prepare(NativeScriptSession& session, const FixtureBytes& fixture) {
    std::string error;
    Check(session.LoadMainBytes(fixture.Main, fixture.Main.size(), error), "load portable fixture");
    Check(session.LoadStreamedScriptBytes(0, fixture.External, error), "load portable streamed payload");
}

void ReachSavePoint(NativeScriptSession& session) {
    Services services;
    const auto first = session.RunPass(services, 100);
    Check(first.Status == NativeScriptStatus::Waiting && first.Executed == 17, "main reaches portable save wait");
    const auto second = session.RunPass(services, 100);
    Check(second.Status == NativeScriptStatus::Waiting && second.Executed == 4, "child threads reach portable save wait");
    Check(session.Threads().size() == 3 && session.Threads()[0].StackDepth == 1 &&
        session.Threads()[1].IsExternal && session.Threads()[1].StackDepth == 1 &&
        session.Threads()[2].ThisMustBeTheOnlyMissionRunning && session.Threads()[2].StackDepth == 1,
        "save point owns main mission streamed stacks");
    Check(session.StreamedScripts()[0].Users == 1 && session.State().AlreadyRunningMission &&
        session.State().LaRiotsEnabled && session.State().MaximumWantedLevel == 3 &&
        session.State().Clock.Hours == 7 && session.State().Clock.Minutes == 45 &&
        session.State().FloatStats[42] == 17.0f && session.State().Relationships[1][0] == (1u << 2),
        "save point owns globals policies clock stats relationships");
}

struct ContinueEvent {
    std::uint32_t Time = 0, Thread = 0;
    std::uint16_t Opcode = 0;
    bool operator==(const ContinueEvent&) const = default;
};
struct Sink final : NativeScriptCommitSink {
    std::vector<ContinueEvent> Events;
    void OnScriptCommit(NativeScriptRequestId, std::size_t thread, const NativeScriptState& state,
        const NativeScriptThreadState& script) noexcept override {
        Events.push_back({state.TimeMs, std::uint32_t(thread), script.LastOpcode});
    }
};

struct Continued {
    PublicGraph Graph;
    std::vector<ContinueEvent> Events;
    std::array<std::size_t, 4> Executed{};
    bool operator==(const Continued&) const = default;
};

Continued Continue(NativeScriptSession& session) {
    Services services;
    Sink sink;
    Continued continued;
    const std::array<std::uint32_t, 4> times{3, 4, 10, 12};
    const std::array<std::size_t, 4> expected{3, 3, 3, 1};
    for (std::size_t i = 0; i < times.size(); ++i) {
        std::string error;
        Check(session.AdvanceTime(times[i], error), "advance restored graph time");
        const auto result = session.RunPass(services, 100, &sink);
        Check(result.Status == NativeScriptStatus::Waiting && result.Executed == expected[i],
            "restored graph deterministic pass");
        continued.Executed[i] = result.Executed;
    }
    Check(std::ranges::all_of(session.Threads(), [](const auto& thread) { return !thread.Active; }) &&
        session.StreamedScripts()[0].Users == 0 && !session.State().AlreadyRunningMission,
        "restored graph releases mission and streamed users");
    std::int32_t global = 0;
    Check(session.ReadGlobal(12, global) && global == 123, "restored main returns and writes global");
    continued.Graph = Graph(session);
    continued.Graph.Session = 0; // process-local owner epoch intentionally differs
    continued.Events = std::move(sink.Events);
    return continued;
}

Bytes ReadFile(const char* path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    Check(bool(file), "open portable envelope for read");
    const auto size = file.tellg();
    Check(size > 0, "portable envelope has bytes");
    Bytes bytes((std::size_t(size)));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(bytes.size()));
    Check(bool(file), "read complete portable envelope");
    return bytes;
}

void WriteFile(const char* path, const Bytes& bytes) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    Check(bool(file), "open portable envelope for write");
    file.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
    Check(bool(file), "write complete portable envelope");
}

int WriteMode(const char* path) {
    const auto fixture = MakeFixture();
    std::string error;
    std::vector<std::uint8_t> rejected{9, 8, 7};
    NativeScriptPortableSaveInfo rejectedInfo{8, 8};
    NativeScriptSession unloaded;
    Check(NativeScriptPortableSave::Encode(unloaded, rejected, rejectedInfo, error) ==
        NativeScriptPortableSaveStatus::NotLoaded && rejected == Bytes({9, 8, 7}) &&
        rejectedInfo == NativeScriptPortableSaveInfo{8, 8}, "unloaded owner rejects without changing output");
    NativeScriptSession busy;
    Prepare(busy, fixture);
    Services services;
    Check(busy.RunPass(services, 1).Status == NativeScriptStatus::BudgetYield && busy.PassOutstanding(),
        "open scheduler pass fixture");
    Check(NativeScriptPortableSave::Encode(busy, rejected, rejectedInfo, error) ==
        NativeScriptPortableSaveStatus::Busy && rejected == Bytes({9, 8, 7}) &&
        rejectedInfo == NativeScriptPortableSaveInfo{8, 8}, "open pass cannot be serialized");
    Bytes terminate; Op(terminate, 0x004E);
    Bytes underflow; Op(underflow, 0x0051);
    const auto faultPayload = Fixture(underflow, terminate, std::uint32_t(terminate.size()));
    NativeScriptSession faulted;
    Check(faulted.LoadMainBytes(faultPayload, faultPayload.size(), error) &&
        faulted.Run(services, 100).Status == NativeScriptStatus::Error,
        "faulted scheduler fixture");
    Check(NativeScriptPortableSave::Encode(faulted, rejected, rejectedInfo, error) ==
        NativeScriptPortableSaveStatus::InvalidState && rejected == Bytes({9, 8, 7}) &&
        rejectedInfo == NativeScriptPortableSaveInfo{8, 8}, "faulted owner cannot be serialized");

    NativeScriptSession source;
    Prepare(source, fixture);
    ReachSavePoint(source);
    const auto sourceGraph = Graph(source);
    std::vector<std::uint8_t> envelope{1, 2, 3};
    NativeScriptPortableSaveInfo info{9, 9};
    const auto encoded = NativeScriptPortableSave::Encode(source, envelope, info, error);
    if (encoded != NativeScriptPortableSaveStatus::Ok)
        std::fprintf(stderr, "portable encode status=%u error=%s\n", unsigned(encoded), error.c_str());
    Check(encoded == NativeScriptPortableSaveStatus::Ok, "encode portable owner graph");
    Check(info.Major == 1 && info.Minor == 0 && info.Bytes == envelope.size() && info.PayloadBytes + 80 == info.Bytes &&
        info.Globals == 4 && info.Threads == 3 && info.ActiveThreads == 3 && info.StreamedScripts == 1 &&
        info.SourceFingerprint && info.SchemaFingerprint && info.PayloadChecksum,
        "portable envelope inspectable identity");
    Check(std::search(envelope.begin(), envelope.end(), fixture.External.begin(), fixture.External.end()) == envelope.end(),
        "portable envelope contains no streamed script asset bytes");
    NativeScriptPortableSaveInfo inspected;
    Check(NativeScriptPortableSave::Inspect(envelope, inspected, error) == NativeScriptPortableSaveStatus::Ok &&
        inspected == info && Graph(source) == sourceGraph, "inspection does not mutate source graph");
    WriteFile(path, envelope);
    std::printf("portable-save-writer-ok checks=%zu bytes=%zu threads=3 active=3 globals=4\n", s_Checks, envelope.size());
    return 0;
}

int ReadMode(const char* path) {
    const auto envelope = ReadFile(path);
    const auto fixture = MakeFixture();
    std::string error;
    NativeScriptSession throwaway;
    Check(throwaway.LoadMainBytes(fixture.Main, fixture.Main.size(), error), "consume a process-local session epoch");
    NativeScriptSession restored;
    Prepare(restored, fixture);
    const auto destinationSession = restored.SessionId();
    const auto before = Graph(restored);
    const auto beforeCanonical = Canonical(restored);
    NativeScriptPortableSaveInfo sentinel{7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7};

    auto truncated = envelope; truncated.pop_back();
    auto info = sentinel;
    Check(NativeScriptPortableSave::Restore(restored, truncated, info, error) == NativeScriptPortableSaveStatus::Truncated &&
        info == sentinel && Graph(restored) == before, "truncation retains destination and output");
    auto version = envelope; version[12] = 2;
    Check(NativeScriptPortableSave::Restore(restored, version, info, error) == NativeScriptPortableSaveStatus::UnsupportedVersion &&
        info == sentinel && Graph(restored) == before, "version mismatch retains destination");
    auto checksum = envelope; checksum.back() ^= 0x80;
    Check(NativeScriptPortableSave::Restore(restored, checksum, info, error) == NativeScriptPortableSaveStatus::ChecksumMismatch &&
        Graph(restored) == before, "checksum mismatch retains destination");
    auto trailing = envelope; trailing.push_back(0);
    Check(NativeScriptPortableSave::Restore(restored, trailing, info, error) == NativeScriptPortableSaveStatus::InvalidEnvelope &&
        Graph(restored) == before, "trailing bytes retain destination");
    Check(Canonical(restored) == beforeCanonical, "all damaged envelopes retain the complete canonical destination graph");

    NativeScriptSession missingStream;
    Check(missingStream.LoadMainBytes(fixture.Main, fixture.Main.size(), error), "load destination without streamed asset");
    const auto missingBefore = Graph(missingStream);
    const auto missingCanonical = Canonical(missingStream);
    Check(NativeScriptPortableSave::Restore(missingStream, envelope, info, error) == NativeScriptPortableSaveStatus::SourceMismatch &&
        Graph(missingStream) == missingBefore && Canonical(missingStream) == missingCanonical,
        "missing external asset retains destination");
    auto changedStreamFixture = fixture;
    changedStreamFixture.External[11] ^= 1;
    NativeScriptSession wrongStream;
    Prepare(wrongStream, changedStreamFixture);
    const auto wrongStreamBefore = Graph(wrongStream);
    const auto wrongStreamCanonical = Canonical(wrongStream);
    Check(NativeScriptPortableSave::Restore(wrongStream, envelope, info, error) ==
        NativeScriptPortableSaveStatus::SourceMismatch && Graph(wrongStream) == wrongStreamBefore &&
        Canonical(wrongStream) == wrongStreamCanonical,
        "changed streamed payload retains destination");
    const auto changed = MakeFixture(true);
    NativeScriptSession wrongSource;
    Prepare(wrongSource, changed);
    const auto wrongBefore = Graph(wrongSource);
    const auto wrongCanonical = Canonical(wrongSource);
    Check(NativeScriptPortableSave::Restore(wrongSource, envelope, info, error) == NativeScriptPortableSaveStatus::SourceMismatch &&
        Graph(wrongSource) == wrongBefore && Canonical(wrongSource) == wrongCanonical,
        "changed SCM source retains destination");

    Check(NativeScriptPortableSave::Restore(restored, envelope, info, error) == NativeScriptPortableSaveStatus::Ok,
        "restore portable owner graph in restarted process");
    Check(restored.SessionId() == destinationSession && restored.SessionId() != throwaway.SessionId() &&
        restored.Threads().size() == 3 && restored.Threads()[0].StackDepth == 1 &&
        restored.Threads()[1].IsExternal && restored.Threads()[2].ThisMustBeTheOnlyMissionRunning &&
        restored.StreamedScripts()[0].Users == 1, "restart preserves graph but allocates no stale session identity");
    std::vector<std::uint8_t> canonical;
    NativeScriptPortableSaveInfo canonicalInfo;
    Check(NativeScriptPortableSave::Encode(restored, canonical, canonicalInfo, error) == NativeScriptPortableSaveStatus::Ok &&
        canonical == envelope && canonicalInfo == info, "restored graph re-encodes byte-identically");

    NativeScriptSession baseline;
    Prepare(baseline, fixture);
    ReachSavePoint(baseline);
    const auto expected = Continue(baseline);
    const auto actual = Continue(restored);
    Check(actual == expected && actual.Executed == std::array<std::size_t, 4>{3, 3, 3, 1} &&
        actual.Events.size() == 10, "restarted graph continuation is semantically identical");
    std::printf("portable-save-reader-ok checks=%zu restart-session=%llu canonical=%zu continuation=3,3,3,1\n",
        s_Checks, static_cast<unsigned long long>(destinationSession), envelope.size());
    return 0;
}

int Spawn(const char* executable, const char* mode, const char* path) {
    const auto child = fork();
    if (child == 0) {
        execl(executable, executable, mode, path, static_cast<char*>(nullptr));
        std::fprintf(stderr, "exec failed: %s\n", std::strerror(errno));
        _exit(127);
    }
    Check(child > 0, "fork restarted-process fixture");
    int status = 0;
    Check(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
        "restarted child process succeeds");
    return status;
}
} // namespace

int main(int argc, char** argv) {
    if (argc == 3 && std::string(argv[1]) == "--write") return WriteMode(argv[2]);
    if (argc == 3 && std::string(argv[1]) == "--read") return ReadMode(argv[2]);
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <portable-envelope-path>\n", argv[0]);
        return 2;
    }
    Spawn(argv[0], "--write", argv[1]);
    Spawn(argv[0], "--read", argv[1]);
    std::printf("sa-core-portable-save-ok checks=%zu restart=exec envelope=v1 owner=script-session "
        "asset-bytes=external corruption=truncation,version,checksum,source continuation=equal\n", s_Checks);
    return 0;
}
