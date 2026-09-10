#include "app/platform/linux/RealtimeScriptHost.h"
#include <cassert>
#include <cmath>
#include <filesystem>
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

namespace {
static_assert(std::is_nothrow_copy_assignable_v<RealtimeScriptHostEvent>);
static_assert(std::is_nothrow_move_constructible_v<RealtimeScriptHostEvent>);
NativeScriptServiceResult Ready() { return {NativeScriptServiceStatus::Ready, {}}; }
NativeScriptServiceResult Error(std::string message) { return {NativeScriptServiceStatus::Error, std::move(message)}; }
NativeScriptServiceResult Unsupported(std::string message) { return {NativeScriptServiceStatus::Unsupported, std::move(message)}; }
bool Finite(NativeScriptPosition p) { return std::isfinite(p.X) && std::isfinite(p.Y) && std::isfinite(p.Z); }
}

RealtimeScriptHost::RealtimeScriptHost(RealtimeGameplay& gameplay): m_Gameplay(gameplay) {}
RealtimeScriptHost::~RealtimeScriptHost() {
    if (m_PendingLoad && m_Cancel) m_Cancel(*m_PendingLoad);
}
bool RealtimeScriptHost::InitializeBeforeWorker(const char* gameDir, std::string& error,
    std::shared_ptr<const NativeCollisionContext> collision) {
    if (!gameDir || !*gameDir) { error = "script host needs game directory"; return false; }
    if (m_Initialized || m_Sealed) { error = "script host initialization must precede worker startup, once"; return false; }
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
    m_Initialized = true;
    error.clear(); return true;
}
void RealtimeScriptHost::SetLiveWorldLoader(WorldLoader loader, CancelLoad cancel) {
    assert(!m_PendingLoad);
    assert(!loader || cancel); // every asynchronous owner has cancellation
    m_Loader = std::move(loader); m_Cancel = std::move(cancel);
}
void RealtimeScriptHost::SealStartup() { m_Sealed = true; m_EntryExits.SealStartup(); }
NativeScriptResult RealtimeScriptHost::RunPass(std::size_t quota) {
    if (!m_Initialized) return {NativeScriptStatus::Error, 0, 0, 0, "script host not initialized"};
    return m_Session.RunPass(*this, quota);
}
bool RealtimeScriptHost::AdvanceTime(std::uint32_t nowMs, std::string& error) { return m_Session.AdvanceTime(nowMs, error); }

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
        if (old.Opcode != event.Opcode || old.Arguments != event.Arguments || old.Index != event.Index || old.StateArgument != event.StateArgument)
            return Error("service request ID reused with different command/arguments");
        return Ready();
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
    if (m_PendingLoad && *m_PendingLoad != request.Id) return Error("another world request is pending");
    if (m_PendingLoad && (m_PendingPosition != request.Position || m_PendingGround != requireGround))
        return Error("pending world request ID reused with changed command/position");
    RealtimeScriptWorldPublication publication;
    if (m_Loader) {
        auto result = m_Loader(request, publication);
        if (result.Status == NativeScriptServiceStatus::Pending) {
            m_PendingLoad = request.Id; m_PendingPosition = request.Position; m_PendingGround = requireGround;
        }
        else m_PendingLoad.reset();
        if (result.Status != NativeScriptServiceStatus::Ready) return result;
    } else {
        if (m_Sealed) return Unsupported("startup sealed: collision/scene needs live worker world loader");
        char err[512]{};
        auto scene = std::make_shared<WorldShotScene>();
        const auto p = request.Position;
        if (!StreamPager_Update(p.X, p.Y, p.Z, *scene, publication.Frame, err, sizeof(err))) return Error(err);
        publication.Scene = std::move(scene); publication.Center = p;
    }
    if (!publication.Scene || publication.Center != request.Position || publication.Frame.instances <= 0 ||
        publication.Frame.tris <= 0 || publication.Scene->meshes.empty()) return Error("world loader did not supply requested resident scene");
    auto world = std::make_shared<RealtimeGameplayWorld>();
    std::string error;
    NativeCollisionSnapshot snapshot;
    // Pure owned data, including when a live loader supplies the render scene.
    // Never trust a callback's collision as a replacement for source residency.
    if (!m_CollisionContext->Snapshot(request.Position.X, request.Position.Y, snapshot, error) ||
        !world->Rebuild(snapshot, error)) return Error(error);
    if (!world->TriangleCount() && !world->SphereCount() && !world->BoxCount())
        return Error("loaded region has no source COL primitives");
    float ground;
    const auto p = request.Position;
    if (requireGround && !world->Ground(p.X, p.Y, p.Z + 1.0f, p.Z - 150.0f, ground))
        return Error("LOAD_SCENE has no actual resident ground at requested position");
    publication.Collision = world;
    m_World = std::move(world); m_Publication = std::move(publication); ++m_WorldRevision;
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
NativeScriptReferenceResult<NativeScriptBlipRef> RealtimeScriptHost::CreateContactBlip(const NativeScriptContactBlipRequest& request) {
    if (!m_Initialized) return {Error("radar service requires initialized host"), {}};
    if (m_PendingLoad) return {Error("entity service cannot cross pending world request"), {}};
    for (const auto& event : m_Events) if (event.Id == request.Id) return {Error("entity request ID already owned by player/world service"), {}};
    return m_Entities.CreateContactBlip(request);
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
