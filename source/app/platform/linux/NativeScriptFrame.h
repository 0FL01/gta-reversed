// Bounded committed-pass presentation seam. RunPass remains the sole scheduler.
// Not a gameplay host, save codec, service implementation or second clock.
#pragma once

#include "NativeScriptSession.h"
#include "NativeSourceClock.h"
#include "NativeSourcePad.h"

#include <memory>

// Value-only observations; clock/fade are setter states, NOT new authorities.
// Revisions identify changes. An opcode observation does not implement its
// missing presentation consumers (e.g. source stat notifications).
struct NativeScriptFrameEvent {
    NativeScriptRequestId Id;
    std::size_t ThreadIndex = 0;
    std::uint64_t ThreadGeneration = 0;
    std::uint16_t Opcode = 0;
    NativeScriptClock Clock;
    NativeScriptFade Fade;
    NativeScriptWriteEvent Output;
    bool operator==(const NativeScriptFrameEvent&) const = default;
};

struct NativeScriptFrameSnapshot {
    std::uint64_t Epoch = 0, Frame = 0;
    NativeSourceClockState SampledClock;
    NativeSourcePadFrame SampledPad;
    bool HasPad = false;
    NativeScriptState State;
    std::vector<NativeScriptThreadState> Threads;
    // Value-only external registry state; script payload bytes remain private
    // to NativeScriptSession and cannot escape through presentation snapshots.
    std::vector<NativeScriptStreamedState> StreamedScripts;
    // Cell i corresponds to source global byte offset 8 + 4*i.
    std::vector<std::int32_t> Globals;
    std::vector<NativeScriptFrameEvent> Events;
    bool operator==(const NativeScriptFrameSnapshot&) const = default;
};

enum class NativeScriptFrameStatus { Committed, Open, Paused, Rejected, Fault };
struct NativeScriptFrameResult {
    NativeScriptFrameStatus Status = NativeScriptFrameStatus::Rejected;
    NativeScriptResult Script;
    std::string Message;
};

class NativeScriptFrame final : private NativeScriptCommitSink {
public:
    // Borrowed session must outlive this seam and is exclusively driven by it
    // during an open pass. Services must outlive continuation/cancellation.
    explicit NativeScriptFrame(NativeScriptSession& session) : m_Session(session) {}
    NativeScriptFrame(const NativeScriptFrame&) = delete;
    NativeScriptFrame& operator=(const NativeScriptFrame&) = delete;

    // Caller ticks/samples the existing owners. Copy their values exactly once;
    // advance VM time once, then call RunPass. Paused frames do not run scripts.
    // Both Pending and BudgetYield continue with this frozen sample. This is a
    // bounded host policy; the bare Session API remains unchanged.
    NativeScriptFrameResult Begin(const NativeSourceClock& clock, const NativeSourcePad& pad,
        NativeScriptServices& services, std::size_t quota);
    NativeScriptFrameResult Continue(NativeScriptServices& services, std::size_t quota);
    // Successful reload invalidates current presentation; already retained
    // shared_ptr<const Snapshot> values remain alive and immutable. Failed
    // reload does not change the epoch or presentation. No asset ownership.
    std::shared_ptr<const NativeScriptFrameSnapshot> LastCommitted();
    bool PassOpen();

private:
    void SyncEpoch();
    NativeScriptFrameResult Drive(NativeScriptServices& services, std::size_t quota);
    void OnScriptCommit(NativeScriptRequestId id, std::size_t threadIndex,
        const NativeScriptState& state, const NativeScriptThreadState& thread) noexcept override;

    NativeScriptSession& m_Session;
    std::uint64_t m_Epoch = 0, m_Frame = 0;
    bool m_Open = false, m_InCall = false, m_Faulted = false, m_PassFinished = false;
    NativeScriptServices* m_Services = nullptr;
    NativeScriptResult m_Fault;
    NativeScriptResult m_CompletedPass;
    std::shared_ptr<NativeScriptFrameSnapshot> m_Preparing;
    std::shared_ptr<const NativeScriptFrameSnapshot> m_Committed;
};
