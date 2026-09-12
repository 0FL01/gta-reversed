// Main-thread transaction identity for asynchronous SCM services. Worker
// tickets are value-only and may cross threads; all state transitions remain
// on the service owner thread. Prepared is deliberately not script Ready.
#pragma once

#include "app/platform/linux/NativeScriptSession.h"

#include <array>
#include <cstdint>
#include <string>

enum class NativeScriptAsyncPrepareStatus : std::uint8_t {
    Pending,
    Prepared,
    Error,
};

struct NativeScriptAsyncPrepareResult {
    NativeScriptAsyncPrepareStatus Status = NativeScriptAsyncPrepareStatus::Error;
    std::string Message;
};

struct NativeScriptServiceIdentity {
    NativeScriptRequestId Id;
    std::uint16_t Opcode = 0;
    std::uint8_t ArgumentCount = 0;
    std::array<std::uint32_t, 8> Arguments{};
    bool operator==(const NativeScriptServiceIdentity&) const = default;
};

struct NativeScriptServiceTicket {
    std::uint64_t Owner = 0;
    std::uint64_t Attempt = 0;
    bool operator==(const NativeScriptServiceTicket&) const = default;
};

enum class NativeScriptServiceTransactionPhase : std::uint8_t {
    Idle,
    Preparing,
    Pending,
    Prepared,
    Cancelling,
    Cancelled,
    Committed,
    Failed,
};

enum class NativeScriptServiceTransactionStatus : std::uint8_t {
    Ok,
    Started,
    Existing,
    AlreadyCommitted,
    InvalidInput,
    Conflict,
    Busy,
    Stale,
    InvalidPhase,
    Overflow,
};

struct NativeScriptServiceTransactionState {
    NativeScriptServiceTransactionPhase Phase = NativeScriptServiceTransactionPhase::Idle;
    NativeScriptServiceIdentity Identity;
    NativeScriptServiceTicket Ticket;
    std::uint64_t Revision = 0;
    std::uint64_t Attempts = 0;
    std::uint64_t Commits = 0;
    std::uint64_t CancelRequests = 0;
    std::uint64_t Cancellations = 0;
    std::uint64_t Failures = 0;
    std::uint32_t Polls = 0;
    bool operator==(const NativeScriptServiceTransactionState&) const = default;
};

class NativeScriptServiceTransaction {
public:
    NativeScriptServiceTransaction();
    NativeScriptServiceTransaction(const NativeScriptServiceTransaction&) = delete;
    NativeScriptServiceTransaction& operator=(const NativeScriptServiceTransaction&) = delete;

    NativeScriptServiceTransactionStatus Begin(
        const NativeScriptServiceIdentity& identity, NativeScriptServiceTicket& ticket) noexcept;
    NativeScriptServiceTransactionStatus MarkPending(const NativeScriptServiceTicket& ticket) noexcept;
    NativeScriptServiceTransactionStatus MarkPrepared(const NativeScriptServiceTicket& ticket) noexcept;
    NativeScriptServiceTransactionStatus RequestCancel(
        const NativeScriptServiceIdentity& identity, NativeScriptServiceTicket& ticket) noexcept;
    NativeScriptServiceTransactionStatus AcknowledgeCancel(const NativeScriptServiceTicket& ticket) noexcept;
    NativeScriptServiceTransactionStatus Commit(const NativeScriptServiceTicket& ticket) noexcept;
    NativeScriptServiceTransactionStatus Fail(const NativeScriptServiceTicket& ticket) noexcept;
    NativeScriptServiceTransactionStatus Release(const NativeScriptServiceTicket& ticket) noexcept;

    const NativeScriptServiceTransactionState& State() const noexcept { return m_State; }

private:
    bool Current(const NativeScriptServiceTicket& ticket) const noexcept;
    NativeScriptServiceTransactionStatus Start(
        const NativeScriptServiceIdentity& identity, NativeScriptServiceTicket& ticket) noexcept;

    NativeScriptServiceTransactionState m_State;
    std::uint64_t m_NextAttempt = 0;
};
