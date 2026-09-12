#include "app/platform/linux/NativeScriptServiceTransaction.h"

#include <bit>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {
int s_Checks = 0;

void Check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "service transaction probe: %s\n", message);
        std::exit(2);
    }
    ++s_Checks;
}

NativeScriptServiceIdentity Identity(std::uint64_t instruction = 2, float x = 1.0f) {
    NativeScriptServiceIdentity identity;
    identity.Id = {7, instruction, 100};
    identity.Opcode = 0x04E4;
    identity.ArgumentCount = 2;
    identity.Arguments[0] = std::bit_cast<std::uint32_t>(x);
    identity.Arguments[1] = std::bit_cast<std::uint32_t>(2.0f);
    return identity;
}

using Bytes = std::vector<std::uint8_t>;
void Put(Bytes& bytes, std::uint32_t value, unsigned count) {
    for (unsigned i = 0; i < count; ++i) bytes.push_back(std::uint8_t(value >> (8 * i)));
}
void Patch(Bytes& bytes, std::size_t offset, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bytes.at(offset + i) = std::uint8_t(value >> (8 * i));
}
void Op(Bytes& bytes, std::uint16_t value) { Put(bytes, value, 2); }
void I8(Bytes& bytes, std::uint8_t value) { bytes.push_back(4); bytes.push_back(value); }
void I32(Bytes& bytes, std::uint32_t value) { bytes.push_back(1); Put(bytes, value, 4); }
void Float(Bytes& bytes, float value) { bytes.push_back(6); Put(bytes, std::bit_cast<std::uint32_t>(value), 4); }
Bytes Fixture(const Bytes& code) {
    Bytes bytes;
    const auto chunk = [&](std::uint8_t index, const Bytes& payload) {
        const auto next = std::uint32_t(bytes.size() + payload.size() + 8);
        Op(bytes, 2); I32(bytes, next); bytes.push_back(index);
        bytes.insert(bytes.end(), payload.begin(), payload.end());
    };
    chunk(115, Bytes(16));
    chunk(0, Bytes(4));
    Bytes info(16);
    Patch(info, 0, 104 + std::uint32_t(code.size()));
    chunk(1, info); chunk(2, Bytes(8)); chunk(3, Bytes(4));
    Bytes extra(8); Patch(extra, 0, 16); chunk(4, extra);
    Check(bytes.size() == 104, "generated service fixture header");
    bytes.insert(bytes.end(), code.begin(), code.end());
    return bytes;
}

struct Services final : NativeScriptServices {
    NativeScriptServiceTransaction Transaction;
    NativeScriptRequestId FirstId;
    unsigned Calls = 0;
    unsigned WorkerPolls = 0;
    unsigned Effects = 0;
    bool WorkerPrepared = false;
    bool PreparedWasNotReady = false;

    NativeScriptServiceResult RequestCollision(const NativeScriptCollisionRequest&) override {
        return {NativeScriptServiceStatus::Unsupported, "not used"};
    }
    NativeScriptServiceResult LoadScene(const NativeScriptSceneRequest& request) override {
        ++Calls;
        if (!FirstId.Session) FirstId = request.Id;
        Check(request.Id == FirstId, "session retry preserves exact service request ID");
        NativeScriptServiceIdentity identity;
        identity.Id = request.Id;
        identity.Opcode = 0x03CB;
        identity.ArgumentCount = 3;
        identity.Arguments[0] = std::bit_cast<std::uint32_t>(request.Position.X);
        identity.Arguments[1] = std::bit_cast<std::uint32_t>(request.Position.Y);
        identity.Arguments[2] = std::bit_cast<std::uint32_t>(request.Position.Z);
        NativeScriptServiceTicket ticket;
        const auto admission = Transaction.Begin(identity, ticket);
        Check(admission == NativeScriptServiceTransactionStatus::Started ||
            admission == NativeScriptServiceTransactionStatus::Existing, "service attempt admitted");
        if (Transaction.State().Phase == NativeScriptServiceTransactionPhase::Cancelling) {
            return {NativeScriptServiceStatus::Pending, "cancellation acknowledgement pending"};
        }
        ++WorkerPolls;
        if (!WorkerPrepared) {
            Check(Transaction.MarkPending(ticket) == NativeScriptServiceTransactionStatus::Ok,
                "worker Pending recorded");
            return {NativeScriptServiceStatus::Pending, {}};
        }
        Check(Transaction.MarkPrepared(ticket) == NativeScriptServiceTransactionStatus::Ok,
            "worker result becomes internally Prepared");
        PreparedWasNotReady = Transaction.State().Commits == 0 && Effects == 0;
        ++Effects;
        Check(Transaction.Commit(ticket) == NativeScriptServiceTransactionStatus::Ok &&
            Transaction.Release(ticket) == NativeScriptServiceTransactionStatus::Ok,
            "external effect commits and releases transaction");
        return {NativeScriptServiceStatus::Ready, {}};
    }
    NativeScriptServiceResult CreatePlayer(const NativeScriptPlayerRequest&) override {
        return {NativeScriptServiceStatus::Unsupported, "not used"};
    }
    NativeScriptServiceTicket Cancel() {
        NativeScriptServiceTicket ticket;
        Check(Transaction.RequestCancel(Transaction.State().Identity, ticket) ==
            NativeScriptServiceTransactionStatus::Started, "session service cancel starts once");
        return ticket;
    }
};

void SessionBarrier() {
    Bytes code;
    Op(code, 0x03CB); Float(code, 1.0f); Float(code, 2.0f); Float(code, 3.0f);
    Op(code, 1); I8(code, 0);
    NativeScriptSession session;
    const auto bytes = Fixture(code);
    std::string error;
    Check(session.LoadMainBytes(bytes, bytes.size(), error), error.c_str());
    Services services;
    for (unsigned i = 0; i < 6; ++i) {
        Check(session.Step(services).Status == NativeScriptStatus::Advanced, "generated header advances");
    }
    const auto before = session.State();
    Check(session.Step(services).Status == NativeScriptStatus::Pending && session.State() == before &&
        services.WorkerPolls == 1 && services.Effects == 0, "first script poll is an effect-free barrier");
    const auto firstTicket = services.Cancel();
    Check(session.Step(services).Status == NativeScriptStatus::Pending && services.WorkerPolls == 1 &&
        session.State() == before, "cancelling script poll does not reach worker or advance VM");
    Check(services.Transaction.AcknowledgeCancel(firstTicket) == NativeScriptServiceTransactionStatus::Ok,
        "script worker acknowledges cancelled attempt");
    Check(session.Step(services).Status == NativeScriptStatus::Pending && services.WorkerPolls == 2 &&
        services.Transaction.State().Ticket.Attempt == firstTicket.Attempt + 1,
        "same script instruction retries Pending under a new attempt");
    services.WorkerPrepared = true;
    const auto committed = session.Step(services);
    Check(committed.Status == NativeScriptStatus::Advanced && committed.Executed == 1 &&
        services.PreparedWasNotReady && services.Effects == 1 &&
        services.Transaction.State().Commits == 1 && session.State().IP != before.IP,
        "script advances only after one actual retry effect commits");
}

std::vector<NativeScriptServiceTransactionPhase> Run() {
    using Phase = NativeScriptServiceTransactionPhase;
    using Status = NativeScriptServiceTransactionStatus;
    NativeScriptServiceTransaction transaction;
    NativeScriptServiceTransaction foreign;
    NativeScriptServiceTicket first, repeated, second;
    std::vector<Phase> trace;
    const auto identity = Identity();

    Check(transaction.State().Phase == Phase::Idle && transaction.State().Attempts == 0, "fresh owner is idle");
    Check(transaction.Begin(identity, first) == Status::Started && first.Owner && first.Attempt == 1,
        "first prepare allocates stable owner/attempt ticket");
    trace.push_back(transaction.State().Phase);
    Check(transaction.Begin(identity, repeated) == Status::Existing && repeated == first,
        "same request polls one attempt ticket");
    Check(transaction.MarkPending(first) == Status::Ok && transaction.MarkPending(first) == Status::Ok &&
        transaction.State().Polls == 2, "pending polls retain attempt and count");
    trace.push_back(transaction.State().Phase);

    auto changed = identity;
    changed.Arguments[0] = std::bit_cast<std::uint32_t>(3.0f);
    const auto beforeConflict = transaction.State();
    Check(transaction.Begin(changed, repeated) == Status::Conflict && transaction.State() == beforeConflict,
        "same request ID cannot change arguments");
    Check(transaction.Begin(Identity(3), repeated) == Status::Busy && transaction.State() == beforeConflict,
        "another request cannot cross active transaction");

    Check(transaction.RequestCancel(identity, repeated) == Status::Started && repeated == first &&
        transaction.State().CancelRequests == 1, "cancel emits one exact worker ticket");
    trace.push_back(transaction.State().Phase);
    Check(transaction.RequestCancel(identity, repeated) == Status::Existing &&
        transaction.State().CancelRequests == 1, "repeated cancel does not notify worker twice");
    Check(transaction.MarkPrepared(first) == Status::InvalidPhase,
        "cancelled attempt cannot expose late prepared result");
    Check(transaction.AcknowledgeCancel(first) == Status::Ok && transaction.State().Cancellations == 1,
        "worker cancellation acknowledgement closes first attempt");
    trace.push_back(transaction.State().Phase);

    Check(transaction.Begin(identity, second) == Status::Started && second.Owner == first.Owner && second.Attempt == 2,
        "same script request retries with new attempt generation");
    trace.push_back(transaction.State().Phase);
    Check(transaction.MarkPrepared(first) == Status::Stale && transaction.State().Phase == Phase::Preparing,
        "stale first-attempt result cannot prepare retry");
    Check(foreign.MarkPrepared(second) == Status::Stale, "ticket cannot cross transaction owner");
    Check(transaction.MarkPending(second) == Status::Ok && transaction.MarkPrepared(second) == Status::Ok,
        "retry advances pending to internally prepared");
    trace.push_back(transaction.State().Phase);
    Check(transaction.State().Commits == 0, "prepared is not a script-visible commit");
    Check(transaction.Commit(second) == Status::Ok && transaction.State().Commits == 1,
        "prepared retry commits exactly once");
    trace.push_back(transaction.State().Phase);
    Check(transaction.Commit(second) == Status::AlreadyCommitted && transaction.State().Commits == 1,
        "commit replay is idempotent");
    Check(transaction.Begin(identity, repeated) == Status::AlreadyCommitted && repeated == second,
        "committed request cannot restart before journal release");
    Check(transaction.Release(second) == Status::Ok && transaction.State().Phase == Phase::Idle &&
        transaction.State().Attempts == 2 && transaction.State().Commits == 1 &&
        transaction.State().CancelRequests == 1 && transaction.State().Cancellations == 1,
        "release preserves lifetime transaction totals");
    trace.push_back(transaction.State().Phase);

    auto invalid = Identity(4);
    invalid.ArgumentCount = 1;
    invalid.Arguments[1] = 1;
    const auto idle = transaction.State();
    Check(transaction.Begin(invalid, repeated) == Status::InvalidInput && transaction.State() == idle,
        "noncanonical trailing arguments reject atomically");
    Check(transaction.Begin(Identity(4), repeated) == Status::Started &&
        transaction.MarkPrepared(repeated) == Status::Ok && transaction.Fail(repeated) == Status::Ok &&
        transaction.State().Failures == 1, "prepared validation failure has explicit terminal transaction state");
    Check(transaction.Release(repeated) == Status::Ok && transaction.State().Phase == Phase::Idle,
        "failed transaction releases without a commit");
    return trace;
}
}

int main() {
    const auto first = Run();
    const auto second = Run();
    Check(first == second, "cancel/retry transaction trace is deterministic twice");
    SessionBarrier();
    std::printf("sa-core-service-transactions-ok checks=%d attempts=2 cancel=1 commit=1 "
        "phases=prepare,pending,cancelling,cancelled,retry,prepared,committed,idle "
        "session-stable-id=1 session-effect=once ready-before-commit=0 deterministic=twice\n",
        s_Checks);
    return 0;
}
