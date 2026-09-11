// Bounded CPU-only handoff to realtime_streaming::Worker's sole parser thread.
#pragma once

#include "app/platform/linux/NativeGeneratedVehicleAssets.h"

#include <cassert>
#include <limits>
#include <tuple>

namespace realtime_streaming { class Worker; }

// Parent constructs this BEFORE starting Worker, from its startup path and
// immutable catalog (an aliasing shared_ptr into NativeCollisionContext works).
// Pass the SAME gameDir used for pager startup: CarPose repeats that file-path
// offset on the worker. Main never changes it. Lifetime extends through Stop.
class NativeVehicleAssetSource {
public:
    NativeVehicleAssetSource(std::string gameDir, std::shared_ptr<const NativeCollisionAssets> catalog)
        : GameDir(std::move(gameDir)), Catalog(std::move(catalog)) {}
    virtual ~NativeVehicleAssetSource() = default;
    virtual NativeGeneratedVehicleAssetResult Load(const NativeCarGeneratorModelDefinition& definition,
        NativeGeneratedVehicleAsset& out) const;
    const std::string GameDir;
    const std::shared_ptr<const NativeCollisionAssets> Catalog;
};

struct NativeVehicleAssetRequest {
    // Owner identifies the parent session; Request is its stable request ID;
    // Revision identifies its model-demand epoch, NOT the world's generation.
    std::uint64_t Owner{}, Request{}, Revision{}, SourceFrame{};
    NativeCarGeneratorModelDefinition Definition;
};

struct NativeVehicleAssetTicket {
    // Sequence never resets during this Worker's lifetime. Identity is a copied,
    // immutable request, never a pointer borrowed from the mutable registry.
    std::uint64_t Sequence{};
    std::shared_ptr<const NativeVehicleAssetRequest> Identity;
    bool operator==(const NativeVehicleAssetTicket&) const = default;
};

enum class NativeVehicleAssetAdmission { Accepted, Existing, Busy, Conflict, Invalid, Disabled, Stopped };
// Ready means a completion can be taken, including Error/Unsupported. Only
// completion.Result.Status == Ready supplies model CPU data; never GPU readiness.
enum class NativeVehicleAssetPhase { Missing, Waiting, Running, Cancelling, Cancelled, Ready, Stopped };
struct NativeVehicleAssetSubmission {
    NativeVehicleAssetAdmission Status{};
    NativeVehicleAssetTicket Ticket;
};
struct NativeVehicleAssetCompletion {
    NativeVehicleAssetTicket Ticket;
    // Diagnostic parser-world generation at job start; never writes/resets it.
    std::uint64_t WorkerGeneration{};
    NativeGeneratedVehicleAssetResult Result;
    NativeGeneratedVehicleAsset Asset; // CPU readiness only; no vehicle or GL owner
};

// State transitions are short and serialized by Worker's existing mutex.
// Exactly one slot: waiting OR processing OR ready. Cancellation of processing
// retains the slot until the uninterruptible parser returns. No history/cache
// or unbounded queue; after Take, consumers own their immutable CPU packet.
// Inline mailbox logic keeps existing default Worker probes link-compatible.
class NativeVehicleAssetQueue {
    friend class realtime_streaming::Worker;
    static auto DefinitionIdentity(const NativeCarGeneratorModelDefinition& d) {
        return std::tie(d.ModelId, d.ModelName, d.TextureName, d.Type, d.TypeName, d.HandlingName,
            d.GameName, d.AnimationGroup, d.ClassName, d.Frequency, d.Flags, d.ComponentRules,
            d.Misc, d.WheelSizeFront, d.WheelSizeRear, d.WheelUpgradeClass, d.Source, d.Line);
    }
    NativeVehicleAssetSubmission Submit(const NativeVehicleAssetRequest& request, bool enabled) {
        if (m_Phase == NativeVehicleAssetPhase::Stopped) return {NativeVehicleAssetAdmission::Stopped, {}};
        if (!enabled) return {NativeVehicleAssetAdmission::Disabled, {}};
        if (!request.Owner || !request.Request)
            return {NativeVehicleAssetAdmission::Invalid, {}};
        if (m_Ticket.Identity && m_Ticket.Identity->Owner == request.Owner &&
            m_Ticket.Identity->Request == request.Request && m_Ticket.Identity->Revision == request.Revision) {
            const auto& old = *m_Ticket.Identity;
            if (old.SourceFrame != request.SourceFrame || DefinitionIdentity(old.Definition) != DefinitionIdentity(request.Definition))
                return {NativeVehicleAssetAdmission::Conflict, m_Ticket};
            if (m_Phase != NativeVehicleAssetPhase::Cancelled) return {NativeVehicleAssetAdmission::Existing, m_Ticket};
        }
        if (m_Phase != NativeVehicleAssetPhase::Missing && m_Phase != NativeVehicleAssetPhase::Cancelled)
            return {NativeVehicleAssetAdmission::Busy, {}};
        if (m_Sequence == std::numeric_limits<std::uint64_t>::max())
            return {NativeVehicleAssetAdmission::Invalid, {}};
        m_Ticket = {++m_Sequence, std::make_shared<const NativeVehicleAssetRequest>(request)};
        m_Phase = NativeVehicleAssetPhase::Waiting;
        return {NativeVehicleAssetAdmission::Accepted, m_Ticket};
    }
    NativeVehicleAssetPhase Phase(const NativeVehicleAssetTicket& ticket) const {
        if (m_Phase == NativeVehicleAssetPhase::Stopped) return m_Phase;
        return ticket.Identity && ticket == m_Ticket ? m_Phase : NativeVehicleAssetPhase::Missing;
    }
    bool Cancel(const NativeVehicleAssetTicket& ticket) {
        switch (Phase(ticket)) {
        case NativeVehicleAssetPhase::Waiting: m_Phase = NativeVehicleAssetPhase::Cancelled; return true;
        case NativeVehicleAssetPhase::Running:
        case NativeVehicleAssetPhase::Ready: m_Phase = NativeVehicleAssetPhase::Cancelling; return true;
        case NativeVehicleAssetPhase::Cancelling:
        case NativeVehicleAssetPhase::Cancelled: return true;
        default: return false;
        }
    }
    std::shared_ptr<const NativeVehicleAssetCompletion> Take(const NativeVehicleAssetTicket& ticket) {
        if (Phase(ticket) != NativeVehicleAssetPhase::Ready) return {};
        m_Phase = NativeVehicleAssetPhase::Missing;
        m_Ticket = {};
        return std::move(m_Result);
    }
    bool Waiting() const { return m_Phase == NativeVehicleAssetPhase::Waiting; }
    bool Retiring() const { return m_Phase == NativeVehicleAssetPhase::Cancelling && bool(m_Result); }
    std::shared_ptr<const NativeVehicleAssetCompletion> Retire() {
        assert(Retiring());
        m_Phase = NativeVehicleAssetPhase::Cancelled;
        return std::move(m_Result);
    }
    NativeVehicleAssetTicket Begin() {
        assert(Waiting());
        m_Phase = NativeVehicleAssetPhase::Running;
        return m_Ticket;
    }
    void Finish(std::shared_ptr<const NativeVehicleAssetCompletion>& result) {
        assert(result->Ticket == m_Ticket);
        if (m_Phase == NativeVehicleAssetPhase::Cancelling) {
            m_Phase = NativeVehicleAssetPhase::Cancelled;
        } else {
            assert(m_Phase == NativeVehicleAssetPhase::Running);
            m_Result = std::move(result);
            m_Phase = NativeVehicleAssetPhase::Ready;
        }
        // Cancelled result stays in worker local, destroyed after mutex unlock.
    }
    std::shared_ptr<const NativeVehicleAssetCompletion> Stop() {
        m_Phase = NativeVehicleAssetPhase::Stopped;
        m_Ticket = {};
        return std::move(m_Result);
    }
    std::uint64_t m_Sequence{};
    NativeVehicleAssetTicket m_Ticket;
    NativeVehicleAssetPhase m_Phase = NativeVehicleAssetPhase::Missing;
    std::shared_ptr<const NativeVehicleAssetCompletion> m_Result;
};
