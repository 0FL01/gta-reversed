// Generated SCM + TEST services only: VM decode/transaction evidence, not world boot.
#include "app/platform/linux/NativeScriptSession.h"

#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <stdexcept>

namespace {
using Bytes = std::vector<std::uint8_t>;
using Status = NativeScriptStatus;
using ServiceStatus = NativeScriptServiceStatus;
using Operands = std::array<Bytes, 13>;
constexpr std::uint32_t CodeStart = 216;
std::size_t s_Checks = 0;

void Check(bool condition, const char* message) {
    ++s_Checks;
    if (!condition) { std::fprintf(stderr, "NativeCarGeneratorVmProbe FAIL: %s\n", message); std::exit(1); }
}
void Put(Bytes& bytes, std::uint32_t value, unsigned count) {
    for (unsigned i = 0; i < count; ++i) bytes.push_back(std::uint8_t(value >> (8 * i)));
}
void Patch(Bytes& bytes, std::size_t offset, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bytes.at(offset + i) = std::uint8_t(value >> (8 * i));
}
Bytes Integer(std::int32_t value, unsigned tag = 1) {
    Bytes bytes{std::uint8_t(tag)}; Put(bytes, std::uint32_t(value), tag == 4 ? 1 : tag == 5 ? 2 : 4); return bytes;
}
Bytes Float(float value) { Bytes bytes{6}; Put(bytes, std::bit_cast<std::uint32_t>(value), 4); return bytes; }
Bytes Variable(bool global, std::uint16_t offset) { Bytes bytes{std::uint8_t(global ? 2 : 3)}; Put(bytes, offset, 2); return bytes; }
Bytes Array(bool global, std::uint16_t base, bool globalIndex, std::uint16_t index, unsigned count, bool floating) {
    Bytes bytes{std::uint8_t(global ? 7 : 8)}; Put(bytes, base, 2); Put(bytes, index, 2);
    Put(bytes, count, 1); Put(bytes, (globalIndex ? 0x80 : 0) | (floating ? 1 : 0), 1); return bytes;
}
void Append(Bytes& bytes, const Bytes& more) { bytes.insert(bytes.end(), more.begin(), more.end()); }
Bytes Command(std::uint16_t opcode, const Operands& operands, unsigned count = 13) {
    Bytes bytes; Put(bytes, opcode, 2);
    for (unsigned i = 0; i < count; ++i) Append(bytes, operands[i]);
    return bytes;
}
void Assign(Bytes& code, bool global, std::uint16_t offset, const Bytes& value, bool floating = false) {
    Put(code, (global ? 4 : 6) + (floating ? 1 : 0), 2); Append(code, Variable(global, offset)); Append(code, value);
}
Operands Inputs() {
    return {Float(11.25f), Float(-22.5f), Float(33.75f), Float(94.125f), Integer(401, 5),
        Integer(-2, 4), Integer(123), Integer(256), Integer(257), Integer(-258),
        Integer(65537), Integer(-65538), Variable(true, 8)};
}
Bytes Fixture(const Bytes& code, bool usedObjects = false) {
    Bytes bytes;
    auto chunk = [&](unsigned index, const Bytes& payload) {
        Put(bytes, 2, 2); Append(bytes, Integer(std::int32_t(bytes.size() + 6 + payload.size())));
        Put(bytes, index, 1); Append(bytes, payload);
    };
    Bytes globals(128); Patch(globals, 0, 0x12345678); Patch(globals, 4, 0x23456789);
    chunk(115, globals);
    Bytes objects; Put(objects, usedObjects ? 2 : 0, 4);
    if (usedObjects) { objects.resize(52); objects[28] = 'x'; }
    chunk(0, objects);
    Bytes metadata(16); Patch(metadata, 0, CodeStart + (usedObjects ? 48 : 0) + std::uint32_t(code.size()));
    chunk(1, metadata); chunk(2, Bytes(8)); chunk(3, Bytes(4));
    Bytes extra(8); Patch(extra, 0, 128); chunk(4, extra);
    Check(bytes.size() == CodeStart + (usedObjects ? 48 : 0), "generated SCM header extent");
    Append(bytes, code); return bytes;
}

struct Services final : NativeScriptServices {
    ServiceStatus Mode = ServiceStatus::Ready;
    std::int32_t Reference = 0;
    bool Throw = false;
    std::vector<NativeScriptCarGeneratorRequest> Creates;
    std::vector<NativeScriptCarGeneratorSwitchRequest> Switches;
    std::vector<NativeScriptRequestId> Committed;
    NativeScriptServiceResult RequestCollision(const NativeScriptCollisionRequest&) override { return {}; }
    NativeScriptServiceResult LoadScene(const NativeScriptSceneRequest&) override { return {}; }
    NativeScriptServiceResult CreatePlayer(const NativeScriptPlayerRequest&) override { return {}; }
    NativeScriptServiceResult Respond(const NativeScriptRequestId& id) {
        if (Throw) throw std::runtime_error("TEST generator service exception");
        if (Mode == ServiceStatus::Ready && std::find(Committed.begin(), Committed.end(), id) == Committed.end()) Committed.push_back(id);
        return {Mode, "TEST generator service"};
    }
    NativeScriptReferenceResult<NativeScriptCarGeneratorRef> CreateCarGenerator(const NativeScriptCarGeneratorRequest& request) override {
        Creates.push_back(request); return {Respond(request.Id), {Reference}};
    }
    NativeScriptServiceResult SwitchCarGenerator(const NativeScriptCarGeneratorSwitchRequest& request) override {
        Switches.push_back(request); return Respond(request.Id);
    }
};
void Load(NativeScriptSession& session, Services& services, const Bytes& code, unsigned setup = 0, bool usedObjects = false) {
    const auto bytes = Fixture(code, usedObjects); std::string error;
    Check(session.LoadMainBytes(bytes, bytes.size(), error), "load generated complete SCM");
    for (unsigned i = 0; i < 6 + setup; ++i) Check(session.Step(services).Status == Status::Advanced, "headers/setup advance");
}
void Fields(const NativeScriptCarGeneratorRequest& r) {
    Check(r.Position == NativeScriptPosition{11.25f, -22.5f, 33.75f} && r.AngleDegrees == 94.125f &&
        r.ModelId == 401 && r.PrimaryColor == -2 && r.SecondaryColor == 123 && r.ForceSpawn == 256 &&
        r.AlarmChance == 257 && r.DoorLockChance == -258 && r.MinDelay == 65537 && r.MaxDelay == -65538,
        "all twelve authored inputs preserve order, signed values and precision");
}
void Reject(const Bytes& code, const Bytes& setup = {}, unsigned setupCount = 0, Status expected = Status::Error) {
    NativeScriptSession session; Services services; Bytes combined = setup; Append(combined, code);
    Load(session, services, combined, setupCount); const auto before = session.State();
    std::int32_t global = 0; Check(session.ReadGlobal(8, global), "snapshot global");
    const auto result = session.Step(services);
    if (result.Status != expected || result.Executed || session.State() != before || !services.Creates.empty() || !services.Switches.empty()) {
        std::fprintf(stderr, "reject diagnostic checks=%zu bytes=%zu setup=%u opcode=%04X status=%d expected=%d executed=%zu message=%s\n",
            s_Checks, code.size(), setupCount, result.Opcode, int(result.Status), int(expected), result.Executed, result.Message.c_str());
    }
    Check(result.Status == expected && result.Executed == 0 && result.IP == before.IP && session.State() == before &&
        services.Creates.empty() && services.Switches.empty() && services.Committed.empty(), "bad instruction rejected before ANY effect");
    std::int32_t after = 0; Check(session.ReadGlobal(8, after) && after == global, "rejection preserves global output");
    Check(session.Step(services).Status == expected && session.State() == before && services.Creates.empty(), "fault stays unadvanced on retry");
}
void Basic() {
    Check(NativeScriptCarGeneratorRef{}.Value == -1, "default generator ref sentinel");
    for (const auto reference : {0, 499, -1}) {
        NativeScriptSession session; Services services; services.Reference = reference;
        auto code = Command(0x014B, Inputs());
        Operands operands{}; operands[0] = Variable(true, 8); operands[1] = Integer(0);
        Append(code, Command(0x014C, operands, 2)); Put(code, 0x0FFF, 2);
        Load(session, services, code); const auto before = session.State();
        Check(session.Step(services).Status == Status::Advanced && services.Creates.size() == 1, "014B commits once");
        Fields(services.Creates.back());
        std::int32_t output = 0, neighbor = 0;
        Check(session.ReadGlobal(8, output) && output == reference && session.ReadGlobal(12, neighbor) && neighbor == 0x23456789,
            "thirteenth operand writes exact output without neighbor overflow, including zero and source -1");
        const auto write = session.State().LastOutputWrite;
        Check(write.IP == before.IP && write.Variable == 8 && write.Global && write.Value == reference && write.Sequence == 1,
            "014B output write provenance");
        Check(session.Step(services).Status == Status::Advanced && services.Switches.size() == 1 &&
            services.Switches.back().Generator.Value == reference && services.Switches.back().Count == 0 &&
            session.State().LastOutputWrite == write && services.Committed.size() == 2, "014C consumes exact reference and has no output");
        const auto terminal = session.State();
        Check(session.Step(services).Status == Status::Unsupported && session.State() == terminal && services.Committed.size() == 2,
            "following unknown stays strict and unadvanced");
    }
    // Source special random model is independent of SCM's used-object table.
    for (const bool used : {false, true}) {
        NativeScriptSession session; Services services; auto operands = Inputs(); operands[4] = Integer(-1);
        Load(session, services, Command(0x014B, operands), 0, used);
        Check(session.Step(services).Status == Status::Advanced && services.Creates.back().ModelId == -1,
            "model -1 is random popcycle sentinel, never a used-object lookup");
    }
    for (const auto count : {std::numeric_limits<std::int32_t>::min(), -65537, -1, 0, 1, 100, 101, 65536, std::numeric_limits<std::int32_t>::max()}) {
        NativeScriptSession session; Services services; Operands operands{}; operands[0] = Integer(0); operands[1] = Integer(count);
        Load(session, services, Command(0x014C, operands, 2));
        Check(session.Step(services).Status == Status::Advanced && services.Switches.back().Count == count,
            "014C signed int32 source count reaches host without invented range restriction");
    }
}
void BanksAndArrays() {
    for (const bool global : {false, true}) for (const bool array : {false, true}) for (const bool globalIndex : {false, true}) {
        NativeScriptSession session; Services services; auto operands = Inputs(); Bytes setup;
        Assign(setup, globalIndex, globalIndex ? 124 : 31, Integer(1));
        for (unsigned i = 0; i < 12; ++i) {
            const auto base = std::uint16_t(global ? 8 + i * 4 : i);
            Assign(setup, global, std::uint16_t(base + (array ? (global ? 4 : 1) : 0)), operands[i], i < 4);
            operands[i] = array ? Array(global, base, globalIndex, globalIndex ? 124 : 31, 2, i < 4) : Variable(global, base);
        }
        operands[12] = array ? Array(global, global ? 72 : 20, globalIndex, globalIndex ? 124 : 31, 2, false) : Variable(global, global ? 72 : 20);
        auto code = setup; Append(code, Command(0x014B, operands)); Load(session, services, code, 13);
        Check(session.Step(services).Status == Status::Advanced, "all 13 operands support valid scalar/array banks"); Fields(services.Creates.back());
        const auto write = session.State().LastOutputWrite;
        Check(write.Global == global && write.Variable == (global ? 72 + (array ? 4 : 0) : 20 + (array ? 1 : 0)) && write.Value == 0,
            "resolved output array bank/address commits slot zero");
    }
}
void Malformed() {
    const auto valid = Command(0x014B, Inputs());
    // Every byte boundary, especially the eight formerly overflowing operands.
    for (std::size_t end = 2; end < valid.size(); ++end) Reject(Bytes(valid.begin(), valid.begin() + end));
    for (unsigned i = 0; i < 13; ++i) {
        auto operands = Inputs(); operands[i] = i < 4 ? Integer(1) : Float(1); Reject(Command(0x014B, operands));
        for (const bool global : {false, true}) {
            operands = Inputs(); operands[i] = Variable(global, global ? 136 : 34); Reject(Command(0x014B, operands));
            operands = Inputs(); operands[i] = Array(global, global ? 8 : 0, false, 30, 1, i >= 4); Reject(Command(0x014B, operands));
            operands[i] = Array(global, global ? 8 : 0, false, 30, 0, i < 4); Reject(Command(0x014B, operands));
            operands[i] = Array(global, global ? 8 : 0, true, 136, 1, i < 4); Reject(Command(0x014B, operands));
            operands[i] = Array(global, global ? 8 : 0, false, 34, 1, i < 4); Reject(Command(0x014B, operands));
            for (const auto index : {-1, 2, std::numeric_limits<std::int32_t>::max()}) {
                Bytes setup; Assign(setup, false, 30, Integer(index));
                operands[i] = Array(global, global ? 8 : 0, false, 30, 2, i < 4); Reject(Command(0x014B, operands), setup, 1);
            }
            Bytes setup; Assign(setup, false, 30, Integer(1));
            operands[i] = Array(global, global ? 132 : 33, false, 30, 2, i < 4); Reject(Command(0x014B, operands), setup, 1);
        }
    }
    for (unsigned i = 0; i < 4; ++i) for (const auto value : {std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
        auto operands = Inputs(); operands[i] = Float(value); Reject(Command(0x014B, operands));
    }
    auto negated = valid; negated[1] |= 0x80; Reject(negated, {}, 0, Status::Unsupported);
    for (unsigned i = 0; i < 2; ++i) {
        Operands operands{}; operands[0] = Integer(0); operands[1] = Integer(100);
        operands[i] = Float(1); Reject(Command(0x014C, operands, 2));
        operands[i] = Variable(false, 34); Reject(Command(0x014C, operands, 2));
    }
    Operands operands{}; operands[0] = Integer(0); operands[1] = Integer(100);
    const auto switching = Command(0x014C, operands, 2);
    for (std::size_t end = 2; end < switching.size(); ++end) Reject(Bytes(switching.begin(), switching.begin() + end));
}
void Transactions() {
    for (const bool create : {false, true}) {
        Operands operands = Inputs();
        if (create) {
            // Timer-backed integer inputs + output array index change while pending.
            for (unsigned i = 4; i < 12; ++i) operands[i] = Variable(false, 32);
            operands[12] = Array(true, 8, false, 32, 2, false);
        } else { operands[0] = Variable(false, 32); operands[1] = Variable(false, 33); }
        const auto command = Command(create ? 0x014B : 0x014C, operands, create ? 13 : 2);
        NativeScriptSession session; Services services; services.Mode = ServiceStatus::Pending; services.Reference = 499;
        auto code = command; Put(code, 0, 2); Load(session, services, code);
        const auto before = session.State();
        Check(session.Step(services).Status == Status::Pending && session.State() == before && services.Committed.empty(), "Pending never commits state/output/effect");
        const auto id = create ? services.Creates.back().Id : services.Switches.back().Id;
        std::string error; Check(session.AdvanceTime(1, error), "advance timers while pending"); const auto timed = session.State();
        Check(session.Step(services).Status == Status::Pending && session.State() == timed && services.Committed.empty(), "repeated Pending is unadvanced");
        services.Mode = ServiceStatus::Ready;
        Check(session.Step(services).Status == Status::Advanced && services.Committed.size() == 1 && services.Committed.front() == id,
            "Ready commits once with identical pending request identity");
        if (create) {
            for (const auto& r : services.Creates) Check(r.Id == id && r.ModelId == 0 && r.PrimaryColor == 0 && r.SecondaryColor == 0 &&
                r.ForceSpawn == 0 && r.AlarmChance == 0 && r.DoorLockChance == 0 && r.MinDelay == 0 && r.MaxDelay == 0 &&
                r.Position == NativeScriptPosition{11.25f, -22.5f, 33.75f} && r.AngleDegrees == 94.125f, "all pending inputs retained");
            std::int32_t first = 0, second = 0;
            Check(session.ReadGlobal(8, first) && first == 499 && session.ReadGlobal(12, second) && second == 0x23456789,
                "Pending retains resolved thirteenth output, not new timer index");
            const auto replay = services.Creates.front(); services.CreateCarGenerator(replay);
        } else {
            for (const auto& r : services.Switches) Check(r.Id == id && r.Generator.Value == 0 && r.Count == 0, "014C Pending retains ref/count");
            const auto replay = services.Switches.front(); services.SwitchCarGenerator(replay);
        }
        Check(services.Committed.size() == 1, "TEST service implements required ID replay idempotence");
        Check(session.Step(services).Status == Status::Advanced && services.Committed.size() == 1, "next instruction does not repeat Ready service");
        for (const auto mode : {ServiceStatus::Unsupported, ServiceStatus::Error}) for (const bool pendingFirst : {false, true}) {
            NativeScriptSession failed; Services failing; failing.Mode = pendingFirst ? ServiceStatus::Pending : mode;
            Load(failed, failing, command); const auto original = failed.State();
            if (pendingFirst) Check(failed.Step(failing).Status == Status::Pending, "pending before terminal service result");
            failing.Mode = mode;
            const auto result = failed.Step(failing);
            Check(result.Status == (mode == ServiceStatus::Error ? Status::Error : Status::Unsupported) && result.Executed == 0 &&
                failed.State() == original && failing.Committed.empty(), "service failure preserves IP/count/output");
            const auto calls = failing.Creates.size() + failing.Switches.size();
            failed.Step(failing); Check(failing.Creates.size() + failing.Switches.size() == calls && failed.State() == original, "terminal fault never retries host");
        }
        NativeScriptSession failed; Services failing; failing.Throw = true; Load(failed, failing, command); const auto original = failed.State();
        Check(failed.Step(failing).Status == Status::Error && failed.State() == original && failing.Committed.empty(), "service exception leaves VM unadvanced");
    }
}
} // namespace

int main() {
    Basic(); BanksAndArrays(); Malformed(); Transactions();
    std::printf("NativeCarGeneratorVmProbe PASS checks=%zu operands=13 output=12 pending=frozen zeroRef=valid count=int32 modelMinus1=random fixtures=TEST-services-not-world\n", s_Checks);
}
