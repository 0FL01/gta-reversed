// Test-only mock services. This probe proves VM behavior, NOT world/player boot.
#include "app/platform/linux/NativeScriptEntities.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <utility>

namespace {
using Bytes = std::vector<std::uint8_t>;
using Status = NativeScriptStatus;
using ServiceStatus = NativeScriptServiceStatus;
constexpr std::uint32_t FixtureCode = 104;
std::size_t s_Checks = 0;

void Check(bool condition, const char* message) {
    ++s_Checks;
    if (!condition) { std::fprintf(stderr, "native-script-probe FAIL: %s\n", message); std::exit(1); }
}

void Put(Bytes& bytes, std::uint32_t value, unsigned count) {
    for (unsigned i = 0; i < count; ++i) bytes.push_back(std::uint8_t(value >> (i * 8)));
}
void Patch(Bytes& bytes, std::size_t offset, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bytes.at(offset + i) = std::uint8_t(value >> (i * 8));
}
void Op(Bytes& b, std::uint16_t opcode) { Put(b, opcode, 2); }
void I8(Bytes& b, std::int8_t v) { b.push_back(4); Put(b, std::uint8_t(v), 1); }
void I16(Bytes& b, std::int16_t v) { b.push_back(5); Put(b, std::uint16_t(v), 2); }
void I32(Bytes& b, std::int32_t v) { b.push_back(1); Put(b, std::uint32_t(v), 4); }
void F(Bytes& b, float value) { b.push_back(6); Put(b, std::bit_cast<std::uint32_t>(value), 4); }
void Var(Bytes& b, std::uint16_t offset, bool global = true) { b.push_back(global ? 2 : 3); Put(b, offset, 2); }
void Array(Bytes& b, bool global, std::uint16_t base, bool globalIndex, std::uint16_t indexVar, std::uint8_t count, bool floating = false) {
    b.push_back(global ? 7 : 8); Put(b, base, 2); Put(b, indexVar, 2);
    Put(b, count, 1); Put(b, (globalIndex ? 0x80 : 0) | (floating ? 1 : 0), 1);
}

// Entirely generated metadata/code, never cut/pasted asset bytes.
Bytes Fixture(const Bytes& code, const std::vector<Bytes>& missions = {}, const std::vector<std::array<char,24>>& used = {}) {
    Bytes b;
    auto chunk = [&](std::uint8_t index, const Bytes& payload) {
        const auto next = std::uint32_t(b.size() + payload.size() + 8);
        Op(b, 2); I32(b, std::int32_t(next)); b.push_back(index);
        b.insert(b.end(), payload.begin(), payload.end());
    };
    chunk(115, Bytes(16)); // globals: offsets 8..23
    Bytes objects; Put(objects,std::uint32_t(used.size()),4);
    for (const auto& name:used) objects.insert(objects.end(),name.begin(),name.end());
    chunk(0, objects);
    Bytes info(16);
    const auto codeStart = FixtureCode + std::uint32_t(missions.size()) * 4 + std::uint32_t(used.size()) * 24;
    const auto mainSize = codeStart + std::uint32_t(code.size());
    Patch(info, 0, mainSize);
    Patch(info, 8, std::uint32_t(missions.size()));
    std::uint32_t offset = mainSize, largest = 0;
    for (const auto& mission : missions) {
        Put(info, offset, 4);
        offset += std::uint32_t(mission.size());
        largest = std::max(largest, std::uint32_t(mission.size()));
    }
    Patch(info, 4, largest);
    Patch(info, 12, missions.empty() ? 0 : 1024);
    chunk(1, info);
    chunk(2, Bytes(8));
    chunk(3, Bytes(4));
    Bytes extra(8); Patch(extra, 0, 16);
    chunk(4, extra);
    Check(b.size() == codeStart, "independent generated header size");
    b.insert(b.end(), code.begin(), code.end());
    for (const auto& mission : missions) b.insert(b.end(), mission.begin(), mission.end());
    return b;
}

struct MockServices final : NativeScriptServices {
    std::array<ServiceStatus, 3> Mode{ServiceStatus::Ready, ServiceStatus::Ready, ServiceStatus::Ready};
    std::array<unsigned, 3> Calls{}, Completions{};
    std::array<NativeScriptRequestId, 3> LastId{};
    std::vector<NativeScriptRequestId> ReadyIds;
    NativeScriptCollisionRequest Collision;
    NativeScriptSceneRequest Scene;
    NativeScriptPlayerRequest Player;
    bool Throw = false;
    std::array<ServiceStatus, 4> NewMode{ServiceStatus::Ready, ServiceStatus::Ready, ServiceStatus::Ready, ServiceStatus::Ready};
    std::array<unsigned, 4> NewCalls{}, NewCompletions{};
    std::array<NativeScriptRequestId, 4> NewLastId{};
    std::vector<unsigned> NewOrder;
    NativeScriptGroupRef Group{0x00030002}; // TEST generation 3, group slot 2
    NativeScriptPedRef Ped{0x00000503}; // TEST pool slot 5, generation 3
    NativeScriptHeadingRequest Heading;
    float PedHeading = 0.25f, CameraHeading = -1;
    bool InVehicle = false;
    std::array<ServiceStatus, 6> EntityMode{ServiceStatus::Unsupported, ServiceStatus::Unsupported, ServiceStatus::Unsupported, ServiceStatus::Unsupported, ServiceStatus::Unsupported, ServiceStatus::Unsupported};
    std::array<unsigned, 6> EntityCalls{}, EntityCompletions{};
    std::array<NativeScriptRequestId, 6> EntityIds{};
    NativeScriptEntryExitFlagRequest EntryExit;
    NativeScriptGarageRequest Garage;
    NativeScriptRestartRequest Restart;
    ServiceStatus RestartMode = ServiceStatus::Unsupported;
    unsigned RestartCalls = 0;
    NativeScriptServiceResult AddRestart(const NativeScriptRestartRequest& r) override {
        Restart = r; ++RestartCalls;
        if (Throw) throw std::runtime_error("TEST-ONLY restart exception");
        return {RestartMode,"TEST-ONLY restart service"};
    }
    ServiceStatus GarageMode = ServiceStatus::Unsupported;
    unsigned GarageCalls = 0;
    NativeScriptPickupRequest Pickup;
    ServiceStatus PickupMode = ServiceStatus::Unsupported;
    unsigned PickupCalls = 0;
    std::int32_t PickupReference = 0x00010020;
    NativeScriptPickupReferenceRequest PickupOperation;
    std::array<ServiceStatus, 2> PickupOperationMode{ServiceStatus::Unsupported, ServiceStatus::Unsupported};
    std::array<unsigned, 2> PickupOperationCalls{}, PickupOperationCompletions{};
    std::array<NativeScriptRequestId, 2> PickupOperationIds{};
    bool PickupCollected = false;
    NativeScriptServiceResult PickupOperationRespond(unsigned kind, const NativeScriptPickupReferenceRequest& r) {
        PickupOperation = r; ++PickupOperationCalls[kind]; PickupOperationIds[kind] = r.Id;
        if (Throw) throw std::runtime_error("TEST-ONLY pickup operation exception");
        if (PickupOperationMode[kind] == ServiceStatus::Ready &&
            std::find(ReadyIds.begin(), ReadyIds.end(), r.Id) == ReadyIds.end()) {
            ReadyIds.push_back(r.Id); ++PickupOperationCompletions[kind];
        }
        return {PickupOperationMode[kind], "TEST-ONLY pickup operation service"};
    }
    NativeScriptPickupCollectedResult HasPickupBeenCollected(const NativeScriptPickupReferenceRequest& r) override {
        return {PickupOperationRespond(0, r), PickupCollected};
    }
    NativeScriptServiceResult RemoveScriptPickup(const NativeScriptPickupReferenceRequest& r) override {
        return PickupOperationRespond(1, r);
    }
    NativeScriptReferenceResult<NativeScriptPickupRef> CreatePickup(const NativeScriptPickupRequest& r) override {
        Pickup = r; ++PickupCalls;
        if (Throw) throw std::runtime_error("TEST-ONLY pickup service exception");
        return {{PickupMode,"TEST-ONLY pickup service"}, {PickupReference}};
    }
    NativeScriptServiceResult DeactivateGarage(const NativeScriptGarageRequest& r) override {
        Garage=r; ++GarageCalls;
        if (Throw) throw std::runtime_error("TEST-ONLY garage service exception");
        return {GarageMode,"TEST-ONLY garage service"};
    }
    NativeScriptServiceResult SetEntryExitFlag(const NativeScriptEntryExitFlagRequest& r) override {
        EntryExit = r; return EntityRespond(3, r.Id);
    }
    NativeScriptLockedPropertyRequest Property;
    NativeScriptForSalePropertyRequest Sale;
    NativeScriptReferenceResult<NativeScriptPickupRef> CreateForSaleProperty(const NativeScriptForSalePropertyRequest& r) override {
        Sale = r; return {EntityRespond(4, r.Id), {EntityReference}};
    }
    NativeScriptContactBlipRequest Blip;
    NativeScriptCoordinateBlipRequest Coordinate;
    NativeScriptBlipDisplayRequest Display;
    std::int32_t EntityReference = 0x00020001;
    NativeScriptServiceResult EntityRespond(unsigned kind, NativeScriptRequestId id) {
        ++EntityCalls[kind]; EntityIds[kind] = id;
        if (Throw) throw std::runtime_error("test-only entity service exception");
        if (EntityMode[kind] == ServiceStatus::Ready && std::find(ReadyIds.begin(), ReadyIds.end(), id) == ReadyIds.end()) {
            ReadyIds.push_back(id); ++EntityCompletions[kind];
        }
        return {EntityMode[kind], "test-only entity service"};
    }
    NativeScriptReferenceResult<NativeScriptPickupRef> CreateLockedProperty(const NativeScriptLockedPropertyRequest& r) override {
        Property = r; return {EntityRespond(0, r.Id), {EntityReference}};
    }
    NativeScriptReferenceResult<NativeScriptBlipRef> CreateContactBlip(const NativeScriptContactBlipRequest& r) override {
        Blip = r; return {EntityRespond(1, r.Id), {EntityReference}};
    }
    NativeScriptReferenceResult<NativeScriptBlipRef> CreateCoordinateBlip(const NativeScriptCoordinateBlipRequest& r) override {
        Coordinate = r; return {EntityRespond(5, r.Id), {EntityReference}};
    }
    NativeScriptServiceResult SetBlipDisplay(const NativeScriptBlipDisplayRequest& r) override {
        Display = r; return EntityRespond(2, r.Id);
    }

    NativeScriptServiceResult NewRespond(unsigned kind, NativeScriptRequestId id) {
        ++NewCalls[kind];
        NewLastId[kind] = id;
        if (Throw) throw std::runtime_error("test-only new service exception");
        if (NewMode[kind] == ServiceStatus::Ready && std::find(ReadyIds.begin(), ReadyIds.end(), id) == ReadyIds.end()) {
            ReadyIds.push_back(id);
            ++NewCompletions[kind];
            NewOrder.push_back(kind);
            if (kind == 2) CameraHeading = PedHeading;
            if (kind == 3 && !InVehicle) PedHeading = Heading.Radians;
        }
        return {NewMode[kind], "test-only typed mock"};
    }
    NativeScriptReferenceResult<NativeScriptGroupRef> GetPlayerGroup(const NativeScriptPlayerLookupRequest& request) override {
        Check(request.PlayerIndex == 0, "mock group resolves player index");
        return {NewRespond(0, request.Id), Group};
    }
    NativeScriptReferenceResult<NativeScriptPedRef> GetPlayerChar(const NativeScriptPlayerLookupRequest& request) override {
        Check(request.PlayerIndex == 0, "mock char resolves player index");
        return {NewRespond(1, request.Id), Ped};
    }
    NativeScriptServiceResult SetCameraBehindPlayer(const NativeScriptCameraRequest& request) override {
        return NewRespond(2, request.Id);
    }
    NativeScriptServiceResult SetCharHeading(const NativeScriptHeadingRequest& request) override {
        Heading = request;
        Check(request.Ped.Value == Ped.Value, "mock heading resolves generation-bearing ped ref");
        return NewRespond(3, request.Id);
    }

    NativeScriptServiceResult Respond(unsigned kind, NativeScriptRequestId id) {
        ++Calls[kind];
        LastId[kind] = id;
        if (Throw) throw std::runtime_error("test-only service exception");
        if (Mode[kind] == ServiceStatus::Ready && std::find(ReadyIds.begin(), ReadyIds.end(), id) == ReadyIds.end()) {
            ReadyIds.push_back(id);
            ++Completions[kind];
        }
        return {Mode[kind], "test-only mock"};
    }
    NativeScriptServiceResult RequestCollision(const NativeScriptCollisionRequest& request) override {
        Collision = request; return Respond(0, request.Id);
    }
    NativeScriptServiceResult LoadScene(const NativeScriptSceneRequest& request) override {
        Scene = request; return Respond(1, request.Id);
    }
    NativeScriptServiceResult CreatePlayer(const NativeScriptPlayerRequest& request) override {
        Player = request; return Respond(2, request.Id);
    }
};

void LoadFixture(NativeScriptSession& session, MockServices& services, const Bytes& code) {
    const auto fixture = Fixture(code);
    std::string error;
    Check(session.LoadMainBytes(fixture, fixture.size(), error), error.c_str());
    const auto result = session.Run(services, 6);
    Check(result.Status == Status::BudgetYield && result.Executed == 6, "six actual header GOTOs");
    Check(session.State().IP == FixtureCode, "fixture body boundary");
}

Bytes ServiceCode(unsigned kind, std::uint16_t output = 8, bool global = true) {
    Bytes b;
    if (kind == 0) { Op(b, 0x04E4); F(b, 1.5f); F(b, -2.0f); }
    if (kind == 1) { Op(b, 0x03CB); F(b, 1.5f); F(b, -2.0f); F(b, 12.5f); }
    if (kind == 2) { Op(b, 0x0053); I8(b, 0); F(b, 1.5f); F(b, -2.0f); F(b, 12.0f); Var(b, output, global); }
    return b;
}

void RealAsset(const char* gameDir) {
    NativeScriptSession session;
    MockServices services;
    std::string error;
    Check(session.LoadMain(gameDir, error), error.c_str());
    const auto& metadata = session.Metadata();
    Check(metadata.MainSize == 194125 && metadata.CodeStart == 55976 && metadata.GlobalBytes == 43800, "owned SCM size/global facts");
    Check(metadata.MissionOffsets.size() == 135 && metadata.MissionOffsets.front() == 194125, "mission file offsets");
    Check(metadata.LargestMission == 68439 && metadata.MissionLocals == 964, "mission capacities");
    Check(metadata.StreamedScripts == 79 && metadata.LargestStreamed == 35122 && metadata.Build == 569, "streamed metadata");
    Check(session.AdvanceTime(123, error), "set native game time");
    // Derived numeric diagnostics from the strict read-only schema probe, not a
    // copy of the runtime dispatch table. No borrowed SCM payloads are fixtures.
    constexpr std::uint32_t ips[] = {
        0, 43808, 53156, 53720, 55948, 55960, 55976, 55987, 55993,
        55998, 56003, 56008, 56012, 56016, 56022, 56034, 56051,
        56061, 56070, 56079, 56089, 56096, 56102, 56124
    };
    constexpr std::uint16_t opcodes[] = {
        2, 2, 2, 2, 2, 2, 0x03A4, 0x016A, 0x042C, 0x030D, 0x0997,
        0x01F0, 0x0111, 0x00C0, 0x04E4, 0x03CB, 0x062A, 0x062A,
        0x062A, 0x062A, 0x0629, 0x0629, 0x0053
    };
    for (unsigned i = 0; i < std::size(opcodes); ++i) {
        Check(session.State().IP == ips[i], "real pre-instruction IP");
        if (i == 14 || i == 15 || i == 22) {
            const unsigned kind = i == 14 ? 0 : i == 15 ? 1 : 2;
            services.Mode[kind] = ServiceStatus::Pending;
            const auto before = session.State();
            auto result = session.Step(services);
            Check(result.Status == Status::Pending && result.Executed == 0, "real service pending");
            Check(session.State() == before, "pending has no script-visible commit");
            const auto id = services.LastId[kind];
            result = session.Step(services);
            Check(result.Status == Status::Pending && services.LastId[kind] == id, "poll uses stable ID");
            Check(session.State() == before && services.Completions[kind] == 0, "poll no duplicate effects");
            Check(!session.LoadMain(gameDir, error), "reload rejects in-flight service");
            services.Mode[kind] = ServiceStatus::Ready;
        }
        const auto result = session.Step(services);
        Check(result.Status == Status::Advanced && result.Executed == 1, "real instruction commits once");
        Check(session.State().IP == ips[i + 1] && session.State().LastOpcode == opcodes[i], "real instruction boundary/opcode");
    }
    const auto& s = session.State();
    Check(s.Commands == 23 && s.IP == 56124, "exact bounded startup endpoint, NOT full boot");
    Check(s.IntStats[28] == 147 && s.FloatStats[1] == 187 && s.IntStats[108] == 1339, "documented total setters");
    Check(s.MaximumWantedLevel == 6 && s.MaximumChaosLevel == 6900 && !s.DeathArrestCheckEnabled, "wanted and thread state");
    Check(s.Clock.Hours == 8 && s.Clock.Minutes == 0 && s.Clock.Seconds == 0 && s.Clock.LastTickMs == 123 && s.Clock.Revision == 1, "source clock setter effects");
    Check(s.IntStats[45] == 800 && s.IntStats[40] == 0 && s.IntStats[61] == 0, "float operands route to int stat IDs");
    Check(s.FloatStats[23] == 50 && s.FloatStats[21] == 200 && s.FloatStats[68] == 0, "int operand routes to float stat ID");
    Check(s.StatWrites == 9 && s.UnprocessedStatNotifications == 6 && !s.StatNotificationsImplemented, "notification limitation explicit");
    Check(services.Completions == std::array<unsigned, 3>{1, 1, 1}, "three real requests completed by TEST mocks only");
    Check(services.Collision.X == 2488.562255859375f && services.Collision.Y == -1666.864501953125f, "authored collision coordinates");
    Check(services.Scene.Position.Z == 13.375699996948242f, "authored load scene Z");
    Check(services.Player.PlayerIndex == 0 && services.Player.Position.Z == 12.875699996948242f, "player Z differs from scene Z, never ray ceiling");
    Check(s.LastOutputWrite.Sequence == 1 && s.LastOutputWrite.IP == 56102 && s.LastOutputWrite.Variable == 8 && s.LastOutputWrite.Global && s.LastOutputWrite.Value == 0, "actual output write event despite initial zero");
    std::int32_t global = -1;
    Check(session.ReadGlobal(8, global) && global == 0, "player index output global bytes");
    Check(session.AdvanceTime(123, error), "process source zero-duration fade");
    Check(s.Fade.Alpha == 255 && s.Fade.EffectsScale == 0 && s.Fade.Revision == 1, "fade direction zero raises alpha and mutes fader state");
    Check(session.AdvanceTime(123, error) && !s.Fade.Fading && !s.Fade.MusicFading && s.Fade.MusicFadedOut, "source next fade update completes flags");

    NativeScriptSession quotaSession;
    MockServices quotaServices;
    Check(quotaSession.LoadMain(gameDir, error), "fresh real quota session");
    const auto quota = quotaSession.Run(quotaServices, 23);
    Check(quota.Status == Status::BudgetYield && quota.Executed == 23 && quotaSession.State().IP == 56124, "budget endpoint isn't boot completion");
    Check(quotaServices.LastId[2].Session != services.LastId[2].Session, "session request identity is unique");
    // Preserve the original 23-command/pending assertions, then exercise the
    // remaining actual prefix. The four services cannot commit before Ready.
    Check(session.Run(services, 24).Status == Status::BudgetYield && s.IP == 56333, "GOTO, genuine NOP and 22 relationship writes");
    Check(s.RelationshipRevision == 22 && s.Relationships[8][1] == 1 && s.Relationships[8][4] == (1u << 7), "real directional relationships persist");
    Check(s.Relationships[7][3] == ((1u << 0) | (1u << 9) | (1u << 14)), "relationship categories use pedtype bit masks");
    constexpr std::uint32_t newIPs[]{56333, 56341, 56349, 56351, 56361};
    for (unsigned kind = 0; kind < 4; ++kind) {
        services.NewMode[kind] = ServiceStatus::Pending;
        const auto before = s;
        const float oldHeading = services.PedHeading, oldCamera = services.CameraHeading;
        const auto pending = session.Step(services);
        Check(pending.Status == Status::Pending && pending.IP == newIPs[kind], "real typed barrier at exact IP");
        const auto id = services.NewLastId[kind];
        Check(session.Step(services).Status == Status::Pending && services.NewLastId[kind] == id, "new typed request ID stable");
        Check(s == before && services.PedHeading == oldHeading && services.CameraHeading == oldCamera, "pending preserves VM and mock external state");
        services.NewMode[kind] = ServiceStatus::Ready;
        Check(session.Step(services).Status == Status::Advanced && s.IP == newIPs[kind + 1], "new typed Ready commits once");
    }
    Check(session.ReadGlobal(44, global) && global == services.Group.Value, "GET_PLAYER_GROUP stores generation-bearing service result");
    Check(session.ReadGlobal(12, global) && global == services.Ped.Value, "GET_PLAYER_CHAR stores actual mock pool reference");
    Check(services.NewOrder == std::vector<unsigned>{0, 1, 2, 3} && services.CameraHeading == 0.25f, "camera executes before heading and retains pre-heading state");
    Check(std::abs(services.PedHeading - 262.0f * std::numbers::pi_v<float> / 180.0f) < 1e-6f, "heading uses source degrees-to-radians conversion");
    const auto firstWait = session.RunPass(services, 100);
    Check(firstWait.Status == Status::Waiting && firstWait.Executed == 2 && firstWait.ThreadIndex == 0, "launch then first WAIT finishes main processing pass");
    Check(s.Commands == 53 && s.IP == 56369 && s.WakeTimeMs == 123 && s.Waiting, "successful 53-command prefix endpoint");
    Check(session.Threads().size() == 2 && session.Threads()[1].Commands == 0 && session.Threads()[1].IP == 200000, "mission is real owned thread deferred to next pass");
    Check(session.Threads()[1].UsesMissionCleanup && session.Threads()[1].ThisMustBeTheOnlyMissionRunning && !session.Threads()[1].IsExternal && s.AlreadyRunningMission, "source launch ownership flags");
    Check(session.Threads()[1].Locals.size() == 1024 && std::all_of(session.Threads()[1].Locals.begin(), session.Threads()[1].Locals.end(), [](auto v) { return v == 0; }), "mission locals wiped independently of main timers");
    const auto mainBefore = s;
    const auto missionPrefix = session.RunPass(services, 117);
    Check(missionPrefix.Status == Status::BudgetYield && missionPrefix.Executed == 117 && missionPrefix.ThreadIndex == 1 && missionPrefix.IP == 200868 && missionPrefix.Opcode == 5,
        "real mission-0 commits 117 source-supported instructions up to first entity-service barrier");
    Check(static_cast<const NativeScriptThreadState&>(s) == static_cast<const NativeScriptThreadState&>(mainBefore), "mission initialization does not resume/change main thread");
    Check(!s.LaRiotsEnabled && s.LaRiotsRevision == 1, "real 06C8 false write is persistent observable policy");
    const auto& missionState = session.Threads()[1];
    Check(missionState.Commands == 117 && missionState.LastOutputWrite.Sequence == 115 && missionState.LastOutputWrite.IP == 200858 && missionState.LastOutputWrite.Variable == 6360,
        "exact mission command/write counts and final source write location");
    Check(missionState.LastOutputWrite.Value == std::bit_cast<std::int32_t>(16.100000381469727f), "last numeric initialization bits");
    for (unsigned index = 0; index < 32; ++index) {
        Check(session.ReadGlobal(std::uint16_t(2912 + index * 4), global) && global == 0, "real property status table zeroed");
        Check(session.ReadGlobal(std::uint16_t(3040 + index * 4), global) && global == std::int32_t(index), "real property index table initialized");
    }
    Check(session.ReadGlobal(6040, global) && global == 1, "real nonzero initialization flag");
    constexpr float initialFloats[]{5, -5, 8.5f, -1.5f, -30, 32, 0};
    for (unsigned i = 0; i < std::size(initialFloats); ++i) {
        Check(session.ReadGlobal(std::uint16_t(6000 + i * 4), global) && std::bit_cast<float>(global) == initialFloats[i], "real signed float initialization");
    }
    for (auto expected : {std::pair{6096, -1969.27001953125f}, {6224, 282.4700012207031f}, {6352, 34.599998474121094f},
                          {6100, -2243.6201171875f}, {6228, 133.1999969482422f}, {6356, 34.79999923706055f},
                          {6104, 426.4971923828125f}, {6232, 2530.68896484375f}, {6360, 16.100000381469727f}}) {
        Check(session.ReadGlobal(std::uint16_t(expected.first), global) && std::bit_cast<float>(global) == expected.second, "real property positions authored numeric values");
    }
    Check(session.ReadGlobal(6740, global) && global == 0, "first pickup output still untouched");
    const auto initialized = s;
    const auto missionBeforeFault = missionState;
    const auto missionFault = session.RunPass(services, 100);
    Check(missionFault.Status == Status::Unsupported && missionFault.ThreadIndex == 1 && missionFault.IP == 200868 && missionFault.Opcode == 0x0517 && missionFault.Executed == 0, "first entity-service opcode stays unsupported at exact IP");
    Check(s == initialized && session.Threads()[1] == missionBeforeFault, "pickup faults unadvanced before main resumes");
    Check(session.RunPass(services, 100).Executed == 0 && s == initialized, "mission world-service fault terminal");
    Check(services.Completions == std::array<unsigned, 3>{1, 1, 1} && services.NewCompletions == std::array<unsigned, 4>{1, 1, 1, 1}, "numeric/policy prefix causes no fake entity-service calls");

    NativeScriptSession scheduled;
    MockServices scheduledServices;
    Check(scheduled.LoadMain(gameDir, error), "fresh scheduler real load");
    const auto fullPass = scheduled.RunPass(scheduledServices, 1000);
    Check(fullPass.Status == Status::Waiting && fullPass.Executed == 53 && scheduled.State().IP == 56369 && scheduled.Threads()[1].Commands == 0, "one true scheduler pass commits all 53 prefix instructions only");
    const auto scheduledPrefix = scheduled.RunPass(scheduledServices, 1000);
    Check(scheduledPrefix.Status == Status::Unsupported && scheduledPrefix.Executed == 117 && scheduledPrefix.IP == 200868 && scheduledPrefix.Opcode == 0x0517,
        "uninterrupted actual second pass counts committed prefix and first real service barrier");
    std::printf("native-script-real TEST-MOCKS main-commands=53 main-ip=56369 barriers=7 outputWrites=3 relationships=22 mission-thread=1 mission-commands=117 mission-writes=115 riots-writes=1 mission-next-fault=0517@200868 stats=9 notifications-unimplemented=6 fullboot=0\n");
}

void Barriers() {
    for (unsigned kind = 0; kind < 3; ++kind) {
        for (auto failure : {ServiceStatus::Error, ServiceStatus::Unsupported}) {
            NativeScriptSession session;
            MockServices services;
            LoadFixture(session, services, ServiceCode(kind));
            services.Mode[kind] = ServiceStatus::Pending;
            const auto before = session.State();
            Check(session.Step(services).Status == Status::Pending, "generated service barrier pending");
            Check(session.State() == before, "generated pending state unchanged");
            services.Mode[kind] = failure;
            auto result = session.Step(services);
            Check(result.Status == (failure == ServiceStatus::Error ? Status::Error : Status::Unsupported), "service failure kind preserved");
            Check(session.State() == before && !result.Message.empty() && result.IP == FixtureCode, "failed service no partial instruction");
            const auto calls = services.Calls;
            session.Step(services);
            Check(services.Calls == calls && session.State() == before, "terminal fault does not retry side effects");
            Check(services.Completions[kind] == 0, "no fake completion");
        }
    }
    NativeScriptSession session;
    MockServices services;
    LoadFixture(session, services, ServiceCode(2, 3, false));
    Check(session.Step(services).Status == Status::Advanced, "local player output supported");
    Check(!session.State().LastOutputWrite.Global && session.State().LastOutputWrite.Variable == 3, "local output event");
    LoadFixture(session, services, ServiceCode(0));
    services.Throw = true;
    const auto before = session.State();
    Check(session.Step(services).Status == Status::Error && session.State() == before, "host exception fault without VM commit");

    services.Throw = false;
    Bytes timerInput; Op(timerInput, 0x04E4); Var(timerInput, 32, false); F(timerInput, 2);
    LoadFixture(session, services, timerInput);
    services.Mode[0] = ServiceStatus::Pending;
    Check(session.Step(services).Status == Status::Pending, "timer-backed pending request");
    const auto request = services.Collision;
    std::string error;
    Check(session.AdvanceTime(1, error) && session.State().Locals[32] == 1, "timers advance while host pending");
    Check(session.Step(services).Status == Status::Pending && services.Collision.X == request.X && services.Collision.Id == request.Id,
        "pending request freezes decoded operands despite timer changes");
    services.Mode[0] = ServiceStatus::Ready;
    Check(session.Step(services).Status == Status::Advanced && services.Collision.X == request.X, "ready uses original decoded request");
}

void RejectCode(const Bytes& code, Status expected = Status::Error) {
    NativeScriptSession session;
    MockServices services;
    LoadFixture(session, services, code);
    const auto before = session.State();
    std::int32_t oldGlobal = 0, newGlobal = 0;
    Check(session.ReadGlobal(8, oldGlobal), "read before rejected instruction");
    const auto result = session.Step(services);
    Check(result.Status == expected && result.Executed == 0 && result.IP == FixtureCode && !result.Message.empty(), "explicit malformed/unknown fault at same IP");
    Check(session.State() == before && services.Calls == std::array<unsigned, 3>{} && services.NewCalls == std::array<unsigned, 4>{} &&
        services.PickupOperationCalls == std::array<unsigned, 2>{}, "decode fault before services or state effects");
    Check(session.ReadGlobal(8, newGlobal) && newGlobal == oldGlobal, "decode fault does not write output");
}

void Malformed() {
    RejectCode({0xFF, 0x7F}, Status::Unsupported);
    RejectCode({0x6A, 0x81}, Status::Unsupported); // no conditions in this slice
    const auto player = ServiceCode(2);
    for (std::size_t n = 2; n < player.size(); ++n) RejectCode(Bytes(player.begin(), player.begin() + n));
    RejectCode(ServiceCode(2, 0)); // cannot overwrite header
    RejectCode(ServiceCode(2, 22)); // 4-byte write crosses globals boundary
    RejectCode(ServiceCode(2, 34, false));
    auto code = player; code.back() = 255; RejectCode(code);
    code = player; code[4] = 4; RejectCode(code); // float input replaced by int tag
    for (auto value : {std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
        code.clear(); Op(code, 0x04E4); F(code, value); F(code, 2); RejectCode(code);
    }
    for (auto id : {82, 119, 343, -1}) {
        code.clear(); Op(code, 0x0629); I32(code, id); I8(code, 0); RejectCode(code);
    }
    code.clear(); Op(code, 0x062A); I16(code, 165); F(code, 2147483648.0f); RejectCode(code);
    code.clear(); Op(code, 0x0629); I16(code, 165); I32(code, std::numeric_limits<std::int32_t>::max()); RejectCode(code);
    code.clear(); Op(code, 0x0111); I8(code, 2); RejectCode(code);
    code.clear(); Op(code, 0x00C0); I8(code, 8); I8(code, 60); RejectCode(code);
    code.clear(); Op(code, 0x01F0); I8(code, 7); RejectCode(code);
    code.clear(); Op(code, 0x042C); I8(code, -1); RejectCode(code);
    code.clear(); Op(code, 0x0001); I16(code, -1); RejectCode(code);
    code.clear(); Op(code, 0x016A); I8(code, 0); I8(code, 2); RejectCode(code);
    for (auto target : {-1, 8, 105, 107, 99999}) {
        code.clear(); Op(code, 2); I32(code, target); RejectCode(code);
    }
    code.clear(); Op(code, 0x01F0); code.push_back(7); RejectCode(code); // truncated array

    NativeScriptSession session;
    MockServices services;
    Bytes valid; Op(valid, 0x0001); I8(valid, 0);
    auto fixture = Fixture(valid);
    std::string error;
    Check(session.LoadMainBytes(fixture, fixture.size(), error), "valid generated load");
    const auto before = session.State();
    const auto original = fixture;
    Patch(fixture, 3, 999999);
    Check(!session.LoadMainBytes(fixture, fixture.size(), error) && session.State() == before, "invalid header load transactional");
    for (std::size_t n = 0; n < original.size(); ++n) {
        Check(!session.LoadMainBytes(std::span(original).first(n), original.size(), error), "truncated SCM prefix rejected");
        Check(session.State() == before, "truncated load keeps old session");
    }
    Check(!session.LoadMain(nullptr, error) && session.State() == before, "invalid file API keeps session");
    Check(!session.LoadMain("/path-that-does-not-exist-native-script-probe", error), "file open error clear");
    code.clear(); Op(code, 0x01F0); Var(code, 8);
    fixture = Fixture(code); Patch(fixture, 8, 6);
    Check(session.LoadMainBytes(fixture, fixture.size(), error), "generated global operand fixture");
    Check(session.Run(services, 7).Status == Status::BudgetYield && session.State().MaximumChaosLevel == 6900, "global byte-offset input read");
    code.clear(); Op(code, 0x04E4); Var(code, 8); F(code, 1);
    fixture = Fixture(code); Patch(fixture, 8, 0x7FC00000);
    Check(session.LoadMainBytes(fixture, fixture.size(), error), "generated NaN global fixture");
    Check(session.Run(services, 7).Status == Status::Error && services.Calls[0] == 0, "nonfinite variable float rejected before host");
    code.clear(); Op(code, 0x01F0); I8(code, 2); Op(code, 0x7FFF);
    LoadFixture(session, services, code);
    const auto unknown = session.Run(services, 10);
    Check(unknown.Status == Status::Unsupported && unknown.Executed == 1 && unknown.IP == FixtureCode + 4,
        "unknown preserves preceding committed instruction, no speculative advance");
    Check(session.State().MaximumWantedLevel == 2 && session.State().MaximumChaosLevel == 365, "prior instruction remains committed");
}

void TimeAndQuota() {
    NativeScriptSession session;
    MockServices services;
    Check(session.Step(services).Status == Status::Error, "unloaded Step fails");
    std::string error;
    Check(!session.AdvanceTime(0, error), "unloaded time fails");
    Bytes code;
    Op(code, 1); I8(code, 0);
    Op(code, 0x01F0); I8(code, 1);
    Op(code, 1); I8(code, 10);
    Op(code, 0x00C0); I8(code, 8); I8(code, 0);
    LoadFixture(session, services, code);
    const auto before = session.State();
    Check(session.Run(services, 0).Status == Status::Error && session.State() == before, "zero budget fails without state mutation");
    auto result = session.Run(services, 50);
    Check(result.Status == Status::Waiting && result.Executed == 1 && session.State().MaximumWantedLevel == 0, "WAIT zero yields processing pass");
    result = session.Run(services, 50);
    Check(result.Status == Status::Waiting && result.Executed == 2 && session.State().MaximumWantedLevel == 1, "next pass can resume WAIT zero");
    Check(session.State().WakeTimeMs == 10, "WAIT absolute wake time");
    Check(session.AdvanceTime(9, error), "advance before wake");
    Check(session.Run(services, 10).Status == Status::Waiting, "sleep before deadline");
    Check(session.AdvanceTime(10, error), "advance to wake");
    Check(session.Run(services, 1).Status == Status::BudgetYield && session.State().Clock.LastTickMs == 10, "wake exact deadline and clock tick reset");
    Check(session.State().Locals[32] == 10 && session.State().Locals[33] == 10, "source script timers");
    const auto after = session.State();
    Check(!session.AdvanceTime(9, error) && session.State() == after, "time reversal rejected transactionally");
    Check(session.Step(services).Status == Status::Error && session.State() == after, "end of code isn't successful boot");

    code.clear(); Op(code, 2); I32(code, FixtureCode);
    LoadFixture(session, services, code);
    Check(session.Run(services, 123).Status == Status::BudgetYield && session.State().Commands == 129 && session.State().IP == FixtureCode, "backward GOTO yields on quota, not done");
    code.clear(); Op(code, 2); I32(code, FixtureCode + 11);
    Op(code, 0x01F0); I8(code, 3);
    Op(code, 1); I8(code, 0);
    LoadFixture(session, services, code);
    Check(session.Run(services, 100).Status == Status::Waiting && session.State().MaximumWantedLevel == 0, "forward GOTO follows proven boundary");

    code.clear(); Op(code, 1); I8(code, 1);
    LoadFixture(session, services, code);
    Check(session.AdvanceTime(std::numeric_limits<std::uint32_t>::max(), error), "maximum supported VM time");
    const auto maximum = session.State();
    Check(session.Step(services).Status == Status::Error && session.State() == maximum, "wake overflow no partial commit");

    code.clear(); Op(code, 0x016A); I16(code, 1000); I8(code, 0);
    Op(code, 0x016A); I16(code, 1000); I8(code, 1);
    LoadFixture(session, services, code);
    Check(session.Step(services).Status == Status::Advanced, "finite fade starts");
    Check(session.AdvanceTime(500, error) && session.State().Fade.Alpha == 127.5f, "source fade half-time alpha");
    Check(session.Step(services).Status == Status::Advanced, "fade direction reversal");
    Check(session.AdvanceTime(1000, error) && session.State().Fade.Alpha == 0 && !session.State().Fade.Fading, "source fade subtraction and completion");

    code.clear(); Op(code, 0x0629); I8(code, 68); I8(code, 7);
    Op(code, 0x062A); I16(code, 165); F(code, -9.75f);
    LoadFixture(session, services, code);
    Check(session.Run(services, 2).Status == Status::BudgetYield, "generated cross-bank stat setters");
    Check(session.State().FloatStats[68] == 7.0f && session.State().IntStats[45] == -9,
        "stat ID chooses bank; source float-to-int truncates toward zero");
    Check(session.State().UnprocessedStatNotifications == 2, "generated stat effects remain explicitly unnotified");
}

void ThreadsAndMissions() {
    NativeScriptSession session;
    MockServices services;
    std::string error;
    Bytes main, mission0, mission1;
    Op(main, 6); Var(main, 0, false); I8(main, 77);
    Op(main, 4); Var(main, 8); I8(main, 7);
    Op(main, 0x0417); I8(main, 0);
    Op(main, 1); I8(main, 0);
    Op(main, 4); Var(main, 12); Var(main, 8);
    Op(main, 1); I8(main, 20);
    Op(main, 0x0417); I8(main, 1);
    Op(main, 1); I8(main, 0);
    Op(main, 0x004E);
    Op(mission0, 6); Var(mission0, 0, false); I8(mission0, 11);
    Op(mission0, 6); Var(mission0, 1000, false); I8(mission0, 55);
    Op(mission0, 4); Var(mission0, 8); I8(mission0, 13);
    Op(mission0, 1); I8(mission0, 10);
    Op(mission0, 6); Var(mission0, 0, false); I8(mission0, 14);
    Op(mission0, 0x004E);
    Op(mission1, 0);
    Op(mission1, 2); I32(mission1, -16);
    Op(mission1, 6); Var(mission1, 0, false); I8(mission1, 99); // skipped
    Check(mission1.size() == 16, "independent mission-relative label");
    Op(mission1, 6); Var(mission1, 0, false); Var(mission1, 12);
    Op(mission1, 7); Var(mission1, 1, false); F(mission1, 1.25f);
    Op(mission1, 1); I8(mission1, 0);
    Op(mission1, 0x004E);
    const auto fixture = Fixture(main, {mission0, mission1});
    Check(session.LoadMainBytes(fixture, fixture.size(), error), error.c_str());
    auto result = session.RunPass(services, 9);
    Check(result.Status == Status::BudgetYield && result.Executed == 9 && session.Threads().size() == 2 && session.Threads()[1].Commands == 0, "budget preserves pre-processing next snapshot after launch");
    Check(session.Step(services).Status == Status::Error, "main observation cannot interleave unfinished scheduler pass");
    result = session.RunPass(services, 1);
    Check(result.Status == Status::Waiting && result.Executed == 1 && session.Threads()[1].Commands == 0, "budget continuation finishes main pass without running inserted head");
    result = session.RunPass(services, 1);
    Check(result.Status == Status::BudgetYield && result.ThreadIndex == 1 && session.Threads()[1].Locals[0] == 11 && session.State().Locals[0] == 77, "next pass starts mission before main with separate local bank");
    result = session.RunPass(services, 100);
    std::int32_t value = 0;
    Check(result.Status == Status::Waiting && result.Executed == 5 && session.ReadGlobal(12, value) && value == 13, "mission shared-global write observed by main later in same pass");
    Check(session.Threads()[1].Locals[1000] == 55 && session.Threads()[1].WakeTimeMs == 10 && session.State().WakeTimeMs == 20, "mission extended locals and per-thread wake deadlines");
    Check(session.AdvanceTime(9, error) && session.RunPass(services, 100).Executed == 0, "two sleeping threads do not execute early");
    Check(session.Threads()[1].Locals[32] == 9 && session.State().Locals[32] == 9, "main and mission timer banks advance once");
    Check(session.AdvanceTime(10, error), "mission wake deadline");
    result = session.RunPass(services, 100);
    Check(result.Executed == 2 && !session.Threads()[1].Active && session.Threads()[1].Locals[0] == 14 && !session.State().AlreadyRunningMission, "mission termination releases owned slot while main still sleeps");
    Check(session.AdvanceTime(20, error), "main wake deadline");
    result = session.RunPass(services, 100);
    Check(result.Executed == 2 && session.Threads().size() == 2 && session.Threads()[1].Commands == 0 && session.Threads()[1].Locals[1000] == 0 && session.Threads()[1].Locals[32] == 0 && session.Threads()[1].Generation == 2, "replacement mission reuses idle head, resets locals/timers and starts next pass");
    result = session.RunPass(services, 100);
    Check(result.Executed == 6 && !session.State().Active && session.Threads()[1].Locals[0] == 13 && std::bit_cast<float>(session.Threads()[1].Locals[1]) == 1.25f, "negative mission-relative branch, float assignment and main termination");
    Check(session.Threads()[1].BaseIP == 200000 && session.Threads()[1].MissionIndex == 1 && session.Threads()[1].Locals[1000] == 0, "mission storage and idle thread state replaced together");
    Check(session.RunPass(services, 100).Executed == 1 && !session.State().AlreadyRunningMission && !session.Threads()[1].Active, "WAIT zero mission resumes next pass and terminates");
    Check(session.RunPass(services, 100).Executed == 0, "empty active list stable");

    Bytes loop; Op(loop, 0x0417); I8(loop, 0); Op(loop, 1); I8(loop, 0); Op(loop, 2); I32(loop, FixtureCode + 4);
    Bytes finish; Op(finish, 0x004E);
    const auto repeated = Fixture(loop, {finish});
    Check(session.LoadMainBytes(repeated, repeated.size(), error), "repeated mission slot fixture");
    for (unsigned pass = 0; pass < 100; ++pass) {
        Check(session.RunPass(services, 100).Status == Status::Waiting && session.Threads().size() == 2 && session.Threads()[1].Commands == 0 && session.Threads()[1].Generation == pass + 1,
            "source idle-slot reuse and next snapshot permit repeated missions beyond pool size");
    }

    // A malformed mission instruction must not partially write either bank.
    Bytes bad; Op(bad, 6); Var(bad, 1024, false); I8(bad, 8);
    Bytes launch; Op(launch, 0x0417); I8(launch, 0); Op(launch, 1); I8(launch, 0);
    auto malformed = Fixture(launch, {bad});
    Check(session.LoadMainBytes(malformed, malformed.size(), error), "load generated malformed mission code (headers valid)");
    Check(session.RunPass(services, 100).Executed == 8, "launch malformed mission only on first pass");
    const auto mainBefore = session.State();
    const auto threadBefore = session.Threads()[1];
    result = session.RunPass(services, 100);
    Check(result.Status == Status::Error && result.ThreadIndex == 1 && result.IP == 200000 && result.Executed == 0, "malformed mission precise unadvanced fault");
    Check(session.State() == mainBefore && session.Threads()[1] == threadBefore, "malformed mission leaves both thread banks intact");

    // All metadata failures retain the already-loaded thread/global state.
    const auto valid = Fixture(launch, {mission0, mission1});
    for (auto offset : {48u, 52u, 56u, 60u, 64u}) {
        auto corrupt = valid;
        Patch(corrupt, offset, 0xFFFFFFFF);
        Check(!session.LoadMainBytes(corrupt, corrupt.size(), error) && session.State() == mainBefore && session.Threads()[1] == threadBefore, "mission header bounds failure transactional");
    }
    auto corrupt = valid;
    Patch(corrupt, 64, std::uint32_t(valid.size() - mission0.size() - mission1.size())); // duplicate mission offset
    Check(!session.LoadMainBytes(corrupt, corrupt.size(), error), "mission offsets strictly increasing");
    Check(!session.LoadMainBytes(std::span(valid).first(valid.size() - 1), valid.size(), error), "startup requires complete retained mission payload");
    Bytes doubleLaunch = launch;
    doubleLaunch.resize(4); // one launch, then a competing launch before WAIT
    Op(doubleLaunch, 0x0417); I8(doubleLaunch, 0);
    malformed = Fixture(doubleLaunch, {mission0});
    Check(session.LoadMainBytes(malformed, malformed.size(), error), "load competing mission launch fixture");
    Check(session.RunPass(services, 100).Status == Status::Error && session.State().Commands == 7 && session.Threads().size() == 2 && session.Threads()[1].Commands == 0, "live mission slot conflict faults without overwrite/duplicate thread");
    for (auto target : {-1, -69000, std::numeric_limits<std::int32_t>::min(), 199999}) {
        bad.clear(); Op(bad, 2); I32(bad, target);
        malformed = Fixture(launch, {bad});
        Check(session.LoadMainBytes(malformed, malformed.size(), error), "load invalid mission label fixture");
        session.RunPass(services, 100);
        result = session.RunPass(services, 100);
        Check(result.Status == Status::Error && result.IP == 200000 && session.Threads()[1].Commands == 0, "mission label bounds/alignment fault before advance");
    }
}

void NumericAndRelationships() {
    NativeScriptSession session;
    MockServices services;
    Bytes code;
    Op(code, 4); Var(code, 8); I8(code, 5);
    Op(code, 5); Var(code, 12); F(code, 3.5f);
    Op(code, 6); Var(code, 0, false); Var(code, 8);
    Op(code, 7); Var(code, 1, false); Var(code, 12);
    Op(code, 0x00D6); I8(code, 1); // two AND conditions
    Op(code, 0x001A); I8(code, 6); Var(code, 8);
    Op(code, 0x801A); I8(code, 4); Var(code, 8);
    Op(code, 0x004D); I32(code, -1); // not taken: source must not evaluate PC
    Op(code, 1); I8(code, 0);
    LoadFixture(session, services, code);
    Check(session.Run(services, 50).Status == Status::Waiting && session.State().Commands == 15, "AND with NOT condition and untaken conditional branch");
    Check(session.State().Condition && session.State().AndOrState == 0 && session.State().Locals[0] == 5 && std::bit_cast<float>(session.State().Locals[1]) == 3.5f, "numeric bit-preserving assignments and compare aggregation");
    code.clear(); Op(code, 0x801A); I8(code, 4);
    LoadFixture(session, services, code);
    const auto malformedNot = session.Step(services);
    Check(malformedNot.Status == Status::Error && malformedNot.Opcode == 0x801A && malformedNot.IP == FixtureCode && malformedNot.Executed == 0, "malformed NOT predicate retains exact raw opcode and IP");
    code.clear();
    Op(code, 0x00D6); I8(code, 21); // two OR conditions, both false
    Op(code, 0x001A); I8(code, 0); I8(code, 1);
    Op(code, 0x001A); I8(code, -1); I8(code, 1);
    Op(code, 0x004D); I32(code, FixtureCode + 30);
    Op(code, 4); Var(code, 8); I8(code, 99);
    Check(code.size() == 30, "independent conditional target");
    Op(code, 1); I8(code, 0);
    LoadFixture(session, services, code);
    Check(session.Run(services, 50).Status == Status::Waiting && !session.State().Condition && session.State().LastOutputWrite.Sequence == 0, "OR false takes branch without speculative skipped assignment");
    for (auto opcode : {4, 5, 6, 7}) {
        code.clear(); Op(code, std::uint16_t(opcode)); Var(code, 8, opcode >= 6);
        if (opcode % 2) F(code, 1); else I8(code, 1);
        RejectCode(code);
    }
    for (auto count : {-1, 8, 20, 28}) { code.clear(); Op(code, 0x00D6); I8(code, std::int8_t(count)); RejectCode(code); }
    code.clear(); Op(code, 0x0746); I8(code, 4); I8(code, 2); I8(code, 31);
    Op(code, 0x0746); I8(code, 1); I8(code, 2); I8(code, 31);
    Op(code, 0x0746); I8(code, 1); I8(code, 2); I8(code, 31);
    auto fixture = Fixture(code);
    std::string error;
    Check(session.LoadMainBytes(fixture, fixture.size(), error), "relationship fixture load");
    std::array<std::array<std::uint32_t, 5>, 32> defaults{};
    defaults[2][0] = 1u << 31;
    defaults[3][4] = 1u << 2;
    Check(session.SeedRelationships(defaults), "host can seed owned ped.dat baseline before first command");
    Check(session.Run(services, 9).Status == Status::BudgetYield, "relationship overwrite and repeated membership");
    Check(session.State().Relationships[2][1] == (1u << 31) && session.State().Relationships[2][0] == 0 && session.State().Relationships[2][4] == 0 && session.State().Relationships[3][4] == (1u << 2), "set clears competing categories only for selected directional target bit");
    Check(!session.SeedRelationships(defaults), "cannot overwrite relationships after execution begins");
    for (auto args : {std::array<int, 3>{5, 0, 0}, {-1, 0, 0}, {1, 32, 0}, {1, 0, 32}}) {
        code.clear(); Op(code, 0x0746); for (auto arg : args) I8(code, std::int8_t(arg)); RejectCode(code);
    }
}

// Fault after valid setup: verify all four global cells, the complete owned
// state, exact fault location/opcode, and zero external effects/partial writes.
void RejectAfterSetup(const Bytes& setup, unsigned commands, const Bytes& bad) {
    NativeScriptSession session;
    MockServices services;
    Bytes code = setup;
    code.insert(code.end(), bad.begin(), bad.end());
    LoadFixture(session, services, code);
    if (commands) Check(session.Run(services, commands).Status == Status::BudgetYield, "valid setup before numeric fault");
    const auto before = session.State();
    std::array<std::int32_t, 4> globals{};
    for (unsigned i = 0; i < globals.size(); ++i) Check(session.ReadGlobal(std::uint16_t(8 + 4 * i), globals[i]), "snapshot all generated globals");
    const auto result = session.Step(services);
    const auto opcode = std::uint16_t(bad[0] | unsigned(bad[1]) << 8);
    Check(result.Status == Status::Error && result.Executed == 0 && result.IP == FixtureCode + setup.size() && result.Opcode == opcode, "numeric/array error exact unadvanced opcode/IP");
    Check(session.State() == before && services.Calls == std::array<unsigned, 3>{} && services.NewCalls == std::array<unsigned, 4>{}, "numeric error rolls back all VM state before any service");
    for (unsigned i = 0; i < globals.size(); ++i) {
        std::int32_t value = 0;
        Check(session.ReadGlobal(std::uint16_t(8 + 4 * i), value) && value == globals[i], "numeric error rolls back every global cell");
    }
    Check(session.Step(services).Executed == 0 && session.State() == before, "numeric fault terminal");
}

void ArithmeticArraysAndPolicy() {
    // Every simple arithmetic opcode, both index banks, independent RHS bank,
    // typed array reads AND writes. Expected values are hand-calculated.
    for (bool global : {false, true}) for (bool globalIndex : {false, true}) for (bool floating : {false, true}) {
        constexpr std::int32_t integerResults[]{25, 17, 84, 5};
        constexpr float floatResults[]{25, 17, 84, 5.25f};
        for (unsigned operation = 0; operation < 4; ++operation) {
            NativeScriptSession session;
            MockServices services;
            Bytes code;
            Op(code, globalIndex ? 4 : 6); Var(code, globalIndex ? 8 : 0, globalIndex); I8(code, 1);
            Op(code, std::uint16_t((global ? 4 : 6) + floating)); Var(code, global ? 16 : 2, global);
            if (floating) F(code, 21); else I8(code, 21);
            Op(code, std::uint16_t((global ? 6 : 4) + floating)); Var(code, global ? 3 : 20, !global);
            if (floating) F(code, 4); else I8(code, 4);
            const auto arithmeticIP = FixtureCode + code.size();
            const auto opcode = std::uint16_t(8 + operation * 4 + (global ? 0 : 2) + floating);
            Op(code, opcode);
            Array(code, global, global ? 12 : 1, globalIndex, globalIndex ? 8 : 0, 2, floating);
            Array(code, !global, global ? 2 : 16, globalIndex, globalIndex ? 8 : 0, 2, floating);
            LoadFixture(session, services, code);
            const auto result = session.Run(services, 4);
            Check(result.Status == Status::BudgetYield && result.Executed == 4 && result.IP == FixtureCode + code.size() && result.Opcode == opcode, "checked arithmetic exact next IP/opcode");
            std::int32_t value = 0;
            if (global) Check(session.ReadGlobal(16, value), "read arithmetic global result");
            else value = std::bit_cast<std::int32_t>(session.State().Locals[2]);
            Check(floating ? std::bit_cast<float>(value) == floatResults[operation] : value == integerResults[operation], "source arithmetic result with independent array/index banks");
            const auto& write = session.State().LastOutputWrite;
            Check(write.Sequence == 4 && write.IP == arithmeticIP && write.Global == global && write.Variable == (global ? 16 : 2) && write.Value == value, "array write event records resolved byte offset/local index");
        }
    }

    // Assignment destinations need no previous finite value. Array references
    // are resolved against pre-instruction state even when writing the index.
    for (bool global : {false, true}) for (bool floating : {false, true}) {
        NativeScriptSession session;
        MockServices services;
        Bytes code;
        Op(code, 6); Var(code, 0, false); I8(code, 0);
        Op(code, global ? 4 : 6); Var(code, global ? 12 : 1, global); I32(code, 0x7FC00000);
        Op(code, std::uint16_t((global ? 4 : 6) + floating));
        Array(code, global, global ? 12 : 1, false, 0, 1, floating);
        if (floating) F(code, -7.5f); else I8(code, -7);
        LoadFixture(session, services, code);
        Check(session.Run(services, 3).Status == Status::BudgetYield, "numeric array assignment overwrites old NaN bits");
        Check(session.State().LastOutputWrite.Value == (floating ? std::bit_cast<std::int32_t>(-7.5f) : -7), "numeric array assignment bit-preserving value");
    }
    {
        NativeScriptSession session;
        MockServices services;
        Bytes code;
        Op(code, 4); Var(code, 8); I8(code, 0);
        Op(code, 8); Array(code, true, 8, true, 8, 2); I8(code, 1);
        Op(code, 4); Array(code, true, 8, true, 8, 2); I8(code, 55);
        LoadFixture(session, services, code);
        Check(session.Run(services, 3).Status == Status::BudgetYield, "index alias resolved before arithmetic write");
        std::int32_t value = 0;
        Check(session.ReadGlobal(8, value) && value == 1 && session.ReadGlobal(12, value) && value == 55, "following instruction observes committed index value");
    }

    for (bool global : {false, true}) {
        auto integerFault = [&](std::uint16_t operation, std::int32_t lhs, std::int32_t rhs) {
            Bytes setup, bad;
            Op(setup, global ? 4 : 6); Var(setup, global ? 12 : 1, global); I32(setup, lhs);
            Op(bad, std::uint16_t(operation + (global ? 0 : 2))); Var(bad, global ? 12 : 1, global); I32(bad, rhs);
            RejectAfterSetup(setup, 1, bad);
        };
        integerFault(8, std::numeric_limits<std::int32_t>::max(), 1);
        integerFault(12, std::numeric_limits<std::int32_t>::min(), 1);
        integerFault(16, std::numeric_limits<std::int32_t>::min(), -1);
        integerFault(20, std::numeric_limits<std::int32_t>::min(), -1);
        integerFault(20, 123, 0);
        auto floatFault = [&](std::uint16_t operation, float lhs, float rhs) {
            Bytes setup, bad;
            // Integer assignment installs arbitrary bit patterns independently
            // of the float decoder's finite validation.
            Op(setup, global ? 4 : 6); Var(setup, global ? 12 : 1, global); I32(setup, std::bit_cast<std::int32_t>(lhs));
            Op(bad, std::uint16_t(operation + (global ? 0 : 2))); Var(bad, global ? 12 : 1, global); F(bad, rhs);
            RejectAfterSetup(setup, 1, bad);
        };
        const float maximum = std::numeric_limits<float>::max();
        floatFault(9, maximum, maximum);
        floatFault(13, -maximum, maximum);
        floatFault(17, maximum, 2);
        floatFault(21, maximum, 0.5f);
        floatFault(21, 1, 0);
        floatFault(21, 1, -0.0f);
        floatFault(9, std::numeric_limits<float>::quiet_NaN(), 1);
        floatFault(9, std::numeric_limits<float>::infinity(), 1);
        floatFault(9, 1, std::numeric_limits<float>::quiet_NaN());
    }
    for (std::uint16_t opcode = 8; opcode < 24; ++opcode) {
        Bytes bad; Op(bad, opcode); Var(bad, 8, (opcode & 2) != 0);
        if (opcode & 1) F(bad, 1); else I8(bad, 1);
        RejectCode(bad); // bank mismatch, independent of scalar/array operands
    }

    for (bool global : {false, true}) for (bool globalIndex : {false, true}) {
        for (auto index : {-1, std::numeric_limits<std::int32_t>::min(), 2, std::numeric_limits<std::int32_t>::max()}) {
            Bytes setup, bad;
            Op(setup, globalIndex ? 4 : 6); Var(setup, globalIndex ? 8 : 0, globalIndex); I32(setup, index);
            Op(bad, global ? 4 : 6); Array(bad, global, global ? 12 : 1, globalIndex, globalIndex ? 8 : 0, 2); I8(bad, 42);
            RejectAfterSetup(setup, 1, bad); // negative -1 could still resolve inside buffer
            bad.clear(); Op(bad, 4); Var(bad, 20); Array(bad, global, global ? 12 : 1, globalIndex, globalIndex ? 8 : 0, 2);
            RejectAfterSetup(setup, 1, bad); // RHS faults must not commit valid output
        }
    }
    for (bool global : {false, true}) {
        Bytes setup;
        Op(setup, 6); Var(setup, 0, false); I8(setup, 1);
        for (auto base : {global ? 4 : 34, global ? 20 : 33, global ? 22 : 65535}) {
            Bytes bad; Op(bad, global ? 4 : 6); Array(bad, global, std::uint16_t(base), false, 0, 2); I8(bad, 42);
            RejectAfterSetup(setup, 1, bad); // base and resolved buffer bounds checked independently
        }
    }
    for (bool globalIndex : {false, true}) {
        Bytes bad; Op(bad, 4); Array(bad, true, 12, globalIndex, globalIndex ? 4 : 34, 2); I8(bad, 42);
        RejectCode(bad);
    }
    for (auto count : {0, 1}) {
        Bytes bad; Op(bad, 5); Array(bad, true, 12, false, 0, std::uint8_t(count)); F(bad, 1);
        RejectCode(bad); // zero length / wrong float element type
    }
    Bytes complete; Op(complete, 4); Array(complete, true, 12, false, 0, 1); I8(complete, 42);
    for (std::size_t n = 2; n < complete.size(); ++n) RejectCode(Bytes(complete.begin(), complete.begin() + n));

    NativeScriptSession session;
    MockServices services;
    Bytes policy;
    Op(policy, 0x06C8); I32(policy, -7); // original SETNE, not a strict 0/1 validator
    Op(policy, 1); I8(policy, 0);
    Op(policy, 0x06C8); I8(policy, 0);
    Op(policy, 0x06C8); I8(policy, 0);
    LoadFixture(session, services, policy);
    Check(!session.State().LaRiotsEnabled && session.State().LaRiotsRevision == 0, "source game initialization default policy");
    auto result = session.Step(services);
    Check(result.Status == Status::Advanced && result.IP == FixtureCode + 7 && result.Opcode == 0x06C8 && session.State().LaRiotsEnabled && session.State().LaRiotsRevision == 1, "riots SETNE policy exact next IP and nonzero semantics");
    Check(session.Step(services).Status == Status::Waiting && session.State().LaRiotsEnabled, "riots policy survives WAIT boundary");
    std::string error;
    Check(session.AdvanceTime(17, error) && session.State().LaRiotsEnabled, "persistent policy survives game time advancement");
    Check(session.Run(services, 2).Status == Status::BudgetYield && !session.State().LaRiotsEnabled && session.State().LaRiotsRevision == 3, "false and repeated false setter both observable");
    LoadFixture(session, services, policy);
    Check(!session.State().LaRiotsEnabled && session.State().LaRiotsRevision == 0, "fresh load resets owned policy");
}

Bytes NewServiceCode(unsigned kind, std::uint16_t output = 8, float heading = 262.0f) {
    Bytes code;
    if (kind < 2) { Op(code, kind == 0 ? 0x07AF : 0x01F5); I8(code, 0); Var(code, output); }
    if (kind == 2) Op(code, 0x0373);
    if (kind == 3) { Op(code, 0x0173); I32(code, 0x00000503); F(code, heading); }
    return code;
}

void NewBarriers() {
    for (unsigned kind = 0; kind < 4; ++kind) {
        for (auto failure : {ServiceStatus::Error, ServiceStatus::Unsupported}) {
            NativeScriptSession session;
            MockServices services;
            LoadFixture(session, services, NewServiceCode(kind));
            const auto before = session.State();
            const float oldHeading = services.PedHeading, oldCamera = services.CameraHeading;
            services.NewMode[kind] = ServiceStatus::Pending;
            Check(session.Step(services).Status == Status::Pending && session.State() == before, "new service Pending has no script effects");
            services.NewMode[kind] = failure;
            const auto result = session.Step(services);
            Check(result.Status == (failure == ServiceStatus::Error ? Status::Error : Status::Unsupported) && result.IP == FixtureCode && result.Executed == 0, "new service explicit failure retains IP/status");
            const auto calls = services.NewCalls;
            Check(session.Step(services).Executed == 0 && services.NewCalls == calls && session.State() == before, "new service failure terminal without duplicate call or output");
            Check(services.PedHeading == oldHeading && services.CameraHeading == oldCamera && services.NewCompletions[kind] == 0, "mock world unchanged on failed typed request");
        }
    }
    for (unsigned kind = 0; kind < 2; ++kind) {
        RejectCode(NewServiceCode(kind, 0));
        RejectCode(NewServiceCode(kind, 22));
        const auto complete = NewServiceCode(kind);
        for (std::size_t n = 2; n < complete.size(); ++n) RejectCode(Bytes(complete.begin(), complete.begin() + n));
        NativeScriptSession session;
        MockServices services;
        LoadFixture(session, services, complete);
        services.Group.Value = services.Ped.Value = -1;
        const auto before = session.State();
        Check(session.Step(services).Status == Status::Error && session.State() == before, "Ready lookup cannot manufacture missing script handle");
    }
    NativeScriptSession session;
    MockServices services;
    LoadFixture(session, services, NewServiceCode(0));
    services.Group.Value = std::bit_cast<std::int32_t>(0x80010002u);
    Check(session.Step(services).Status == Status::Advanced, "signed group generation bits remain opaque");
    std::int32_t value = 0;
    Check(session.ReadGlobal(8, value) && value == services.Group.Value, "all group generation bits preserved");
    for (auto degrees : {-90.0f, 450.0f, 810.0f}) {
        LoadFixture(session, services, NewServiceCode(3, 8, degrees));
        Check(session.Step(services).Status == Status::Advanced, "generated heading service");
        const auto fixed = degrees < 0 ? degrees + 360 : degrees > 360 ? degrees - 360 : degrees;
        Check(std::abs(services.Heading.Radians - fixed * (std::numbers::pi_v<float> / 180.0f)) < 1e-6f, "source FixAngleDegrees adjusts once, not arbitrary modulo");
        const auto completions = services.NewCompletions;
        services.SetCharHeading(services.Heading);
        Check(services.NewCompletions == completions, "mock Ready effects idempotent for repeated ID");
    }
    services.InVehicle = true;
    const float oldHeading = services.PedHeading;
    LoadFixture(session, services, NewServiceCode(3));
    Check(session.Step(services).Status == Status::Advanced && services.PedHeading == oldHeading, "host enforces source in-vehicle heading no-op");
    RejectCode(NewServiceCode(3, 8, std::numeric_limits<float>::infinity()));

    Bytes main; Op(main, 0x0417); I8(main, 0); Op(main, 1); I8(main, 0); Op(main, 1); I8(main, 0);
    Bytes mission = ServiceCode(0); Op(mission, 1); I8(mission, 0);
    const auto fixture = Fixture(main, {mission});
    std::string error;
    Check(session.LoadMainBytes(fixture, fixture.size(), error), "scheduler pending mission fixture");
    Check(session.RunPass(services, 100).Executed == 8, "mission service deferred with insertion");
    services.Mode[0] = ServiceStatus::Pending;
    const auto mainBefore = session.State();
    auto result = session.RunPass(services, 100);
    const auto id = services.LastId[0];
    Check(result.Status == Status::Pending && result.ThreadIndex == 1 && result.IP == 200000 && session.State() == mainBefore, "scheduler Pending stays on mission before main");
    Check(session.AdvanceTime(5, error), "advance game time during mission service poll");
    Check(session.RunPass(services, 100).Status == Status::Pending && services.LastId[0] == id, "scheduler preserves mission request identity across timer updates");
    services.Mode[0] = ServiceStatus::Ready;
    result = session.RunPass(services, 100);
    Check(result.Status == Status::Waiting && result.Executed == 3 && session.Threads()[1].Commands == 2 && session.State().Commands == 9, "Ready resumes current pass through mission WAIT then main WAIT");
    Check(session.Threads()[1].Locals[32] == 5 && session.State().Locals[32] == 5, "pending continuation never double-advances timers");
}
Bytes EntityCode(unsigned kind, std::uint16_t output = 8, bool timer = false) {
    Bytes code;
    if (kind < 2 || kind == 4 || kind == 5) {
        Op(code, kind == 5 ? 0x04CE : kind == 4 ? 0x0518 : kind == 0 ? 0x0517 : 0x0570);
        if (timer) Var(code, 32, false); else F(code, 1.5f);
        F(code, -2.0f); F(code, 12.25f);
        if (kind == 4) I32(code, -30000);
        if (kind == 0 || kind == 4) { code.push_back(9); for (const char c : std::array<char, 8>{'P','R','O','P','_','4',0,0}) code.push_back(c); }
        else I8(code, 32);
        Var(code, output);
    } else { Op(code, 0x018B); I32(code, 0x00020001); I8(code, 2); }
    return code;
}
void EntityBarriers() {
    for (unsigned kind : {0u,1u,2u,4u,5u}) {
        NativeScriptSession session;
        MockServices services;
        const auto code = EntityCode(kind);
        LoadFixture(session, services, code);
        const auto before = session.State();
        services.EntityMode[kind] = ServiceStatus::Pending;
        Check(session.Step(services).Status == Status::Pending && session.State() == before, "entity Pending writes no output/state");
        const auto id = services.EntityIds[kind];
        Check(session.Step(services).Status == Status::Pending && services.EntityIds[kind] == id && session.State() == before &&
            services.EntityCompletions[kind] == 0, "repeated entity Pending retains ID with no allocation or state commit");
        services.EntityMode[kind] = ServiceStatus::Ready;
        const auto result = session.Step(services);
        Check(result.Status == Status::Advanced && result.Executed == 1 && result.IP == FixtureCode + code.size() && services.EntityCompletions[kind] == 1,
            "entity Ready advances exact typed instruction once");
        if (kind < 2 || kind == 4 || kind == 5) {
            std::int32_t ref = -1;
            Check(session.ReadGlobal(8, ref) && ref == services.EntityReference && session.State().LastOutputWrite.Sequence == before.LastOutputWrite.Sequence + 1,
                "only Ready writes owned service handle output");
            const auto position = kind == 5 ? services.Coordinate.Position : kind == 4 ? services.Sale.Position : kind == 0 ? services.Property.Position : services.Blip.Position;
            Check(position == NativeScriptPosition{1.5f, -2, 12.25f}, "typed entity float coordinates unchanged");
            if (kind == 4) Check(services.Sale.Price == -30000 && services.Sale.Text == std::array<char,8>{'P','R','O','P','_','4',0,0},
                "0518 typed FFFITextOutput preserves signed ammo bits and eight-byte label before sixth output");
            else if (kind == 0) Check(services.Property.Text == std::array<char, 8>{'P','R','O','P','_','4',0,0}, "8-byte GXT key copied exactly into owned request");
            else Check((kind == 5 ? services.Coordinate.Sprite : services.Blip.Sprite) == 32, "typed source radar sprite argument");
        } else Check(services.Display.Blip.Value == services.EntityReference && services.Display.Display == 2 &&
            session.State().LastOutputWrite == before.LastOutputWrite, "018B generation reference/display with no output write");
        for (const auto failure : {ServiceStatus::Error, ServiceStatus::Unsupported}) {
            LoadFixture(session, services, code);
            const auto unchanged = session.State();
            services.EntityMode[kind] = ServiceStatus::Pending;
            Check(session.Step(services).Status == Status::Pending, "entity deferred failure begins Pending");
            services.EntityMode[kind] = failure;
            const auto failed = session.Step(services);
            Check(failed.Status == (failure == ServiceStatus::Error ? Status::Error : Status::Unsupported) && failed.Executed == 0 && session.State() == unchanged,
                "entity service capacity/unsupported failure rolls VM back completely");
            const auto calls = services.EntityCalls;
            Check(session.Step(services).Executed == 0 && services.EntityCalls == calls && session.State() == unchanged, "entity terminal failure never retries/duplicates");
        }
        LoadFixture(session, services, code);
        const auto unchanged = session.State(); services.Throw = true;
        Check(session.Step(services).Status == Status::Error && session.State() == unchanged, "entity exception does not write VM output");
        services.Throw = false;
        if (kind < 2 || kind == 4 || kind == 5) {
            LoadFixture(session, services, code); services.EntityReference = -1; services.EntityMode[kind] = ServiceStatus::Ready;
            const auto invalid = session.State();
            Check(session.Step(services).Status == Status::Error && session.State() == invalid, "Ready invalid entity reference rejected before output write");
            RejectCode(EntityCode(kind, 0)); RejectCode(EntityCode(kind, 22));
        }
        for (std::size_t n = 2; n < code.size(); ++n) RejectCode(Bytes(code.begin(), code.begin() + n));
    }
    NativeScriptSession session; MockServices services;
    LoadFixture(session, services, EntityCode(0, 8, true));
    services.EntityMode[0] = ServiceStatus::Pending;
    Check(session.Step(services).Status == Status::Pending, "timer-backed property argument starts deferred");
    const auto request = services.Property;
    std::string error; Check(session.AdvanceTime(7, error), "timers advance during property Pending");
    Check(session.Step(services).Status == Status::Pending && services.Property.Id == request.Id && services.Property.Position == request.Position &&
        services.Property.Text == request.Text, "pending property freezes inputs while underlying local timer mutates");
    services.EntityMode[0] = ServiceStatus::Ready;
    Check(session.Step(services).Status == Status::Advanced && services.EntityCompletions[0] == 1, "frozen property commits once after readiness");
    LoadFixture(session,services,EntityCode(4,8,true)); services.EntityMode[4]=ServiceStatus::Pending;
    Check(session.Step(services).Status==Status::Pending,"0518 timer-backed request pending"); const auto sale=services.Sale;
    Check(session.AdvanceTime(9,error) && session.Step(services).Status==Status::Pending && services.Sale.Id==sale.Id &&
        services.Sale.Position==sale.Position && services.Sale.Price==sale.Price && services.Sale.Text==sale.Text,"0518 all six operands frozen across deferred timer changes");
    services.EntityMode[4]=ServiceStatus::Ready;
    Check(session.Step(services).Status==Status::Advanced && services.EntityCompletions[4]==1,"0518 frozen request commits once");
}
void EntryExitBarriers() {
    Bytes code; Op(code, 0x09B4); F(code, 1.5f); F(code, -2.25f); F(code, 10); I16(code, 16384); I8(code, -2);
    NativeScriptSession session; MockServices services;
    LoadFixture(session, services, code); const auto before = session.State();
    services.EntityMode[3] = ServiceStatus::Pending;
    Check(session.Step(services).Status == Status::Pending && session.State() == before, "09B4 Pending atomic");
    const auto request = services.EntryExit;
    Check(request.X == 1.5f && request.Y == -2.25f && request.Radius == 10 && request.Mask == 16384 && request.State == -2,
        "09B4 exact FFFII request, preserves noncanonical integer bool");
    Check(session.Step(services).Status == Status::Pending && services.EntryExit.Id == request.Id && session.State() == before,
        "09B4 Pending stable identity");
    services.EntityMode[3] = ServiceStatus::Ready;
    Check(session.Step(services).Status == Status::Advanced && session.State().IP == FixtureCode + code.size() &&
        session.State().Condition == before.Condition && session.State().LastOutputWrite == before.LastOutputWrite && services.EntityCompletions[3] == 1,
        "09B4 Ready advances one typed instruction, no compare/output");
    for (auto status : {ServiceStatus::Error, ServiceStatus::Unsupported}) {
        LoadFixture(session, services, code); const auto unchanged = session.State(); services.EntityMode[3] = status;
        Check(session.Step(services).Status == (status == ServiceStatus::Error ? Status::Error : Status::Unsupported) && session.State() == unchanged,
            "09B4 failure retains full VM state");
        const auto calls = services.EntityCalls;
        Check(session.Step(services).Executed == 0 && services.EntityCalls == calls, "09B4 terminal never polls again");
    }
    LoadFixture(session, services, code); const auto unchanged = session.State(); services.Throw = true;
    Check(session.Step(services).Status == Status::Error && session.State() == unchanged, "09B4 throwing service atomic");
    for (std::size_t n = 2; n < code.size(); ++n) RejectCode(Bytes(code.begin(), code.begin()+n));
    auto bad = code; bad[2] = 9; RejectCode(bad); // string in float operand
    Bytes infinite; Op(infinite, 0x09B4); F(infinite, std::numeric_limits<float>::infinity()); F(infinite, 0); F(infinite, 10); I16(infinite, 16384); I8(infinite, 0);
    RejectCode(infinite);
}
void GarageBarriers() {
    Bytes code; Op(code,0x02B9); code.push_back(9);
    const std::array<char,8> name{'f','i','x','t','u','r','e',0}; code.insert(code.end(),name.begin(),name.end());
    NativeScriptSession session; MockServices services;
    LoadFixture(session,services,code); const auto before=session.State();
    services.GarageMode=ServiceStatus::Pending;
    Check(session.Step(services).Status==Status::Pending && session.State()==before && services.Garage.Name==name,"02B9 TEST service typed text8 Pending atomic");
    const auto id=services.Garage.Id;
    Check(session.Step(services).Status==Status::Pending && services.Garage.Id==id && services.Garage.Name==name && session.State()==before,"02B9 Pending exact identity/name stable");
    services.GarageMode=ServiceStatus::Ready;
    Check(session.Step(services).Status==Status::Advanced && session.State().IP==FixtureCode+code.size() && session.State().Commands==before.Commands+1 && session.State().LastOutputWrite==before.LastOutputWrite && session.State().Condition==before.Condition,"02B9 Ready one instruction no compare/output mutation");
    for (const auto status:{ServiceStatus::Error,ServiceStatus::Unsupported}) {
        LoadFixture(session,services,code); const auto old=session.State(); services.GarageMode=status;
        const auto result=session.Step(services);
        Check(result.Status==(status==ServiceStatus::Error ? Status::Error : Status::Unsupported) && session.State()==old,"02B9 failure fully atomic");
        const auto calls=services.GarageCalls;
        Check(session.Step(services).Executed==0 && services.GarageCalls==calls,"02B9 terminal never repeats side effect");
    }
    LoadFixture(session,services,code); const auto old=session.State(); services.Throw=true;
    Check(session.Step(services).Status==Status::Error && session.State()==old,"02B9 throwing service fully atomic");
    for (std::size_t n=2;n<code.size();++n) RejectCode(Bytes(code.begin(),code.begin()+n));
    auto bad=code; bad[2]=6; RejectCode(bad);
}
void RestartBarriers() {
    for (const auto opcode : {0x016C,0x016D}) {
        Bytes code; Op(code,opcode); F(code,1.25f); F(code,-2.5f); F(code,10); F(code,725); I32(code,-123456);
        NativeScriptSession session; MockServices services;
        LoadFixture(session,services,code); const auto before = session.State();
        services.RestartMode = ServiceStatus::Pending;
        Check(session.Step(services).Status == Status::Pending && session.State() == before,"restart Pending atomic");
        const auto request = services.Restart;
        Check(request.Kind == (opcode == 0x016C ? NativeRestartKind::Hospital : NativeRestartKind::Police) &&
            request.Position == NativeScriptPosition{1.25f,-2.5f,10} && request.HeadingDegrees == 725 && request.WhenToUse == -123456,
            "restart exact F,F,F,F,I raw angle and signed threshold");
        Check(session.Step(services).Status == Status::Pending && services.Restart == request && session.State() == before,"restart retry ID and operands stable");
        services.RestartMode = ServiceStatus::Ready;
        Check(session.Step(services).Status == Status::Advanced && session.State().IP == FixtureCode+code.size() &&
            session.State().Commands == before.Commands+1 && session.State().LastOutputWrite == before.LastOutputWrite &&
            session.State().Condition == before.Condition,"restart commits once, no write/condition");
        for (const auto mode : {ServiceStatus::Unsupported,ServiceStatus::Error}) {
            LoadFixture(session,services,code); const auto old = session.State(); services.RestartMode = mode;
            Check(session.Step(services).Status == (mode == ServiceStatus::Error ? Status::Error : Status::Unsupported) && session.State() == old,"restart failed service unadvanced");
            const auto calls = services.RestartCalls;
            Check(session.Step(services).Executed == 0 && services.RestartCalls == calls,"restart terminal never retries");
        }
        LoadFixture(session,services,code); const auto old = session.State(); services.Throw = true;
        Check(session.Step(services).Status == Status::Error && session.State() == old,"restart throwing service atomic");
        for (std::size_t n = 2; n < code.size(); ++n) RejectCode(Bytes(code.begin(),code.begin()+n));
        auto bad = code; bad[2] = 9; RejectCode(bad);
        bad = code; bad[22] = 6; RejectCode(bad); // float supplied to final integer
        Bytes nonfinite; Op(nonfinite,opcode); F(nonfinite,0); F(nonfinite,0); F(nonfinite,10);
        F(nonfinite,std::numeric_limits<float>::infinity()); I8(nonfinite,0); RejectCode(nonfinite);
    }
}
void PickupBarriers() {
    Bytes code; Op(code,0x0213); I16(code,1277); I32(code,259); F(code,1.25f); F(code,-2.5f); F(code,10); Var(code,8);
    NativeScriptSession session; MockServices services;
    LoadFixture(session,services,code); const auto before=session.State();
    services.PickupMode=ServiceStatus::Pending;
    Check(session.Step(services).Status==Status::Pending && session.State()==before,"0213 TEST service Pending atomic");
    const auto id=services.Pickup.Id;
    Check(services.Pickup.Model==1277 && services.Pickup.Type==259 && services.Pickup.Position==NativeScriptPosition{1.25f,-2.5f,10} &&
        services.Pickup.UsedObjectName==std::array<char,24>{},"0213 exact I,I,F,F,F,out contract, raw integer type preserved");
    Check(session.Step(services).Status==Status::Pending && services.Pickup.Id==id && session.State()==before,"0213 Pending identity stable");
    services.PickupMode=ServiceStatus::Ready;
    Check(session.Step(services).Status==Status::Advanced && session.State().Condition==before.Condition &&
        session.State().LastOutputWrite.Variable==8 && session.State().LastOutputWrite.Value==0x10020,"0213 TEST Ready writes sixth operand once, no condition mutation");
    LoadFixture(session,services,code); services.PickupReference=-1;
    Check(session.Step(services).Status==Status::Advanced && session.State().LastOutputWrite.Value==-1,"0213 genuine full-pool sentinel is output, not fake allocation or lookup error");
    for (const auto status:{ServiceStatus::Error,ServiceStatus::Unsupported}) {
        LoadFixture(session,services,code); const auto old=session.State(); services.PickupMode=status;
        Check(session.Step(services).Status==(status==ServiceStatus::Error ? Status::Error : Status::Unsupported) && session.State()==old,"0213 failed service leaves VM atomic");
        const auto calls=services.PickupCalls;
        Check(session.Step(services).Executed==0 && services.PickupCalls==calls,"0213 terminal cannot repeat service");
    }
    LoadFixture(session,services,code); services.Throw=true; const auto old=session.State();
    Check(session.Step(services).Status==Status::Error && session.State()==old,"0213 service exception atomic");
    for (std::size_t n=2;n<code.size();++n) RejectCode(Bytes(code.begin(),code.begin()+n));
    auto bad=code; bad[2]=6; RejectCode(bad);
    Bytes negative; Op(negative,0x0213); I32(negative,std::numeric_limits<std::int32_t>::min()); I8(negative,3); F(negative,0); F(negative,0); F(negative,0); Var(negative,8);
    LoadFixture(session,services,negative); services.Throw=false; const auto calls=services.PickupCalls;
    Check(session.Step(services).Status==Status::Error && services.PickupCalls==calls,"0213 INT_MIN used-object index fails before host without signed overflow");
    Patch(negative,3,std::uint32_t(-1));
    std::vector<std::array<char,24>> used(2); used[1]={'t','e','s','t','_','m','o','d','e','l',0};
    const auto payload=Fixture(negative,{},used); std::string error;
    Check(session.LoadMainBytes(payload,payload.size(),error),"generated used-object table accepted");
    for (int i=0;i<6;++i) Check(session.Step(services).Status==Status::Advanced,"generated used-object header traversal");
    services.PickupMode=ServiceStatus::Ready;
    Check(session.Step(services).Status==Status::Advanced && services.Pickup.Model==-1 && services.Pickup.UsedObjectName==used[1],
        "0213 negative model indexes immutable24-byte table exactly, no name heuristics");
    const auto oversized=Fixture(code,{},std::vector<std::array<char,24>>(395)); const auto state=session.State();
    Check(!session.LoadMainBytes(oversized,oversized.size(),error) && session.State()==state,"used-object source395 capacity rejected before replacing live session");
}
void PickupOperationsAndFloatCopy() {
    constexpr auto ref = std::int32_t(0x81230020u);
    NativeScriptSession session; MockServices services; Bytes code;
    Op(code, 4); Var(code, 8); I32(code, ref);
    Op(code, 6); Var(code, 0, false); Var(code, 8);
    Op(code, 0x00D6); I8(code, 1); // two AND predicates
    Op(code, 0x0214); Var(code, 8);
    Op(code, 0x8214); Var(code, 0, false);
    Op(code, 0x0215); Var(code, 8);
    LoadFixture(session, services, code);
    Check(session.Run(services, 3).Status == Status::BudgetYield, "pickup operation full-reference/IF setup");
    services.PickupOperationMode[0] = ServiceStatus::Pending;
    services.PickupCollected = true;
    const auto before = session.State();
    Check(session.Step(services).Status == Status::Pending && session.State() == before &&
        services.PickupOperation.Pickup.Value == ref, "0214 Pending preserves full generation-bearing pickup reference");
    const auto firstId = services.PickupOperation.Id;
    Check(session.Step(services).Status == Status::Pending && services.PickupOperation.Id == firstId &&
        session.State() == before, "0214 Pending polls one stable request without compare mutation");
    services.PickupOperationMode[0] = ServiceStatus::Ready;
    Check(session.Step(services).Status == Status::Advanced && session.State().Condition &&
        session.State().AndOrState == 1 && services.PickupOperationCompletions[0] == 1,
        "0214 Ready applies first source AND condition exactly once");
    services.PickupCollected = false;
    Check(session.Step(services).Status == Status::Advanced && session.State().Condition &&
        session.State().AndOrState == 0 && session.State().LastOpcode == 0x8214,
        "NOT 0214 inverts the consumed false result and completes source AND aggregation");
    services.PickupOperationMode[1] = ServiceStatus::Ready;
    const auto writes = session.State().LastOutputWrite;
    Check(session.Step(services).Status == Status::Advanced && services.PickupOperation.Pickup.Value == ref &&
        services.PickupOperationCompletions[1] == 1 && session.State().LastOutputWrite == writes && session.State().Condition,
        "0215 passes the full reference without output or condition mutation");

    for (bool global : {false, true}) for (bool globalIndex : {false, true}) {
        NativeScriptSession arraySession; MockServices arrayServices; Bytes array;
        Op(array, globalIndex ? 4 : 6); Var(array, globalIndex ? 8 : 0, globalIndex); I8(array, 1);
        Op(array, global ? 4 : 6); Var(array, global ? 16 : 2, global); I32(array, ref);
        Op(array, 0x0214); Array(array, global, global ? 12 : 1, globalIndex,
            globalIndex ? 8 : 0, 2);
        LoadFixture(arraySession, arrayServices, array);
        arrayServices.PickupOperationMode[0] = ServiceStatus::Ready;
        Check(arraySession.Run(arrayServices, 3).Status == Status::BudgetYield &&
            arrayServices.PickupOperation.Pickup.Value == ref && !arraySession.State().Condition,
            "0214 reads source integer arrays across independent array/index banks");
    }
    for (auto opcode : {std::uint16_t(0x0214), std::uint16_t(0x0215)}) {
        Bytes complete; Op(complete, opcode); I32(complete, ref);
        for (std::size_t n = 2; n < complete.size(); ++n) RejectCode(Bytes(complete.begin(), complete.begin() + n));
    }
    Bytes bad; Op(bad, 0x8215); I32(bad, ref); RejectCode(bad, Status::Unsupported);
    bad.clear(); Op(bad, 0x0214); Array(bad, true, 12, false, 0, 1, true); RejectCode(bad);

    struct MissingPickupServices final : NativeScriptServices {
        NativeScriptServiceResult RequestCollision(const NativeScriptCollisionRequest&) override { return {}; }
        NativeScriptServiceResult LoadScene(const NativeScriptSceneRequest&) override { return {}; }
        NativeScriptServiceResult CreatePlayer(const NativeScriptPlayerRequest&) override { return {}; }
    } missing;
    for (auto opcode : {std::uint16_t(0x0214), std::uint16_t(0x0215)}) {
        NativeScriptSession absent; Bytes operation; Op(operation, opcode); I32(operation, ref);
        const auto fixture = Fixture(operation); std::string error;
        Check(absent.LoadMainBytes(fixture, fixture.size(), error), "missing pickup-service fixture load");
        Check(absent.Run(missing, 6).Status == Status::BudgetYield, "missing pickup-service header traversal");
        const auto unsupported = absent.Step(missing);
        Check(unsupported.Status == Status::Unsupported && unsupported.IP == FixtureCode &&
            unsupported.Opcode == opcode && unsupported.Executed == 0,
            "missing 0214/0215 service is explicit Unsupported without advancement");
    }
    {
        NativeScriptSession absent; Bytes operation; Op(operation, 0x8214); I32(operation, ref);
        const auto fixture = Fixture(operation); std::string error;
        Check(absent.LoadMainBytes(fixture, fixture.size(), error) && absent.Run(missing, 6).Status == Status::BudgetYield,
            "missing negated pickup-service fixture setup");
        const auto unsupported = absent.Step(missing);
        Check(unsupported.Status == Status::Unsupported && unsupported.Opcode == 0x8214 && unsupported.IP == FixtureCode,
            "missing negated 0214 preserves exact source opcode on explicit Unsupported");
    }

    for (bool globalIndex : {false, true}) {
        NativeScriptSession copySession; MockServices copyServices; Bytes copy;
        Op(copy, globalIndex ? 4 : 6); Var(copy, globalIndex ? 8 : 0, globalIndex); I8(copy, 1);
        Op(copy, 5); Var(copy, 16); F(copy, -7.25f);
        const auto copyIp = FixtureCode + copy.size();
        Op(copy, 0x0086);
        Array(copy, true, 8, globalIndex, globalIndex ? 8 : 0, 2, true);
        Array(copy, true, 12, globalIndex, globalIndex ? 8 : 0, 2, true);
        LoadFixture(copySession, copyServices, copy);
        Check(copySession.Run(copyServices, 3).Status == Status::BudgetYield, "0086 global float array copy executes");
        std::int32_t value = 0;
        Check(copySession.ReadGlobal(12, value) && std::bit_cast<float>(value) == -7.25f &&
            copySession.State().LastOutputWrite.IP == copyIp && copySession.State().LastOutputWrite.Variable == 12 &&
            copySession.State().LastOutputWrite.Global && copySession.State().LastOutputWrite.Value == value,
            "0086 bit-preserving global float copy records resolved destination");
    }
    bad.clear(); Op(bad, 0x0086); Var(bad, 0, false); Var(bad, 8); RejectCode(bad);
    bad.clear(); Op(bad, 0x0086); Var(bad, 8); Var(bad, 0, false); RejectCode(bad);
    bad.clear(); Op(bad, 0x0086); Var(bad, 8); F(bad, 1); RejectCode(bad);
    Bytes complete; Op(complete, 0x0086); Var(complete, 8); Var(complete, 12);
    for (std::size_t n = 2; n < complete.size(); ++n) RejectCode(Bytes(complete.begin(), complete.begin() + n));
}
void SchemaAndCorpus() {
    Check(NativeScriptSchemaRevision() == "53ed1c2561bf6ca70dc16afca5d8f3a406066158" &&
        NativeScriptSchemaSha256() == "797f32be6d3ebae87fd65b57ccc0c0b1cbc2e129c089668761e366740b5bd671" &&
        NativeScriptSchemaVersion() == "1.65", "pinned schema identity");
    const auto* stunt = NativeScriptLookupSchema(0x0814);
    Check(stunt && stunt->OperandCount == 16 &&
        std::all_of(stunt->Operands.begin(), stunt->Operands.begin() + 15,
            [](auto type) { return type == NativeScriptOperandType::Float; }) &&
        stunt->Operands[15] == NativeScriptOperandType::Integer &&
        stunt->Semantics == NativeScriptSemanticCoverage::Unsupported,
        "0814 complete known form remains semantic Unsupported");
    Check(!NativeScriptLookupSchema(0x0FFF), "unknown opcode has no schema substitution");

    Bytes code; Op(code, 0x0814);
    for (int i = 0; i < 15; ++i) F(code, float(i));
    I16(code, 500);
    NativeScriptSession session; MockServices services; LoadFixture(session, services, code);
    NativeScriptInstructionForm form; std::string error;
    Check(session.InspectInstruction(0, form, error) && form.IP == FixtureCode &&
        form.NextIP == FixtureCode + code.size() && form.Opcode == 0x0814 &&
        form.OperandCount == 16 && form.OperandTags[15] == 5 &&
        form.Semantics == NativeScriptSemanticCoverage::Unsupported &&
        form.ThreadForm == NativeScriptThreadForm::Main, "read-only inspection returns exact fixture form");
    const auto state = session.State();
    const auto threads = std::vector<NativeScriptThreadState>(session.Threads().begin(), session.Threads().end());
    const auto result = session.Step(services);
    Check(result.Status == Status::Unsupported && result.Opcode == 0x0814 && result.IP == FixtureCode &&
        !result.Executed && session.State() == state &&
        std::vector<NativeScriptThreadState>(session.Threads().begin(), session.Threads().end()) == threads,
        "known schema never executes through default NOP");

    NativeScriptCorpusManifest corpus; const auto before = corpus;
    Check(corpus.Observe(form, error) && corpus.Observe(form, error), "same corpus site visit is idempotent form aggregation");
    auto summary = corpus.Summary();
    Check(summary.Encounters == 2 && summary.Sites == 1 && summary.Threads == 1 && summary.Opcodes == 1 &&
        summary.OperandForms == 1 && summary.MainSites == 1 && summary.UnsupportedSites == 1,
        "corpus summary distinguishes sites, encounters and unsupported semantics");
    const auto fingerprint = corpus.Fingerprint();
    auto changed = form; changed.OperandTags[15] = 4;
    const auto retained = corpus;
    Check(!corpus.Observe(changed, error) && corpus.Sites() == retained.Sites() &&
        corpus.Threads() == retained.Threads() && corpus.Fingerprint() == fingerprint,
        "changed site form rejects atomically");
    auto badArray = form; badArray.ArrayCounts[0] = 2;
    Check(!corpus.Observe(badArray, error) && corpus.Fingerprint() == fingerprint,
        "scalar operand cannot claim array metadata");
    auto unknown = form; unknown.Opcode = unknown.RawOpcode = 0x0FFF;
    Check(!corpus.Observe(unknown, error) && corpus.Fingerprint() == fingerprint,
        "unknown opcode cannot enter corpus as inferred form");
    auto foreign = form; foreign.Session++;
    Check(!corpus.Observe(foreign, error) && corpus.Fingerprint() == fingerprint,
        "mixed session corpus rejects atomically");
    Check(before.Sites().empty() && before.Threads().empty(), "manifest snapshots are independent values");
}
} // namespace

int main(int argc, char** argv) {
    RealAsset(argc > 1 ? argv[1] : "/game");
    Barriers();
    Malformed();
    TimeAndQuota();
    ThreadsAndMissions();
    NumericAndRelationships();
    ArithmeticArraysAndPolicy();
    NewBarriers();
    EntityBarriers();
    EntryExitBarriers();
    GarageBarriers();
    RestartBarriers();
    PickupBarriers();
    PickupOperationsAndFloatCopy();
    SchemaAndCorpus();
    std::printf("native-script-probe PASS checks=%zu services=TEST-ONLY no-worldboot-claim\n", s_Checks);
}
