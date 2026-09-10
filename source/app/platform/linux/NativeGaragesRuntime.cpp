#include "app/platform/linux/NativeGaragesRuntime.h"
#include "app/platform/linux/RealtimeStreaming.h"
#include <cmath>
#include <exception>
#include <cstdio>

namespace {
NativeScriptServiceResult Error(std::string message) { return {NativeScriptServiceStatus::Error,std::move(message)}; }
NativeScriptServiceResult Unsupported(std::string message) { return {NativeScriptServiceStatus::Unsupported,std::move(message)}; }
const char* Required(NativeGarageRequirement r) {
    switch (r) {
    case NativeGarageRequirement::RestoreAndOpen: return "stored-car restoration and source door opening";
    case NativeGarageRequirement::DoorMotion: return "source door motion/render/COL publication";
    case NativeGarageRequirement::DoorObstruction: return "source door-obstruction vehicle query";
    case NativeGarageRequirement::VehicleCapacity: return "source garage vehicle-capacity query";
    case NativeGarageRequirement::SourceTypeUpdate: return "unported original garage type/state body";
    case NativeGarageRequirement::TidyUp: return "source vehicle-pool census and tidy/removal consumer (CarPresent=false is not a census)";
    case NativeGarageRequirement::WantedPolicy: return "source respray ignored-by-cops/last-garage policy consumer";
    case NativeGarageRequirement::ImpoundVehicles: return "source impound stored-vehicle operations";
    default: return "unknown garage requirement";
    }
}
}

NativeScriptServiceResult NativeGaragesRuntime::MakeView(const RealtimeGameplayState& player,const RealtimeGameplayCamera& camera,
    const NativeGarages& garages,NativeGaragesRuntimeInput input,NativeGarageView& out) {
    if (!player.Ready || !player.MissionCreated || !player.PlayerOnFootTask || !garages.Ped1Collision()) return Error("garage driver requires the live new-game player owner");
    if (player.InVehicle && !input.Replay && !input.Coop) return Unsupported("garage driver requires actual vehicle COL, matrix, model and subtype; render bounds are insufficient");
    for (float v:{player.PedHeading,player.PedRoot.X,player.PedRoot.Y,player.PedRoot.Z,camera.Position.X,camera.Position.Y,camera.Position.Z})
        if (!std::isfinite(v)) return Error("nonfinite source player pose or camera position");
    NativeGarageView next;
    next.Frame=input.Frame; next.Replay=input.Replay; next.Coop=input.Coop;
    next.Player.ModelId=0; next.Player.Collision=garages.Ped1Collision();
    next.Player.Matrix.Position={player.PedRoot.X,player.PedRoot.Y,player.PedRoot.Z};
    // Exactly the RotateZ angle used by RealtimeGameplay::Pose's actual actor.
    // SetScriptHeading also retains the SCM angle in PedCurrentRotation; the
    // renderer's float add/subtract round-trip need not be bit-identical to it.
    const float angle=player.PedHeading-3.14159265358979323846f*.5f;
    const float c=std::cos(angle),s=std::sin(angle);
    next.Player.Matrix.Basis={NativeCollisionVector{c,s,0},{-s,c,0},{0,0,1}};
    next.Camera={camera.Position.X,camera.Position.Y,camera.Position.Z};
    out=std::move(next); return {NativeScriptServiceStatus::Ready,{}};
}

NativeScriptServiceResult NativeGaragesRuntime::Tick(const realtime_streaming::CpuWorld& published,NativeGaragesRuntimeInput input) try {
    if (m_Fault) return *m_Fault;
    NativeGaragesRuntimeFrame next;
    const auto view=MakeView(m_Gameplay.State(),m_Gameplay.Camera(),m_Garages,input,next.View);
    if (view.Status!=NativeScriptServiceStatus::Ready) return view;
    if (!published.Error.empty() || published.Scene.meshes.empty() || !published.SourceCollision || !published.Overrides ||
        published.SourceCollision->Overrides!=published.Overrides) return Error("garage driver needs one committed source scene/COL override publication");
    if (m_PublishedCollision && (published.Generation<m_Frame.Generation ||
        (published.Generation==m_Frame.Generation && published.SourceCollision!=m_PublishedCollision))) return Error("stale or reused garage world generation");
    next.View.PublishedOverrides=published.Overrides;
    // No detached acknowledgement: exact immutable replacement data must match
    // the actual registry's first common update for every registered door.
    for (const auto& door:m_Garages.Doors()) if (door.Garage) {
        const auto* g=m_Garages.Resolve(*door.Garage);
        if (!g || !NativeGarages::DoorPublished(*g,door,published.Overrides.get(),
            input.Replay || input.Coop ? g->Flags : NativeGarages::UpdateCollisionFlags(*g)))
            return Unsupported("garage placement publication disagrees with requested pose/collision for garage "+std::to_string(door.Garage->Index));
    }
    std::string error;
    NativeScriptServiceResult result{NativeScriptServiceStatus::Ready,{}};
    result.Message.reserve(512);
    NativeScriptServiceResult fault{NativeScriptServiceStatus::Unsupported,{}};
    fault.Message.reserve(512);
    if (!m_Garages.Tick(next.View,error)) return Error(error);
    next.Generation=published.Generation; next.Revision=m_Frame.Revision+1;
    next.Camera=m_Garages.Frame().Camera; next.BaselineCamera=m_Gameplay.Camera();
    // CCamera::CamControl's on-foot garage branch (retail537B67) selects fixed
    // mode15 only when the garage contribution applies; normal follow-ped is4.
    // Preserve the real existing follow camera outside garages. Enter/exit and
    // first-person restrictions need their original camera consumer, not a flag
    // assignment described as a completed physical camera transition.
    if (next.Camera.Outside || next.Camera.Garage || next.Camera.Previous || next.Camera.AvoidFirstPerson) {
        next.Barrier=NativeGaragesRuntimeBarrier::GarageCamera;
        next.Garage=next.Camera.Garage ? next.Camera.Garage : next.Camera.Previous ? next.Camera.Previous : next.Camera.AvoidFirstPerson;
        result.Status=NativeScriptServiceStatus::Unsupported;
        result.Message="original garage camera consumer required (fixed/follow transition or first-person restriction)";
    }
    for (const auto& update:m_Garages.Frame().Updates) if (update.Status!=NativeScriptServiceStatus::Ready) {
        ++next.UnsupportedUpdates;
        if (result.Status==NativeScriptServiceStatus::Ready) {
            next.Barrier=NativeGaragesRuntimeBarrier::GarageUpdate; next.Garage=update.Garage; next.Requirement=update.Requirement;
            std::array<char,512> message{};
            std::snprintf(message.data(),message.size(),"garage %zu requires %s",update.Garage.Index,Required(update.Requirement));
            result.Status=NativeScriptServiceStatus::Unsupported; result.Message=message.data();
        }
    }
    if (result.Status!=NativeScriptServiceStatus::Ready) { fault.Message=result.Message; m_Fault=std::move(fault); }
    m_PublishedCollision=published.SourceCollision; m_Frame=std::move(next); return result;
} catch (const std::exception& e) { return Error(e.what()); }
