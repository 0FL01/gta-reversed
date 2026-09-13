#include "app/platform/linux/RealtimeScriptHost.h"
#include <algorithm>
#include <cassert>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <limits>
#include <utility>
#include <type_traits>

using int32 = std::int32_t;
using uint32 = std::uint32_t;
using int64 = std::int64_t;
using uint64 = std::uint64_t;
#ifndef __stdcall
#define __stdcall
#endif
#include "oswrapper/oswrapper.h"
#include "app/platform/linux/TexSample.h"

namespace {
static_assert(std::is_nothrow_copy_assignable_v<RealtimeScriptHostEvent>);
static_assert(std::is_nothrow_move_constructible_v<RealtimeScriptHostEvent>);
static_assert(std::is_nothrow_move_assignable_v<RealtimeScriptWorldPublication>);
NativeScriptServiceResult Ready() { return {NativeScriptServiceStatus::Ready, {}}; }
NativeScriptServiceResult Pending(std::string message = {}) { return {NativeScriptServiceStatus::Pending, std::move(message)}; }
NativeScriptServiceResult Error(std::string message) { return {NativeScriptServiceStatus::Error, std::move(message)}; }
NativeScriptServiceResult Unsupported(std::string message) { return {NativeScriptServiceStatus::Unsupported, std::move(message)}; }
bool Finite(NativeScriptPosition p) { return std::isfinite(p.X) && std::isfinite(p.Y) && std::isfinite(p.Z); }
NativeScriptServiceIdentity WorldIdentity(const NativeScriptSceneRequest& request, bool requireGround) {
    NativeScriptServiceIdentity identity;
    identity.Id = request.Id;
    identity.Opcode = requireGround ? 0x03CB : 0x04E4;
    identity.ArgumentCount = requireGround ? 3 : 2;
    identity.Arguments[0] = std::bit_cast<std::uint32_t>(request.Position.X);
    identity.Arguments[1] = std::bit_cast<std::uint32_t>(request.Position.Y);
    if (requireGround) identity.Arguments[2] = std::bit_cast<std::uint32_t>(request.Position.Z);
    return identity;
}
}

RealtimeScriptHost::RealtimeScriptHost(RealtimeGameplay& gameplay): m_Gameplay(gameplay) {}
RealtimeScriptHost::~RealtimeScriptHost() {
    if (!m_PendingLoad || !m_Cancel) return;
    NativeScriptServiceTicket ticket;
    if (m_WorldTransaction.RequestCancel(m_WorldTransaction.State().Identity, ticket) !=
        NativeScriptServiceTransactionStatus::Started) return;
    try { m_Cancel(ticket); } catch (...) {}
}
bool RealtimeScriptHost::SeedSourceRngAfterRwInit(std::string& error) {
    const auto readiness = m_SourceRng.Readiness();
    if (readiness == NativeSourceRngStatus::Ready) { error.clear(); return true; }
    if (m_Sealed || readiness != NativeSourceRngStatus::Unseeded) {
        error = "source RNG seed requires unsealed main-thread startup"; return false;
    }
    if (!TexSample_IsEngineStarted()) {
        error = "source RNG seed must follow actual native RW initialization"; return false;
    }
    // GameInit 0x5BF3B0: RwInitialize, srand(RsTimer()). Native OS_TimeMS is
    // the platform timer authority; neither the VM clock nor a fixture seed.
    const auto seed = OS_TimeMS();
    if (m_SourceRng.SeedOnce(seed) != NativeSourceRngStatus::Ready) {
        error = "source RNG initial seed rejected"; return false;
    }
    error.clear(); return true;
}
bool RealtimeScriptHost::InitializeBeforeWorker(const char* gameDir, std::string& error,
    std::shared_ptr<const NativeCollisionContext> collision) {
    if (!gameDir || !*gameDir) { error = "script host needs game directory"; return false; }
    if (m_Initialized || m_Sealed) { error = "script host initialization must precede worker startup, once"; return false; }
    if (!SeedSourceRngAfterRwInit(error)) return false;
    if (!collision) collision = NativeCollisionContext::LoadBeforeWorker(gameDir, 900.0f, error);
    if (!collision) return false;
    m_CollisionContext = std::move(collision);
    std::error_code pathError;
    const auto absoluteGameDir = std::filesystem::absolute(gameDir, pathError).string();
    if (pathError) { error = "script host game path: " + pathError.message(); return false; }
    if (!m_Session.LoadMain(absoluteGameDir.c_str(), error)) return false;
    if (!m_Gameplay.Initialize(gameDir, error, RealtimeGameplayModel::BasePlayer)) return false;
    if (!m_Entities.LoadBeforeWorker(gameDir, error)) return false;
    if (!m_EntryExits.LoadBeforeWorker(gameDir, error)) return false;
    if (!m_Restarts.LoadBeforeWorker(gameDir, error)) return false;
    if (!m_Garages.LoadBeforeWorker(gameDir, *m_CollisionContext, error)) return false;
    if (!m_CarGenerators.LoadBeforeWorker(gameDir, State().TimeMs, error) ||
        !m_CarGeneratorResidency.Initialize(m_CarGenerators, m_CollisionContext->Population, error)) return false;
    std::vector<NativePlacementOverride> placements;
    for (const auto& door : m_Garages.Doors()) {
        if (!door.RequiresDynamicPublication) continue;
        const auto* garage = door.Garage ? m_Garages.Resolve(*door.Garage) : nullptr;
        // Early SCM world queries observe InitDoorsAtStart, before the first
        // common garage update. Door metadata may already describe that update.
        placements.push_back({NativePlacementIdentity::From(door.Placement), door.SourcePose.Position,
                              door.SourcePose.Basis, garage ? bool(garage->Flags & 0x40) : door.CollisionEnabled});
    }
    m_InitialPlacementOverrides = std::make_shared<const NativePlacementOverrides>(std::move(placements));
    // These are native producer authorities. Generator definitions and retained
    // runtime demands do not imply that construction/world insertion is ported.
    if (!m_Vehicles.BindProducer(NativeVehicleProducer::NativeScm, error) ||
        !m_Vehicles.BindProducer(NativeVehicleProducer::NativeGameplayController, error) ||
        !m_Vehicles.BindProducer(NativeVehicleProducer::CarGenerator, error) ||
        !m_Vehicles.SealProducerExtent(error)) return false;
    m_Initialized = true;
    error.clear(); return true;
}
bool RealtimeScriptHost::PrepareInitialGarageWorldBeforeWorker(std::string& error) try {
    const auto root = m_Gameplay.State().PedRoot;
    if (!m_Initialized || m_Sealed || m_Loader || m_PendingLoad || !ResolvePed(PedRef()) ||
        !Finite({root.X, root.Y, root.Z}) || m_Gameplay.State().Ticks != 0 || !m_LoadedScene || !m_Publication.Scene ||
        m_Publication.Center != *m_LoadedScene || !m_Publication.SourceCollision ||
        m_Publication.Overrides != m_InitialPlacementOverrides ||
        m_Publication.SourceCollision->Overrides != m_InitialPlacementOverrides || m_Garages.m_LastFrame) {
        error = "initial garage world preparation requires the startup player/world before sealing or garage Tick";
        return false;
    }
    auto entries = std::vector<NativePlacementOverride>(m_InitialPlacementOverrides->Entries().begin(), m_InitialPlacementOverrides->Entries().end());
    bool changed = false;
    for (const auto& door : m_Garages.Doors()) {
        if (!door.Garage) continue;
        const auto* garage = m_Garages.Resolve(*door.Garage);
        if (!garage) { error = "initial garage world has an unresolved door owner"; return false; }
        const auto pose = NativeGarages::DoorPose(*garage, door);
        const auto* published = m_InitialPlacementOverrides->Find(door.Placement);
        const auto rendered = published ? NativeGarageMatrix{published->Position, published->Basis} : door.Authored;
        if (pose != rendered) {
            error = "initial garage world preparation requires unchanged rendered door poses";
            return false;
        }
        const bool collision = NativeGarages::UpdateCollisionFlags(*garage) & 0x40;
        if (collision == (published ? published->CollisionEnabled : true)) continue;
        changed = true;
        const auto identity = NativePlacementIdentity::From(door.Placement);
        const auto it = std::ranges::find_if(entries, [&](const auto& entry) { return entry.Identity == identity; });
        if (it != entries.end()) it->CollisionEnabled = collision;
        else entries.push_back({identity, pose.Position, pose.Basis, collision});
    }
    if (!changed) { m_InitialGarageWorldPrepared = true; error.clear(); return true; }
    if (m_InitialGarageWorldPrepared) { error = "initial garage world was already prepared with different collision state"; return false; }
    auto overrides = std::make_shared<const NativePlacementOverrides>(std::move(entries));
    auto snapshot = std::make_shared<NativeCollisionSnapshot>();
    auto world = std::make_shared<RealtimeGameplayWorld>();
    const auto p = m_Publication.Center;
    if (!m_CollisionContext->Snapshot(p.X, p.Y, *snapshot, error, overrides) || !world->Rebuild(*snapshot, error)) return false;
    if (!world->TriangleCount() && !world->SphereCount() && !world->BoxCount()) {
        error = "prepared garage world has no source COL primitives"; return false;
    }
    // All allocating/fallible work precedes the single main-thread handoff.
    // Scene reuse is valid only because every effective pose was checked above.
    auto publication = m_Publication;
    publication.Overrides = overrides;
    publication.SourceCollision = std::move(snapshot);
    publication.Collision = world;
    const auto residency = ReconcileCarGeneratorsBeforeWorldCommit(m_WorldRevision + 1, publication.SourceCollision);
    if (residency.Status != NativeCarGeneratorResidencyStatus::Ready) { error = residency.Detail; return false; }
    m_Publication = std::move(publication);
    m_World = std::move(world);
    m_InitialPlacementOverrides = std::move(overrides);
    m_InitialGarageWorldPrepared = true;
    ++m_WorldRevision;
    error.clear(); return true;
} catch (const std::exception& e) { error = e.what(); return false; }
void RealtimeScriptHost::SetLiveWorldLoader(WorldLoader loader, CancelLoad cancel) {
    assert(!m_PendingLoad);
    assert(m_WorldTransaction.State().Phase == NativeScriptServiceTransactionPhase::Idle);
    assert(!loader || cancel); // every asynchronous owner has cancellation
    m_Loader = std::move(loader); m_Cancel = std::move(cancel);
}
bool RealtimeScriptHost::CancelPendingWorld(const NativeScriptRequestId& id, std::string& error) {
    if (m_InWorldService) {
        error = "world cancellation cannot reenter a service callback";
        return false;
    }
    if (!m_PendingLoad || *m_PendingLoad != id || !m_Cancel) {
        error = "no matching asynchronous world request to cancel";
        return false;
    }
    NativeScriptServiceTicket ticket;
    const auto status = m_WorldTransaction.RequestCancel(m_WorldTransaction.State().Identity, ticket);
    if (status == NativeScriptServiceTransactionStatus::Existing) { error.clear(); return true; }
    if (status != NativeScriptServiceTransactionStatus::Started) {
        error = "world transaction rejected cancellation";
        return false;
    }
    m_PendingWorldPublication.reset();
    try {
        m_Cancel(ticket);
    } catch (const std::exception& exception) {
        error = "world cancellation exception: " + std::string(exception.what());
        return false;
    } catch (...) {
        error = "unknown world cancellation exception";
        return false;
    }
    error.clear();
    return true;
}
bool RealtimeScriptHost::AcknowledgeWorldCancellation(
    const NativeScriptServiceTicket& ticket, std::string& error) {
    if (m_InWorldService || !m_PendingLoad || m_WorldTransaction.AcknowledgeCancel(ticket) !=
        NativeScriptServiceTransactionStatus::Ok) {
        error = "stale or invalid world cancellation acknowledgement";
        return false;
    }
    error.clear();
    return true;
}
void RealtimeScriptHost::SetRadarSpriteReady(RadarSpriteReady ready) { m_RadarSpriteReady = std::move(ready); }
NativeScriptServiceResult RealtimeScriptHost::PrepareContactBlipRequest(NativeScriptContactBlipRequest& request) const {
    request.RadarSpriteReady = false;
    // Property sprites are prepared and owned by NativeScriptEntities itself.
    if (request.Sprite == 31 || request.Sprite == 32) return Ready();
    if (!m_RadarSpriteReady) return Unsupported("contact sprite has no registered radar consumer");
    try {
        if (!m_RadarSpriteReady(request.Sprite)) return Unsupported("contact sprite is not ready in the registered radar consumer");
    } catch (const std::exception& exception) {
        return Error("radar sprite readiness exception: " + std::string(exception.what()));
    } catch (...) {
        return Error("unknown radar sprite readiness exception");
    }
    request.RadarSpriteReady = true;
    return Ready();
}
void RealtimeScriptHost::SealStartup() {
    m_Sealed = true; m_EntryExits.SealStartup(); m_Garages.SealStartup(); m_CarGenerators.SealStartup();
    m_Restarts.SealStartup();
}
NativeScriptResult RealtimeScriptHost::RunPass(std::size_t quota) {
    if (!m_Initialized) return {NativeScriptStatus::Error, 0, 0, 0, "script host not initialized"};
    return m_Session.RunPass(*this, quota);
}
bool RealtimeScriptHost::AdvanceTime(std::uint32_t nowMs, std::string& error) { return m_Session.AdvanceTime(nowMs, error); }

NativeRestartSelection RealtimeScriptHost::QueryRestart(NativeRestartQuery query) const {
    if (!m_Initialized) return {NativeRestartStatus::Error,{},"restart query requires initialized host"};
    query.CityUnlocked = float(State().IntStats[181-120]); // STAT_CITY_UNLOCKED
    return m_Restarts.Query(query);
}
NativeScriptServiceResult RealtimeScriptHost::AddRestart(const NativeScriptRestartRequest& request) {
    if (!m_Initialized) return Error("restart registration requires initialized host");
    if (request.Kind != NativeRestartKind::Hospital && request.Kind != NativeRestartKind::Police) return Error("invalid restart kind");
    if (m_PendingLoad && *m_PendingLoad == request.Id) return Error("restart ID already owns pending world request");
    RealtimeScriptHostEvent event{request.Id, std::uint16_t(request.Kind == NativeRestartKind::Hospital ? 0x016C : 0x016D),
        {request.Position.X,request.Position.Y,request.Position.Z,request.HeadingDegrees},request.WhenToUse};
    if (auto old = Replay(event)) return *old;
    auto result = m_Restarts.Add(request);
    if (result.Status == NativeScriptServiceStatus::Ready) Commit(event);
    return result;
}
NativeScriptServiceResult RealtimeScriptHost::ChangeGarageType(const NativeScriptGarageTypeRequest& request) {
    if (!m_Initialized) return Error("garage type change requires initialized host");
    RealtimeScriptHostEvent event{.Id=request.Id, .Opcode=0x02FA, .StateArgument=request.Type, .Name=request.Name};
    if (auto old = Replay(event)) return *old;
    const auto result = m_Garages.ChangeType(request.Name, request.Type);
    if (result.Status == NativeScriptServiceStatus::Ready) Commit(event);
    return result;
}

NativeScriptServiceResult RealtimeScriptHost::AddStuntJump(const NativeScriptStuntJumpRequest& request) {
    if (!m_Initialized) return Error("stunt-jump registration requires initialized host");
    if (m_PendingLoad) return Error("stunt-jump service cannot cross pending world request");
    RealtimeScriptHostEvent event{.Id=request.Id, .Opcode=0x0814,
        .Arguments={request.StartCenter.X, request.StartCenter.Y, request.StartCenter.Z,
            request.StartHalfSize.X, request.StartHalfSize.Y, request.StartHalfSize.Z,
            request.EndCenter.X, request.EndCenter.Y, request.EndCenter.Z,
            request.EndHalfSize.X, request.EndHalfSize.Y, request.EndHalfSize.Z,
            request.Camera.X, request.Camera.Y, request.Camera.Z}, .Index=request.Reward};
    if (auto old = Replay(event)) return *old;
    std::size_t index = 0;
    std::string error;
    const auto status = m_StuntJumps.Add(
        {request.StartCenter.X, request.StartCenter.Y, request.StartCenter.Z},
        {request.StartHalfSize.X, request.StartHalfSize.Y, request.StartHalfSize.Z},
        {request.EndCenter.X, request.EndCenter.Y, request.EndCenter.Z},
        {request.EndHalfSize.X, request.EndHalfSize.Y, request.EndHalfSize.Z},
        {request.Camera.X, request.Camera.Y, request.Camera.Z}, request.Reward, index, error);
    if (status != NativeStuntJumpStatus::Ok) return Error(error);
    Commit(event);
    return Ready();
}
NativeScriptReferenceResult<NativeScriptObjectRef> RealtimeScriptHost::CreateObjectInternal(
    const NativeScriptObjectRequest& request, bool noOffset) {
    NativeScriptReferenceResult<NativeScriptObjectRef> result;
    if (!m_Initialized) {
        result.Result = Error("object creation requires initialized host");
        return result;
    }
    if (m_PendingLoad) {
        result.Result = Error("object creation cannot cross pending world request");
        return result;
    }
    std::array<char, 24> name = request.UsedObjectName;
    if (request.ModelId >= 0) {
        const auto model = m_CollisionContext->Population.Models.find(request.ModelId);
        if (model == m_CollisionContext->Population.Models.end()) {
            result.Result = Unsupported("object model is absent from the source IDE");
            return result;
        }
        name.fill('\0');
        const auto copy = std::min(model->second.Name.size(), name.size() - 1);
        std::copy_n(model->second.Name.data(), copy, name.data());
    }
    const auto length = std::find(name.begin(), name.end(), '\0');
    if (length == name.begin()) {
        result.Result = Error("object model name is missing");
        return result;
    }
    const std::string modelName(name.data(), std::size_t(length - name.begin()));
    const auto lookup = m_CollisionContext->Assets.LookupModel(modelName);
    if (lookup.Status == NativeCollisionModelStatus::Unsupported) {
        result.Result = Unsupported("object model source identity is unsupported");
        return result;
    }
    int32 resolvedModel = request.ModelId;
    if (resolvedModel < 0) {
        resolvedModel = -1;
        for (const auto& [modelId, ide] : m_CollisionContext->Population.Models) {
            if (ide.Name.size() != modelName.size()) continue;
            bool equal = true;
            for (std::size_t i = 0; i < modelName.size(); ++i) {
                if (std::tolower(static_cast<unsigned char>(ide.Name[i])) !=
                    std::tolower(static_cast<unsigned char>(modelName[i]))) { equal = false; break; }
            }
            if (equal) { resolvedModel = modelId; break; }
        }
        if (resolvedModel < 0) {
            result.Result = Unsupported("object model has no source IDE identity");
            return result;
        }
    }
    RealtimeScriptHostEvent event{.Id=request.Id, .Opcode=std::uint16_t(noOffset ? 0x029B : 0x0107),
        .Arguments={request.Position.X, request.Position.Y, request.Position.Z}, .Index=request.ModelId};
    event.ModelName = name;
    if (auto old = Replay(event)) {
        if (old->Status != NativeScriptServiceStatus::Ready) { result.Result = *old; return result; }
        const auto previous = std::ranges::find(m_Events, request.Id, &RealtimeScriptHostEvent::Id);
        if (previous == m_Events.end()) { result.Result = Error("object replay event is missing"); return result; }
        result.Result = *old;
        result.Reference = {previous->Reference};
        return result;
    }
    const NativeScriptObjectSource source{resolvedModel, name,
        lookup.Status == NativeCollisionModelStatus::Ready ? lookup.Model : nullptr};
    const auto created = m_Objects.Create(request, source, noOffset);
    if (created.Result.Status != NativeScriptServiceStatus::Ready) {
        result.Result = created.Result;
        return result;
    }
    event.Reference = created.Reference.Value;
    Commit(event);
    return created;
}
NativeScriptReferenceResult<NativeScriptObjectRef> RealtimeScriptHost::CreateObjectNoOffset(
    const NativeScriptObjectRequest& request) { return CreateObjectInternal(request, true); }
NativeScriptReferenceResult<NativeScriptObjectRef> RealtimeScriptHost::CreateObject(
    const NativeScriptObjectRequest& request) { return CreateObjectInternal(request, false); }
NativeScriptServiceResult RealtimeScriptHost::SetObjectHeading(const NativeScriptObjectHeadingRequest& request) {
    if (!m_Initialized) return Error("object heading requires initialized host");
    RealtimeScriptHostEvent event{.Id=request.Id, .Opcode=0x0177,
        .Arguments={request.Degrees}, .Index=request.Object.Value};
    if (auto old = Replay(event)) return *old;
    std::string error;
    if (m_Objects.SetHeading(request.Object, request.Degrees, error) != NativeScriptObjectStatus::Ok) return Error(error);
    Commit(event);
    return Ready();
}
NativeScriptServiceResult RealtimeScriptHost::MarkObjectNoLongerNeeded(const NativeScriptObjectCleanupRequest& request) {
    if (!m_Initialized) return Error("object cleanup requires initialized host");
    RealtimeScriptHostEvent event{.Id=request.Id, .Opcode=0x01C7, .Index=request.Object.Value};
    if (auto old = Replay(event)) return *old;
    std::string error;
    if (m_Objects.MarkNoLongerNeeded(request.Object, error) != NativeScriptObjectStatus::Ok) return Error(error);
    Commit(event);
    return Ready();
}
NativeScriptServiceResult RealtimeScriptHost::SetObjectCollisionDamageEffect(const NativeScriptObjectDamageRequest& request) {
    if (!m_Initialized) return Error("object collision damage requires initialized host");
    RealtimeScriptHostEvent event{.Id=request.Id, .Opcode=0x07F7, .Index=request.Object.Value,
        .StateArgument=request.Effect};
    if (auto old = Replay(event)) return *old;
    std::string error;
    if (m_Objects.SetCollisionDamageEffect(request.Object, request.Effect, error) != NativeScriptObjectStatus::Ok) return Error(error);
    Commit(event);
    return Ready();
}
NativeScriptServiceResult RealtimeScriptHost::FreezeObjectPosition(const NativeScriptObjectFreezeRequest& request) {
    if (!m_Initialized) return Error("object freeze requires initialized host");
    RealtimeScriptHostEvent event{.Id=request.Id, .Opcode=0x0550, .Index=request.Object.Value,
        .StateArgument=request.Frozen ? 1 : 0};
    if (auto old = Replay(event)) return *old;
    std::string error;
    if (m_Objects.SetStatic(request.Object, request.Frozen, error) != NativeScriptObjectStatus::Ok) return Error(error);
    Commit(event);
    return Ready();
}
NativeScriptServiceResult RealtimeScriptHost::SetObjectDynamic(const NativeScriptObjectDynamicRequest& request) {
    if (!m_Initialized) return Error("object dynamic state requires initialized host");
    RealtimeScriptHostEvent event{.Id=request.Id, .Opcode=0x0392, .Index=request.Object.Value,
        .StateArgument=request.Dynamic ? 1 : 0};
    if (auto old = Replay(event)) return *old;
    std::string error;
    if (m_Objects.SetStatic(request.Object, !request.Dynamic, error) != NativeScriptObjectStatus::Ok) return Error(error);
    Commit(event);
    return Ready();
}
NativeScriptServiceResult RealtimeScriptHost::SetObjectVelocity(const NativeScriptObjectVelocityRequest& request) {
    if (!m_Initialized) return Error("object velocity requires initialized host");
    RealtimeScriptHostEvent event{.Id=request.Id, .Opcode=0x0381,
        .Arguments={request.Velocity.X, request.Velocity.Y, request.Velocity.Z}, .Index=request.Object.Value};
    if (auto old = Replay(event)) return *old;
    std::string error;
    if (m_Objects.SetVelocity(request.Object, request.Velocity, error) != NativeScriptObjectStatus::Ok) return Error(error);
    Commit(event);
    return Ready();
}
NativeScriptServiceResult RealtimeScriptHost::SetObjectProofs(const NativeScriptObjectProofRequest& request) {
    if (!m_Initialized) return Error("object proofs require initialized host");
    RealtimeScriptHostEvent event{.Id=request.Id, .Opcode=0x09CA, .Index=request.Object.Value,
        .StateArgument=request.Proofs};
    if (auto old = Replay(event)) return *old;
    std::string error;
    if (m_Objects.SetProofs(request.Object, request.Proofs, error) != NativeScriptObjectStatus::Ok) return Error(error);
    Commit(event);
    return Ready();
}
NativeScriptServiceResult RealtimeScriptHost::RotateObject(const NativeScriptObjectRotateRequest& request) {
    if (!m_Initialized) return Error("object rotation requires initialized host");
    RealtimeScriptHostEvent event{.Id=request.Id, .Opcode=0x034D,
        .Arguments={request.Rotation.X, request.Rotation.Y, request.Rotation.Z}, .Index=request.Object.Value,
        .StateArgument=request.Relative ? 1 : 0};
    if (auto old = Replay(event)) return *old;
    std::string error;
    if (m_Objects.SetRotation(request.Object, request.Rotation, request.Relative, error) != NativeScriptObjectStatus::Ok) return Error(error);
    Commit(event);
    return Ready();
}
NativeScriptServiceResult RealtimeScriptHost::SetObjectRotation(const NativeScriptObjectRotateRequest& request) {
    if (!m_Initialized) return Error("object rotation requires initialized host");
    RealtimeScriptHostEvent event{.Id=request.Id, .Opcode=0x0453,
        .Arguments={request.Rotation.X, request.Rotation.Y, request.Rotation.Z}, .Index=request.Object.Value};
    if (auto old = Replay(event)) return *old;
    std::string error;
    if (m_Objects.SetRotation(request.Object, request.Rotation, false, error) != NativeScriptObjectStatus::Ok) return Error(error);
    Commit(event);
    return Ready();
}
NativeScriptServiceResult RealtimeScriptHost::SetObjectAreaVisible(const NativeScriptObjectAreaRequest& request) {
    if (!m_Initialized) return Error("object area requires initialized host");
    RealtimeScriptHostEvent event{.Id=request.Id, .Opcode=0x0566, .Index=request.Object.Value,
        .StateArgument=request.Area};
    if (auto old = Replay(event)) return *old;
    std::string error;
    if (m_Objects.SetArea(request.Object, request.Area, error) != NativeScriptObjectStatus::Ok) return Error(error);
    Commit(event);
    return Ready();
}
NativeScriptObjectCoordinatesResult RealtimeScriptHost::GetObjectCoordinates(
    const NativeScriptObjectCoordinatesRequest& request) {
    NativeScriptObjectCoordinatesResult result;
    if (!m_Initialized) { result.Result = Error("object coordinates require initialized host"); return result; }
    const auto* object = m_Objects.Resolve(request.Object);
    if (!object) { result.Result = Error("stale source object reference"); return result; }
    RealtimeScriptHostEvent event{.Id=request.Id, .Opcode=0x01BB, .Arguments={object->Position.X, object->Position.Y, object->Position.Z},
        .Index=request.Object.Value};
    if (auto old = Replay(event)) {
        result.Result = *old;
        if (result.Result.Status == NativeScriptServiceStatus::Ready) {
            const auto previous = std::ranges::find(m_Events, request.Id, &RealtimeScriptHostEvent::Id);
            if (previous == m_Events.end()) { result.Result = Error("object coordinate replay event is missing"); return result; }
            result.Position = {previous->Arguments[0], previous->Arguments[1], previous->Arguments[2]};
        }
        return result;
    }
    result.Position = object->Position;
    Commit(event);
    result.Result = Ready();
    return result;
}
NativeScriptObjectCoordinatesResult RealtimeScriptHost::GetObjectOffsetInWorld(
    const NativeScriptObjectCoordinatesRequest& request) {
    NativeScriptObjectCoordinatesResult result;
    if (!m_Initialized) { result.Result = Error("object offset requires initialized host"); return result; }
    NativeScriptPosition position;
    std::string error;
    if (m_Objects.GetOffsetInWorld(request.Object, request.Offset, position, error) != NativeScriptObjectStatus::Ok) {
        result.Result = Error(error);
        return result;
    }
    RealtimeScriptHostEvent event{.Id=request.Id, .Opcode=0x0400,
        .Arguments={position.X, position.Y, position.Z, request.Offset.X, request.Offset.Y, request.Offset.Z},
        .Index=request.Object.Value};
    if (auto old = Replay(event)) {
        result.Result = *old;
        if (result.Result.Status == NativeScriptServiceStatus::Ready) {
            const auto previous = std::ranges::find(m_Events, request.Id, &RealtimeScriptHostEvent::Id);
            if (previous == m_Events.end()) { result.Result = Error("object offset replay event is missing"); return result; }
            result.Position = {previous->Arguments[0], previous->Arguments[1], previous->Arguments[2]};
        }
        return result;
    }
    result.Position = position;
    Commit(event);
    result.Result = Ready();
    return result;
}
NativeScriptObjectHeadingResult RealtimeScriptHost::GetObjectHeading(const NativeScriptObjectCoordinatesRequest& request) {
    NativeScriptObjectHeadingResult result;
    if (!m_Initialized) { result.Result = Error("object heading requires initialized host"); return result; }
    const auto* object = m_Objects.Resolve(request.Object);
    if (!object) { result.Result = Error("stale source object reference"); return result; }
    RealtimeScriptHostEvent event{.Id=request.Id, .Opcode=0x0176, .Arguments={object->HeadingDegrees}, .Index=request.Object.Value};
    if (auto old = Replay(event)) {
        result.Result = *old;
        if (result.Result.Status == NativeScriptServiceStatus::Ready) {
            const auto previous = std::ranges::find(m_Events, request.Id, &RealtimeScriptHostEvent::Id);
            if (previous == m_Events.end()) { result.Result = Error("object heading replay event is missing"); return result; }
            result.Degrees = previous->Arguments[0];
        }
        return result;
    }
    result.Degrees = object->HeadingDegrees;
    Commit(event);
    result.Result = Ready();
    return result;
}
NativeScriptServiceResult RealtimeScriptHost::ConnectObjectLods(const NativeScriptObjectLodRequest& request) {
    if (!m_Initialized) return Error("object LOD connection requires initialized host");
    RealtimeScriptHostEvent event{.Id=request.Id, .Opcode=0x0827, .Index=request.Child.Value,
        .Reference=request.Parent.Value};
    if (auto old = Replay(event)) return *old;
    std::string error;
    if (m_Objects.ConnectLods(request.Child, request.Parent, error) != NativeScriptObjectStatus::Ok) return Error(error);
    Commit(event);
    return Ready();
}

NativeScriptPedRef RealtimeScriptHost::PedRef() const { return {static_cast<std::int32_t>((0u << 8) | m_PedGeneration)}; }
NativeScriptGroupRef RealtimeScriptHost::GroupRef() const { return {static_cast<std::int32_t>(0u | (std::uint32_t{m_Group.Generation} << 16))}; }
const RealtimeGameplay* RealtimeScriptHost::ResolvePed(NativeScriptPedRef ref) const {
    return m_PedActive && ref.Value == PedRef().Value && m_Gameplay.State().Ready ? &m_Gameplay : nullptr;
}
const RealtimeScriptGroup* RealtimeScriptHost::ResolveGroup(NativeScriptGroupRef ref) const {
    return m_Group.Active && ref.Value == GroupRef().Value && ResolvePed(m_Group.Leader) ? &m_Group : nullptr;
}
std::optional<NativeScriptServiceResult> RealtimeScriptHost::Replay(const RealtimeScriptHostEvent& event) {
    if (m_Entities.OwnsRequest(event.Id)) return Error("service request ID already owned by property/radar service");
    for (const auto& old : m_Events) {
        if (old.Id != event.Id) continue;
        if (old.Opcode != event.Opcode || old.Arguments != event.Arguments || old.Index != event.Index || old.StateArgument != event.StateArgument || old.Name != event.Name || old.GeneratorArguments != event.GeneratorArguments || old.ModelName != event.ModelName)
            return Error("service request ID reused with different command/arguments");
        return NativeScriptServiceResult{old.Status, old.Status == NativeScriptServiceStatus::Ready ? "" : "replayed failed service request"};
    }
    // Reserve the event journal before any live effect. Its state snapshots
    // contain no allocating members, so Commit cannot fail after publication.
    if (m_Events.size() == m_Events.capacity()) m_Events.reserve(m_Events.empty() ? 16 : m_Events.size() * 2);
    return {};
}
void RealtimeScriptHost::Commit(RealtimeScriptHostEvent event) {
    event.Player = m_Gameplay.State(); event.Camera = m_Gameplay.Camera();
    m_Events.push_back(std::move(event));
}

NativeScriptServiceResult RealtimeScriptHost::PublishWorld(const NativeScriptSceneRequest& request, bool requireGround) {
    if (!m_Initialized) return Error("world service requires initialized host");
    if (!Finite(request.Position)) return Error("nonfinite world request");
    if (m_InWorldService) return Error("world service callback reentry rejected");
    m_InWorldService = true;
    struct ResetCallGuard {
        bool& Value;
        ~ResetCallGuard() { Value = false; }
    } resetCall{m_InWorldService};
    const auto identity = WorldIdentity(request, requireGround);
    NativeScriptServiceTicket ticket;
    const auto admission = m_WorldTransaction.Begin(identity, ticket);
    if (admission == NativeScriptServiceTransactionStatus::Conflict)
        return Error("pending world request ID reused with changed command/position");
    if (admission == NativeScriptServiceTransactionStatus::Busy)
        return Error("another world request is pending");
    if (admission == NativeScriptServiceTransactionStatus::AlreadyCommitted) return Ready();
    if (admission != NativeScriptServiceTransactionStatus::Started &&
        admission != NativeScriptServiceTransactionStatus::Existing)
        return Error("world transaction admission rejected");
    m_PendingLoad = request.Id;
    if (m_WorldTransaction.State().Phase == NativeScriptServiceTransactionPhase::Cancelling)
        return Pending("world cancellation acknowledgement pending");

    const bool wasPrepared = m_WorldTransaction.State().Phase == NativeScriptServiceTransactionPhase::Prepared;
    const auto releaseFailure = [&](std::string message, bool notifyDiscard) {
        if (notifyDiscard && m_Cancel) try { m_Cancel(ticket); } catch (...) {}
        m_WorldTransaction.Fail(ticket); m_WorldTransaction.Release(ticket);
        m_PendingWorldPublication.reset(); m_PendingLoad.reset();
        return Error(std::move(message));
    };
    RealtimeScriptWorldPublication publication;
    if (wasPrepared) {
        if (!m_PendingWorldPublication)
            return releaseFailure("prepared world transaction lost its owned publication", bool(m_Loader));
        publication = *m_PendingWorldPublication;
    } else if (m_Loader) {
        NativeScriptAsyncPrepareResult result;
        try {
            result = m_Loader(request, ticket, publication);
        } catch (const std::exception& exception) {
            return releaseFailure("world preparation exception: " + std::string(exception.what()), true);
        } catch (...) {
            return releaseFailure("unknown world preparation exception", true);
        }
        if (result.Status == NativeScriptAsyncPrepareStatus::Pending) {
            if (m_WorldTransaction.MarkPending(ticket) != NativeScriptServiceTransactionStatus::Ok)
                return releaseFailure("world transaction rejected Pending", true);
            return Pending(std::move(result.Message));
        }
        if (result.Status == NativeScriptAsyncPrepareStatus::Error) {
            return releaseFailure(std::move(result.Message), false);
        }
        if (result.Status != NativeScriptAsyncPrepareStatus::Prepared)
            return releaseFailure("world worker returned an invalid preparation status", true);
        if (m_WorldTransaction.MarkPrepared(ticket) != NativeScriptServiceTransactionStatus::Ok)
            return releaseFailure("world transaction rejected Prepared", true);
    } else {
        if (m_Sealed) {
            m_WorldTransaction.Fail(ticket); m_WorldTransaction.Release(ticket); m_PendingLoad.reset();
            return Unsupported("startup sealed: collision/scene needs live worker world loader");
        }
        char err[512]{};
        auto scene = std::make_shared<WorldShotScene>();
        const auto p = request.Position;
        publication.Overrides = m_InitialPlacementOverrides;
        if (!StreamPager_Update(p.X, p.Y, p.Z, *scene, publication.Frame, err, sizeof(err), publication.Overrides)) {
            m_WorldTransaction.Fail(ticket); m_WorldTransaction.Release(ticket); m_PendingLoad.reset();
            return Error(err);
        }
        publication.Scene = std::move(scene); publication.Center = p;
        if (m_WorldTransaction.MarkPrepared(ticket) != NativeScriptServiceTransactionStatus::Ok)
            return Error("world transaction rejected synchronous preparation");
    }
    if (publication.Overrides != m_InitialPlacementOverrides)
        return releaseFailure("world loader placement snapshot mismatch", bool(m_Loader));
    if (!publication.Scene || publication.Center != request.Position || publication.Frame.instances <= 0 ||
        publication.Frame.tris <= 0 || publication.Scene->meshes.empty())
        return releaseFailure("world loader did not supply requested resident scene", bool(m_Loader));
    std::shared_ptr<const RealtimeGameplayWorld> world = publication.Collision;
    std::string error;
    // Pure owned data, including when a live loader supplies the render scene.
    // Never trust a callback's collision as a replacement for source residency.
    if (!m_PendingWorldPublication) {
        auto snapshot = std::make_shared<NativeCollisionSnapshot>();
        auto rebuilt = std::make_shared<RealtimeGameplayWorld>();
        if (!m_CollisionContext->Snapshot(request.Position.X, request.Position.Y, *snapshot, error, publication.Overrides) ||
            !rebuilt->Rebuild(*snapshot, error)) return releaseFailure(error, bool(m_Loader));
        world = std::move(rebuilt);
        publication.SourceCollision = std::move(snapshot);
    }
    if (!world->TriangleCount() && !world->SphereCount() && !world->BoxCount())
        return releaseFailure("loaded region has no source COL primitives", bool(m_Loader));
    float ground;
    const auto p = request.Position;
    if (requireGround && !world->Ground(p.X, p.Y, p.Z + 1.0f, p.Z - 150.0f, ground))
        return releaseFailure("LOAD_SCENE has no actual resident ground at requested position", bool(m_Loader));
    publication.Collision = world;
    const auto residency = ReconcileCarGeneratorsBeforeWorldCommit(m_WorldRevision + 1, publication.SourceCollision);
    if (residency.Status == NativeCarGeneratorResidencyStatus::PendingCleanup) {
        m_PendingWorldPublication = publication;
        return Pending(residency.Detail);
    }
    if (residency.Status != NativeCarGeneratorResidencyStatus::Ready)
        return releaseFailure(residency.Detail, bool(m_Loader));
    if (m_WorldRevision == std::numeric_limits<std::uint64_t>::max())
        return releaseFailure("world publication revision exhausted", bool(m_Loader));
    if (m_WorldTransaction.State().Revision > std::numeric_limits<std::uint64_t>::max() - 2 ||
        m_WorldTransaction.State().Commits == std::numeric_limits<std::uint64_t>::max())
        return releaseFailure("world transaction commit sequence exhausted", bool(m_Loader));
    if (m_WorldTransaction.Commit(ticket) != NativeScriptServiceTransactionStatus::Ok)
        return Error("world transaction could not record committed publication");
    m_World = std::move(world); m_Publication = std::move(publication); ++m_WorldRevision;
    if (m_WorldTransaction.Release(ticket) != NativeScriptServiceTransactionStatus::Ok)
        return Error("world transaction could not release committed publication");
    m_PendingWorldPublication.reset(); m_PendingLoad.reset();
    return Ready();
}
NativeScriptServiceResult RealtimeScriptHost::RequestCollision(const NativeScriptCollisionRequest& request) {
    RealtimeScriptHostEvent event{.Id=request.Id, .Opcode=0x04E4, .Arguments={request.X, request.Y}};
    if (auto result = Replay(event)) return *result;
    // 04E4 has XY only. Pager residency is XY based; Z is not a ground result.
    const NativeScriptPosition region{request.X, request.Y, 0};
    auto result = PublishWorld({request.Id, region});
    if (result.Status == NativeScriptServiceStatus::Ready) { m_CollisionRegion = region; Commit(event); }
    return result;
}
NativeScriptServiceResult RealtimeScriptHost::LoadScene(const NativeScriptSceneRequest& request) {
    const auto p = request.Position;
    RealtimeScriptHostEvent event{.Id=request.Id, .Opcode=0x03CB, .Arguments={p.X, p.Y, p.Z}};
    if (auto result = Replay(event)) return *result;
    if (!m_CollisionRegion || m_CollisionRegion->X != p.X || m_CollisionRegion->Y != p.Y)
        return Unsupported("LOAD_SCENE requires the requested collision region in this bounded host");
    auto result = PublishWorld(request, true);
    if (result.Status != NativeScriptServiceStatus::Ready) return result;
    m_LoadedScene = p; Commit(event); return Ready();
}
NativeScriptServiceResult RealtimeScriptHost::CreatePlayer(const NativeScriptPlayerRequest& request) {
    const auto p = request.Position;
    RealtimeScriptHostEvent event{.Id=request.Id, .Opcode=0x0053, .Arguments={p.X, p.Y, p.Z}, .Index=request.PlayerIndex};
    if (auto result = Replay(event)) return *result;
    if (request.PlayerIndex != 0) return Unsupported("only player0 has a native gameplay binding");
    if (m_PedActive) return Error("player0 already allocated");
    if (!m_World || !m_LoadedScene || m_LoadedScene->X != p.X || m_LoadedScene->Y != p.Y)
        return Error("0053 requires published collision and scene at authored XY");
    std::string error;
    if (!m_Gameplay.SpawnScriptPlayer(*m_World, {p.X, p.Y, p.Z}, error)) return Error(error);
    // Real bounded pool allocation, not a player index masquerading as a ref.
    m_PedGeneration = static_cast<std::uint8_t>((m_PedGeneration + 1) & 0x7f);
    m_PedActive = true;
    // Script thing generation occupies the high word (TheScripts.cpp 0x483720).
    m_Group.Generation = m_Group.Generation >= 0xfffe ? 1 : m_Group.Generation + 1;
    m_Group.Active = m_Group.MissionGroup = true; m_Group.Leader = PedRef();
    Commit(event); return Ready();
}
NativeScriptReferenceResult<NativeScriptGroupRef> RealtimeScriptHost::GetPlayerGroup(const NativeScriptPlayerLookupRequest& request) {
    RealtimeScriptHostEvent event{.Id=request.Id, .Opcode=0x07AF, .Index=request.PlayerIndex};
    if (auto result = Replay(event)) return {*result, GroupRef()};
    if (request.PlayerIndex != 0 || !ResolveGroup(GroupRef())) return {Error("player group is not allocated"), {}};
    event.Reference = GroupRef().Value; Commit(event); return {Ready(), GroupRef()};
}
NativeScriptReferenceResult<NativeScriptPedRef> RealtimeScriptHost::GetPlayerChar(const NativeScriptPlayerLookupRequest& request) {
    RealtimeScriptHostEvent event{.Id=request.Id, .Opcode=0x01F5, .Index=request.PlayerIndex};
    if (auto result = Replay(event)) return {*result, PedRef()};
    if (request.PlayerIndex != 0 || !ResolvePed(PedRef())) return {Error("player ped is not allocated"), {}};
    event.Reference = PedRef().Value; Commit(event); return {Ready(), PedRef()};
}
NativeScriptServiceResult RealtimeScriptHost::SetCameraBehindPlayer(const NativeScriptCameraRequest& request) {
    RealtimeScriptHostEvent event{.Id=request.Id, .Opcode=0x0373};
    if (auto result = Replay(event)) return *result;
    if (!m_World || !ResolvePed(PedRef())) return Error("camera has no live target/world");
    std::string error;
    if (!m_Gameplay.SetScriptCameraBehind(*m_World, error)) return Error(error);
    Commit(event); return Ready();
}
NativeScriptServiceResult RealtimeScriptHost::SetCharHeading(const NativeScriptHeadingRequest& request) {
    RealtimeScriptHostEvent event{.Id=request.Id, .Opcode=0x0173, .Arguments={request.Radians}, .Index=request.Ped.Value};
    if (auto result = Replay(event)) return *result;
    if (!ResolvePed(request.Ped)) return Error("stale or unallocated ped reference");
    std::string error;
    if (!m_Gameplay.SetScriptHeading(request.Radians, error)) return Error(error);
    Commit(event); return Ready();
}
NativeScriptReferenceResult<NativeScriptPickupRef> RealtimeScriptHost::CreateLockedProperty(const NativeScriptLockedPropertyRequest& request) {
    if (!m_Initialized) return {Error("property service requires initialized host"), {}};
    if (m_PendingLoad) return {Error("entity service cannot cross pending world request"), {}};
    for (const auto& event : m_Events) if (event.Id == request.Id) return {Error("entity request ID already owned by player/world service"), {}};
    return m_Entities.CreateLockedProperty(request);
}
NativeScriptReferenceResult<NativeScriptPickupRef> RealtimeScriptHost::CreateForSaleProperty(const NativeScriptForSalePropertyRequest& request) {
    if (!m_Initialized) return {Error("property service requires initialized host"), {}};
    if (m_PendingLoad) return {Error("entity service cannot cross pending world request"), {}};
    for (const auto& event : m_Events) if (event.Id == request.Id) return {Error("entity request ID already owned by player/world service"), {}};
    return m_Entities.CreateForSaleProperty(request);
}
bool RealtimeScriptHost::TickProperties(NativeScriptPosition camera, bool alive, NativeScriptPropertyInput input, std::string& error) {
    if (!m_Initialized || !m_PedActive || !Finite(camera)) { error = "property consumer requires live player and finite camera"; return false; }
    input.Money = m_PlayerInfo.Money;
    std::int32_t mission = 0;
    if (State().OnAMissionFlag && !m_Session.ReadGlobal(State().OnAMissionFlag, mission)) { error = "invalid declared mission flag"; return false; }
    input.OnMission = State().OnAMissionFlag && mission == 1;
    const auto& player = m_Gameplay.State();
    m_Entities.Tick({player.PedRoot.X, player.PedRoot.Y, player.PedRoot.Z}, camera, alive, player.InVehicle, input);
    error.clear(); return true;
}
bool RealtimeScriptHost::TickPlayerEntities(NativeScriptPosition camera, NativeScriptPropertyInput input, std::string& error) {
    const auto& activity = m_Gameplay.Activity();
    input.Busy = NativePlayerPickupBusy(activity);
    input.Coop = activity.CoopGame;
    if (!TickProperties(camera, activity.Alive, input, error)) return false;
    return m_Entities.UpdatePlayerActivity(input.FrameCounter, activity.Revision, activity, error);
}
std::shared_ptr<const NativeVehiclePoolSnapshot> RealtimeScriptHost::PublishVehicles(std::uint64_t frame, std::string& error) {
    if (!m_Initialized || m_Gameplay.State().CarPresent) {
        error = "vehicle census requires initialized host; controller car needs a registered source lifecycle";
        return {};
    }
    // Base-player startup creates no controller car. New SCM vehicle services
    // must allocate from m_Vehicles before publishing a rendered vehicle.
    return m_Vehicles.Publish(frame, error);
}
NativeScriptReferenceResult<NativeScriptPickupRef> RealtimeScriptHost::CreatePickup(const NativeScriptPickupRequest& request) {
    if (!m_Initialized) return {Error("pickup service requires initialized host"), {}};
    if (m_PendingLoad) return {Error("entity service cannot cross pending world request"), {}};
    for (const auto& event : m_Events) if (event.Id == request.Id) return {Error("entity request ID already owned by player/world service"), {}};
    const auto camera = m_Gameplay.Camera().Position;
    return m_Entities.CreatePickup(request, {camera.X, camera.Y, camera.Z}, State().TimeMs);
}
NativeScriptReferenceResult<NativeScriptBlipRef> RealtimeScriptHost::CreateContactBlip(const NativeScriptContactBlipRequest& request) {
    if (!m_Initialized) return {Error("radar service requires initialized host"), {}};
    if (m_PendingLoad) return {Error("entity service cannot cross pending world request"), {}};
    for (const auto& event : m_Events) if (event.Id == request.Id) return {Error("entity request ID already owned by player/world service"), {}};
    // Entity-owned replays must not become dependent on later GPU/context state.
    if (m_Entities.OwnsRequest(request.Id)) return m_Entities.CreateContactBlip(request);
    auto prepared = request;
    const auto readiness = PrepareContactBlipRequest(prepared);
    if (readiness.Status != NativeScriptServiceStatus::Ready) return {readiness, {}};
    return m_Entities.CreateContactBlip(prepared);
}
NativeScriptReferenceResult<NativeScriptBlipRef> RealtimeScriptHost::CreateCoordinateBlip(const NativeScriptCoordinateBlipRequest& request) {
    if (!m_Initialized) return {Error("radar service requires initialized host"), {}};
    if (m_PendingLoad) return {Error("entity service cannot cross pending world request"), {}};
    for (const auto& event : m_Events) if (event.Id == request.Id) return {Error("entity request ID already owned by player/world service"), {}};
    return m_Entities.CreateCoordinateBlip(request, m_RadarSpriteReady);
}
NativeScriptServiceResult RealtimeScriptHost::SetBlipDisplay(const NativeScriptBlipDisplayRequest& request) {
    if (!m_Initialized) return Error("radar service requires initialized host");
    if (m_PendingLoad) return Error("entity service cannot cross pending world request");
    for (const auto& event : m_Events) if (event.Id == request.Id) return Error("entity request ID already owned by player/world service");
    return m_Entities.SetBlipDisplay(request);
}
NativeScriptServiceResult RealtimeScriptHost::SetEntryExitFlag(const NativeScriptEntryExitFlagRequest& request) {
    if (!m_Initialized) return Error("ENEX service requires initialized host");
    if (m_PendingLoad) return Error("ENEX service cannot cross pending world request");
    if (!Finite({request.X, request.Y, request.Radius})) return Error("nonfinite ENEX request");
    RealtimeScriptHostEvent event{.Id=request.Id, .Opcode=0x09B4,
        .Arguments={request.X, request.Y, request.Radius}, .Index=request.Mask, .StateArgument=request.State};
    if (auto result = Replay(event)) return *result;
    const auto result = m_EntryExits.SetFlag(request);
    if (result.Status == NativeScriptServiceStatus::Ready) Commit(event);
    return result;
}
NativeScriptServiceResult RealtimeScriptHost::DeactivateGarage(const NativeScriptGarageRequest& request) {
    if (!m_Initialized) return Error("garage service requires initialized host");
    if (m_PendingLoad) return Error("garage service cannot cross pending world request");
    RealtimeScriptHostEvent event{.Id=request.Id, .Opcode=0x02B9, .Name=request.Name};
    if (auto result=Replay(event)) return *result;
    if (const auto ref=m_Garages.Find(request.Name)) event.Reference=static_cast<int32>(ref->Index);
    const auto result=m_Garages.Deactivate(request.Name);
    if (result.Status==NativeScriptServiceStatus::Ready) Commit(event);
    return result;
}
NativeScriptPickupCollectedResult RealtimeScriptHost::HasPickupBeenCollected(const NativeScriptPickupReferenceRequest& request) {
    if (!m_Initialized) return {Error("pickup query service requires initialized host"), false};
    if (m_PendingLoad) return {Error("pickup query service cannot cross pending world request"), false};
    for (const auto& event : m_Events) if (event.Id == request.Id)
        return {Error("pickup query request ID already owned by player/world service"), false};
    // The source ring survives pickup removal, so this must not use ResolvePickup.
    return m_Entities.HasPickupBeenCollected(request);
}
NativeScriptServiceResult RealtimeScriptHost::RemoveScriptPickup(const NativeScriptPickupReferenceRequest& request) {
    if (!m_Initialized) return Error("pickup removal service requires initialized host");
    if (m_PendingLoad) return Error("pickup removal service cannot cross pending world request");
    for (const auto& event : m_Events) if (event.Id == request.Id)
        return Error("pickup removal request ID already owned by player/world service");
    return m_Entities.RemoveScriptPickup(request);
}

NativeCarGeneratorResidencyResult RealtimeScriptHost::ReconcileCarGeneratorsBeforeWorldCommit(
    std::uint64_t generation, std::shared_ptr<const NativeCollisionSnapshot> sourceCollision,
    const NativeCarGeneratorResidencyCleanup* cleanup) {
    if (!m_Initialized) {
        NativeCarGeneratorResidencyResult result;
        result.Detail = "generator residency requires initialized host";
        return result;
    }
    return m_CarGeneratorResidency.Reconcile(m_CarGenerators, generation, std::move(sourceCollision),
        State().TimeMs, cleanup, &m_Vehicles);
}
NativeCarGeneratorResidencyResult RealtimeScriptHost::ReconcilePendingWorldCleanup(
    const NativeCarGeneratorResidencyCleanup& cleanup) {
    if (!m_PendingLoad || !m_PendingWorldPublication ||
        m_WorldTransaction.State().Phase != NativeScriptServiceTransactionPhase::Prepared) {
        NativeCarGeneratorResidencyResult result;
        result.Detail = "generator cleanup requires the exact pending Prepared world";
        return result;
    }
    return ReconcileCarGeneratorsBeforeWorldCommit(
        m_WorldRevision + 1, m_PendingWorldPublication->SourceCollision, &cleanup);
}

NativeScriptReferenceResult<NativeScriptCarGeneratorRef> RealtimeScriptHost::CreateCarGenerator(
    const NativeScriptCarGeneratorRequest& request) {
    if (!m_Initialized) return {Error("generator service requires initialized host"), {}};
    if (m_PendingLoad) return {Error("generator service cannot cross pending world request"), {}};
    const auto p = request.Position;
    if (!Finite(p) || !std::isfinite(request.AngleDegrees)) return {Error("nonfinite generator request"), {}};
    RealtimeScriptHostEvent event{.Id=request.Id, .Opcode=0x014B,
        .Arguments={p.X, p.Y, p.Z, request.AngleDegrees},
        .GeneratorArguments={request.ModelId, request.PrimaryColor, request.SecondaryColor, request.ForceSpawn,
            request.AlarmChance, request.DoorLockChance, request.MinDelay, request.MaxDelay}};
    if (auto result = Replay(event)) {
        if (result->Status != NativeScriptServiceStatus::Ready) return {*result, {}};
        const auto old = std::ranges::find(m_Events, request.Id, &RealtimeScriptHostEvent::Id);
        return {*result, {old->Reference}};
    }
    const NativeCarGeneratorCreateRequest native{request.Id, p, request.AngleDegrees, request.ModelId,
        request.PrimaryColor, request.SecondaryColor, request.ForceSpawn, request.AlarmChance,
        request.DoorLockChance, request.MinDelay, request.MaxDelay};
    const auto result = m_CarGenerators.Create(native, State().TimeMs);
    event.Status = result.Result.Status;
    event.Reference = result.Reference.Value; Commit(event);
    return {result.Result, {result.Reference.Value}};
}

NativeScriptServiceResult RealtimeScriptHost::SwitchCarGenerator(const NativeScriptCarGeneratorSwitchRequest& request) {
    if (!m_Initialized) return Error("generator service requires initialized host");
    if (m_PendingLoad) return Error("generator service cannot cross pending world request");
    RealtimeScriptHostEvent event{.Id=request.Id, .Opcode=0x014C,
        .Index=request.Generator.Value, .StateArgument=request.Count};
    if (auto result = Replay(event)) return *result;
    const auto result = m_CarGenerators.Switch({request.Id, {request.Generator.Value}, request.Count}, State().TimeMs);
    event.Status = result.Status;
    event.Reference = request.Generator.Value; Commit(event);
    return result;
}
NativeScriptServiceResult RealtimeScriptHost::SetCarGeneratorOwned(const NativeScriptCarGeneratorOwnedRequest& request) {
    if (!m_Initialized) return Error("generator service requires initialized host");
    if (m_PendingLoad) return Error("generator service cannot cross pending world request");
    RealtimeScriptHostEvent event{.Id=request.Id, .Opcode=0x0A17,
        .Index=request.Generator.Value, .StateArgument=request.Owned ? 1 : 0};
    if (auto result = Replay(event)) return *result;
    std::string error;
    const auto result = m_CarGenerators.SetPlayerOwned(
        {request.Generator.Value}, request.Owned, error);
    event.Status = result.Status;
    event.Reference = request.Generator.Value;
    Commit(event);
    return result;
}
