// Generated SCM fixtures are tests, not original assets or real world owners.
#include "NativeScriptFrame.h"

#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <type_traits>

namespace {
using Bytes = std::vector<std::uint8_t>;
using FS = NativeScriptFrameStatus;
using SS = NativeScriptStatus;
std::size_t s_Checks = 0;

void Check(bool value, const char* message) {
    ++s_Checks;
    if (!value) { std::fprintf(stderr, "sa-core-frame FAIL: %s\n", message); std::exit(1); }
}
void Put(Bytes& b, std::uint32_t value, unsigned count) {
    for (unsigned i = 0; i < count; ++i) b.push_back(std::uint8_t(value >> (8 * i)));
}
void Patch(Bytes& b, std::size_t offset, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) b.at(offset + i) = std::uint8_t(value >> (8 * i));
}
void Op(Bytes& b, std::uint16_t value) { Put(b, value, 2); }
void I8(Bytes& b, std::uint8_t value) { b.push_back(4); b.push_back(value); }
void I32(Bytes& b, std::uint32_t value) { b.push_back(1); Put(b, value, 4); }
void F(Bytes& b, float value) { b.push_back(6); Put(b, std::bit_cast<std::uint32_t>(value), 4); }
void Wait(Bytes& b, std::uint8_t ms) { Op(b, 1); I8(b, ms); }
void Clock(Bytes& b, std::uint8_t hours) { Op(b, 0xC0); I8(b, hours); I8(b, 30); }
void Fade(Bytes& b, std::uint8_t direction) { Op(b, 0x16A); I32(b, 1000); I8(b, direction); }
void Write(Bytes& b, std::uint8_t value) { Op(b, 4); b.push_back(2); Put(b, 8, 2); I8(b, value); }
Bytes Fixture(const Bytes& code, const std::vector<Bytes>& missions = {}) {
    Bytes b;
    auto chunk = [&](std::uint8_t index, const Bytes& payload) {
        const auto next = std::uint32_t(b.size() + payload.size() + 8);
        Op(b, 2); I32(b, next); b.push_back(index);
        b.insert(b.end(), payload.begin(), payload.end());
    };
    chunk(115, Bytes(16));
    chunk(0, Bytes(4));
    Bytes info(16);
    const auto codeStart = 104u + std::uint32_t(missions.size()) * 4;
    const auto mainSize = codeStart + std::uint32_t(code.size());
    Patch(info, 0, mainSize); Patch(info, 8, std::uint32_t(missions.size()));
    std::uint32_t offset = mainSize, largest = 0;
    for (const auto& mission : missions) {
        Put(info, offset, 4); offset += std::uint32_t(mission.size());
        largest = std::max(largest, std::uint32_t(mission.size()));
    }
    Patch(info, 4, largest); Patch(info, 12, missions.empty() ? 0 : 1024);
    chunk(1, info); chunk(2, Bytes(8)); chunk(3, Bytes(4));
    Bytes extra(8); Patch(extra, 0, 16); chunk(4, extra);
    Check(b.size() == codeStart, "generated header size");
    b.insert(b.end(), code.begin(), code.end());
    for (const auto& mission : missions) b.insert(b.end(), mission.begin(), mission.end());
    return b;
}
void Load(NativeScriptSession& session, const Bytes& bytes) {
    std::string error;
    Check(session.LoadMainBytes(bytes, bytes.size(), error), error.c_str());
}
struct Services final : NativeScriptServices {
    NativeScriptServiceStatus Mode = NativeScriptServiceStatus::Unsupported;
    unsigned Calls = 0;
    NativeScriptRequestId LastId;
    NativeScriptFrame* Frame = nullptr;
    NativeSourceClock* ClockOwner = nullptr;
    NativeSourcePad* PadOwner = nullptr;
    NativeScriptServiceResult RequestCollision(const NativeScriptCollisionRequest& r) override {
        ++Calls; LastId = r.Id;
        if (Frame) {
            Check(Frame->Continue(*this, 1).Status == FS::Rejected, "service reentry rejected");
            Check(Frame->Begin(*ClockOwner, *PadOwner, *this, 1).Status == FS::Rejected, "service begin reentry rejected");
        }
        return {Mode, "test-only collision / real world unavailable"};
    }
    NativeScriptServiceResult LoadScene(const NativeScriptSceneRequest&) override { return {}; }
    NativeScriptServiceResult CreatePlayer(const NativeScriptPlayerRequest&) override { return {}; }
};
void Tick(NativeSourceClock& clock, std::uint64_t ms) {
    Check(clock.Tick(ms * 1000000) == NativeSourceClockStatus::Ok, "clock tick");
}

void Boundary() {
    NativeScriptSession session;
    NativeScriptFrame frame(session);
    NativeSourceClock clock;
    NativeSourcePad pad;
    Services services;
    Check(frame.Begin(clock, pad, services, 10).Status == FS::Rejected, "unloaded rejected");
    Bytes code; Clock(code, 6); Fade(code, 0); Write(code, 7); Wait(code, 0);
    Clock(code, 9); Fade(code, 1); Write(code, 8); Wait(code, 0); Op(code, 0x4E);
    const auto bytes = Fixture(code);
    Load(session, bytes);
    Check(frame.Begin(clock, pad, services, 10).Status == FS::Rejected, "uninitialised rejected");
    Check(clock.Initialise(0) == NativeSourceClockStatus::Ok, "initialise");
    Tick(clock, 10);
    NativeSourcePadFrame input;
    Check(pad.SubmitSample({1, 10, 12, -8, 1}, input) == NativeSourcePadStatus::Ok, "sample");
    const auto initial = session.State();
    Check(frame.Begin(clock, pad, services, 0).Status == FS::Rejected && session.State() == initial, "zero quota atomic");
    // The six executable SCM header jumps precede the authored fixture body.
    auto r = frame.Begin(clock, pad, services, 7);
    Check(r.Status == FS::Open && r.Script.Status == SS::BudgetYield, "quota starts one pass");
    Check(!frame.LastCommitted(), "no partial snapshot");
    const auto partial = session.State();
    Tick(clock, 30);
    Check(pad.SubmitSample({2, 30, 0, 0, 0}, input) == NativeSourcePadStatus::Ok, "next sample");
    Check(frame.Begin(clock, pad, services, 10).Status == FS::Rejected && session.State() == partial, "cannot begin over open pass");
    Services other;
    Check(frame.Continue(other, 10).Status == FS::Rejected, "cannot switch service owner");
    Check(frame.Continue(services, 0).Status == FS::Rejected, "zero continuation quota");
    r = frame.Continue(services, 3);
    Check(r.Status == FS::Committed && r.Script.Executed == 3, "exact quota final WAIT completes");
    const auto first = frame.LastCommitted();
    const auto firstCopy = *first;
    Check(first->Frame == 1 && first->State.TimeMs == 10 && first->SampledClock.GameMs == 10, "frozen pass time");
    Check(first->State.Locals[32] == 10 && first->SampledPad.Sample.Seq == 1, "one timer/input advance");
    Check(first->Events.size() == 10 && first->Globals[0] == 7, "committed events and globals");
    Check(first->Events[6].Opcode == 0xC0 && first->Events[6].Clock.Hours == 6, "clock event value");
    Check(first->Events[7].Opcode == 0x16A && first->Events[7].Fade.Direction == 0, "fade event value");
    Check(first->Events[8].Output.Value == 7, "output event value");
    for (std::size_t i = 0; i < first->Events.size(); ++i) {
        Check(first->Events[i].Id.Instruction == i + 1, "execution event order");
        Check(first->Events[i].Id.Session == first->Epoch, "event epoch");
    }
    Check(clock.SetUserPause(true) == NativeSourceClockStatus::Ok, "pause");
    const auto beforePause = session.State();
    Tick(clock, 40);
    Check(frame.Begin(clock, pad, services, 10).Status == FS::Paused, "pause no pass");
    Check(session.State() == beforePause && frame.LastCommitted() == first, "pause retains state and presentation");
    Check(clock.SetUserPause(false) == NativeSourceClockStatus::Ok, "resume");
    Check(clock.SetCodePause(true) == NativeSourceClockStatus::Ok, "code pause");
    Check(frame.Begin(clock, pad, services, 10).Status == FS::Paused && session.State() == beforePause, "code pause no pass");
    Check(clock.SetCodePause(false) == NativeSourceClockStatus::Ok, "code resume");
    Tick(clock, 50);
    Check(frame.Begin(clock, pad, services, 10).Status == FS::Committed, "second frame");
    const auto second = frame.LastCommitted();
    Check(second->Frame == 2 && second->Globals[0] == 8 && second->State.Clock.Hours == 9, "second frame data");
    Check(*first == firstCopy && first->Globals[0] == 7, "retained snapshot immutable");
    const auto stateBefore = session.State();
    Check(clock.Initialise(0) == NativeSourceClockStatus::Ok, "clock reset for rejection");
    Check(frame.Begin(clock, pad, services, 10).Status == FS::Rejected && session.State() == stateBefore, "backward game time atomic");
    std::string error;
    const auto epoch = session.SessionId();
    Check(!session.LoadMainBytes({}, 0, error) && session.SessionId() == epoch, "failed reload epoch retained");
    Check(frame.LastCommitted() == second, "failed reload presentation retained");
    Load(session, bytes);
    Check(session.SessionId() != epoch && !frame.LastCommitted() && !frame.PassOpen(), "successful reload invalidates current epoch");
    Check(*first == firstCopy, "reload cannot mutate retained snapshot");
    Check(frame.Begin(clock, pad, services, 1).Status == FS::Open, "open before reload");
    Load(session, bytes);
    Check(frame.Continue(services, 10).Status == FS::Rejected && !frame.PassOpen(), "reload cancels old open pass");
    Check(frame.Begin(clock, pad, services, 100).Status == FS::Committed && frame.LastCommitted()->Frame == 1, "new epoch frame counter restarts");
}

void PendingAndFault() {
    NativeScriptSession session;
    NativeScriptFrame frame(session);
    NativeSourceClock clock; NativeSourcePad pad; Services services;
    Check(clock.Initialise(0) == NativeSourceClockStatus::Ok, "pending clock init");
    Bytes code; Wait(code, 0); Clock(code, 7); Op(code, 0x4E4); F(code, 1.0f); F(code, 2.0f);
    Fade(code, 1); Wait(code, 0); Op(code, 0xFFFF);
    Load(session, Fixture(code));
    Check(frame.Begin(clock, pad, services, 10).Status == FS::Committed, "baseline before pending");
    const auto prior = frame.LastCommitted();
    services.Mode = NativeScriptServiceStatus::Pending;
    services.Frame = &frame; services.ClockOwner = &clock; services.PadOwner = &pad;
    Tick(clock, 5);
    auto r = frame.Begin(clock, pad, services, 10);
    Check(r.Status == FS::Open && r.Script.Status == SS::Pending, "pending opens frame");
    const auto id = services.LastId;
    Check(frame.LastCommitted() == prior, "pending retains previous presentation");
    Tick(clock, 15);
    Check(frame.Begin(clock, pad, services, 10).Status == FS::Rejected, "pending freezes pass time");
    Check(frame.Continue(services, 10).Status == FS::Open && services.LastId == id, "pending poll same id");
    services.Mode = NativeScriptServiceStatus::Ready;
    Check(frame.Continue(services, 10).Status == FS::Committed, "ready completes pending frame");
    const auto ready = frame.LastCommitted();
    Check(services.Calls == 3 && ready->Events.size() == 4, "poll attempts not presentation events");
    Check(ready->Events[1].Id == id && ready->Events[1].Opcode == 0x4E4, "one ready commit in exact order");
    Check(ready->State.TimeMs == 5 && ready->State.Locals[32] == 5, "no time advancement during continuation");
    r = frame.Begin(clock, pad, services, 10);
    Check(r.Status == FS::Fault && r.Script.Status == SS::Unsupported, "unsupported remains fault");
    Check(frame.LastCommitted() == ready, "fault retains completed presentation");
    const auto faultState = session.State();
    Check(frame.Continue(services, 10).Status == FS::Fault && session.State() == faultState, "fault sticky");
}

void MissionAndEmpty() {
    NativeScriptSession session; NativeScriptFrame frame(session);
    NativeSourceClock clock; NativeSourcePad pad; Services services;
    Check(clock.Initialise(0) == NativeSourceClockStatus::Ok, "mission clock init");
    Bytes code; Op(code, 0x417); I8(code, 0); Wait(code, 0); Clock(code, 8); Op(code, 0x4E);
    Bytes mission; Clock(mission, 3); Op(mission, 0x4E);
    Load(session, Fixture(code, {mission}));
    Check(frame.Begin(clock, pad, services, 10).Status == FS::Committed, "mission launch pass");
    Check(frame.LastCommitted()->Events.size() == 8 && frame.LastCommitted()->Threads[1].Commands == 0, "new thread deferred to next pass");
    Tick(clock, 1);
    Check(frame.Begin(clock, pad, services, 10).Status == FS::Committed, "mission next pass");
    const auto snapshot = frame.LastCommitted();
    Check(snapshot->Events.size() == 4 && snapshot->Events[0].ThreadIndex == 1 && snapshot->Events[2].ThreadIndex == 0, "mission head-first order");
    Check(snapshot->Events[0].Clock.Hours == 3 && snapshot->Events[2].Clock.Hours == 8, "ordered setter values not only final state");
    Tick(clock, 2);
    const auto r = frame.Begin(clock, pad, services, 10);
    Check(r.Status == FS::Committed && r.Script.Executed == 0 && frame.LastCommitted()->Events.empty(), "empty active list commits empty frame");
}

void Real(const char* gameDir) {
    NativeScriptSession session; std::string error;
    Check(session.LoadMain(gameDir, error), error.c_str());
    NativeScriptFrame frame(session); NativeSourceClock clock; NativeSourcePad pad; Services services;
    Check(clock.Initialise(0) == NativeSourceClockStatus::Ok, "real clock init");
    Tick(clock, 123);
    const auto r = frame.Begin(clock, pad, services, 100);
    Check(r.Status == FS::Fault && r.Script.Status == SS::Unsupported && r.Script.Opcode == 0x4E4 &&
        r.Script.IP == 56022 && r.Script.Executed == 14, "real explicit collision frontier unchanged");
    Check(!frame.LastCommitted() && services.Calls == 1, "no fake real frame/boot publication");
    std::printf("real-frame-frontier opcode=04E4 ip=%u commits=%zu boot=false\n", r.Script.IP, r.Script.Executed);
}
} // namespace

int main(int argc, char** argv) {
    static_assert(std::is_trivially_copyable_v<NativeScriptFrameEvent>);
    static_assert(std::is_same_v<decltype(std::declval<NativeScriptFrame&>().LastCommitted()),
        std::shared_ptr<const NativeScriptFrameSnapshot>>);
    static_assert(!std::is_assignable_v<decltype(*std::declval<NativeScriptFrame&>().LastCommitted()),
        NativeScriptFrameSnapshot>);
    Boundary(); PendingAndFault(); MissionAndEmpty();
    if (argc == 2) Real(argv[1]);
    else Check(argc == 1, "usage: sa_core_frame_probe [game-dir]");
    std::printf("sa-core-frame-ok checks=%zu\n", s_Checks);
}
