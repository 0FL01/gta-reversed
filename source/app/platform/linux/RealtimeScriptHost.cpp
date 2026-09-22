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
template<std::size_t N> std::string FixedName(const std::array<char, N>& value) {
    return {value.data(), strnlen(value.data(), value.size())};
}
bool EqualNoCase(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](unsigned char x, unsigned char y) {
        return std::tolower(x) == std::tolower(y);
    });
}
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

RealtimeScriptHost::RealtimeScriptHost(RealtimeGameplay& gameplay): m_Gameplay(gameplay) {
    m_ScriptSpriteImages.fill(-1);
}
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
    if (!m_ZonePopulation.LoadBeforeWorker(gameDir, error)) return false;
    if (!m_MissionText.LoadBeforeWorker(gameDir, error) || !m_Cutscene.Initialize(gameDir, error) ||
        !m_CarRecordings.LoadBeforeWorker(gameDir, error) || !m_BeatTrack.LoadBeforeWorker(gameDir, error) ||
        !m_MissionAudio.LoadBeforeWorker(gameDir, error)) return false;
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
bool RealtimeScriptHost::AdoptLiveWorld(std::uint64_t generation,
    RealtimeScriptWorldPublication publication, std::string& error) {
    if(!m_Initialized||m_PendingLoad||m_WorldTransaction.State().Phase!=NativeScriptServiceTransactionPhase::Idle||
        generation<=m_WorldRevision||!publication.Scene||!publication.Collision||!publication.SourceCollision||
        publication.Scene->meshes.empty()||publication.Overrides!=m_InitialPlacementOverrides){
        error="live world publication identity mismatch generation="+std::to_string(generation)+
            " greater-than="+std::to_string(m_WorldRevision)+" pending="+std::to_string(bool(m_PendingLoad))+
            " phase="+std::to_string(unsigned(m_WorldTransaction.State().Phase))+
            " scene="+std::to_string(bool(publication.Scene))+" collision="+std::to_string(bool(publication.Collision))+
            " source="+std::to_string(bool(publication.SourceCollision))+
            " overrides="+std::to_string(publication.Overrides==m_InitialPlacementOverrides);return false;
    }
    m_World=publication.Collision;
    m_Publication=std::move(publication);
    m_WorldRevision=generation;
    error.clear();return true;
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
bool RealtimeScriptHost::AdvanceTime(std::uint32_t nowMs, std::string& error) {
    if (nowMs < m_Session.State().TimeMs) return m_Session.AdvanceTime(nowMs, error);
    m_MissionText.BeginFrame();
    m_ScriptRectangles.clear();
    m_Cutscene.AdvanceTime(nowMs);
    m_MissionAudio.Advance(nowMs);
    std::vector<NativeCarRecordingUpdate> recordingUpdates;
    if (!m_CarRecordings.Advance(nowMs, recordingUpdates, error)) return false;
    for (const auto& update : recordingUpdates) {
        if (update.Finished) continue;
        const auto* vehicle = m_Vehicles.Resolve({update.Vehicle.Value});
        if (!vehicle || vehicle->Producer != NativeVehicleProducer::NativeScm) {
            error = "car recording playback vehicle became stale";
            return false;
        }
        auto state = vehicle->State;
        state.Matrix.Basis = update.Basis;
        state.Matrix.Position = update.Position;
        if (!m_Vehicles.Update({update.Vehicle.Value}, state, error)) return false;
    }
    return m_Session.AdvanceTime(nowMs, error);
}

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
NativeScriptServiceResult RealtimeScriptHost::AddSetPiece(const NativeScriptSetPieceRequest& request) {
    if (!m_Initialized) return Error("set-piece service requires initialized host");
    RealtimeScriptHostEvent event{.Id=request.Id, .Opcode=0x04F8, .Index=request.Type};
    std::copy(request.Coordinates.begin(), request.Coordinates.end(), event.Arguments.begin());
    if (auto old=Replay(event)) return *old;
    std::string error;
    const auto status=m_SetPieces.Add(request,error);
    if (status==NativeSetPieceStatus::InvalidInput) return Error(error);
    Commit(event);
    return Ready();
}
NativeScriptServiceResult RealtimeScriptHost::InitZonePopulationSettings(const NativeScriptRequestId& id) {
    if (!m_Initialized) return Error("zone population service requires initialized host");
    RealtimeScriptHostEvent event{.Id=id,.Opcode=0x08CA}; if(auto old=Replay(event))return *old;
    const auto result=m_ZonePopulation.Reset(); event.Status=result.Status; Commit(event); return result;
}
NativeScriptServiceResult RealtimeScriptHost::SetZonePopulationType(const NativeScriptZonePopulationRequest& request) {
    if (!m_Initialized) return Error("zone population service requires initialized host");
    RealtimeScriptHostEvent event{.Id=request.Id,.Opcode=0x0767,.StateArgument=request.Value,.Name=request.Name};
    if(auto old=Replay(event))return *old; const auto result=m_ZonePopulation.SetType(request); event.Status=result.Status; Commit(event); return result;
}
NativeScriptServiceResult RealtimeScriptHost::SetZonePopulationRaces(const NativeScriptZonePopulationRequest& request) {
    if (!m_Initialized) return Error("zone population service requires initialized host");
    RealtimeScriptHostEvent event{.Id=request.Id,.Opcode=0x0874,.StateArgument=request.Value,.Name=request.Name};
    if(auto old=Replay(event))return *old; const auto result=m_ZonePopulation.SetRaces(request); event.Status=result.Status; Commit(event); return result;
}
NativeScriptServiceResult RealtimeScriptHost::SetZoneDealerStrength(const NativeScriptZonePopulationRequest& request) {
    if (!m_Initialized) return Error("zone population service requires initialized host");
    RealtimeScriptHostEvent event{.Id=request.Id,.Opcode=0x076A,.StateArgument=request.Value,.Name=request.Name};
    if(auto old=Replay(event))return *old; const auto result=m_ZonePopulation.SetDealer(request); event.Status=result.Status; Commit(event); return result;
}
NativeScriptServiceResult RealtimeScriptHost::SetZoneGangStrength(const NativeScriptZoneGangRequest& request) {
    if (!m_Initialized) return Error("zone population service requires initialized host");
    RealtimeScriptHostEvent event{.Id=request.Id,.Opcode=0x076C,.Arguments={float(request.Gang)},.StateArgument=request.Strength,.Name=request.Name};
    if(auto old=Replay(event))return *old; const auto result=m_ZonePopulation.SetGang(request); event.Status=result.Status; Commit(event); return result;
}
NativeScriptServiceResult RealtimeScriptHost::SetZoneNoCops(const NativeScriptZonePopulationRequest& request) {
    if (!m_Initialized) return Error("zone population service requires initialized host");
    RealtimeScriptHostEvent event{.Id=request.Id,.Opcode=0x09B7,.StateArgument=request.Value,.Name=request.Name};
    if(auto old=Replay(event))return *old; const auto result=m_ZonePopulation.SetNoCops(request); event.Status=result.Status; Commit(event); return result;
}
NativeScriptServiceResult RealtimeScriptHost::AddPathPolicy(const NativeScriptPathPolicyRequest& request){
    if(!m_Initialized)return Error("path policy service requires initialized host");
    RealtimeScriptHostEvent event{.Id=request.Id,.Opcode=std::uint16_t(request.Kind==NativePathPolicyKind::VehicleOn?0x01E7:
        request.Kind==NativePathPolicyKind::VehicleOff?0x01E8:request.Kind==NativePathPolicyKind::VehicleOriginal?0x091D:
        request.Kind==NativePathPolicyKind::PedOn?0x022A:request.Kind==NativePathPolicyKind::PedOff?0x022B:0x091E)};
    std::copy(request.Coordinates.begin(),request.Coordinates.end(),event.Arguments.begin());
    if(auto old=Replay(event))return *old;const auto result=m_PathPolicy.Add(request);event.Status=result.Status;Commit(event);return result;
}
NativeScriptServiceResult RealtimeScriptHost::AddExternalScriptTrigger(const NativeScriptExternalTriggerRequest& request){
if(!m_Initialized)return Error("external trigger service requires initialized host");auto prepared=request;
if(request.ModelId<0){const std::string_view name(request.ModelName.data(),strnlen(request.ModelName.data(),request.ModelName.size()));if(name.empty()||!StreamPager_KnownModelName(name,&prepared.ModelId))return Error("external trigger used-object absent from pager IDE census");}
else if(!StreamPager_KnownModelId(request.ModelId))return Error("external trigger model absent from pager IDE census");
RealtimeScriptHostEvent event{.Id=request.Id,.Opcode=std::uint16_t(request.ObjectModel?0x0929:0x0928),.Arguments={request.Radius},.Index=request.ScriptIndex,.Reference=request.Type,.StateArgument=request.Priority,.GeneratorArguments={prepared.ModelId},.ModelName=request.ModelName};
if(auto old=Replay(event))return *old;const auto result=m_ExternalTriggers.Add(prepared,m_Session.StreamedScripts());event.Status=result.Status;Commit(event);return result;}
NativeScriptServiceResult RealtimeScriptHost::AddCodeScriptBrain(const NativeScriptCodeBrainRequest& request) {
    if (!m_Initialized) return Error("code-use script brain requires initialized host");
    RealtimeScriptHostEvent event{.Id=request.Id,.Opcode=std::uint16_t(request.Attractor ? 0x0884 : 0x07D3),.Index=request.ScriptIndex,.Name=request.Name};
    if (auto old = Replay(event)) return *old;
    const auto result = m_ExternalTriggers.AddCodeUse(request, m_Session.StreamedScripts());
    event.Status = result.Status;
    Commit(event);
    return result;
}
NativeScriptServiceResult RealtimeScriptHost::AttachAnimsToModel(const NativeScriptModelAnimRequest& request) {
    if (!m_Initialized) return Error("script model animation binding requires initialized host");
    if (!StreamPager_KnownModelId(request.ModelId)) return Error("script model animation model absent from pager IDE census");
    RealtimeScriptHostEvent event{.Id=request.Id,.Opcode=0x08E8,.Index=request.ModelId,.Name=request.IfpName};
    if (auto old = Replay(event)) return *old;
    const auto result = m_ModelAnims.Add(request);
    event.Status = result.Status;
    Commit(event);
    return result;
}
NativeScriptServiceResult RealtimeScriptHost::SetIplRequested(const NativeScriptIplRequest& request) {
    if (!m_Initialized) return Error("script IPL request requires initialized host");
    RealtimeScriptHostEvent event{.Id=request.Id,.Opcode=std::uint16_t(request.Requested ? 0x0776 : 0x0777),.ModelName={}};
    std::copy(request.Name.begin(), request.Name.end(), event.ModelName.begin());
    if (auto old = Replay(event)) return *old;
    const auto result = m_IplRequests.Set(request);
    event.Status = result.Status;
    Commit(event);
    return result;
}
NativeScriptServiceResult RealtimeScriptHost::SetClosestObjectVisibility(
    const NativeScriptWorldObjectVisibilityRequest& request) {
    if (!m_Initialized || !m_CollisionContext) return Error("world-object visibility requires initialized source population");
    auto prepared = request;
    if (request.ModelId < 0) {
        const std::string_view name(request.ModelName.data(), strnlen(request.ModelName.data(), request.ModelName.size()));
        if (name.empty() || !StreamPager_KnownModelName(name, &prepared.ModelId)) {
            return Error("world-object visibility used-object absent from pager IDE census");
        }
    } else if (!StreamPager_KnownModelId(request.ModelId)) {
        return Error("world-object visibility model absent from pager IDE census");
    }
    RealtimeScriptHostEvent event{.Id=request.Id,.Opcode=0x0363,
        .Arguments={request.Position.X,request.Position.Y,request.Position.Z,request.Radius},
        .Index=prepared.ModelId,.StateArgument=request.Visible ? 1 : 0,.ModelName=request.ModelName};
    if (auto old = Replay(event)) return *old;
    const auto result = m_WorldObjectOverrides.SetClosestVisibility(prepared, m_CollisionContext->Population);
    event.Status = result.Status;
    Commit(event);
    return result;
}
NativeScriptServiceResult RealtimeScriptHost::SetZoneNamesVisible(const NativeScriptRequestId& id, bool visible) {
    if (!m_Initialized) return Error("zone-name visibility requires initialized host");
    RealtimeScriptHostEvent event{.Id=id,.Opcode=0x09BA,.StateArgument=visible ? 1 : 0};
    if (auto old = Replay(event)) return *old;
    m_ZoneNamesVisible = visible;
    Commit(event);
    return Ready();
}
NativeScriptBooleanResult RealtimeScriptHost::IsPlayerPlaying(const NativeScriptPlayerLookupRequest& request) {
    NativeScriptBooleanResult result;
    if (!m_Initialized || request.PlayerIndex != 0) {
        result.Result = Error("player-playing query requires initialized player0 host");
        return result;
    }
    result.Value = ResolvePed(PedRef()) != nullptr;
    RealtimeScriptHostEvent event{.Id=request.Id,.Opcode=0x0256,.Index=request.PlayerIndex,
        .StateArgument=result.Value ? 1 : 0};
    if (auto old = Replay(event)) {
        result.Result = *old;
        if (result.Result.Status == NativeScriptServiceStatus::Ready) {
            const auto previous = std::ranges::find(m_Events, request.Id, &RealtimeScriptHostEvent::Id);
            if (previous == m_Events.end()) result.Result = Error("player-playing replay event is missing");
            else result.Value = previous->StateArgument != 0;
        }
        return result;
    }
    Commit(event);
    result.Result = Ready();
    return result;
}
NativeScriptIntegerResult RealtimeScriptHost::GetCharAreaVisible(const NativeScriptPedQueryRequest& request) {
    NativeScriptIntegerResult result;
    if (!m_Initialized || !ResolvePed(request.Ped)) {
        result.Result = Error("character-area query requires live source player ped");
        return result;
    }
    result.Value = m_PlayerArea;
    RealtimeScriptHostEvent event{.Id=request.Id,.Opcode=0x09E8,.Index=request.Ped.Value,.StateArgument=result.Value};
    if (auto old = Replay(event)) {
        result.Result = *old;
        if (result.Result.Status == NativeScriptServiceStatus::Ready) {
            const auto previous = std::ranges::find(m_Events, request.Id, &RealtimeScriptHostEvent::Id);
            if (previous == m_Events.end()) result.Result = Error("character-area replay event is missing");
            else result.Value = previous->StateArgument;
        }
        return result;
    }
    Commit(event);
    result.Result = Ready();
    return result;
}
NativeScriptIntegerResult RealtimeScriptHost::GetAreaVisible(const NativeScriptRequestId& id) {
    NativeScriptIntegerResult result;
    if (!m_Initialized) { result.Result = Error("visible-area query requires initialized host"); return result; }
    result.Value = m_PlayerArea;
    RealtimeScriptHostEvent event{.Id=id,.Opcode=0x077E,.StateArgument=result.Value};
    if (auto old = Replay(event)) { result.Result = *old; return result; }
    Commit(event);
    result.Result = Ready();
    return result;
}
NativeScriptIntegerResult RealtimeScriptHost::GetCurrentDayOfWeek(const NativeScriptRequestId& id) {
    NativeScriptIntegerResult result;
    if (!m_Initialized) { result.Result = Error("weekday query requires initialized host"); return result; }
    result.Value = m_DayOfWeek;
    RealtimeScriptHostEvent event{.Id=id,.Opcode=0x07D0,.StateArgument=result.Value};
    if (auto old = Replay(event)) { result.Result = *old; return result; }
    Commit(event);
    result.Result = Ready();
    return result;
}
NativeScriptIntegerResult RealtimeScriptHost::GetCurrentLanguage(const NativeScriptRequestId& id) {
    NativeScriptIntegerResult result;
    if (!m_Initialized) { result.Result = Error("language query requires initialized host"); return result; }
    result.Value = 0; // eLanguage::AMERICAN; NativeMissionText loaded american.gxt
    RealtimeScriptHostEvent event{.Id=id,.Opcode=0x09FB,.StateArgument=result.Value};
    if (auto old = Replay(event)) { result.Result = *old; return result; }
    Commit(event);
    result.Result = Ready();
    return result;
}
NativeScriptBooleanResult RealtimeScriptHost::HasLanguageChanged(const NativeScriptRequestId& id) {
    NativeScriptBooleanResult result;
    if (!m_Initialized) { result.Result = Error("language-change query requires initialized host"); return result; }
    result.Value = false;
    RealtimeScriptHostEvent event{.Id=id,.Opcode=0x0A0F,.StateArgument=0};
    if (auto old = Replay(event)) { result.Result = *old; return result; }
    Commit(event);
    result.Result = Ready();
    return result;
}
NativeScriptIntegerResult RealtimeScriptHost::GetCityPlayerIsIn(const NativeScriptPlayerLookupRequest& request) {
    NativeScriptIntegerResult result;
    if (!m_Initialized || request.PlayerIndex != 0 || !ResolvePed(PedRef())) {
        result.Result = Error("player-city query requires live source player0");
        return result;
    }
    const auto& root = m_Gameplay.State().PedRoot;
    const auto level = m_Restarts.LevelAt({root.X, root.Y, root.Z});
    if (!level) { result.Result = Error("player-city query requires source map-zone coverage"); return result; }
    result.Value = *level;
    RealtimeScriptHostEvent event{.Id=request.Id,.Opcode=0x0842,.Index=request.PlayerIndex,
        .StateArgument=result.Value};
    if (auto old = Replay(event)) { result.Result = *old; return result; }
    Commit(event);
    result.Result = Ready();
    return result;
}
NativeScriptIntegerResult RealtimeScriptHost::GetNumberTagsTagged(const NativeScriptRequestId& id) {
    NativeScriptIntegerResult result;
    if (!m_Initialized) { result.Result = Error("tag query requires initialized host"); return result; }
    result.Value = 0; // CTagManager source new-game static before any tag interaction
    RealtimeScriptHostEvent event{.Id=id,.Opcode=0x08E1,.StateArgument=result.Value};
    if (auto old = Replay(event)) { result.Result = *old; return result; }
    Commit(event);
    result.Result = Ready();
    return result;
}
NativeScriptBooleanResult RealtimeScriptHost::AreCarCheatsActivated(const NativeScriptRequestId& id) {
    NativeScriptBooleanResult result;
    if (!m_Initialized) { result.Result = Error("car-cheat query requires initialized host"); return result; }
    result.Value = m_CarCheatsActivated;
    RealtimeScriptHostEvent event{.Id=id,.Opcode=0x0445,.StateArgument=result.Value ? 1 : 0};
    if (auto old = Replay(event)) {
        result.Result = *old;
        if (result.Result.Status == NativeScriptServiceStatus::Ready) {
            const auto previous = std::ranges::find(m_Events, id, &RealtimeScriptHostEvent::Id);
            if (previous == m_Events.end()) result.Result = Error("car-cheat replay event is missing");
            else result.Value = previous->StateArgument != 0;
        }
        return result;
    }
    Commit(event);
    result.Result = Ready();
    return result;
}
NativeScriptBooleanResult RealtimeScriptHost::HasDeathArrestBeenExecuted(const NativeScriptRequestId& id) {
    NativeScriptBooleanResult result;
    if (!m_Initialized) { result.Result = Error("death/arrest query requires initialized host"); return result; }
    result.Value = m_DeathArrestExecuted;
    RealtimeScriptHostEvent event{.Id=id,.Opcode=0x0112,.StateArgument=result.Value ? 1 : 0};
    if (auto old = Replay(event)) {
        result.Result = *old;
        return result;
    }
    Commit(event);
    result.Result = Ready();
    return result;
}
NativeScriptBooleanResult RealtimeScriptHost::IsCharDead(const NativeScriptPedQueryRequest& request) {
    NativeScriptBooleanResult result;
    if (!m_Initialized) { result.Result = Error("dead-character query requires initialized host"); return result; }
    // SCM uses a zero/deleted handle as the initial dead sentinel before it
    // allocates a scripted speech ped. A live player or owned mission ped is
    // alive in this bounded owner; an absent generation is dead.
    result.Value = !ResolvePed(request.Ped) && !m_ScriptPeds.Resolve(request.Ped);
    RealtimeScriptHostEvent event{.Id=request.Id,.Opcode=0x0118,.Index=request.Ped.Value,
        .StateArgument=result.Value ? 1 : 0};
    if (auto old = Replay(event)) { result.Result = *old; return result; }
    Commit(event);
    result.Result = Ready();
    return result;
}
NativeScriptServiceResult RealtimeScriptHost::SetPedSpeechDisabled(const NativeScriptPedStateRequest& request) {
    RealtimeScriptHostEvent event{.Id=request.Id, .Opcode=0x0A09, .Index=request.Ped.Value,
        .StateArgument=request.Value ? 1 : 0};
    if (auto old = Replay(event)) return *old;
    // Source deliberately ignores a null/deleted ped pointer.
    if (ResolvePed(request.Ped) || m_ScriptPeds.Resolve(request.Ped))
        m_ScriptPedSpeechDisabled[request.Ped.Value] = request.Value;
    Commit(event);
    return Ready();
}
NativeScriptBooleanResult RealtimeScriptHost::IsGarageOpen(const NativeScriptGarageRequest& request) {
    NativeScriptBooleanResult result;
    if (!m_Initialized) { result.Result = Error("garage-open query requires initialized host"); return result; }
    const auto garage = m_Garages.Find(request.Name);
    const auto* entry = garage ? m_Garages.Resolve(*garage) : nullptr;
    if (!entry) { result.Result = Error("garage-open query name is absent"); return result; }
    result.Value = entry->DoorState == 1 || entry->DoorState == 4;
    RealtimeScriptHostEvent event{.Id=request.Id,.Opcode=0x03B0,.StateArgument=result.Value ? 1 : 0,
        .Name=request.Name};
    if (auto old = Replay(event)) { result.Result = *old; return result; }
    Commit(event);
    result.Result = Ready();
    return result;
}
NativeScriptBooleanResult RealtimeScriptHost::HasCharGotWeapon(const NativeScriptPedWeaponRequest& request) {
    NativeScriptBooleanResult result;
    if (!m_Initialized || !ResolvePed(request.Ped) || request.Weapon < 0 ||
        std::size_t(request.Weapon) >= m_PlayerWeapons.size()) {
        result.Result = Error("character-weapon query requires live player and source weapon ID");
        return result;
    }
    result.Value = m_PlayerWeapons[std::size_t(request.Weapon)];
    RealtimeScriptHostEvent event{.Id=request.Id,.Opcode=0x0491,.Index=request.Ped.Value,
        .Reference=result.Value ? 1 : 0,.StateArgument=request.Weapon};
    if (auto old = Replay(event)) { result.Result = *old; return result; }
    Commit(event);
    result.Result = Ready();
    return result;
}
NativeScriptServiceResult RealtimeScriptHost::RequestModel(const NativeScriptModelRequest& request) {
    if (!m_Initialized) return Error("model request requires initialized host");
    int model = request.Model;
    std::string name, texture;
    if (model < 0) {
        const std::string_view used(request.UsedObjectName.data(), strnlen(request.UsedObjectName.data(), request.UsedObjectName.size()));
        if (used.empty() || !StreamPager_KnownModelName(used, &model)) return Error("requested used-object is absent from pager IDE census");
    }
    if (!StreamPager_KnownModelIdentity(model, name, texture)) return Error("requested model has no complete pager IDE identity");
    RealtimeScriptHostEvent event{.Id=request.Id,.Opcode=0x0247,.Index=model,.ModelName=request.UsedObjectName};
    if (auto old = Replay(event)) return *old;
    if (m_ScriptModels.contains(model)) { Commit(event); return Ready(); }
    if (m_PendingModel) {
        if (m_PendingModel->Id != request.Id || m_PendingModel->Model != model) return Error("another script model request is pending");
        return Pending("script model parser request is pending");
    }
    m_PendingModel = PendingScriptModel{request.Id, model, std::move(name), std::move(texture),
        m_CarGenerators.FindModel(model) != nullptr};
    return Pending("script model parser request was submitted");
}
NativeScriptBooleanResult RealtimeScriptHost::HasModelLoaded(const NativeScriptModelRequest& request) {
    NativeScriptBooleanResult result;
    if (!m_Initialized) { result.Result=Error("model-loaded query requires initialized host"); return result; }
    int model=request.Model;
    if (model<0) {
        const std::string_view used(request.UsedObjectName.data(),strnlen(request.UsedObjectName.data(),request.UsedObjectName.size()));
        if(used.empty()||!StreamPager_KnownModelName(used,&model)){result.Result=Error("model-loaded used-object absent from pager IDE census");return result;}
    }
    result.Value=m_ScriptModels.contains(model);
    RealtimeScriptHostEvent event{.Id=request.Id,.Opcode=0x0248,.Index=model,.StateArgument=result.Value?1:0,.ModelName=request.UsedObjectName};
    if(auto old=Replay(event)){result.Result=*old;return result;}
    Commit(event);result.Result=Ready();return result;
}
NativeScriptServiceResult RealtimeScriptHost::MarkModelNoLongerNeeded(const NativeScriptModelRequest& request) {
    if(!m_Initialized)return Error("model release requires initialized host");
    int model=request.Model;
    if(model<0){const std::string_view used(request.UsedObjectName.data(),strnlen(request.UsedObjectName.data(),request.UsedObjectName.size()));if(used.empty()||!StreamPager_KnownModelName(used,&model))return Error("released used-object absent from pager IDE census");}
    RealtimeScriptHostEvent event{.Id=request.Id,.Opcode=0x0249,.Index=model,.ModelName=request.UsedObjectName};
    if(auto old=Replay(event))return *old;
    m_ScriptModels.erase(model);m_ScriptModelCollisions.erase(model);Commit(event);return Ready();
}
NativeScriptServiceResult RealtimeScriptHost::LoadSpecialCharacter(const NativeScriptSpecialModelRequest& request) {
    if(!m_Initialized||request.Slot<1||request.Slot>10)return Error("special-character slot is invalid");
    std::string name(request.Name.data(),strnlen(request.Name.data(),request.Name.size()));
    if(name.empty())return Error("special-character name is empty");
    std::ranges::transform(name,name.begin(),[](unsigned char c){return char(std::tolower(c));});
    const int model=289+request.Slot;
    RealtimeScriptHostEvent event{.Id=request.Id,.Opcode=0x023C,.Index=model,.Name=request.Name};
    if(auto old=Replay(event))return *old;
    if(m_ScriptModels.contains(model)){Commit(event);return Ready();}
    if(m_PendingModel){if(m_PendingModel->Id!=request.Id||m_PendingModel->Model!=model)return Error("another script model request is pending");return Pending("special-character parser request pending");}
    m_PendingModel=PendingScriptModel{request.Id,model,name,name,false};
    return Pending("special-character parser request submitted");
}
NativeScriptBooleanResult RealtimeScriptHost::HasSpecialCharacterLoaded(const NativeScriptSpecialModelRequest& request) {
    NativeScriptBooleanResult result;
    if(request.Slot<1||request.Slot>10){result.Result=Error("special-character slot is invalid");return result;}
    result.Value=m_ScriptModels.contains(289+request.Slot);
    RealtimeScriptHostEvent event{.Id=request.Id,.Opcode=0x023D,.Index=request.Slot,.StateArgument=result.Value?1:0};
    if(auto old=Replay(event)){result.Result=*old;return result;}
    Commit(event);result.Result=Ready();return result;
}
NativeScriptServiceResult RealtimeScriptHost::UnloadSpecialCharacter(const NativeScriptSpecialModelRequest& request) {
    if (!m_Initialized || request.Slot < 1 || request.Slot > 10)
        return Error("special-character unload slot is invalid");
    const int model = 289 + request.Slot;
    RealtimeScriptHostEvent event{.Id=request.Id, .Opcode=0x0296, .Index=model};
    if (auto old = Replay(event)) return *old;
    m_ScriptModels.erase(model);
    m_ScriptModelCollisions.erase(model);
    Commit(event);
    return Ready();
}
NativeScriptServiceResult RealtimeScriptHost::RequestCarRecording(const NativeScriptCarRecordingRequest& request) {
    RealtimeScriptHostEvent event{.Id=request.Id,.Opcode=0x07C0,.Index=request.Recording};
    if(auto old=Replay(event))return *old;
    auto result=m_CarRecordings.Request(request.Recording);
    if(result.Status==NativeScriptServiceStatus::Ready)Commit(event);
    return result;
}
NativeScriptBooleanResult RealtimeScriptHost::HasCarRecordingLoaded(const NativeScriptCarRecordingRequest& request) {
    NativeScriptBooleanResult result;
    result.Value=m_CarRecordings.IsLoaded(request.Recording);
    RealtimeScriptHostEvent event{.Id=request.Id,.Opcode=0x07C1,.Index=request.Recording,.StateArgument=result.Value?1:0};
    if(auto old=Replay(event)){result.Result=*old;return result;}
    Commit(event);result.Result=Ready();return result;
}
NativeScriptServiceResult RealtimeScriptHost::PreloadBeatTrack(const NativeScriptBeatTrackRequest& request) {
    RealtimeScriptHostEvent event{.Id=request.Id, .Opcode=0x0952, .Index=request.Track};
    if (auto old = Replay(event)) return *old;
    auto result = m_BeatTrack.Preload(request.Track);
    if (result.Status == NativeScriptServiceStatus::Ready) Commit(event);
    return result;
}
NativeScriptServiceResult RealtimeScriptHost::PlayBeatTrack(const NativeScriptRequestId& id) {
    RealtimeScriptHostEvent event{.Id=id, .Opcode=0x0954};
    if (auto old = Replay(event)) return *old;
    auto result = m_BeatTrack.Play();
    if (result.Status == NativeScriptServiceStatus::Ready) Commit(event);
    return result;
}
NativeScriptServiceResult RealtimeScriptHost::StopBeatTrack(const NativeScriptRequestId& id) {
    RealtimeScriptHostEvent event{.Id=id, .Opcode=0x0955};
    if (auto old = Replay(event)) return *old;
    auto result = m_BeatTrack.Stop();
    if (result.Status == NativeScriptServiceStatus::Ready) Commit(event);
    return result;
}
NativeScriptServiceResult RealtimeScriptHost::BeginSkippableCutscene(
    const NativeScriptSkipCutsceneRequest& request) {
    RealtimeScriptHostEvent event{.Id=request.Id, .Opcode=0x0707,
        .Index=request.Target};
    if (auto old = Replay(event)) return *old;
    m_CutsceneSkipTarget = request.Target;
    Commit(event);
    return Ready();
}
NativeScriptServiceResult RealtimeScriptHost::EndSkippableCutscene(const NativeScriptRequestId& id) {
    RealtimeScriptHostEvent event{.Id=id, .Opcode=0x0701};
    if (auto old = Replay(event)) return *old;
    m_CutsceneSkipTarget.reset();
    Commit(event);
    return Ready();
}
NativeScriptIntegerResult RealtimeScriptHost::GetBeatTrackStatus(const NativeScriptRequestId& id) {
    NativeScriptIntegerResult result;
    result.Value = m_BeatTrack.State().Status;
    RealtimeScriptHostEvent event{.Id=id, .Opcode=0x0953, .StateArgument=result.Value};
    if (auto old = Replay(event)) { result.Result = *old; return result; }
    Commit(event);
    result.Result = Ready();
    return result;
}
NativeScriptBooleanResult RealtimeScriptHost::AreSubtitlesEnabled(const NativeScriptRequestId& id) {
    NativeScriptBooleanResult result;
    result.Value = true; // CMenuManager::InitialiseChangedLanguageSettings default
    RealtimeScriptHostEvent event{.Id=id, .Opcode=0x09C8, .StateArgument=1};
    if (auto old = Replay(event)) { result.Result = *old; return result; }
    Commit(event);
    result.Result = Ready();
    return result;
}
NativeScriptServiceResult RealtimeScriptHost::SetDensityMultiplier(const NativeScriptDensityRequest& request) {
    if (!std::isfinite(request.Multiplier) || request.Multiplier < 0.0f)
        return Error("density multiplier is invalid");
    RealtimeScriptHostEvent event{.Id=request.Id, .Opcode=std::uint16_t(request.Cars ? 0x01EB : 0x03DE),
        .Arguments={request.Multiplier}};
    if (auto old = Replay(event)) return *old;
    (request.Cars ? m_CarDensityMultiplier : m_PedDensityMultiplier) = request.Multiplier;
    Commit(event);
    return Ready();
}
NativeScriptServiceResult RealtimeScriptHost::SetRandomTrains(const NativeScriptBooleanRequest& request) {
    RealtimeScriptHostEvent event{.Id=request.Id, .Opcode=0x06D7, .StateArgument=request.Value ? 1 : 0};
    if (auto old = Replay(event)) return *old;
    m_RandomTrains = request.Value;
    Commit(event);
    return Ready();
}
NativeScriptReferenceResult<NativeScriptVehicleRef> RealtimeScriptHost::CreateVehicle(
    const NativeScriptVehicleCreateRequest& request) {
    NativeScriptReferenceResult<NativeScriptVehicleRef> result;
    if (!m_Initialized || !std::isfinite(request.Position.X) || !std::isfinite(request.Position.Y) ||
        !std::isfinite(request.Position.Z)) {
        result.Result = Error("script vehicle request is invalid");
        return result;
    }
    std::string name, texture;
    const auto* definition = m_CarGenerators.FindModel(request.ModelId);
    if (!definition || !StreamPager_KnownModelIdentity(request.ModelId, name, texture) ||
        (!m_ScriptModels.contains(request.ModelId) && request.ModelId != 400)) {
        result.Result = Unsupported("script vehicle model is not source-resident");
        return result;
    }
    auto collisionOwner = m_ScriptModelCollisions.contains(request.ModelId) ?
        m_ScriptModelCollisions.at(request.ModelId) : std::shared_ptr<const NativeCollisionModel>{};
    if (!collisionOwner) {
        const auto collision = m_CollisionContext->Assets.LookupModel(name);
        if (collision.Status == NativeCollisionModelStatus::Ready) collisionOwner = collision.Model;
    }
    if (!collisionOwner && request.ModelId == 400) {
        const auto collision = m_CollisionContext->Assets.LookupModel("landstal_col");
        if (collision.Status == NativeCollisionModelStatus::Ready) collisionOwner = collision.Model;
    }
    if (!collisionOwner) {
        result.Result = Unsupported("script vehicle model collision is unavailable");
        return result;
    }
    RealtimeScriptHostEvent event{.Id=request.Id, .Opcode=0x00A5,
        .Arguments={request.Position.X, request.Position.Y, request.Position.Z}, .Index=request.ModelId};
    if (auto old = Replay(event)) {
        result.Result = *old;
        if (old->Status == NativeScriptServiceStatus::Ready) {
            const auto previous = std::ranges::find(m_Events, request.Id, &RealtimeScriptHostEvent::Id);
            if (previous == m_Events.end()) result.Result = Error("script vehicle replay event is missing");
            else result.Reference = {previous->Reference};
        }
        return result;
    }
    NativeVehicleState state;
    state.ModelId = request.ModelId;
    state.Type = definition->Type;
    state.SubType = std::int32_t(definition->Type);
    state.Status = NativeVehicleStatus::Physics;
    state.CreatedBy = NativeVehicleCreatedBy::Mission;
    state.MissionCleanupRegistered = true;
    state.InWorld = true;
    state.Matrix.Position = {request.Position.X, request.Position.Y, request.Position.Z};
    state.Collision = collisionOwner;
    state.ModelCollision = std::make_shared<const NativeVehicleModelCollision>(
        NativeVehicleModelCollision{request.ModelId, collisionOwner});
    const auto created = m_Vehicles.Allocate({NativeVehicleProducer::NativeScm, request.Id, -1, std::move(state)});
    result.Result = created.Result;
    result.Reference = {created.Reference.Value};
    if (result.Result.Status == NativeScriptServiceStatus::Ready) {
        event.Reference = result.Reference.Value;
        Commit(event);
    }
    return result;
}
NativeScriptReferenceResult<NativeScriptVehicleRef> RealtimeScriptHost::CreateMissionTrain(
    const NativeScriptTrainCreateRequest& request) {
    NativeScriptReferenceResult<NativeScriptVehicleRef> result;
    RealtimeScriptHostEvent event{.Id=request.Id, .Opcode=0x06D8,
        .Arguments={request.Position.X, request.Position.Y, request.Position.Z},
        .Index=request.Type, .StateArgument=request.Clockwise ? 1 : 0};
    if (auto old = Replay(event)) {
        result.Result = *old;
        if (old->Status == NativeScriptServiceStatus::Ready) {
            const auto previous = std::ranges::find(m_Events, request.Id, &RealtimeScriptHostEvent::Id);
            if (previous == m_Events.end()) result.Result = Error("mission train replay event is missing");
            else result.Reference = {previous->Reference};
        }
        return result;
    }
    result.Result = m_ScriptTrains.Create(request.Type, request.Position,
        request.Clockwise, result.Reference);
    if (result.Result.Status == NativeScriptServiceStatus::Ready) {
        event.Reference = result.Reference.Value;
        Commit(event);
    }
    return result;
}
NativeScriptServiceResult RealtimeScriptHost::SetTrainSpeed(
    const NativeScriptTrainSpeedRequest& request, bool cruise) {
    RealtimeScriptHostEvent event{.Id=request.Id,
        .Opcode=std::uint16_t(cruise ? 0x06DD : 0x06DC),
        .Arguments={request.Speed}, .Reference=request.Train.Value};
    if (auto old = Replay(event)) return *old;
    auto result = m_ScriptTrains.SetSpeed(request.Train, request.Speed, cruise);
    if (result.Status == NativeScriptServiceStatus::Ready) Commit(event);
    return result;
}
NativeScriptServiceResult RealtimeScriptHost::DeleteMissionTrains(const NativeScriptRequestId& id) {
    RealtimeScriptHostEvent event{.Id=id, .Opcode=0x06D9};
    if (auto old = Replay(event)) return *old;
    auto result = m_ScriptTrains.DeleteMissionTrains();
    if (result.Status == NativeScriptServiceStatus::Ready) Commit(event);
    return result;
}
NativeScriptCarModelResult RealtimeScriptHost::GetRandomResidentCarModel(
    const NativeScriptRequestId& id, bool normalOnly) {
    NativeScriptCarModelResult result;
    if (const auto old = std::ranges::find(m_Events, id, &RealtimeScriptHostEvent::Id);
        old != m_Events.end()) {
        if (old->Opcode != 0x09B2 || old->StateArgument != (normalOnly ? 1 : 0)) {
            result.Result = Error("random car model request ID conflict");
            return result;
        }
        result.ModelId = old->GeneratorArguments[0];
        result.VehicleClass = old->GeneratorArguments[1];
        result.Result = {old->Status, {}};
        return result;
    }
    const auto vehicleClass = [](std::string_view name) {
        static constexpr std::array<std::string_view, 12> names{
            "normal", "poorfamily", "richfamily", "executive", "worker", "big",
            "taxi", "moped", "motorbike", "leisureboat", "workerboat", "bicycle"};
        const auto found = std::ranges::find_if(names, [&](std::string_view candidate) {
            return candidate.size() == name.size() && std::ranges::equal(candidate, name,
                [](char a, char b) { return a == char(std::tolower(static_cast<unsigned char>(b))); });
        });
        return found == names.end() ? -1 : std::int32_t(found - names.begin());
    };
    std::vector<std::pair<std::int32_t, std::int32_t>> candidates;
    const auto addCandidate = [&](std::int32_t model) {
        if (std::ranges::find(candidates, model, &std::pair<std::int32_t, std::int32_t>::first) != candidates.end()) return;
        const auto* definition = m_CarGenerators.FindModel(model);
        if (!definition || definition->Type != NativeVehicleType::Automobile) return;
        const auto klass = vehicleClass(definition->ClassName);
        if (klass < 0 || (normalOnly && klass > 6)) return;
        candidates.emplace_back(model, klass);
    };
    for (const auto& [model, scene] : m_ScriptModels) {
        (void)scene;
        addCandidate(model);
    }
    for (std::size_t slot = 0; slot < NativeVehiclePool::Capacity; ++slot) {
        if (const auto* vehicle = m_Vehicles.AtSlot(slot); vehicle && vehicle->State.InWorld)
            addCandidate(vehicle->State.ModelId);
    }
    if (candidates.empty()) {
        std::string name, texture;
        if (!StreamPager_KnownModelIdentity(400, name, texture)) {
            result.Result = Unsupported("no source-resident car model matches query");
            return result;
        }
        if (m_PendingModel && m_PendingModel->Id != id) {
            result.Result = Error("another script model request is pending");
            return result;
        }
        m_PendingModel = PendingScriptModel{id, 400, std::move(name), std::move(texture), true};
        result.Result = {NativeScriptServiceStatus::Pending, "default source car model residency pending"};
        return result;
    }
    const auto draw = m_SourceRng.Reference().NextRand15();
    if (draw.Status != NativeSourceRngStatus::Ready || !draw.Value) {
        result.Result = Error("source RNG is unavailable for car model query");
        return result;
    }
    const auto selected = candidates[std::size_t(*draw.Value) % candidates.size()];
    result.ModelId = selected.first;
    result.VehicleClass = selected.second;
    RealtimeScriptHostEvent event{.Id=id, .Opcode=0x09B2,
        .StateArgument=normalOnly ? 1 : 0,
        .GeneratorArguments={result.ModelId, result.VehicleClass}};
    Commit(event);
    result.Result = Ready();
    return result;
}
NativeScriptServiceResult RealtimeScriptHost::MarkPedNoLongerNeeded(
    const NativeScriptPedQueryRequest& request) {
    RealtimeScriptHostEvent event{.Id=request.Id, .Opcode=0x01C2,
        .Reference=request.Ped.Value};
    if (auto old = Replay(event)) return *old;
    if (request.Ped.Value < 0) { Commit(event); return Ready(); }
    std::string error;
    const auto status = m_ScriptPeds.Release(request.Ped, error);
    if (status == NativeScriptPedStatus::StaleReference) { Commit(event); return Ready(); }
    if (status != NativeScriptPedStatus::Ok) return Error(error);
    Commit(event);
    return Ready();
}
NativeScriptServiceResult RealtimeScriptHost::MarkVehicleNoLongerNeeded(
    const NativeScriptVehicleStateRequest& request) {
    RealtimeScriptHostEvent event{.Id=request.Id, .Opcode=0x01C3,
        .Reference=request.Vehicle.Value};
    if (auto old = Replay(event)) return *old;
    if (request.Vehicle.Value < 0) { Commit(event); return Ready(); }
    if (m_PlayerScriptVehicle && m_PlayerScriptVehicle->Value == request.Vehicle.Value)
        return Unsupported("cannot release the player's current source vehicle");
    const auto* vehicle = m_Vehicles.Resolve({request.Vehicle.Value});
    if (!vehicle || vehicle->Producer != NativeVehicleProducer::NativeScm) {
        Commit(event);
        return Ready();
    }
    auto state = vehicle->State;
    state.InWorld = false;
    std::string error;
    if (!m_Vehicles.Update({request.Vehicle.Value}, state, error) ||
        !m_Vehicles.Release({request.Vehicle.Value}, error)) return Error(error);
    Commit(event);
    return Ready();
}
NativeScriptServiceResult RealtimeScriptHost::DeletePed(const NativeScriptPedQueryRequest& request) {
    RealtimeScriptHostEvent event{.Id=request.Id, .Opcode=0x009B, .Reference=request.Ped.Value};
    if (auto old = Replay(event)) return *old;
    if (request.Ped.Value == PedRef().Value) return Unsupported("source player deletion is outside this mission owner");
    std::string error;
    if (m_ScriptPeds.Release(request.Ped, error) != NativeScriptPedStatus::Ok) return Error(error);
    m_ScriptCarDriveTasks.erase(request.Ped.Value);
    m_ScriptGoStraightTasks.erase(request.Ped.Value);
    Commit(event);
    return Ready();
}
NativeScriptServiceResult RealtimeScriptHost::DeleteVehicle(const NativeScriptVehicleStateRequest& request) {
    RealtimeScriptHostEvent event{.Id=request.Id, .Opcode=0x00A6, .Reference=request.Vehicle.Value};
    if (auto old = Replay(event)) return *old;
    if (m_PlayerScriptVehicle && m_PlayerScriptVehicle->Value == request.Vehicle.Value)
        return Unsupported("cannot delete the player's current source vehicle");
    const auto* vehicle = m_Vehicles.Resolve({request.Vehicle.Value});
    if (!vehicle || vehicle->Producer != NativeVehicleProducer::NativeScm)
        return Error("script vehicle deletion reference is stale");
    if (const auto* occupancy = m_ScriptPeds.Occupancy(request.Vehicle);
        occupancy && (occupancy->Driver.Value >= 0 || std::ranges::any_of(
            occupancy->Passengers, [](NativeScriptPedRef ped) { return ped.Value >= 0; })))
        return Error("script vehicle deletion requires empty occupancy");
    auto state = vehicle->State;
    state.InWorld = false;
    std::string error;
    if (!m_Vehicles.Update({request.Vehicle.Value}, state, error) ||
        !m_Vehicles.Release({request.Vehicle.Value}, error)) return Error(error);
    m_CarRecordings.Stop(request.Vehicle);
    m_ScriptVehicleLights.erase(request.Vehicle.Value);
    m_ScriptVehicleCollision.erase(request.Vehicle.Value);
    Commit(event);
    return Ready();
}
NativeScriptReferenceResult<NativeScriptPedRef> RealtimeScriptHost::CreateRandomDriver(
    const NativeScriptVehicleStateRequest& request) {
    NativeScriptReferenceResult<NativeScriptPedRef> result;
    RealtimeScriptHostEvent event{.Id=request.Id, .Opcode=0x0560,
        .Reference=request.Vehicle.Value};
    if (auto old = Replay(event)) {
        result.Result = *old;
        if (old->Status == NativeScriptServiceStatus::Ready) {
            const auto previous = std::ranges::find(m_Events, request.Id, &RealtimeScriptHostEvent::Id);
            if (previous == m_Events.end()) result.Result = Error("random driver replay event is missing");
            else result.Reference = {previous->GeneratorArguments[0]};
        }
        return result;
    }
    const auto* vehicle = m_Vehicles.Resolve({request.Vehicle.Value});
    std::string model, texture;
    if (!vehicle || vehicle->Producer != NativeVehicleProducer::NativeScm ||
        !StreamPager_KnownModelIdentity(7, model, texture)) {
        result.Result = Error("random driver requires a live mission car and source ped identity");
        return result;
    }
    const auto& p = vehicle->State.Matrix.Position;
    std::string error;
    result.Result = m_ScriptPeds.CreateDriver(4, 7, request.Vehicle,
        {p[0], p[1], p[2]}, true, result.Reference, error) == NativeScriptPedStatus::Ok
        ? Ready() : Error(error);
    if (result.Result.Status == NativeScriptServiceStatus::Ready) {
        event.GeneratorArguments[0] = result.Reference.Value;
        Commit(event);
    }
    return result;
}
NativeScriptServiceResult RealtimeScriptHost::AssignCarDriveTask(
    const NativeScriptCarDriveTaskRequest& request) {
    const auto* ped = m_ScriptPeds.Resolve(request.Ped);
    const auto* vehicle = m_Vehicles.Resolve({request.Vehicle.Value});
    if (!ped || !ped->Driver || ped->Vehicle.Value != request.Vehicle.Value || !vehicle ||
        vehicle->Producer != NativeVehicleProducer::NativeScm ||
        !std::isfinite(request.Target.X) || !std::isfinite(request.Target.Y) ||
        !std::isfinite(request.Target.Z) || !std::isfinite(request.Speed) || request.Speed < 0.0f)
        return Error("car-drive task request is invalid");
    RealtimeScriptHostEvent event{.Id=request.Id, .Opcode=0x05D1,
        .Arguments={request.Target.X, request.Target.Y, request.Target.Z, request.Speed},
        .Index=request.Ped.Value, .Reference=request.Vehicle.Value,
        .GeneratorArguments={request.DriveStyle, request.ModelId, request.DrivingStyle}};
    if (auto old = Replay(event)) return *old;
    m_ScriptCarDriveTasks[request.Ped.Value] = request;
    Commit(event);
    return Ready();
}
NativeScriptServiceResult RealtimeScriptHost::AssignGoStraightTask(
    const NativeScriptGoStraightTaskRequest& request) {
    const auto* ped = m_ScriptPeds.Resolve(request.Ped);
    const bool player = request.Ped.Value == PedRef().Value && ResolvePed(request.Ped);
    if ((!player && (!ped || ped->InVehicle)) || !Finite(request.Target) || request.MoveState < 0 ||
        request.MoveState > 7 || request.TimeMs < -2)
        return Error("go-straight task request is invalid");
    RealtimeScriptHostEvent event{.Id=request.Id, .Opcode=0x05D3,
        .Arguments={request.Target.X, request.Target.Y, request.Target.Z},
        .Index=request.Ped.Value, .GeneratorArguments={request.MoveState, request.TimeMs}};
    if (auto old = Replay(event)) return *old;
    m_ScriptGoStraightTasks[request.Ped.Value] = request;
    Commit(event);
    return Ready();
}
NativeScriptServiceResult RealtimeScriptHost::SetVehicleHeading(const NativeScriptVehicleHeadingRequest& request) {
    if (!std::isfinite(request.Degrees)) return Error("script vehicle heading is invalid");
    RealtimeScriptHostEvent event{.Id=request.Id, .Opcode=0x0175,
        .Arguments={request.Degrees}, .Reference=request.Vehicle.Value};
    if (auto old = Replay(event)) return *old;
    const auto* vehicle = m_Vehicles.Resolve({request.Vehicle.Value});
    if (!vehicle || vehicle->Producer != NativeVehicleProducer::NativeScm)
        return Error("script vehicle reference is stale");
    auto state = vehicle->State;
    const float radians = request.Degrees * 0.01745329251994329577f;
    const float sine = std::sin(radians), cosine = std::cos(radians);
    state.Matrix.Basis = {{{cosine, sine, 0}, {-sine, cosine, 0}, {0, 0, 1}}};
    std::string error;
    if (!m_Vehicles.Update({request.Vehicle.Value}, state, error)) return Error(error);
    Commit(event);
    return Ready();
}
NativeScriptServiceResult RealtimeScriptHost::SetVehicleLights(const NativeScriptVehicleStateRequest& request) {
    if (request.Value < 0 || request.Value > 2) return Error("vehicle light override is invalid");
    RealtimeScriptHostEvent event{.Id=request.Id, .Opcode=0x067F,
        .Reference=request.Vehicle.Value, .StateArgument=request.Value};
    if (auto old = Replay(event)) return *old;
    const auto* vehicle = m_Vehicles.Resolve({request.Vehicle.Value});
    if (!vehicle || vehicle->Producer != NativeVehicleProducer::NativeScm)
        return Error("script vehicle reference is stale");
    m_ScriptVehicleLights[request.Vehicle.Value] = request.Value;
    Commit(event);
    return Ready();
}
NativeScriptServiceResult RealtimeScriptHost::SetVehicleCollision(const NativeScriptVehicleStateRequest& request) {
    if (request.Value != 0 && request.Value != 1) return Error("vehicle collision switch is invalid");
    RealtimeScriptHostEvent event{.Id=request.Id, .Opcode=0x099A,
        .Reference=request.Vehicle.Value, .StateArgument=request.Value};
    if (auto old = Replay(event)) return *old;
    const auto* vehicle = m_Vehicles.Resolve({request.Vehicle.Value});
    if (!vehicle || vehicle->Producer != NativeVehicleProducer::NativeScm)
        return Error("script vehicle reference is stale");
    m_ScriptVehicleCollision[request.Vehicle.Value] = request.Value != 0;
    Commit(event);
    return Ready();
}
NativeScriptServiceResult RealtimeScriptHost::AddScore(const NativeScriptScoreRequest& request) {
    if (request.PlayerIndex != 0 || !ResolvePed(PedRef())) return Error("score update requires live player0");
    const auto next = std::int64_t(m_PlayerInfo.Money) + request.Amount;
    if (next < std::numeric_limits<std::int32_t>::min() || next > std::numeric_limits<std::int32_t>::max()) {
        return Error("score update overflows source money");
    }
    RealtimeScriptHostEvent event{.Id=request.Id, .Opcode=0x0109,
        .Index=request.PlayerIndex, .StateArgument=request.Amount};
    if (auto old = Replay(event)) return *old;
    m_PlayerInfo.Money = std::int32_t(next);
    Commit(event);
    return Ready();
}
NativeScriptServiceResult RealtimeScriptHost::WarpPedIntoVehiclePassenger(
    const NativeScriptPedVehicleRequest& request) {
    RealtimeScriptHostEvent event{.Id=request.Id, .Opcode=0x0430,
        .Index=request.Ped.Value, .Reference=request.Vehicle.Value, .StateArgument=request.Seat};
    if (auto old = Replay(event)) return *old;
    if (request.Ped.Value != PedRef().Value || !ResolvePed(request.Ped))
        return Error("passenger warp requires the live source player ped");
    const auto* vehicle = m_Vehicles.Resolve({request.Vehicle.Value});
    if (!vehicle || vehicle->Producer != NativeVehicleProducer::NativeScm)
        return Error("passenger warp vehicle reference is stale");
    const auto& p = vehicle->State.Matrix.Position;
    std::string error;
    if (m_ScriptPeds.WarpPassenger(request.Ped, request.Vehicle, request.Seat,
        {p[0], p[1], p[2]}, error) != NativeScriptPedStatus::Ok) return Error(error);
    m_PlayerScriptVehicle = request.Vehicle;
    m_PlayerScriptSeat = request.Seat;
    m_ScriptPlayerPosition = {p[0], p[1], p[2]};
    Commit(event);
    return Ready();
}
NativeScriptServiceResult RealtimeScriptHost::TaskLeaveVehicleImmediately(
    const NativeScriptPedVehicleRequest& request) {
    RealtimeScriptHostEvent event{.Id=request.Id, .Opcode=0x0622,
        .Index=request.Ped.Value, .Reference=request.Vehicle.Value};
    if (auto old = Replay(event)) return *old;
    const auto* vehicle = m_Vehicles.Resolve({request.Vehicle.Value});
    if (!vehicle || vehicle->Producer != NativeVehicleProducer::NativeScm)
        return Error("leave-car task vehicle reference is stale");
    std::string error;
    if (request.Ped.Value == PedRef().Value) {
        if (!m_PlayerScriptVehicle || m_PlayerScriptVehicle->Value != request.Vehicle.Value)
            return Error("source player is not in the supplied vehicle");
        if (m_ScriptPeds.LeaveVehicle(request.Ped, request.Vehicle, error) != NativeScriptPedStatus::Ok)
            return Error(error);
        const auto& p = vehicle->State.Matrix.Position;
        m_ScriptPlayerPosition = {p[0], p[1], p[2]};
        m_PlayerScriptVehicle.reset();
        m_PlayerScriptSeat = -1;
    } else if (m_ScriptPeds.LeaveVehicle(request.Ped, request.Vehicle, error) != NativeScriptPedStatus::Ok) {
        return Error(error);
    }
    Commit(event);
    return Ready();
}
NativeScriptReferenceResult<NativeScriptPedRef> RealtimeScriptHost::CreatePedInsideVehicle(
    const NativeScriptCreatePedInVehicleRequest& request) {
    NativeScriptReferenceResult<NativeScriptPedRef> result;
    RealtimeScriptHostEvent event{.Id=request.Id, .Opcode=std::uint16_t(request.Seat < 0 ? 0x0129 : 0x01C8),
        .Index=request.PedType, .Reference=request.Vehicle.Value, .StateArgument=request.ModelId,
        .GeneratorArguments={request.Seat}};
    if (auto old = Replay(event)) {
        result.Result = *old;
        if (old->Status == NativeScriptServiceStatus::Ready) {
            const auto previous = std::ranges::find(m_Events, request.Id, &RealtimeScriptHostEvent::Id);
            if (previous == m_Events.end()) result.Result = Error("script ped replay event is missing");
            else result.Reference = {previous->GeneratorArguments[1]};
        }
        return result;
    }
    const auto* vehicle = m_Vehicles.Resolve({request.Vehicle.Value});
    if (!vehicle || vehicle->Producer != NativeVehicleProducer::NativeScm) {
        result.Result = Error("script ped vehicle reference is stale");
        return result;
    }
    std::string name, texture;
    if (!StreamPager_KnownModelIdentity(request.ModelId, name, texture) ||
        !m_ScriptModels.contains(request.ModelId)) {
        result.Result = Unsupported("script ped model is not source-resident");
        return result;
    }
    const auto& p = vehicle->State.Matrix.Position;
    std::string error;
    const auto status = request.Seat < 0
        ? m_ScriptPeds.CreateDriver(request.PedType, request.ModelId, request.Vehicle,
            {p[0], p[1], p[2]}, true, result.Reference, error)
        : m_ScriptPeds.CreatePassenger(request.PedType, request.ModelId, request.Vehicle,
            request.Seat, {p[0], p[1], p[2]}, true, result.Reference, error);
    if (status != NativeScriptPedStatus::Ok) {
        result.Result = Error(error);
        return result;
    }
    event.GeneratorArguments[1] = result.Reference.Value;
    Commit(event);
    result.Result = Ready();
    return result;
}
NativeScriptReferenceResult<NativeScriptPedRef> RealtimeScriptHost::CreatePed(
    const NativeScriptPedCreateRequest& request) {
    NativeScriptReferenceResult<NativeScriptPedRef> result;
    RealtimeScriptHostEvent event{.Id=request.Id, .Opcode=0x009A,
        .Arguments={request.Position.X, request.Position.Y, request.Position.Z},
        .Index=request.PedType, .StateArgument=request.ModelId};
    if (auto old = Replay(event)) {
        result.Result = *old;
        if (old->Status == NativeScriptServiceStatus::Ready) {
            const auto previous = std::ranges::find(m_Events, request.Id, &RealtimeScriptHostEvent::Id);
            if (previous == m_Events.end()) result.Result = Error("script ped replay event is missing");
            else result.Reference = {previous->Reference};
        }
        return result;
    }
    std::string name;
    if (!StreamPager_KnownModelId(request.ModelId, &name)) {
        result.Result = Unsupported("script ped model identity is unavailable");
        return result;
    }
    std::string error;
    const auto status = m_ScriptPeds.CreateOnFoot(request.PedType, request.ModelId,
        request.Position, true, result.Reference, error);
    if (status != NativeScriptPedStatus::Ok) {
        result.Result = Error(error);
        return result;
    }
    event.Reference = result.Reference.Value;
    Commit(event);
    result.Result = Ready();
    return result;
}
NativeScriptServiceResult RealtimeScriptHost::SetFixedCameraPosition(
    const NativeScriptFixedCameraRequest& request) {
    if (!Finite(request.Position) || !Finite(request.Offset)) return Error("fixed camera vectors are invalid");
    RealtimeScriptHostEvent event{.Id=request.Id, .Opcode=0x015F,
        .Arguments={request.Position.X, request.Position.Y, request.Position.Z,
            request.Offset.X, request.Offset.Y, request.Offset.Z}};
    if (auto old = Replay(event)) return *old;
    m_ScriptCameraPosition = request.Position;
    m_ScriptCameraOffset = request.Offset;
    m_ScriptCameraFixed = true;
    Commit(event);
    return Ready();
}
NativeScriptServiceResult RealtimeScriptHost::PointCameraAtPoint(
    const NativeScriptPointCameraRequest& request) {
    if (!Finite(request.Position) || request.Position.Z <= -100.0f ||
        request.SwitchType < 0 || request.SwitchType > 2) {
        return Unsupported("point camera requires finite source position and implemented switch type");
    }
    RealtimeScriptHostEvent event{.Id=request.Id, .Opcode=0x0160,
        .Arguments={request.Position.X, request.Position.Y, request.Position.Z},
        .StateArgument=request.SwitchType};
    if (auto old = Replay(event)) return *old;
    m_ScriptCameraTarget = request.Position;
    m_ScriptCameraSwitchType = request.SwitchType;
    Commit(event);
    return Ready();
}
NativeScriptServiceResult RealtimeScriptHost::SetWidescreen(const NativeScriptBooleanRequest& request) {
    RealtimeScriptHostEvent event{.Id=request.Id, .Opcode=0x02A3,
        .StateArgument=request.Value ? 1 : 0};
    if (auto old = Replay(event)) return *old;
    m_ScriptWidescreen = request.Value;
    Commit(event);
    return Ready();
}
NativeScriptVehicleResult RealtimeScriptHost::GetPedVehicleNoSave(const NativeScriptPedQueryRequest& request) {
    NativeScriptVehicleResult result;
    const bool player = request.Ped.Value == PedRef().Value && ResolvePed(request.Ped);
    if (!player && !m_ScriptPeds.Resolve(request.Ped)) {
        result.Result = Error("ped-vehicle query reference is stale");
        return result;
    }
    result.Reference = player && m_PlayerScriptVehicle ? *m_PlayerScriptVehicle
        : m_ScriptPeds.VehicleForPed(request.Ped);
    if (result.Reference.Value < 0 || !m_Vehicles.Resolve({result.Reference.Value})) {
        result.Result = Error("ped is not in a source-owned vehicle");
        return result;
    }
    RealtimeScriptHostEvent event{.Id=request.Id, .Opcode=0x03C0,
        .Index=request.Ped.Value, .Reference=result.Reference.Value};
    if (auto old = Replay(event)) {
        result.Result = *old;
        if (old->Status == NativeScriptServiceStatus::Ready) {
            const auto previous = std::ranges::find(m_Events, request.Id, &RealtimeScriptHostEvent::Id);
            if (previous == m_Events.end()) result.Result = Error("ped-vehicle replay event is missing");
            else result.Reference = {previous->Reference};
        }
        return result;
    }
    Commit(event);
    result.Result = Ready();
    return result;
}
NativeScriptVehicleStatsResult RealtimeScriptHost::GetWheelieStats(const NativeScriptVehicleStateRequest& request) {
    NativeScriptVehicleStatsResult result;
    if (request.Vehicle.Value != 0 || !ResolvePed(PedRef())) {
        result.Result = Error("wheelie query requires live player0");
        return result;
    }
    // This source-owned script vehicle has no wheelie/stoppie/two-wheel physical
    // accumulator yet and has not moved; the exact initial CAutomobile values are zero.
    RealtimeScriptHostEvent event{.Id=request.Id, .Opcode=0x04FC,
        .Index=request.Vehicle.Value};
    if (auto old = Replay(event)) {
        result.Result = *old;
        return result;
    }
    Commit(event);
    result.Result = Ready();
    return result;
}
NativeScriptBooleanResult RealtimeScriptHost::IsVehicleInAirProper(const NativeScriptVehicleStateRequest& request) {
    NativeScriptBooleanResult result;
    const auto* vehicle = m_Vehicles.Resolve({request.Vehicle.Value});
    if (!vehicle || vehicle->Producer != NativeVehicleProducer::NativeScm) {
        result.Result = Error("in-air query vehicle reference is stale");
        return result;
    }
    // The bounded script vehicle has no suspension/airborne update owner yet;
    // its source construction state is stationary and not properly airborne.
    result.Value = false;
    RealtimeScriptHostEvent event{.Id=request.Id, .Opcode=0x01F3,
        .Reference=request.Vehicle.Value, .StateArgument=0};
    if (auto old = Replay(event)) {
        result.Result = *old;
        return result;
    }
    Commit(event);
    result.Result = Ready();
    return result;
}
NativeScriptBooleanResult RealtimeScriptHost::IsVehicleDead(const NativeScriptVehicleStateRequest& request) {
    NativeScriptBooleanResult result;
    const auto* vehicle = m_Vehicles.Resolve({request.Vehicle.Value});
    result.Value = !vehicle || vehicle->State.Status == NativeVehicleStatus::Wrecked;
    RealtimeScriptHostEvent event{.Id=request.Id, .Opcode=0x0119,
        .Reference=request.Vehicle.Value, .StateArgument=result.Value ? 1 : 0};
    if (auto old = Replay(event)) { result.Result = *old; return result; }
    Commit(event);
    result.Result = Ready();
    return result;
}
NativeScriptBooleanResult RealtimeScriptHost::IsVehiclePlaybackActive(
    const NativeScriptVehicleStateRequest& request) {
    NativeScriptBooleanResult result;
    const auto* vehicle = m_Vehicles.Resolve({request.Vehicle.Value});
    if (!vehicle || vehicle->Producer != NativeVehicleProducer::NativeScm) {
        result.Result = Error("car-recording query vehicle reference is stale");
        return result;
    }
    result.Value = m_CarRecordings.IsPlaybackActive(request.Vehicle);
    RealtimeScriptHostEvent event{.Id=request.Id, .Opcode=0x060E,
        .Reference=request.Vehicle.Value, .StateArgument=0};
    if (auto old = Replay(event)) { result.Result = *old; return result; }
    Commit(event);
    result.Result = Ready();
    return result;
}
NativeScriptServiceResult RealtimeScriptHost::StartVehiclePlayback(
    const NativeScriptVehicleStateRequest& request) {
    const auto* vehicle = m_Vehicles.Resolve({request.Vehicle.Value});
    if (!vehicle || vehicle->Producer != NativeVehicleProducer::NativeScm)
        return Error("car-recording playback vehicle reference is stale");
    RealtimeScriptHostEvent event{.Id=request.Id, .Opcode=0x05EB,
        .Index=request.Value, .Reference=request.Vehicle.Value};
    if (auto old = Replay(event)) return *old;
    auto result = m_CarRecordings.Start(request.Vehicle, request.Value);
    if (result.Status == NativeScriptServiceStatus::Ready) Commit(event);
    return result;
}
bool RealtimeScriptHost::FulfillPendingModel(const NativeScriptRequestId& id,
    std::shared_ptr<const WorldShotScene> scene,
    std::shared_ptr<const NativeCollisionModel> collision, std::string& error) {
    if (!m_PendingModel || m_PendingModel->Id != id || !scene || !scene->stats.atomics || !scene->stats.triangles) {
        error = "script model completion does not match pending request";
        return false;
    }
    m_ScriptModels[m_PendingModel->Model] = std::move(scene);
    if (collision) m_ScriptModelCollisions[m_PendingModel->Model] = std::move(collision);
    m_PendingModel.reset();
    error.clear();
    return true;
}
NativeScriptBooleanResult RealtimeScriptHost::QueryPlayerState(const NativeScriptPlayerStateQueryRequest& request) {
    NativeScriptBooleanResult result;
    if (!m_Initialized) { result.Result = Error("player-state query requires initialized host"); return result; }
    if (request.Kind == NativeScriptPlayerStateQueryKind::ControlEnabled ||
        request.Kind == NativeScriptPlayerStateQueryKind::CanStartMission) {
        if (request.Reference != 0) { result.Result = Error("player-control query requires player0"); return result; }
        result.Value = request.Kind == NativeScriptPlayerStateQueryKind::ControlEnabled
            ? m_PlayerControlEnabled
            : ResolvePed(PedRef()) && m_PlayerControlEnabled &&
                (m_Gameplay.State().PlayerOnFootTask || m_Gameplay.State().InVehicle || m_PlayerScriptVehicle.has_value());
    } else {
        if (!ResolvePed({request.Reference})) { result.Result = Error("vehicle-class query requires live source player ped"); return result; }
        // This bounded slice has one ordinary model400 Automobile only;
        // train/flying/boat families remain later P5 owners.
        const auto* scriptVehicle = m_PlayerScriptVehicle ? m_Vehicles.Resolve({m_PlayerScriptVehicle->Value}) : nullptr;
        const bool inVehicle = m_Gameplay.State().InVehicle || scriptVehicle;
        result.Value = request.Kind == NativeScriptPlayerStateQueryKind::InAnyVehicle
            ? inVehicle
            : request.Kind == NativeScriptPlayerStateQueryKind::InVehicleModel && inVehicle &&
                (scriptVehicle ? scriptVehicle->State.ModelId == request.ModelId : request.ModelId == 400);
    }
    const auto opcode = request.Kind == NativeScriptPlayerStateQueryKind::InTrain ? 0x09AE :
        request.Kind == NativeScriptPlayerStateQueryKind::InFlyingVehicle ? 0x04C8 :
        request.Kind == NativeScriptPlayerStateQueryKind::InBoat ? 0x04A7 :
        request.Kind == NativeScriptPlayerStateQueryKind::InVehicleModel ? 0x00DD :
        request.Kind == NativeScriptPlayerStateQueryKind::InAnyVehicle ? 0x00DF :
        request.Kind == NativeScriptPlayerStateQueryKind::CanStartMission ? 0x03EE : 0x09E7;
    RealtimeScriptHostEvent event{.Id=request.Id,.Opcode=std::uint16_t(opcode),.Index=request.Reference,
        .Reference=request.ModelId,.StateArgument=result.Value ? 1 : 0};
    if (auto old = Replay(event)) {
        result.Result = *old;
        if (result.Result.Status == NativeScriptServiceStatus::Ready) {
            const auto previous = std::ranges::find(m_Events, request.Id, &RealtimeScriptHostEvent::Id);
            if (previous == m_Events.end()) result.Result = Error("player-state replay event is missing");
            else result.Value = previous->StateArgument != 0;
        }
        return result;
    }
    Commit(event);
    result.Result = Ready();
    return result;
}
NativeScriptServiceResult RealtimeScriptHost::ForceWeatherNow(const NativeScriptWeatherRequest& request) {
    if (!m_Initialized) return Error("weather service requires initialized host");
    RealtimeScriptHostEvent event{.Id=request.Id,.Opcode=0x01B6,.Index=request.Weather};
    if (auto old = Replay(event)) return *old;
    auto result = m_Weather.ForceNow(request.Weather);
    event.Status = result.Status;
    Commit(event);
    return result;
}
NativeScriptServiceResult RealtimeScriptHost::ReleaseWeather(const NativeScriptRequestId& id) {
    if (!m_Initialized) return Error("weather release requires initialized host");
    RealtimeScriptHostEvent event{.Id=id,.Opcode=0x01B7};
    if (auto old = Replay(event)) return *old;
    auto result = m_Weather.Release();
    event.Status = result.Status;
    Commit(event);
    return result;
}
NativeScriptServiceResult RealtimeScriptHost::GivePlayerClothes(const NativeScriptClothesRequest& request) {
    if (!m_Initialized || request.PlayerIndex != 0 || !ResolvePed(PedRef())) {
        return Error("clothes service requires live source player0");
    }
    RealtimeScriptHostEvent event{.Id=request.Id,.Opcode=0x087B,.Index=request.PlayerIndex,
        .StateArgument=request.BodyPart,.Name={},.ModelName={}};
    std::copy_n(request.Texture.data(), event.Name.size(), event.Name.data());
    event.ModelName = {};
    std::copy(request.Model.begin(), request.Model.end(), event.ModelName.begin());
    if (auto old = Replay(event)) return *old;
    auto result = m_Clothes.Give(request);
    event.Status = result.Status;
    Commit(event);
    return result;
}
NativeScriptServiceResult RealtimeScriptHost::BuildPlayerModel(const NativeScriptPlayerLookupRequest& request) {
    if (!m_Initialized || request.PlayerIndex != 0 || !ResolvePed(PedRef())) {
        return Error("player-model build requires live source player0");
    }
    RealtimeScriptHostEvent event{.Id=request.Id,.Opcode=0x070D,.Index=request.PlayerIndex};
    if (auto old = Replay(event)) return *old;
    auto result = m_Clothes.Build(request.PlayerIndex);
    event.Status = result.Status;
    Commit(event);
    return result;
}
NativeScriptServiceResult RealtimeScriptHost::StoreClothesState(const NativeScriptRequestId& id) {
    if (!m_Initialized || !ResolvePed(PedRef())) return Error("clothes store requires live source player");
    RealtimeScriptHostEvent event{.Id=id,.Opcode=0x0793};
    if (auto old = Replay(event)) return *old;
    auto result = m_Clothes.Store();
    event.Status = result.Status;
    Commit(event);
    return result;
}
NativeScriptServiceResult RealtimeScriptHost::SetFadeColour(const NativeScriptFadeColourRequest& request) {
    if (!m_Initialized || request.Red < 0 || request.Red > 255 || request.Green < 0 || request.Green > 255 ||
        request.Blue < 0 || request.Blue > 255) {
        return Error("fade colour requires initialized host and byte channels");
    }
    RealtimeScriptHostEvent event{.Id=request.Id,.Opcode=0x0169,
        .GeneratorArguments={request.Red,request.Green,request.Blue}};
    if (auto old = Replay(event)) return *old;
    m_FadeColour = {std::uint8_t(request.Red),std::uint8_t(request.Green),std::uint8_t(request.Blue)};
    Commit(event);
    return Ready();
}
NativeScriptServiceResult RealtimeScriptHost::SetAreaVisible(const NativeScriptAreaRequest& request) {
    if (!m_Initialized || request.Area < 0 || request.Area > 255) {
        return Error("visible area requires initialized host and source byte value");
    }
    RealtimeScriptHostEvent event{.Id=request.Id,.Opcode=0x04BB,.Index=request.Area};
    if (auto old = Replay(event)) return *old;
    m_PlayerArea = request.Area;
    Commit(event);
    return Ready();
}
NativeScriptServiceResult RealtimeScriptHost::SetPlayerControl(const NativeScriptPlayerControlRequest& request) {
    if (!m_Initialized || request.PlayerIndex != 0 || !ResolvePed(PedRef())) {
        return Error("player-control service requires live source player0");
    }
    RealtimeScriptHostEvent event{.Id=request.Id,.Opcode=0x01B4,.Index=request.PlayerIndex,
        .StateArgument=request.Enabled ? 1 : 0};
    if (auto old = Replay(event)) return *old;
    m_PlayerControlEnabled = request.Enabled;
    Commit(event);
    return Ready();
}
NativeScriptServiceResult RealtimeScriptHost::SetPedHealth(const NativeScriptPedHealthRequest& request) {
    if (!m_Initialized || request.Health < 0) return Error("ped health request is invalid");
    RealtimeScriptHostEvent event{.Id=request.Id, .Opcode=0x0223,
        .Index=request.Ped.Value, .StateArgument=request.Health};
    if (auto old = Replay(event)) return *old;
    if (request.Ped.Value == PedRef().Value) {
        if (!ResolvePed(request.Ped)) return Error("player ped reference is stale");
        m_PlayerHealth = float(request.Health);
    } else {
        std::string error;
        if (m_ScriptPeds.SetHealth(request.Ped, float(request.Health), error) != NativeScriptPedStatus::Ok)
            return Error(error);
    }
    Commit(event);
    return Ready();
}
NativeScriptServiceResult RealtimeScriptHost::RemoveAllPedWeapons(const NativeScriptPedQueryRequest& request) {
    if (!m_Initialized || (!ResolvePed(request.Ped) && !m_ScriptPeds.Resolve(request.Ped)))
        return Error("remove-weapons ped reference is stale");
    RealtimeScriptHostEvent event{.Id=request.Id, .Opcode=0x048F, .Index=request.Ped.Value};
    if (auto old = Replay(event)) return *old;
    if (request.Ped.Value == PedRef().Value) m_PlayerWeapons.fill(false);
    // Script peds are constructed unarmed in this bounded owner.
    Commit(event);
    return Ready();
}
NativeScriptBooleanResult RealtimeScriptHost::IsPedSwimming(const NativeScriptPedQueryRequest& request) {
    NativeScriptBooleanResult result;
    if (!m_Initialized || (!ResolvePed(request.Ped) && !m_ScriptPeds.Resolve(request.Ped))) {
        result.Result = Error("swimming query ped reference is stale");
        return result;
    }
    result.Value = false; // Current bounded actors are grounded or vehicle occupants.
    RealtimeScriptHostEvent event{.Id=request.Id, .Opcode=0x0965,
        .Index=request.Ped.Value, .StateArgument=0};
    if (auto old = Replay(event)) { result.Result = *old; return result; }
    Commit(event);
    result.Result = Ready();
    return result;
}
NativeScriptServiceResult RealtimeScriptHost::SetPlayerNeverTired(const NativeScriptPlayerControlRequest& request) {
    if (!m_Initialized || request.PlayerIndex != 0 || !ResolvePed(PedRef()))
        return Error("never-tired service requires live source player0");
    RealtimeScriptHostEvent event{.Id=request.Id, .Opcode=0x0330,
        .Index=request.PlayerIndex, .StateArgument=request.Enabled ? 1 : 0};
    if (auto old = Replay(event)) return *old;
    m_PlayerNeverTired = request.Enabled;
    Commit(event);
    return Ready();
}
NativeScriptServiceResult RealtimeScriptHost::ShutAllCharsUp(const NativeScriptBooleanRequest& request) {
    RealtimeScriptHostEvent event{.Id=request.Id, .Opcode=0x09F5,
        .StateArgument=request.Value ? 1 : 0};
    if (auto old = Replay(event)) return *old;
    m_AllCharsShutUp = request.Value;
    Commit(event);
    return Ready();
}
NativeScriptPositionResult RealtimeScriptHost::GetPedCoordinates(const NativeScriptPedQueryRequest& request) {
    NativeScriptPositionResult result;
    if (!m_Initialized) {
        result.Result = Error("ped-coordinate query requires initialized host");
        return result;
    }
    if (request.Ped.Value == PedRef().Value && ResolvePed(request.Ped)) {
        result.Value = m_ScriptPlayerPosition.value_or(NativeScriptPosition{
            m_Gameplay.State().PedRoot.X, m_Gameplay.State().PedRoot.Y, m_Gameplay.State().PedRoot.Z});
    } else if (const auto* ped = m_ScriptPeds.Resolve(request.Ped)) {
        result.Value = ped->Position;
        if (ped->InVehicle) {
            if (const auto* vehicle = m_Vehicles.Resolve({ped->Vehicle.Value})) {
                const auto& p = vehicle->State.Matrix.Position;
                result.Value = {p[0], p[1], p[2]};
            }
        }
    } else {
        result.Result = Error("ped-coordinate reference is stale");
        return result;
    }
    RealtimeScriptHostEvent event{.Id=request.Id, .Opcode=0x00A0,
        .Arguments={result.Value.X, result.Value.Y, result.Value.Z}, .Reference=request.Ped.Value};
    if (auto old = Replay(event)) {
        result.Result = *old;
        return result;
    }
    Commit(event);
    result.Result = Ready();
    return result;
}
NativeScriptServiceResult RealtimeScriptHost::RemoveTextureDictionary(const NativeScriptRequestId& id) {
    RealtimeScriptHostEvent event{.Id=id, .Opcode=0x0391};
    if (auto old = Replay(event)) return *old;
    if (m_ScriptTextureRevision == std::numeric_limits<std::uint64_t>::max())
        return Error("script texture revision exhausted");
    m_ScriptTextureDictionary.reset();
    m_ScriptSpriteImages.fill(-1);
    ++m_ScriptTextureRevision;
    Commit(event);
    return Ready();
}
NativeScriptServiceResult RealtimeScriptHost::LoadTextureDictionary(
    const NativeScriptTextureDictionaryRequest& request) {
    const auto name = FixedName(request.Name);
    if (!m_Initialized || name.empty() || name.size() > 15 || !std::all_of(name.begin(), name.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '_';
    })) return Error("invalid script texture dictionary name");
    RealtimeScriptHostEvent event{.Id=request.Id, .Opcode=0x0390};
    std::copy(name.begin(), name.end(), event.ModelName.begin());
    if (auto old = Replay(event)) return *old;
    if (m_ScriptTextureDictionary) {
        if (!EqualNoCase(m_ScriptTextureDictionary->Name, name))
            return Error("another script texture dictionary is resident");
        Commit(event);
        return Ready();
    }
    if (m_PendingTexture) {
        if (m_PendingTexture->Id != request.Id || !EqualNoCase(m_PendingTexture->Name, name))
            return Error("another script texture dictionary request is pending");
        return Pending("script texture parser request pending");
    }
    m_PendingTexture = PendingScriptTexture{request.Id, name};
    return Pending("script texture parser request pending");
}
bool RealtimeScriptHost::FulfillPendingTexture(const NativeScriptRequestId& id,
    std::shared_ptr<const NativeScriptTextureDictionaryPacket> packet, std::string& error) {
    if (!m_PendingTexture || m_PendingTexture->Id != id || !packet || packet->Images.empty() ||
        !EqualNoCase(m_PendingTexture->Name, packet->Name)) {
        error = "script texture completion does not match pending request";
        return false;
    }
    for (std::size_t i = 0; i < packet->Images.size(); ++i) {
        const auto& image = packet->Images[i];
        if (!image.w || !image.h || image.rgba.empty() || !strnlen(image.name, sizeof(image.name))) {
            error = "script texture completion contains invalid image";
            return false;
        }
        for (std::size_t j = 0; j < i; ++j) if (EqualNoCase(image.name, packet->Images[j].name)) {
            error = "script texture completion contains duplicate image";
            return false;
        }
    }
    if (m_ScriptTextureRevision == std::numeric_limits<std::uint64_t>::max()) {
        error = "script texture revision exhausted";
        return false;
    }
    m_ScriptTextureDictionary = std::move(packet);
    m_PendingTexture.reset();
    ++m_ScriptTextureRevision;
    error.clear();
    return true;
}
NativeScriptServiceResult RealtimeScriptHost::LoadSprite(const NativeScriptSpriteRequest& request) {
    const auto name = FixedName(request.Name);
    if (!m_Initialized || request.Slot <= 0 || std::size_t(request.Slot) > m_ScriptSpriteImages.size() ||
        name.empty() || !m_ScriptTextureDictionary) return Error("invalid script sprite request");
    RealtimeScriptHostEvent event{.Id=request.Id, .Opcode=0x038F, .Index=request.Slot};
    std::copy(name.begin(), name.end(), event.ModelName.begin());
    if (auto old = Replay(event)) return *old;
    const auto found = std::find_if(m_ScriptTextureDictionary->Images.begin(),
        m_ScriptTextureDictionary->Images.end(), [&](const auto& image) {
            return EqualNoCase(name, std::string_view(image.name, strnlen(image.name, sizeof(image.name))));
        });
    if (found == m_ScriptTextureDictionary->Images.end()) return Error("script sprite is absent from resident dictionary");
    m_ScriptSpriteImages[std::size_t(request.Slot - 1)] =
        static_cast<std::int32_t>(found - m_ScriptTextureDictionary->Images.begin());
    Commit(event);
    return Ready();
}
NativeScriptServiceResult RealtimeScriptHost::ReportAudioEventAtPosition(
    const NativeScriptAudioEventRequest& request) {
    if (!m_Initialized || !Finite(request.Position) || request.Event < 0 || request.Event > 65535)
        return Error("invalid positional audio event");
    RealtimeScriptHostEvent event{.Id=request.Id, .Opcode=0x097A,
        .Arguments={request.Position.X, request.Position.Y, request.Position.Z}, .Index=request.Event};
    if (auto old = Replay(event)) return *old;
    Commit(event);
    return Ready();
}
NativeScriptServiceResult RealtimeScriptHost::DrawScriptRectangle(
    const NativeScriptRectangleRequest& request) {
    if (!m_Initialized || !std::isfinite(request.X) || !std::isfinite(request.Y) ||
        !std::isfinite(request.Width) || !std::isfinite(request.Height) || request.Width < 0.0f ||
        request.Height < 0.0f || std::ranges::any_of(request.Colour, [](auto value) { return value < 0 || value > 255; }) ||
        (request.TextureSlot >= 0 && (request.TextureSlot == 0 ||
            std::size_t(request.TextureSlot) > m_ScriptSpriteImages.size() ||
            m_ScriptSpriteImages[std::size_t(request.TextureSlot - 1)] < 0)))
        return Error("invalid script rectangle request");
    RealtimeScriptHostEvent event{.Id=request.Id,
        .Opcode=std::uint16_t(request.TextureSlot < 0 ? 0x038E : 0x038D),
        .Arguments={request.X, request.Y, request.Width, request.Height},
        .Index=request.TextureSlot};
    std::copy(request.Colour.begin(), request.Colour.end(), event.GeneratorArguments.begin());
    if (auto old = Replay(event)) return *old;
    if (m_ScriptRectangles.size() >= 128) return Error("script rectangle capacity exceeded");
    m_ScriptRectangles.push_back(request);
    Commit(event);
    return Ready();
}
NativeScriptServiceResult RealtimeScriptHost::LoadMissionText(const NativeScriptMissionTextRequest& request) {
    if (!m_Initialized) return Error("mission text service requires initialized host");
    RealtimeScriptHostEvent event{.Id=request.Id,.Opcode=0x054C,.Name=request.Name};
    if (auto old = Replay(event)) return *old;
    auto result = m_MissionText.Select(request.Name);
    event.Status = result.Status;
    Commit(event);
    return result;
}
NativeScriptServiceResult RealtimeScriptHost::ClearText(const NativeScriptMissionTextRequest& request) {
    if (!m_Initialized) return Error("text removal requires initialized host");
    RealtimeScriptHostEvent event{.Id=request.Id, .Opcode=0x03D5, .Name=request.Name};
    if (auto old = Replay(event)) return *old;
    auto result = m_MissionText.Remove(request.Name);
    if (result.Status == NativeScriptServiceStatus::Ready && m_LastPrint && m_LastPrint->Key == request.Name)
        m_LastPrint.reset();
    event.Status = result.Status;
    Commit(event);
    return result;
}
NativeScriptServiceResult RealtimeScriptHost::UseTextCommands(const NativeScriptTextCommandsRequest& request) {
    if (!m_Initialized) return Error("text-command service requires initialized host");
    RealtimeScriptHostEvent event{.Id=request.Id,.Opcode=0x03F0,.StateArgument=request.Enabled ? 1 : 0};
    if (auto old = Replay(event)) return *old;
    auto result = m_MissionText.SetCommandsEnabled(request.Enabled);
    event.Status = result.Status;
    Commit(event);
    return result;
}
NativeScriptServiceResult RealtimeScriptHost::SetTextDrawBeforeFade(const NativeScriptRequestId& id, bool enabled) {
    if (!m_Initialized) return Error("text fade-order service requires initialized host");
    RealtimeScriptHostEvent event{.Id=id,.Opcode=0x03E0,.StateArgument=enabled ? 1 : 0};
    if (auto old = Replay(event)) return *old;
    auto result = m_MissionText.SetDrawBeforeFade(enabled);
    event.Status = result.Status;
    Commit(event);
    return result;
}
NativeScriptServiceResult RealtimeScriptHost::SetTextFont(const NativeScriptRequestId& id, std::int32_t font) {
    if (!m_Initialized) return Error("text-font service requires initialized host");
    RealtimeScriptHostEvent event{.Id=id,.Opcode=0x0349,.StateArgument=font};
    if (auto old = Replay(event)) return *old;
    auto result = m_MissionText.SetFont(font);
    event.Status = result.Status;
    Commit(event);
    return result;
}
NativeScriptServiceResult RealtimeScriptHost::SetTextStyle(const NativeScriptTextStyleRequest& request) {
    if (!m_Initialized) return Error("text-style service requires initialized host");
    RealtimeScriptHostEvent event{.Id = request.Id, .Opcode = request.Opcode};
    event.Arguments[0] = request.Floats[0];
    event.Arguments[1] = request.Floats[1];
    std::copy(request.Integers.begin(), request.Integers.end(), event.GeneratorArguments.begin());
    if (auto old = Replay(event)) return *old;
    auto result = m_MissionText.SetStyle(request.Opcode, request.Floats, request.Integers);
    event.Status = result.Status;
    Commit(event);
    return result;
}
NativeScriptServiceResult RealtimeScriptHost::DisplayText(const NativeScriptTextDisplayRequest& request) {
    if (!m_Initialized) return Error("text display requires initialized host");
    RealtimeScriptHostEvent event{.Id = request.Id, .Opcode = 0x033E,
        .Arguments = {request.X, request.Y}, .Name = request.Key};
    if (auto old = Replay(event)) return *old;
    auto result = m_MissionText.Display(request.X, request.Y, request.Key);
    event.Status = result.Status;
    Commit(event);
    return result;
}
NativeScriptServiceResult RealtimeScriptHost::PrintNow(const NativeScriptPrintRequest& request) {
    if (!m_Initialized || request.TimeMs < 0 || (request.Flag != 0 && request.Flag != 1) ||
        !m_MissionText.HasActiveKey(request.Key)) return Error("mission print request is invalid");
    RealtimeScriptHostEvent event{.Id=request.Id, .Opcode=0x00BC,
        .Index=request.TimeMs, .StateArgument=request.Flag, .Name=request.Key};
    if (auto old = Replay(event)) return *old;
    m_LastPrint = request;
    Commit(event);
    return Ready();
}
NativeScriptServiceResult RealtimeScriptHost::ApplyCameraCommand(
    const NativeScriptCameraCommandRequest& request) {
    for (const auto value : request.Floats)
        if (!std::isfinite(value)) return Error("script camera command contains nonfinite data");
    RealtimeScriptHostEvent event{.Id=request.Id, .Opcode=request.Opcode,
        .GeneratorArguments={request.Integers[0], request.Integers[1]}};
    std::copy(request.Floats.begin(), request.Floats.end(), event.Arguments.begin());
    if (auto old = Replay(event)) return *old;
    m_ScriptCameraCommands[request.Opcode] = request;
    Commit(event);
    return Ready();
}
NativeScriptServiceResult RealtimeScriptHost::ClearPrints(const NativeScriptRequestId& id) {
    RealtimeScriptHostEvent event{.Id=id, .Opcode=0x00BE};
    if (auto old = Replay(event)) return *old;
    m_LastPrint.reset();
    m_MissionText.ClearDraws();
    Commit(event);
    return Ready();
}
NativeScriptServiceResult RealtimeScriptHost::ClearMissionAudio(
    const NativeScriptRequestId& id, std::int32_t slot) {
    RealtimeScriptHostEvent event{.Id=id, .Opcode=0x040D, .Index=slot};
    if (auto old = Replay(event)) return *old;
    auto result = m_MissionAudio.Clear(slot);
    event.Status = result.Status;
    Commit(event);
    return result;
}
NativeScriptServiceResult RealtimeScriptHost::LoadMissionAudio(
    const NativeScriptMissionAudioRequest& request) {
    RealtimeScriptHostEvent event{.Id=request.Id, .Opcode=0x03CF,
        .Index=request.Slot, .StateArgument=request.AudioId};
    if (auto old = Replay(event)) return *old;
    auto result = m_MissionAudio.Request(request.Slot, request.AudioId);
    event.Status = result.Status;
    Commit(event);
    return result;
}
NativeScriptBooleanResult RealtimeScriptHost::HasMissionAudioLoaded(
    const NativeScriptRequestId& id, std::int32_t slot) {
    NativeScriptBooleanResult result;
    if (slot < 1 || slot > 4) {
        result.Result = Error("mission audio query slot is invalid");
        return result;
    }
    result.Value = m_MissionAudio.Loaded(slot);
    RealtimeScriptHostEvent event{.Id=id, .Opcode=0x03D0, .Index=slot,
        .StateArgument=result.Value ? 1 : 0};
    if (auto old = Replay(event)) {
        result.Result = *old;
        if (old->Status == NativeScriptServiceStatus::Ready) {
            const auto previous = std::ranges::find(m_Events, id, &RealtimeScriptHostEvent::Id);
            if (previous == m_Events.end()) result.Result = Error("mission audio replay event is missing");
            else result.Value = previous->StateArgument != 0;
        }
        return result;
    }
    Commit(event);
    result.Result = Ready();
    return result;
}
NativeScriptServiceResult RealtimeScriptHost::PlayMissionAudio(
    const NativeScriptRequestId& id, std::int32_t slot) {
    RealtimeScriptHostEvent event{.Id=id, .Opcode=0x03D1, .Index=slot};
    if (auto old = Replay(event)) return *old;
    auto result = m_MissionAudio.Play(slot, State().TimeMs);
    event.Status = result.Status;
    Commit(event);
    return result;
}
NativeScriptBooleanResult RealtimeScriptHost::HasMissionAudioFinished(
    const NativeScriptRequestId& id, std::int32_t slot) {
    NativeScriptBooleanResult result;
    if (slot < 1 || slot > 4) {
        result.Result = Error("mission audio finished query slot is invalid");
        return result;
    }
    result.Value = m_MissionAudio.Finished(slot);
    RealtimeScriptHostEvent event{.Id=id, .Opcode=0x03D2, .Index=slot,
        .StateArgument=result.Value ? 1 : 0};
    if (auto old = Replay(event)) {
        result.Result = *old;
        if (old->Status == NativeScriptServiceStatus::Ready) {
            const auto previous = std::ranges::find(m_Events, id, &RealtimeScriptHostEvent::Id);
            if (previous == m_Events.end()) result.Result = Error("mission audio finished replay event is missing");
            else result.Value = previous->StateArgument != 0;
        }
        return result;
    }
    Commit(event);
    result.Result = Ready();
    return result;
}
NativeScriptServiceResult RealtimeScriptHost::LoadCutscene(const NativeScriptCutsceneRequest& request) {
    if (!m_Initialized) return Error("cutscene load requires initialized host");
    RealtimeScriptHostEvent event{.Id = request.Id, .Opcode = 0x02E4, .Name = request.Name};
    if (auto old = Replay(event)) return *old;
    auto result = m_Cutscene.Load(request.Name);
    event.Status = result.Status;
    Commit(event);
    return result;
}
NativeScriptServiceResult RealtimeScriptHost::StartCutscene(const NativeScriptRequestId& id) {
    if (!m_Initialized) return Error("cutscene start requires initialized host");
    RealtimeScriptHostEvent event{.Id = id, .Opcode = 0x02E7};
    if (auto old = Replay(event)) return *old;
    auto result = m_Cutscene.Start();
    event.Status = result.Status;
    Commit(event);
    return result;
}
NativeScriptServiceResult RealtimeScriptHost::ClearCutscene(const NativeScriptRequestId& id) {
    if (!m_Initialized) return Error("cutscene clear requires initialized host");
    RealtimeScriptHostEvent event{.Id = id, .Opcode = 0x02EA};
    if (auto old = Replay(event)) return *old;
    auto result = m_Cutscene.Unload();
    event.Status = result.Status;
    Commit(event);
    return result;
}
NativeScriptBooleanResult RealtimeScriptHost::HasCutsceneLoaded(const NativeScriptRequestId& id) {
    NativeScriptBooleanResult result;
    if (!m_Initialized) {
        result.Result = Error("cutscene query requires initialized host");
        return result;
    }
    RealtimeScriptHostEvent event{.Id = id, .Opcode = 0x06B9, .StateArgument = m_Cutscene.Loaded()};
    if (auto old = Replay(event)) {
        result.Result = *old;
        result.Value = m_Cutscene.Loaded();
        return result;
    }
    result.Value = m_Cutscene.Loaded();
    event.Status = NativeScriptServiceStatus::Ready;
    Commit(event);
    result.Result = Ready();
    return result;
}
NativeScriptBooleanResult RealtimeScriptHost::HasCutsceneFinished(const NativeScriptRequestId& id) {
    NativeScriptBooleanResult result;
    if (!m_Initialized) {
        result.Result = Error("cutscene completion query requires initialized host");
        return result;
    }
    RealtimeScriptHostEvent event{.Id = id, .Opcode = 0x02E9, .StateArgument = m_Cutscene.Finished()};
    if (auto old = Replay(event)) {
        result.Result = *old;
        result.Value = m_Cutscene.Finished();
        return result;
    }
    result.Value = m_Cutscene.Finished();
    event.Status = NativeScriptServiceStatus::Ready;
    Commit(event);
    result.Result = Ready();
    return result;
}
NativeScriptBooleanResult RealtimeScriptHost::WasCutsceneSkipped(const NativeScriptRequestId& id) {
    NativeScriptBooleanResult result;
    if (!m_Initialized) {
        result.Result = Error("cutscene skip query requires initialized host");
        return result;
    }
    RealtimeScriptHostEvent event{.Id = id, .Opcode = 0x056A, .StateArgument = 0};
    if (auto old = Replay(event)) {
        result.Result = *old;
        return result;
    }
    event.Status = NativeScriptServiceStatus::Ready;
    Commit(event);
    result.Result = Ready();
    result.Value = false; // No source skip input was submitted to this owner.
    return result;
}
NativeScriptStringResult RealtimeScriptHost::GetCharEntryExitName(const NativeScriptPedQueryRequest& request) {
    NativeScriptStringResult result;
    if (!m_Initialized || !ResolvePed(request.Ped)) {
        result.Result = Error("entry-exit name query requires live ped");
        return result;
    }
    RealtimeScriptHostEvent event{.Id = request.Id, .Opcode = 0x094B, .Reference = request.Ped.Value};
    if (auto old = Replay(event)) {
        result.Result = *old;
        return result;
    }
    event.Status = NativeScriptServiceStatus::Ready;
    Commit(event);
    result.Result = Ready();
    return result; // New-game player has not used an entry-exit: source output is empty.
}
NativeScriptServiceResult RealtimeScriptHost::SetUpdateStatsVisible(const NativeScriptRequestId& id, bool visible) {
    if (!m_Initialized) return Error("update-stats visibility requires initialized host");
    RealtimeScriptHostEvent event{.Id=id,.Opcode=0x08F8,.StateArgument=visible ? 1 : 0};
    if (auto old = Replay(event)) return *old;
    m_UpdateStatsVisible = visible;
    Commit(event);
    return Ready();
}
NativeScriptServiceResult RealtimeScriptHost::ClearHelp(const NativeScriptRequestId& id) {
    if (!m_Initialized) return Error("clear-help service requires initialized host");
    RealtimeScriptHostEvent event{.Id=id,.Opcode=0x03E6};
    if (auto old = Replay(event)) return *old;
    auto result = m_Entities.ClearHelp();
    event.Status = result.Status;
    Commit(event);
    return result;
}
NativeScriptServiceResult RealtimeScriptHost::StreamScript(const NativeScriptStreamedRequest& request) {
    if (!m_Initialized || request.ScriptIndex < 0 ||
        std::size_t(request.ScriptIndex) >= m_Session.StreamedScripts().size()) {
        return Error("streamed-script request index is invalid");
    }
    RealtimeScriptHostEvent event{.Id=request.Id,.Opcode=0x08A9,.Index=request.ScriptIndex};
    if (auto old = Replay(event)) return *old;
    if (!m_Session.StreamedScripts()[std::size_t(request.ScriptIndex)].Loaded) {
        return {NativeScriptServiceStatus::Pending, "streamed-script payload is not resident"};
    }
    Commit(event);
    return Ready();
}
NativeScriptServiceResult RealtimeScriptHost::MarkStreamedScriptNoLongerNeeded(
    const NativeScriptStreamedRequest& request) {
    if (!m_Initialized || request.ScriptIndex < 0 ||
        std::size_t(request.ScriptIndex) >= m_Session.StreamedScripts().size()) {
        return Error("streamed-script release index is invalid");
    }
    RealtimeScriptHostEvent event{.Id=request.Id,.Opcode=0x090F,.Index=request.ScriptIndex};
    if (auto old = Replay(event)) return *old;
    m_StreamedNoLongerNeeded[std::size_t(request.ScriptIndex)] = true;
    Commit(event);
    return Ready();
}
bool RealtimeScriptHost::FulfillPendingStreamedScript(const char* gameDir, std::string& error) {
    return m_Session.FulfillPendingStreamedScript(gameDir, error);
}
NativeScriptBooleanResult RealtimeScriptHost::LocateChar(const NativeScriptLocateCharRequest& request) {
    NativeScriptBooleanResult result;
    if (!m_Initialized || !ResolvePed(request.Ped) || !Finite(request.Center) || !Finite(request.Radius) ||
        request.Radius.X < 0 || request.Radius.Y < 0 || request.Radius.Z < 0) {
        result.Result = Error("invalid locate-character query");
        return result;
    }
    const auto& player = m_Gameplay.State();
    NativeScriptPosition point = m_ScriptPlayerPosition.value_or(
        NativeScriptPosition{player.PedRoot.X, player.PedRoot.Y, player.PedRoot.Z});
    bool inVehicle = player.InVehicle;
    if (!request.OnFoot && m_PlayerScriptVehicle) {
        if (const auto* vehicle = m_Vehicles.Resolve({m_PlayerScriptVehicle->Value})) {
            const auto& p = vehicle->State.Matrix.Position;
            point = {p[0], p[1], p[2]};
            inVehicle = true;
        }
    } else if (!request.OnFoot && player.InVehicle) {
        point = {player.Car.X, player.Car.Y, player.Car.Z};
    }
    const bool stopped = !request.Stopped || (inVehicle &&
        (m_PlayerScriptVehicle.has_value() || std::abs(player.Speed) <= 0.01f));
    result.Value = (!request.OnFoot || !inVehicle) && stopped &&
        std::abs(point.X - request.Center.X) <= request.Radius.X &&
        std::abs(point.Y - request.Center.Y) <= request.Radius.Y &&
        (request.TwoDimensional || std::abs(point.Z - request.Center.Z) <= request.Radius.Z);
    RealtimeScriptHostEvent event{.Id=request.Id,.Opcode=std::uint16_t(request.Stopped ? 0x0103 : request.TwoDimensional ? 0x00EC : request.OnFoot ? 0x00FF : 0x00FE),
        .Arguments={request.Center.X,request.Center.Y,request.Center.Z,request.Radius.X,request.Radius.Y,request.Radius.Z},
        .Index=request.Ped.Value,.Reference=result.Value ? 1 : 0,
        .StateArgument=(request.OnFoot ? 2 : 0) | (request.Highlight ? 1 : 0) |
            (request.TwoDimensional ? 4 : 0) | (request.Stopped ? 8 : 0)};
    if (auto old = Replay(event)) {
        result.Result = *old;
        if (result.Result.Status == NativeScriptServiceStatus::Ready) {
            const auto previous = std::ranges::find(m_Events, request.Id, &RealtimeScriptHostEvent::Id);
            if (previous == m_Events.end()) result.Result = Error("locate-character replay event is missing");
            else result.Value = previous->Reference != 0;
        }
        return result;
    }
    Commit(event);
    result.Result = Ready();
    return result;
}
NativeScriptBooleanResult RealtimeScriptHost::DoesObjectExist(const NativeScriptObjectCleanupRequest& request) {
    NativeScriptBooleanResult result;
    if (!m_Initialized) { result.Result = Error("object-existence query requires initialized host"); return result; }
    result.Value = m_Objects.Resolve(request.Object) != nullptr;
    RealtimeScriptHostEvent event{.Id=request.Id,.Opcode=0x03CA,.Index=request.Object.Value,
        .StateArgument=result.Value ? 1 : 0};
    if (auto old = Replay(event)) {
        result.Result = *old;
        if (result.Result.Status == NativeScriptServiceStatus::Ready) {
            const auto previous = std::ranges::find(m_Events, request.Id, &RealtimeScriptHostEvent::Id);
            if (previous == m_Events.end()) result.Result = Error("object-existence replay event is missing");
            else result.Value = previous->StateArgument != 0;
        }
        return result;
    }
    Commit(event);
    result.Result = Ready();
    return result;
}
NativeScriptBooleanResult RealtimeScriptHost::LocateCharObject2D(const NativeScriptLocateCharObjectRequest& request) {
    NativeScriptBooleanResult result;
    const auto* object = m_Objects.Resolve(request.Object);
    if (!m_Initialized || !ResolvePed(request.Ped) || !object || !std::isfinite(request.RadiusX) ||
        !std::isfinite(request.RadiusY) || request.RadiusX < 0 || request.RadiusY < 0) {
        result.Result = Error("invalid locate-character-object query");
        return result;
    }
    const auto& point = m_Gameplay.State().PedRoot;
    result.Value = std::abs(point.X - object->Position.X) <= request.RadiusX &&
        std::abs(point.Y - object->Position.Y) <= request.RadiusY;
    RealtimeScriptHostEvent event{.Id=request.Id,.Opcode=0x0471,
        .Arguments={request.RadiusX,request.RadiusY},.Index=request.Ped.Value,
        .Reference=request.Object.Value,.StateArgument=(request.Highlight ? 2 : 0) | (result.Value ? 1 : 0)};
    if (auto old = Replay(event)) {
        result.Result = *old;
        if (result.Result.Status == NativeScriptServiceStatus::Ready) {
            const auto previous = std::ranges::find(m_Events, request.Id, &RealtimeScriptHostEvent::Id);
            if (previous == m_Events.end()) result.Result = Error("locate-character-object replay event is missing");
            else result.Value = (previous->StateArgument & 1) != 0;
        }
        return result;
    }
    Commit(event);
    result.Result = Ready();
    return result;
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
    const auto publicationGeneration = publication.Generation ? publication.Generation : m_WorldRevision + 1;
    if (publicationGeneration <= m_WorldRevision)
        return releaseFailure("world loader generation is stale", bool(m_Loader));
    const auto residency = ReconcileCarGeneratorsBeforeWorldCommit(publicationGeneration, publication.SourceCollision);
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
    m_World = std::move(world); m_Publication = std::move(publication); m_WorldRevision = publicationGeneration;
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
    // REQUEST_COLLISION prefetches a source region, but LOAD_SCENE may choose a
    // nearby point inside or beyond that packet (the shipped first mission does
    // exactly this). The sole loader must qualify the exact LOAD_SCENE point;
    // do not turn the preceding request center into an invented equality rule.
    // Source LOAD_SCENE only requests/loads the scene; it does not perform a
    // ground probe at the supplied Z coordinate.
    auto result = PublishWorld(request);
    if (result.Status != NativeScriptServiceStatus::Ready) return result;
    m_LoadedScene = p; Commit(event); return Ready();
}
NativeScriptServiceResult RealtimeScriptHost::LoadSceneInDirection(const NativeScriptDirectionalSceneRequest& request) {
    const auto p=request.Position;
    if(!std::isfinite(request.Direction))return Error("directional scene load requires finite heading");
    RealtimeScriptHostEvent event{.Id=request.Id,.Opcode=0x0A0B,.Arguments={p.X,p.Y,p.Z,request.Direction}};
    if(auto result=Replay(event))return *result;
    auto result=PublishWorld({request.Id,p});
    if(result.Status!=NativeScriptServiceStatus::Ready)return result;
    m_LoadedScene=p;Commit(event);return Ready();
}
NativeScriptServiceResult RealtimeScriptHost::ClearArea(const NativeScriptClearAreaRequest& request) {
    if(!m_Initialized||!Finite(request.Position)||!std::isfinite(request.Radius)||request.Radius<0)
        return Error("clear-area request is invalid");
    for (std::size_t slot = 0; slot < NativeVehiclePool::Capacity; ++slot) {
        const auto* vehicle = m_Vehicles.AtSlot(slot);
        if (!vehicle || !vehicle->State.InWorld || vehicle->State.CreatedBy == NativeVehicleCreatedBy::Mission)
            continue;
        const auto& position = vehicle->State.Matrix.Position;
        const float dx = position[0] - request.Position.X;
        const float dy = position[1] - request.Position.Y;
        const float dz = position[2] - request.Position.Z;
        if (dx * dx + dy * dy + dz * dz <= request.Radius * request.Radius)
            return Unsupported("clear-area requires a non-mission population cleanup owner");
    }
    RealtimeScriptHostEvent event{.Id=request.Id,.Opcode=0x0395,
        .Arguments={request.Position.X,request.Position.Y,request.Position.Z,request.Radius},
        .StateArgument=request.IncludeProjectiles?1:0};
    if(auto old=Replay(event))return *old;
    Commit(event);return Ready();
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
    auto prepared=request;
    if(request.Model>=0){
        if(!StreamPager_KnownModelId(request.Model))return {Error("pickup model is absent from sole pager IDE census"),{}};
    }else{
        const std::string_view name(request.UsedObjectName.data(),strnlen(request.UsedObjectName.data(),request.UsedObjectName.size()));
        if(name.empty()||!StreamPager_KnownModelName(name,&prepared.Model))return {Error("pickup used-object name is absent from sole pager IDE census"),{}};
        prepared.UsedObjectName.fill(0);
    }
    const auto camera = m_Gameplay.Camera().Position;
    return m_Entities.CreatePickup(prepared, {camera.X, camera.Y, camera.Z}, State().TimeMs);
}
NativeScriptReferenceResult<NativeScriptPickupRef> RealtimeScriptHost::CreatePickupWithAmmo(const NativeScriptPickupAmmoRequest& request) {
    if(!m_Initialized)return {Error("pickup service requires initialized host"),{}};
    if(m_PendingLoad)return {Error("entity service cannot cross pending world request"),{}};
    for(const auto& event:m_Events)if(event.Id==request.Id)return {Error("entity request ID already owned by player/world service"),{}};
    if(!StreamPager_KnownModelId(request.Model))return {Error("pickup model is absent from sole pager IDE census"),{}};
    return m_Entities.CreatePickupWithAmmo(request,State().TimeMs);
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
NativeScriptServiceResult RealtimeScriptHost::RemoveBlip(const NativeScriptBlipReferenceRequest& request) {
    if (!m_Initialized) return Error("radar service requires initialized host");
    RealtimeScriptHostEvent event{.Id=request.Id, .Opcode=0x0164, .Reference=request.Blip.Value};
    if (auto old = Replay(event)) return *old;
    // CRadar::ClearBlip is a cleanup operation; an already-stale script trace is
    // not promoted to a new error or resurrected generation.
    if (m_Entities.ResolveBlip(request.Blip)) m_Entities.RemoveBlip(request.Blip);
    Commit(event);
    return Ready();
}
NativeScriptBooleanResult RealtimeScriptHost::DoesBlipExist(const NativeScriptBlipReferenceRequest& request) {
    NativeScriptBooleanResult result;
    if (!m_Initialized) { result.Result = Error("radar service requires initialized host"); return result; }
    result.Value = m_Entities.ResolveBlip(request.Blip) != nullptr;
    RealtimeScriptHostEvent event{.Id=request.Id, .Opcode=0x075C,
        .Reference=request.Blip.Value, .StateArgument=result.Value ? 1 : 0};
    if (auto old = Replay(event)) {
        result.Result = *old;
        if (old->Status == NativeScriptServiceStatus::Ready) {
            const auto previous = std::ranges::find(m_Events, request.Id, &RealtimeScriptHostEvent::Id);
            if (previous == m_Events.end()) result.Result = Error("blip-existence replay event is missing");
            else result.Value = previous->StateArgument != 0;
        }
        return result;
    }
    Commit(event);
    result.Result = Ready();
    return result;
}
NativeScriptReferenceResult<NativeScriptUserMarkerRef> RealtimeScriptHost::CreateUserMarker(
    const NativeScriptUserMarkerRequest& request) {
    NativeScriptReferenceResult<NativeScriptUserMarkerRef> result;
    if (!m_Initialized || !Finite(request.Position) || request.Colour < 0 || request.Colour > 15) {
        result.Result = Error("user marker request is invalid");
        return result;
    }
    RealtimeScriptHostEvent event{.Id=request.Id, .Opcode=0x0A40,
        .Arguments={request.Position.X, request.Position.Y, request.Position.Z}, .Index=request.Colour};
    if (auto old = Replay(event)) {
        result.Result = *old;
        if (old->Status == NativeScriptServiceStatus::Ready) {
            const auto previous = std::ranges::find(m_Events, request.Id, &RealtimeScriptHostEvent::Id);
            if (previous == m_Events.end()) result.Result = Error("user marker replay event is missing");
            else result.Reference.Value = previous->Reference;
        }
        return result;
    }
    result.Reference.Value = -1;
    for (std::size_t i = 0; i < m_UserMarkers.size(); ++i) {
        if (m_UserMarkers[i].Used) continue;
        m_UserMarkers[i] = {request.Position, request.Colour, true};
        result.Reference.Value = std::int32_t(i);
        break;
    }
    event.Reference = result.Reference.Value;
    Commit(event);
    result.Result = Ready();
    return result;
}
NativeScriptServiceResult RealtimeScriptHost::RemoveUserMarker(
    const NativeScriptUserMarkerReferenceRequest& request) {
    if (!m_Initialized) return Error("user marker service requires initialized host");
    RealtimeScriptHostEvent event{.Id=request.Id, .Opcode=0x0A41, .Reference=request.Marker.Value};
    if (auto old = Replay(event)) return *old;
    if (request.Marker.Value >= 0 && std::size_t(request.Marker.Value) < m_UserMarkers.size())
        m_UserMarkers[std::size_t(request.Marker.Value)].Used = false;
    Commit(event);
    return Ready();
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
NativeScriptServiceResult RealtimeScriptHost::SwitchEntryExit(const NativeScriptEntryExitSwitchRequest& request) {
    if (!m_Initialized) return Error("entry-exit service requires initialized host");
    RealtimeScriptHostEvent event{.Id=request.Id, .Opcode=0x07FB,
        .StateArgument=request.Enabled ? 1 : 0, .Name=request.Name};
    if (auto old = Replay(event)) return *old;
    const auto result = m_EntryExits.SetEnabledByName(request);
    event.Status = result.Status;
    Commit(event);
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
        m_PendingWorldPublication->Generation ? m_PendingWorldPublication->Generation : m_WorldRevision + 1,
        m_PendingWorldPublication->SourceCollision, &cleanup);
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

NativeScriptReferenceResult<NativeScriptCarGeneratorRef> RealtimeScriptHost::CreateCarGeneratorWithPlate(
    const NativeScriptCarGeneratorPlateRequest& request) {
    if (!m_Initialized) return {Error("generator service requires initialized host"), {}};
    if (m_PendingLoad) return {Error("generator service cannot cross pending world request"), {}};
    const auto& generator = request.Generator;
    const auto p = generator.Position;
    if (!Finite(p) || !std::isfinite(generator.AngleDegrees)) return {Error("nonfinite generator request"), {}};
    RealtimeScriptHostEvent event{.Id=generator.Id, .Opcode=0x09E2,
        .Arguments={p.X, p.Y, p.Z, generator.AngleDegrees},
        .GeneratorArguments={generator.ModelId, generator.PrimaryColor, generator.SecondaryColor,
            generator.ForceSpawn, generator.AlarmChance, generator.DoorLockChance,
            generator.MinDelay, generator.MaxDelay}};
    std::copy(request.PlateText.begin(), request.PlateText.end(), event.ModelName.begin());
    if (auto result = Replay(event)) {
        if (result->Status != NativeScriptServiceStatus::Ready) return {*result, {}};
        const auto old = std::ranges::find(m_Events, generator.Id, &RealtimeScriptHostEvent::Id);
        return {*result, {old->Reference}};
    }
    NativeCarGeneratorCreateRequest native{generator.Id, p, generator.AngleDegrees, generator.ModelId,
        generator.PrimaryColor, generator.SecondaryColor, generator.ForceSpawn, generator.AlarmChance,
        generator.DoorLockChance, generator.MinDelay, generator.MaxDelay};
    std::copy(request.PlateText.begin(), request.PlateText.end(), native.PlateText.begin());
    const auto result = m_CarGenerators.CreateWithPlate(native, State().TimeMs);
    event.Status = result.Result.Status;
    event.Reference = result.Reference.Value;
    Commit(event);
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
