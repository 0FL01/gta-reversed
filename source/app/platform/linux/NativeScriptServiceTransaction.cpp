#include "app/platform/linux/NativeScriptServiceTransaction.h"

#include <atomic>
#include <limits>
#include <stdexcept>

namespace {
std::atomic<std::uint64_t> s_NextOwner{1};

std::uint64_t AllocateOwner() {
    auto value = s_NextOwner.load(std::memory_order_relaxed);
    while (value != std::numeric_limits<std::uint64_t>::max()) {
        if (s_NextOwner.compare_exchange_weak(value, value + 1, std::memory_order_relaxed)) return value;
    }
    throw std::overflow_error("script service transaction owner identity exhausted");
}

bool Valid(const NativeScriptServiceIdentity& identity) {
    if (!identity.Opcode || identity.ArgumentCount > identity.Arguments.size()) return false;
    for (std::size_t i = identity.ArgumentCount; i < identity.Arguments.size(); ++i) {
        if (identity.Arguments[i]) return false;
    }
    return true;
}

bool Increment(std::uint64_t& value) {
    if (value == std::numeric_limits<std::uint64_t>::max()) return false;
    ++value;
    return true;
}
}

NativeScriptServiceTransaction::NativeScriptServiceTransaction() {
    m_State.Ticket.Owner = AllocateOwner();
}

bool NativeScriptServiceTransaction::Current(const NativeScriptServiceTicket& ticket) const noexcept {
    return ticket.Owner == m_State.Ticket.Owner && ticket.Attempt && ticket.Attempt == m_State.Ticket.Attempt;
}

NativeScriptServiceTransactionStatus NativeScriptServiceTransaction::Start(
    const NativeScriptServiceIdentity& identity, NativeScriptServiceTicket& ticket) noexcept {
    if (m_NextAttempt == std::numeric_limits<std::uint64_t>::max() ||
        m_State.Attempts == std::numeric_limits<std::uint64_t>::max() ||
        m_State.Revision == std::numeric_limits<std::uint64_t>::max()) {
        return NativeScriptServiceTransactionStatus::Overflow;
    }
    ++m_NextAttempt;
    ++m_State.Attempts;
    ++m_State.Revision;
    m_State.Phase = NativeScriptServiceTransactionPhase::Preparing;
    m_State.Identity = identity;
    m_State.Ticket.Attempt = m_NextAttempt;
    m_State.Polls = 0;
    ticket = m_State.Ticket;
    return NativeScriptServiceTransactionStatus::Started;
}

NativeScriptServiceTransactionStatus NativeScriptServiceTransaction::Begin(
    const NativeScriptServiceIdentity& identity, NativeScriptServiceTicket& ticket) noexcept {
    if (!Valid(identity)) return NativeScriptServiceTransactionStatus::InvalidInput;
    if (m_State.Phase == NativeScriptServiceTransactionPhase::Idle) return Start(identity, ticket);
    if (m_State.Identity.Id == identity.Id && !(m_State.Identity == identity)) {
        return NativeScriptServiceTransactionStatus::Conflict;
    }
    if (!(m_State.Identity == identity)) return NativeScriptServiceTransactionStatus::Busy;
    if (m_State.Phase == NativeScriptServiceTransactionPhase::Cancelled) return Start(identity, ticket);
    ticket = m_State.Ticket;
    if (m_State.Phase == NativeScriptServiceTransactionPhase::Committed) {
        return NativeScriptServiceTransactionStatus::AlreadyCommitted;
    }
    if (m_State.Phase == NativeScriptServiceTransactionPhase::Failed) {
        return NativeScriptServiceTransactionStatus::InvalidPhase;
    }
    return NativeScriptServiceTransactionStatus::Existing;
}

NativeScriptServiceTransactionStatus NativeScriptServiceTransaction::MarkPending(
    const NativeScriptServiceTicket& ticket) noexcept {
    if (!Current(ticket)) return NativeScriptServiceTransactionStatus::Stale;
    if (m_State.Phase != NativeScriptServiceTransactionPhase::Preparing &&
        m_State.Phase != NativeScriptServiceTransactionPhase::Pending) {
        return NativeScriptServiceTransactionStatus::InvalidPhase;
    }
    if (m_State.Polls == std::numeric_limits<std::uint32_t>::max() ||
        !Increment(m_State.Revision)) return NativeScriptServiceTransactionStatus::Overflow;
    ++m_State.Polls;
    m_State.Phase = NativeScriptServiceTransactionPhase::Pending;
    return NativeScriptServiceTransactionStatus::Ok;
}

NativeScriptServiceTransactionStatus NativeScriptServiceTransaction::MarkPrepared(
    const NativeScriptServiceTicket& ticket) noexcept {
    if (!Current(ticket)) return NativeScriptServiceTransactionStatus::Stale;
    if (m_State.Phase != NativeScriptServiceTransactionPhase::Preparing &&
        m_State.Phase != NativeScriptServiceTransactionPhase::Pending) {
        return NativeScriptServiceTransactionStatus::InvalidPhase;
    }
    if (!Increment(m_State.Revision)) return NativeScriptServiceTransactionStatus::Overflow;
    m_State.Phase = NativeScriptServiceTransactionPhase::Prepared;
    return NativeScriptServiceTransactionStatus::Ok;
}

NativeScriptServiceTransactionStatus NativeScriptServiceTransaction::RequestCancel(
    const NativeScriptServiceIdentity& identity, NativeScriptServiceTicket& ticket) noexcept {
    if (!(m_State.Identity == identity)) {
        return m_State.Identity.Id == identity.Id ? NativeScriptServiceTransactionStatus::Conflict
                                                  : NativeScriptServiceTransactionStatus::Stale;
    }
    ticket = m_State.Ticket;
    if (m_State.Phase == NativeScriptServiceTransactionPhase::Cancelling) {
        return NativeScriptServiceTransactionStatus::Existing;
    }
    if (m_State.Phase != NativeScriptServiceTransactionPhase::Preparing &&
        m_State.Phase != NativeScriptServiceTransactionPhase::Pending &&
        m_State.Phase != NativeScriptServiceTransactionPhase::Prepared) {
        return NativeScriptServiceTransactionStatus::InvalidPhase;
    }
    if (m_State.CancelRequests == std::numeric_limits<std::uint64_t>::max() ||
        !Increment(m_State.Revision)) return NativeScriptServiceTransactionStatus::Overflow;
    ++m_State.CancelRequests;
    m_State.Phase = NativeScriptServiceTransactionPhase::Cancelling;
    return NativeScriptServiceTransactionStatus::Started;
}

NativeScriptServiceTransactionStatus NativeScriptServiceTransaction::AcknowledgeCancel(
    const NativeScriptServiceTicket& ticket) noexcept {
    if (!Current(ticket)) return NativeScriptServiceTransactionStatus::Stale;
    if (m_State.Phase != NativeScriptServiceTransactionPhase::Cancelling) {
        return NativeScriptServiceTransactionStatus::InvalidPhase;
    }
    if (m_State.Cancellations == std::numeric_limits<std::uint64_t>::max() ||
        !Increment(m_State.Revision)) return NativeScriptServiceTransactionStatus::Overflow;
    ++m_State.Cancellations;
    m_State.Phase = NativeScriptServiceTransactionPhase::Cancelled;
    return NativeScriptServiceTransactionStatus::Ok;
}

NativeScriptServiceTransactionStatus NativeScriptServiceTransaction::Commit(
    const NativeScriptServiceTicket& ticket) noexcept {
    if (!Current(ticket)) return NativeScriptServiceTransactionStatus::Stale;
    if (m_State.Phase == NativeScriptServiceTransactionPhase::Committed) {
        return NativeScriptServiceTransactionStatus::AlreadyCommitted;
    }
    if (m_State.Phase != NativeScriptServiceTransactionPhase::Prepared) {
        return NativeScriptServiceTransactionStatus::InvalidPhase;
    }
    if (m_State.Commits == std::numeric_limits<std::uint64_t>::max() ||
        !Increment(m_State.Revision)) return NativeScriptServiceTransactionStatus::Overflow;
    ++m_State.Commits;
    m_State.Phase = NativeScriptServiceTransactionPhase::Committed;
    return NativeScriptServiceTransactionStatus::Ok;
}

NativeScriptServiceTransactionStatus NativeScriptServiceTransaction::Fail(
    const NativeScriptServiceTicket& ticket) noexcept {
    if (!Current(ticket)) return NativeScriptServiceTransactionStatus::Stale;
    if (m_State.Phase != NativeScriptServiceTransactionPhase::Preparing &&
        m_State.Phase != NativeScriptServiceTransactionPhase::Pending &&
        m_State.Phase != NativeScriptServiceTransactionPhase::Prepared) {
        return NativeScriptServiceTransactionStatus::InvalidPhase;
    }
    if (m_State.Failures == std::numeric_limits<std::uint64_t>::max() ||
        !Increment(m_State.Revision)) return NativeScriptServiceTransactionStatus::Overflow;
    ++m_State.Failures;
    m_State.Phase = NativeScriptServiceTransactionPhase::Failed;
    return NativeScriptServiceTransactionStatus::Ok;
}

NativeScriptServiceTransactionStatus NativeScriptServiceTransaction::Release(
    const NativeScriptServiceTicket& ticket) noexcept {
    if (!Current(ticket)) return NativeScriptServiceTransactionStatus::Stale;
    if (m_State.Phase != NativeScriptServiceTransactionPhase::Committed &&
        m_State.Phase != NativeScriptServiceTransactionPhase::Failed) {
        return NativeScriptServiceTransactionStatus::InvalidPhase;
    }
    if (!Increment(m_State.Revision)) return NativeScriptServiceTransactionStatus::Overflow;
    m_State.Phase = NativeScriptServiceTransactionPhase::Idle;
    m_State.Identity = {};
    m_State.Ticket.Attempt = 0;
    m_State.Polls = 0;
    return NativeScriptServiceTransactionStatus::Ok;
}
