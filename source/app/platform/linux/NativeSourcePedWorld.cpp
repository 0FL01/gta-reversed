#include "NativeSourcePedWorld.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace {
using Status = NativeSourcePedWorldStatus;
bool Finite(const NativeCollisionVector& value) {
    return std::isfinite(value[0]) && std::isfinite(value[1]) && std::isfinite(value[2]);
}
bool ValidPed(const NativeSourcePedWorldPed& ped) {
    if (!ped.Identity || !ped.Physical.IsPed || !ped.Physical.DisableTurnForce || !Finite(ped.Physical.Position) ||
        !Finite(ped.Physical.MoveSpeed) || !Finite(ped.Physical.FrictionMoveSpeed) || !std::isfinite(ped.Physical.Mass) ||
        ped.Physical.Mass <= 0 || !std::isfinite(ped.Physical.Elasticity) || ped.Physical.Elasticity < 0 ||
        !std::isfinite(ped.Contact.HeightLimit) || !Finite(ped.Contact.ContactNormal) || !Finite(ped.Contact.ContactOffset) ||
        !std::isfinite(ped.AirResistance) || ped.AirResistance < 0 || ped.Physical.Static || ped.Physical.Attached ||
        ped.Physical.InfiniteMass || ped.Physical.DisableMoveForce || ped.Physical.DontApplySpeed ||
        !ped.Physical.UsesCollision || !ped.Physical.Collidable || ped.Contact.HeadStuckInCollision || ped.ControlPrepared ||
        ped.Contact.HeightLimit != 99999.9921875f) return false;
    for (const auto& basis : ped.Basis) if (!Finite(basis)) return false;
    return true;
}
Status Physical(NativeSourcePhysicalStatus status) {
    switch (status) {
    case NativeSourcePhysicalStatus::Ok: return Status::Ok;
    case NativeSourcePhysicalStatus::InvalidInput: return Status::InvalidInput;
    case NativeSourcePhysicalStatus::Unsupported: return Status::Unsupported;
    case NativeSourcePhysicalStatus::Overflow: return Status::Overflow;
    }
    return Status::Unsupported;
}
Status Model(NativeSourcePedModelStatus status) {
    switch (status) {
    case NativeSourcePedModelStatus::Ok: return Status::Ok;
    case NativeSourcePedModelStatus::InvalidInput: return Status::InvalidInput;
    case NativeSourcePedModelStatus::Unsupported: return Status::Unsupported;
    case NativeSourcePedModelStatus::Overflow: return Status::Overflow;
    }
    return Status::Unsupported;
}
Status Response(NativeSourcePedResponseStatus status) {
    switch (status) {
    case NativeSourcePedResponseStatus::Ok: return Status::Ok;
    case NativeSourcePedResponseStatus::InvalidInput: return Status::InvalidInput;
    case NativeSourcePedResponseStatus::Unsupported: return Status::Unsupported;
    case NativeSourcePedResponseStatus::Overflow: return Status::Overflow;
    case NativeSourcePedResponseStatus::SurfaceUnavailable: return Status::SurfaceUnavailable;
    }
    return Status::Unsupported;
}
NativeCollisionModel StandardPedModel() {
    NativeCollisionModel model;
    model.Name = "source-ped1"; model.Version = 2; model.Flags = 2;
    model.Min = {-0.35f, -0.35f, -1}; model.Max = {0.35f, 0.35f, 0.95f};
    model.BoundCenter = {}; model.BoundRadius = 1;
    for (const float z : {-0.2f, 0.2f, 0.6f}) model.Spheres.push_back({{0, 0, z}, 0.35f, {62, 0, 0, 0}});
    return model;
}
const auto s_StandardPedModel = std::make_shared<const NativeCollisionModel>(StandardPedModel());
}

bool NativeSourcePedWorld::Load(const NativeSourceGroundSnapshot& snapshot, std::string& error) {
    try {
        if (!snapshot.CompleteNormalSector || !snapshot.MembershipAndOverridesVerified || !snapshot.Deduplicated)
            throw std::runtime_error("incomplete ped collision sector authority");
        if (!std::isfinite(snapshot.MinXY[0]) || !std::isfinite(snapshot.MinXY[1]) || !std::isfinite(snapshot.MaxXY[0]) ||
            !std::isfinite(snapshot.MaxXY[1]) || snapshot.MinXY[0] >= snapshot.MaxXY[0] || snapshot.MinXY[1] >= snapshot.MaxXY[1])
            throw std::runtime_error("invalid ped collision sector extent");
        std::vector<StaticTarget> targets;
        targets.reserve(snapshot.Targets.size());
        std::uint64_t previous{};
        bool first = true;
        for (const auto& target : snapshot.Targets) {
            if (!target.VerifiedClassification || !target.SourceEffectiveTransformKnown || !target.CollisionModelKnown || !target.Model ||
                target.EffectiveClass != NativeSourceGroundClass::Building || target.InWorld != NativeSourceGroundKnown::Yes ||
                target.UsesCollision != NativeSourceGroundKnown::Yes || target.NormalSector != NativeSourceGroundKnown::Yes ||
                target.BigBuilding != NativeSourceGroundKnown::No || target.Ignored != NativeSourceGroundKnown::No ||
                !target.SourceListOrdinal || target.Model->Empty || !target.Model->Unsupported.empty())
                throw std::runtime_error("unqualified ped collision target");
            if (!first && *target.SourceListOrdinal <= previous) throw std::runtime_error("unordered ped collision targets");
            for (const auto& existing : targets) if (existing.Identity == target.Identity)
                throw std::runtime_error("duplicate ped collision target identity");
            for (const auto& basis : target.Transform.Basis) if (!Finite(basis)) throw std::runtime_error("invalid ped target transform");
            if (!Finite(target.Transform.Position)) throw std::runtime_error("invalid ped target transform");
            previous = *target.SourceListOrdinal; first = false;
            targets.push_back({target.Identity, target.Model, target.Transform, previous});
        }
        m_Statics = std::move(targets);
        m_Peds.clear();
        m_WorldGeneration = snapshot.WorldGeneration;
        m_MetadataRevision = snapshot.MetadataRevision;
        m_MinXY = snapshot.MinXY; m_MaxXY = snapshot.MaxXY;
        m_Loaded = true;
        error.clear();
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

NativeSourcePedWorldStatus NativeSourcePedWorld::AddPed(const NativeSourcePedWorldPed& ped) {
    if (!m_Loaded) return Status::NotLoaded;
    if (!ValidPed(ped)) return Status::InvalidInput;
    if (std::ranges::find(m_Peds, ped.Identity, &NativeSourcePedWorldPed::Identity) != m_Peds.end()) return Status::DuplicatePed;
    try { m_Peds.push_back(ped); } catch (...) { return Status::Overflow; }
    return Status::Ok;
}

NativeSourcePedWorldStatus NativeSourcePedWorld::RemovePed(std::uint64_t identity) {
    if (!m_Loaded) return Status::NotLoaded;
    const auto found = std::ranges::find(m_Peds, identity, &NativeSourcePedWorldPed::Identity);
    if (found == m_Peds.end()) return Status::PedNotFound;
    m_Peds.erase(found);
    return Status::Ok;
}

NativeSourcePedWorldStatus NativeSourcePedWorld::BeginControl(std::uint64_t identity, float timeStep) {
    if (!m_Loaded) return Status::NotLoaded;
    const auto found = std::ranges::find(m_Peds, identity, &NativeSourcePedWorldPed::Identity);
    if (found == m_Peds.end()) return Status::PedNotFound;
    if (!std::isfinite(timeStep) || timeStep < 0) return Status::InvalidInput;
    if (found->ControlPrepared) return Status::ControlOutstanding;
    auto candidate = *found;
    candidate.Physical.SafePosition = false;
    candidate.Physical.HasHitWall = false;
    candidate.Contact.HasContacted = false;
    if (const auto status = NativeSourceConsumeFrictionMoveSpeed(candidate.Physical); status != NativeSourcePhysicalStatus::Ok) return Physical(status);
    if (const auto status = NativeSourceApplyGravity(candidate.Physical, timeStep); status != NativeSourcePhysicalStatus::Ok) return Physical(status);
    if (const auto status = NativeSourceApplyAirResistance(candidate.Physical, candidate.AirResistance, timeStep);
        status != NativeSourcePhysicalStatus::Ok) return Physical(status);
    candidate.ControlPrepared = true;
    *found = candidate;
    return Status::Ok;
}

NativeSourcePedWorldStatus NativeSourcePedWorld::StepCollision(std::uint64_t identity, float timeStep,
    const NativeSourceSurfaces& surfaces, NativeSourcePedWorldStep& out) {
    if (!m_Loaded) return Status::NotLoaded;
    if (!std::isfinite(timeStep) || timeStep < 0) return Status::InvalidInput;
    const auto original = std::ranges::find(m_Peds, identity, &NativeSourcePedWorldPed::Identity);
    if (original == m_Peds.end()) return Status::PedNotFound;
    if (!original->ControlPrepared) return Status::ControlRequired;
    std::vector<NativeSourcePedWorldPed> peds;
    try { peds = m_Peds; } catch (...) { return Status::Overflow; }
    const auto moving = std::ranges::find(peds, identity, &NativeSourcePedWorldPed::Identity);
    auto& ped = *moving;
    const auto start = ped.Physical.Position;
    const auto originalPosition = start;
    const float oldElasticity = ped.Physical.Elasticity;
    NativeSourcePedWorldStep result;
    result.Start = start;
    if (const auto status = NativeSourceCalculatePedCollisionSteps(ped.Physical, timeStep,
            ped.HasPlayerData, ped.StandingOnEntity, result.Plan); status != NativeSourcePhysicalStatus::Ok) return Physical(status);
    if (result.Plan.PreCheckAtFullSpeed || result.Plan.PreCheckAtHalfSpeed) return Status::Unsupported;
    const float step = result.Plan.Count ? timeStep / float(result.Plan.Count) : std::numeric_limits<float>::infinity();

    const auto inside = [&](const NativeCollisionVector& position) {
        constexpr float radius = 1.0f;
        return position[0] - radius >= m_MinXY[0] && position[1] - radius >= m_MinXY[1] &&
            position[0] + radius < m_MaxXY[0] && position[1] + radius < m_MaxXY[1];
    };
    const auto query = [&](float queryStep, bool& blocked) -> Status {
        ++result.CollisionChecks;
        if (!inside(ped.Physical.Position)) return Status::OutsideCoverage;
        ped.StandingOnEntity = false;
        if (const auto status = NativeSourceBeginPedCollisionCheck(ped.Physical, ped.Contact); status != NativeSourcePedResponseStatus::Ok)
            return Response(status);
        for (const auto& target : m_Statics) {
            NativeSourcePedCollisionInput input;
            input.Other = NativeSourcePedCollisionEntity::Building;
            input.TimeStep = queryStep; input.UsesCollision = input.ModelIsStandardPed1 = true;
            input.WasStanding = ped.Contact.WasStanding;
            NativeSourcePedCollisionShape shape;
            const auto preparation = NativeSourcePreparePedCollision(input, shape);
            if (preparation != NativeSourcePedCollisionStatus::Ok) return preparation == NativeSourcePedCollisionStatus::Overflow ? Status::Overflow : Status::Unsupported;
            NativeSourcePedModelContacts contacts;
            NativeSourceGroundTransform pedTransform{ped.Physical.Position, ped.Basis};
            const auto model = NativeSourceProcessPedModel(shape, pedTransform, *target.Model, target.Transform, contacts);
            if (model != NativeSourcePedModelStatus::Ok) return Model(model);
            ++result.StaticQueries;
            NativeSourcePedEntityResponse response;
            const auto resolved = NativeSourceResolvePedBuilding(ped.Physical, ped.Contact, shape, contacts,
                target.Transform, surfaces, queryStep, response);
            if (resolved != NativeSourcePedResponseStatus::Ok) return Response(resolved);
            result.Supports += response.SupportAccepted;
            result.Contacts += response.ContactCount; result.Applied += response.AppliedCount;
            result.Friction += response.FrictionCount; result.Reports += response.ReportCount;
            if (response.BlockingCollision) { blocked = true; return Status::Ok; }
        }
        for (auto& other : peds) {
            if (other.Identity == identity) continue;
            NativeSourcePedCollisionInput input;
            input.Other = NativeSourcePedCollisionEntity::Ped;
            input.TimeStep = queryStep; input.UsesCollision = input.ModelIsStandardPed1 = true;
            NativeSourcePedCollisionShape shape;
            const auto preparation = NativeSourcePreparePedCollision(input, shape);
            if (preparation != NativeSourcePedCollisionStatus::Ok) return Status::Unsupported;
            NativeSourceGroundTransform otherTransform{other.Physical.Position, other.Basis};
            NativeSourceGroundTransform pedTransform{ped.Physical.Position, ped.Basis};
            NativeSourcePedModelContacts contacts;
            const auto model = NativeSourceProcessPedModel(shape, pedTransform, *s_StandardPedModel, otherTransform, contacts);
            if (model != NativeSourcePedModelStatus::Ok) return Model(model);
            ++result.DynamicQueries;
            NativeSourcePedEntityResponse response;
            const auto resolved = NativeSourceResolvePedPair(ped.Physical, ped.Contact, other.Physical,
                shape, contacts, surfaces, queryStep, response);
            if (resolved != NativeSourcePedResponseStatus::Ok) return Response(resolved);
            result.Contacts += response.ContactCount; result.Applied += response.AppliedCount;
            result.Friction += response.FrictionCount; result.Reports += response.ReportCount;
            if (response.BlockingCollision) { blocked = true; return Status::Ok; }
        }
        return Status::Ok;
    };

    NativeCollisionVector oldPosition = originalPosition;
    for (std::uint16_t index = 1; index < result.Plan.Count; ++index) {
        ped.Physical.Position = oldPosition;
        if (const auto status = NativeSourceApplyMoveSpeed(ped.Physical, float(index) * step); status != NativeSourcePhysicalStatus::Ok)
            return Physical(status);
        bool blocked = false;
        if (const auto status = query(float(index) * step, blocked); status != Status::Ok) return status;
        if (ped.Physical.MoveSpeed[2] == 0 && !ped.Contact.WasStanding && ped.Contact.IsStanding)
            oldPosition[2] = ped.Physical.Position[2];
        ped.Physical.Position = oldPosition;
        if (blocked) {
            ped.Physical.Elasticity = oldElasticity;
            ped.ControlPrepared = false;
            result.Blocked = true; result.End = oldPosition;
            m_Peds = std::move(peds); out = result;
            return Status::Ok;
        }
    }
    ped.Physical.Position = oldPosition;
    if (const auto status = NativeSourceApplyMoveSpeed(ped.Physical, timeStep); status != NativeSourcePhysicalStatus::Ok)
        return Physical(status);
    bool blocked = false;
    if (const auto status = query(timeStep, blocked); status != Status::Ok) return status;
    if (blocked) ped.Physical.Position = oldPosition;
    else {
        ped.Physical.SafePosition = true;
        result.SafePosition = true;
    }
    ped.Physical.Elasticity = oldElasticity;
    ped.ControlPrepared = false;
    result.Blocked = blocked;
    result.End = ped.Physical.Position;
    const float dx = result.End[0] - result.Start[0], dy = result.End[1] - result.Start[1], dz = result.End[2] - result.Start[2];
    const float squared = dx * dx + dy * dy + dz * dz;
    if (!std::isfinite(squared)) return Status::Overflow;
    result.MovingDistance = float(std::sqrt(double(squared)));
    if (!std::isfinite(result.MovingDistance)) return Status::Overflow;
    m_Peds = std::move(peds); out = result;
    return Status::Ok;
}

NativeSourcePedWorldStatus NativeSourcePedWorld::Ped(std::uint64_t identity, NativeSourcePedWorldPed& out) const {
    if (!m_Loaded) return Status::NotLoaded;
    const auto found = std::ranges::find(m_Peds, identity, &NativeSourcePedWorldPed::Identity);
    if (found == m_Peds.end()) return Status::PedNotFound;
    out = *found;
    return Status::Ok;
}
