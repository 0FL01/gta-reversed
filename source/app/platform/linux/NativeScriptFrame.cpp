#include "NativeScriptFrame.h"

#include <cassert>
#include <limits>
#include <type_traits>

static_assert(std::is_trivially_copyable_v<NativeScriptFrameEvent>);

void NativeScriptFrame::SyncEpoch() {
    if (m_Epoch == m_Session.SessionId()) return;
    m_Epoch = m_Session.SessionId();
    m_Frame = 0;
    m_Open = m_Faulted = m_PassFinished = false;
    m_Services = nullptr;
    m_Preparing.reset();
    m_Committed.reset();
    m_Fault = {};
    m_CompletedPass = {};
}

std::shared_ptr<const NativeScriptFrameSnapshot> NativeScriptFrame::LastCommitted() {
    SyncEpoch();
    return m_Committed;
}

bool NativeScriptFrame::PassOpen() {
    SyncEpoch();
    return m_Open;
}

NativeScriptFrameResult NativeScriptFrame::Begin(const NativeSourceClock& clock, const NativeSourcePad& pad,
    NativeScriptServices& services, std::size_t quota) {
    if (m_InCall) return {NativeScriptFrameStatus::Rejected, {}, "reentrant frame call"};
    SyncEpoch();
    if (!quota || !m_Session.Loaded() || !clock.State().Initialised)
        return {NativeScriptFrameStatus::Rejected, {}, "unloaded/uninitialised frame or zero quota"};
    if (m_Faulted) return {NativeScriptFrameStatus::Fault, m_Fault, "sticky script fault"};
    if (m_Open || m_Session.PassOutstanding())
        return {NativeScriptFrameStatus::Rejected, {}, "pass already outstanding"};
    if (clock.State().GameMs < m_Session.State().TimeMs || clock.State().SuspendDepth)
        return {NativeScriptFrameStatus::Rejected, {}, "backward game time or suspended clock"};
    if (clock.State().UserPause || clock.State().CodePause)
        return {NativeScriptFrameStatus::Paused, {}, {}};
    if (m_Frame == std::numeric_limits<std::uint64_t>::max())
        return {NativeScriptFrameStatus::Rejected, {}, "frame sequence exhausted"};

    auto candidate = std::make_shared<NativeScriptFrameSnapshot>();
    candidate->Epoch = m_Epoch;
    candidate->Frame = m_Frame + 1;
    candidate->SampledClock = clock.State();
    candidate->SampledPad = pad.LastFrame();
    candidate->HasPad = pad.HasFrame();
    candidate->Events.reserve(quota); // before advancing time or any host effect
    std::string error;
    if (!m_Session.AdvanceTime(clock.State().GameMs, error))
        return {NativeScriptFrameStatus::Rejected, {}, std::move(error)};
    m_Preparing = std::move(candidate);
    m_Services = &services;
    m_Open = true;
    m_PassFinished = false;
    return Drive(services, quota);
}

NativeScriptFrameResult NativeScriptFrame::Continue(NativeScriptServices& services, std::size_t quota) {
    if (m_InCall) return {NativeScriptFrameStatus::Rejected, {}, "reentrant frame call"};
    SyncEpoch();
    if (m_Faulted) return {NativeScriptFrameStatus::Fault, m_Fault, "sticky script fault"};
    if (!quota || !m_Open || &services != m_Services)
        return {NativeScriptFrameStatus::Rejected, {}, "no open pass, changed services or zero quota"};
    // Exclusive-session invariant: callers may reload, but cannot advance VM
    // time behind the frame's back and then publish a mismatched snapshot.
    if (m_Session.State().TimeMs != m_Preparing->SampledClock.GameMs)
        return {NativeScriptFrameStatus::Rejected, {}, "session time changed during pass"};
    return Drive(services, quota);
}

NativeScriptFrameResult NativeScriptFrame::Drive(NativeScriptServices& services, std::size_t quota) {
    auto& events = m_Preparing->Events;
    if (quota > events.max_size() - events.size())
        return {NativeScriptFrameStatus::Rejected, {}, "event capacity overflow"};
    events.reserve(events.size() + quota);
    struct Guard {
        bool& Flag;
        explicit Guard(bool& flag) : Flag(flag) { Flag = true; }
        ~Guard() { Flag = false; }
    } guard{m_InCall};
    // If allocating the final owned copy throws, a retry must finish that copy,
    // never execute a second pass at the same frame boundary.
    auto result = m_PassFinished ? m_CompletedPass : m_Session.RunPass(services, quota, this);
    if (result.Status == NativeScriptStatus::BudgetYield || result.Status == NativeScriptStatus::Pending)
        return {NativeScriptFrameStatus::Open, result, {}};
    if (result.Status != NativeScriptStatus::Waiting) {
        m_Faulted = true;
        m_Fault = result;
        m_Open = false;
        m_Preparing.reset();
        return {NativeScriptFrameStatus::Fault, result, "partial pass not published; effects are not rolled back"};
    }
    m_CompletedPass = result;
    m_PassFinished = true;
    m_Preparing->State = m_Session.State();
    const auto threads = m_Session.Threads();
    m_Preparing->Threads.assign(threads.begin(), threads.end());
    m_Preparing->Globals.resize(m_Session.Metadata().GlobalBytes / 4);
    for (std::size_t i = 0; i < m_Preparing->Globals.size(); ++i) {
        const bool read = m_Session.ReadGlobal(static_cast<std::uint16_t>(8 + i * 4), m_Preparing->Globals[i]);
        assert(read); // validated SCM global range, not caller input
        (void)read;
    }
    m_Frame = m_Preparing->Frame;
    m_Committed = std::move(m_Preparing);
    m_Open = false;
    m_PassFinished = false;
    m_Services = nullptr;
    return {NativeScriptFrameStatus::Committed, result, {}};
}

void NativeScriptFrame::OnScriptCommit(NativeScriptRequestId id, std::size_t threadIndex,
    const NativeScriptState& state, const NativeScriptThreadState& thread) noexcept {
    assert(m_Preparing && m_Preparing->Events.size() < m_Preparing->Events.capacity());
    m_Preparing->Events.push_back({id, threadIndex, thread.Generation, thread.LastOpcode,
        state.Clock, state.Fade, thread.LastOutputWrite});
}
