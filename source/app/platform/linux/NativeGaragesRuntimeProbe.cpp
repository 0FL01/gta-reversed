#include "app/platform/linux/RealtimeScriptHost.h"
#include "app/platform/linux/NativeGaragesRuntime.h"
#include "app/platform/linux/RealtimeStreaming.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cmath>

namespace {
void Require(bool ok,const std::string& error) { if (!ok) { std::fprintf(stderr,"garage-runtime: %s\n",error.c_str()); std::exit(2); } }
std::vector<std::uint8_t> Flags(const NativeGarages& garages) {
    std::vector<std::uint8_t> flags; for (const auto& g:garages.Entries()) flags.push_back(g.Flags); return flags;
}
std::unique_ptr<realtime_streaming::CpuWorld> Wait(realtime_streaming::Worker& worker) {
    const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(90);
    while (std::chrono::steady_clock::now()<until) {
        if (auto ready=worker.TakeReady()) { Require(ready->Error.empty(),ready->Error); return ready; }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    Require(false,"worker timeout"); return {};
}
void Report(const RealtimeScriptHost& host,const NativeGaragesRuntime& runtime) {
    const auto& f=runtime.Frame();
    std::printf("driver frame=%llu generation=%llu main=%llu mission=%llu updates=%zu unsupported=%zu outside=%d cameraGarage=%d previous=%d avoid=%d\n",
        (unsigned long long)f.View.Frame,(unsigned long long)f.Generation,(unsigned long long)host.Session().Threads()[0].Commands,
        (unsigned long long)host.Session().Threads()[1].Commands,host.Garages().Frame().Updates.size(),f.UnsupportedUpdates,
        f.Camera.Outside,f.Camera.Garage.has_value(),f.Camera.Previous.has_value(),f.Camera.AvoidFirstPerson.has_value());
}
void BranchFixtures(const NativeGarages& garages,NativeGarageView view) {
    const auto entry=[&](int type) { auto i=std::ranges::find_if(garages.Entries(),[&](const auto& g) { return g.Type==type; }); Require(i!=garages.Entries().end(),"real fixture type"); return *i; };
    const auto center=[&](const auto& g) { view.Player.Matrix.Position={(g.Rect[0]+g.Rect[1])*.5f,(g.Rect[2]+g.Rect[3])*.5f,g.Origin[2]+1}; };
    auto g=entry(1); center(g);
    Require(NativeGarages::Transition(g,view)==NativeGarageRequirement::None,"mission garage has no player vehicle even at center");
    g.DoorState=1;
    Require(NativeGarages::Transition(g,view)==NativeGarageRequirement::SourceTypeUpdate,"mission OPEN body is not blanket skipped");
    g=entry(2); center(g);
    Require(NativeGarages::Transition(g,view)==NativeGarageRequirement::None,"bombshop OPEN: null player vehicle returns false from static-car check");
    g=entry(5); center(g);
    Require(NativeGarages::Transition(g,view)==NativeGarageRequirement::WantedPolicy,"on-foot respray still needs wanted policy inside");
    view.Player.Matrix.Position={g.Rect[1]+5,g.Rect[3]+5,g.Origin[2]+1};
    Require(NativeGarages::Transition(g,view)==NativeGarageRequirement::None,"respray outside, actual initial last-garage -1");
    NativeGaragePolicy policy; policy.LastGaragePlayerWasIn=7;
    Require(NativeGarages::Transition(g,view,policy,7)==NativeGarageRequirement::WantedPolicy,"last respray requires clearing wanted policy outside");
    policy.NoResprays=true;
    Require(NativeGarages::Transition(g,view,policy,7)==NativeGarageRequirement::None,"source no-resprays gate precedes wanted work");
    g=entry(33); center(g);
    Require(NativeGarages::Transition(g,view)==NativeGarageRequirement::ImpoundVehicles,"near closed impound cannot skip vehicle operations");
    view.Player.Matrix.Position[0]=g.Rect[1]+60;
    Require(NativeGarages::Transition(g,view)==NativeGarageRequirement::None,"exact 60m impound threshold excluded");
    view.Player.Matrix.Position[0]=std::nextafter(view.Player.Matrix.Position[0],g.Rect[1]);
    Require(NativeGarages::Transition(g,view)==NativeGarageRequirement::ImpoundVehicles,"one float step inside impound threshold");
    center(g); view.Player.Matrix.Position[2]=g.Top-2;
    Require(NativeGarages::Transition(g,view)==NativeGarageRequirement::None,"impound upper Z boundary excluded");
    view.Player.Matrix.Position[2]=g.Origin[2];
    Require(NativeGarages::Transition(g,view)==NativeGarageRequirement::None,"impound lower Z boundary excluded");
    g=entry(44); center(g);
    Require(NativeGarages::Transition(g,view)==NativeGarageRequirement::RestoreAndOpen,"AT400 hangar uses actual hideout handler");
    g.Type=6; view.Player.Matrix.Position={5000,5000,0};
    Require(NativeGarages::Transition(g,view)==NativeGarageRequirement::SourceTypeUpdate,"unknown body is not a far-distance no-op");
    std::printf("TEST-POSITION/POLICY branch fixtures PASS (actual IPL records and source ped COL; not SCM execution)\n");
}
}
int main(int argc,char** argv) {
    Require(argc==2,"game directory required");
    std::setvbuf(stdout,nullptr,_IONBF,0); char message[512]{}; std::string error; E2ELoadInfo info;
    Require(StreamPager_Init(argv[1],info,message,sizeof(message),{true,300,1200}),message);
    {
        auto context=NativeCollisionContext::LoadBeforeWorker(argv[1],900,error); Require(bool(context),error);
        RealtimeGameplay game,cameraFixture; RealtimeScriptHost host(game);
        Require(host.InitializeBeforeWorker(argv[1],error,context),error);
        Require(cameraFixture.Initialize(argv[1],error,RealtimeGameplayModel::BasePlayer),error);
        Require(host.RunPass(1000).Status==NativeScriptStatus::Waiting,"actual main53");
        const auto player=game.State(); const auto camera=game.Camera();
        std::printf("actual-view root=%.9f,%.9f,%.9f rotation=%.9f camera=%.9f,%.9f,%.9f yaw=%.9f\n",player.PedRoot.X,player.PedRoot.Y,player.PedRoot.Z,player.PedCurrentRotation,camera.Position.X,camera.Position.Y,camera.Position.Z,camera.Yaw);
        NativeGarageView view;
        Require(NativeGaragesRuntime::MakeView(player,camera,host.Garages(),{},view).Status==NativeScriptServiceStatus::Ready,"actual view adapter");
        Require(view.Player.Collision==host.Garages().Ped1Collision() && view.Player.Matrix.Position[2]==player.Ped.Z+1,"actual source root, not feet or render bounds");
        const double actorAngle=double(player.PedHeading-3.14159265358979323846f*.5f);
        Require(std::abs(view.Player.Matrix.Basis[1][0]+std::sin(actorAngle))<1e-7 && std::abs(view.Player.Matrix.Basis[1][1]-std::cos(actorAngle))<1e-7,"source actor +Y forward at heading262");
        auto vehicleFixture=player; vehicleFixture.InVehicle=true;
        Require(NativeGaragesRuntime::MakeView(vehicleFixture,camera,host.Garages(),{},view).Status==NativeScriptServiceStatus::Unsupported && !view.Vehicle,"TEST-INPUT vehicle: no substituted ped/render bounds");
        BranchFixtures(host.Garages(),view);
        const auto initialFlags=Flags(host.Garages());
        const auto startup=host.Publication();
        Require(std::ranges::all_of(startup.Overrides->Entries(),[](const auto& p) { return p.CollisionEnabled; }),"actual constructor publication still collides before explicit preparation");
        Require(host.PrepareInitialGarageWorldBeforeWorker(error),error);
        const auto prepared=host.Publication(); const auto overrides=host.InitialPlacementOverrides();
        Require(host.WorldRevision()==3 && prepared.Scene==startup.Scene && prepared.SourceCollision!=startup.SourceCollision &&
            prepared.SourceCollision->Overrides==overrides && Flags(host.Garages())==initialFlags,"production preparation publishes owned COL without an early garage Tick");
        const auto disabled=std::ranges::count_if(overrides->Entries(),[](const auto& p) { return !p.CollisionEnabled; });
        Require(overrides->Entries().size()==14 && disabled==13,"actual source first-common-update overrides");
        NativeGaragesRuntime runtime(host.Garages(),game);
        {
            // Actual old publication: source poses, but OPEN doors still collide.
            realtime_streaming::CpuWorld wrong;
            wrong.Position={startup.Center.X,startup.Center.Y,startup.Center.Z};
            wrong.Scene=*startup.Scene; wrong.Frame=startup.Frame;
            wrong.SourceCollision=startup.SourceCollision; wrong.Overrides=startup.Overrides;
            Require(runtime.Tick(wrong,{}).Status==NativeScriptServiceStatus::Unsupported && runtime.Frame().Revision==0 && Flags(host.Garages())==initialFlags,"old collidable OPEN publication rejected atomically");
        }
        host.SealStartup();
        realtime_streaming::Worker worker(true,context,overrides);
        worker.Request({player.PedRoot.X,player.PedRoot.Y,player.PedRoot.Z},true);
        // The first adapter Tick consumes the actual host handoff, before the
        // first worker result. Its query world is pinned by prepared.Collision.
        auto active=std::make_unique<realtime_streaming::CpuWorld>();
        active->Position={prepared.Center.X,prepared.Center.Y,prepared.Center.Z};
        active->Scene=*prepared.Scene; active->Frame=prepared.Frame;
        active->SourceCollision=prepared.SourceCollision; active->Overrides=prepared.Overrides;
        Require(runtime.Tick(*active,{0,true}).Status==NativeScriptServiceStatus::Unsupported && runtime.Frame().Revision==0 && Flags(host.Garages())==initialFlags,
            "suppressed first update cannot acknowledge an unexecuted common collision change");
        auto result=runtime.Tick(*active,{}); Require(result.Status==NativeScriptServiceStatus::Ready,result.Message); Report(host,runtime);
        Require(host.Garages().Frame().Updates.size()==50 && !runtime.Frame().Camera.Garage && !runtime.Frame().Camera.Outside,"actual initial 50 ready / no garage camera contribution");
        unsigned changed=0;
        for (size_t i=0;i<initialFlags.size();++i) if (initialFlags[i]!=host.Garages().Entries()[i].Flags) {
            ++changed; Require((initialFlags[i]^host.Garages().Entries()[i].Flags)==0x40,"first common update changes collision bit only");
        }
        NativeCollisionSnapshot authored;
        Require(context->Snapshot(player.PedRoot.X,player.PedRoot.Y,authored,error),error);
        unsigned residentSuppressed=0;
        for (const auto& door:overrides->Entries()) if (!door.CollisionEnabled && std::ranges::any_of(authored.Instances,[&](const auto& p) { return door.Identity.Matches(p.Placement); })) {
            ++residentSuppressed;
            Require(std::ranges::none_of(active->SourceCollision->Instances,[&](const auto& p) { return door.Identity.Matches(p.Placement); }),"actual committed source-COL removes that exact door identity");
        }
        Require(changed>0 && residentSuppressed>0 && host.Garages().Revision()==0,"real first-frame collision effect, no script write invented");
        std::printf("first-common-effect garageFlagsChanged=%u initialOverrides=%zu collisionDisabled=%zu residentSourceDoorsSuppressed=%u\n",changed,overrides->Entries().size(),size_t(disabled),residentSuppressed);
        const auto revision=runtime.Frame().Revision;
        Require(runtime.Tick(*active,{}).Status==NativeScriptServiceStatus::Error && runtime.Frame().Revision==revision,"duplicate frame atomic");
        auto originalSnapshot=active->SourceCollision;
        active->SourceCollision=std::make_shared<const NativeCollisionSnapshot>(*originalSnapshot);
        Require(runtime.Tick(*active,{1}).Status==NativeScriptServiceStatus::Error && runtime.Frame().Revision==revision,"same generation with different source packet rejected"); active->SourceCollision=originalSnapshot;
        active=Wait(worker);
        worker.Release(); worker.Request({player.PedRoot.X+10,player.PedRoot.Y,player.PedRoot.Z},true);
        for (std::uint64_t frame=1;frame<=12;++frame) {
            if (frame==1) Require(host.RunPass(135).Status==NativeScriptStatus::BudgetYield,"actual mission135");
            if (frame==2) {
                const auto end=host.RunPass(1000);
                Require(end.Status==NativeScriptStatus::Unsupported && end.Opcode==0x0570 && end.IP==205876 && end.Executed==404 && host.Session().Threads()[1].Commands==539,"actual strict mission539 boundary");
                std::printf("actual SCM main53 mission539 strict0570@205876; subsequent ticks isolate garage driver, no further SCM execution\n");
            }
            if (frame==3) {
                auto incoming=Wait(worker); Require(incoming->Generation>active->Generation,"actual worker generation advances");
                worker.Retire(std::move(active)); active=std::move(incoming); worker.Release();
            }
            if (frame==4) {
                const auto generation=active->Generation, before=runtime.Frame().Revision;
                active->Generation=generation-1;
                Require(runtime.Tick(*active,{frame}).Status==NativeScriptServiceStatus::Error && runtime.Frame().Revision==before,"stale generation atomic");
                active->Generation=generation;
            }
            game.Tick(1.0/60,{},active->Collision);
            result=runtime.Tick(*active,{frame});
            if (frame<12) Require(result.Status==NativeScriptServiceStatus::Ready,result.Message);
            else Require(result.Status==NativeScriptServiceStatus::Unsupported && runtime.Frame().Requirement==NativeGarageRequirement::TidyUp && runtime.Frame().Garage->Index==1,"first real maintenance dependency at counter12 index1");
            if (frame<=3 || frame==11 || frame==12) Report(host,runtime);
        }
        Require(host.Session().Threads()[0].Commands==53 && host.Session().Threads()[1].Commands==539 && host.Garages().Revision()==13,"per-frame consumers preserve actual script boundary/journal");
        Require(!host.Garages().Frame().TidyClose && runtime.Frame().UnsupportedUpdates==1,"far maintenance is not skipped as harmless");
        std::printf("first-garage-barrier frame12 index1 type33 tidyClose=0 detail=%s\n",result.Message.c_str());
        const auto stopped=runtime.Frame().Revision;
        Require(runtime.Tick(*active,{13}).Status==NativeScriptServiceStatus::Unsupported && runtime.Frame().Revision==stopped,"required maintenance cannot be silently skipped next frame");
        // Separate real native player fixture, prepared before SealStartup.
        // A source-world validated spawn tests camera requirements; this is NOT
        // a completed garage/interior transition or another SCM instruction.
        const auto& ganton=*std::ranges::find_if(host.Garages().Entries(),[](const auto& g) { return g.Type==16; });
        const float x=(ganton.Rect[0]+ganton.Rect[1])*.5f,y=(ganton.Rect[2]+ganton.Rect[3])*.5f; float ground;
        Require(active->Collision.Ground(x,y,ganton.Origin[2]+.5f,ganton.Origin[2]-3,ground),"actual garage floor for camera fixture (below roof)");
        std::printf("TEST-POSITION camera x=%.9f y=%.9f sourceBaseZ=%.9f topZ=%.9f actualGround=%.9f\n",x,y,ganton.Origin[2],ganton.Top,ground);
        Require(cameraFixture.SpawnScriptPlayer(active->Collision,{x,y,ground},error),error);
        NativeGaragesRuntime inside(host.Garages(),cameraFixture);
        result=inside.Tick(*active,{13});
        Require(result.Status==NativeScriptServiceStatus::Unsupported && inside.Frame().Barrier==NativeGaragesRuntimeBarrier::GarageCamera && inside.Frame().Camera.Outside,"actual native ped/COL requires original garage camera, not a fake fixed pose");
        Require(cameraFixture.SpawnScriptPlayer(active->Collision,player.Ped,error),error);
        NativeGaragesRuntime outside(host.Garages(),cameraFixture);
        result=outside.Tick(*active,{14});
        Require(result.Status==NativeScriptServiceStatus::Unsupported && outside.Frame().Barrier==NativeGaragesRuntimeBarrier::GarageCamera && !outside.Frame().Camera.Outside && outside.Frame().Camera.Previous,"garage exit camera transition also explicit");
        std::printf("TEST-POSITION native camera entry/exit requirements PASS; no completed door/interior/camera transition\n");
        worker.Stop(std::move(active),{});
        std::printf("NativeGaragesRuntimeProbe PASS main53 mission539 strict0570@205876 idleCounters0..11 nextGarageCounter12/TidyUp/index1\n");
    }
    StreamPager_Shutdown();
}
