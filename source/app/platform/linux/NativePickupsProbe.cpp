// Real SCM/asset services. Synthetic proximity inputs are explicitly fixtures;
// they never certify a completed collection or a native task-manager port.
#include "app/platform/linux/RealtimeScriptHost.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <limits>
#include <rw.h>

#ifndef NATIVE_PICKUPS_STANDALONE
void RealtimeScriptPickupGpuProbe(RealtimeScriptHost& host, NativeScriptPickupRef ref);
#endif

namespace {
void Require(bool ok, const std::string& error) { if (!ok) throw std::runtime_error(error); }
}

int NativePickupsProbe(const char* dir, std::uint64_t& commands, std::uint16_t& opcode, std::uint32_t& ip) {
    int failures = 0;
    const auto check = [&](bool ok, const char* text) { std::printf("%s %s\n", ok ? "PASS" : "FAIL", text); failures += !ok; };
    std::string error;
    RealtimeGameplay game; RealtimeScriptHost host(game);
    const auto* dictionary = rw::TexDictionary::getCurrent();
    Require(host.InitializeBeforeWorker(dir,error),error);
    NativeScriptEntities fixture(1,1); Require(fixture.LoadBeforeWorker(dir,error),error);
    NativeScriptEntities capacity; Require(capacity.LoadBeforeWorker(dir,error),error);
    auto& entities = host.Entities();
    check(rw::TexDictionary::getCurrent()==dictionary && rw::Engine::state==rw::Engine::Started,"save preload restores borrowed RW dictionary/engine");
    check(std::string(entities.PreparedSaveModel().stats.dffName)=="pickupsave.dff" &&
        std::string(entities.PreparedSaveModel().stats.txdName)=="icons4.txd" && entities.SaveGeometry().ColHeaderId==1277 &&
        entities.SaveGeometry().ColLibrary=="models/gta3.img:dynamic.col","actual source MI_PICKUP_SAVEGAME IDE/DFF/TXD/COL identity");
    check(entities.PreparedImages().size()==entities.PreparedModel().images.size()+entities.PreparedForSaleModel().images.size()+entities.PreparedSaveModel().images.size(),
        "all texture indices refer to one immutable startup upload, ordinary images appended");
    Require(host.RunPass(256).Executed==53,"actual main53 first frame");
    Require(host.PrepareInitialGarageWorldBeforeWorker(error),error); host.SealStartup();
    const auto main = static_cast<const NativeScriptThreadState&>(host.State());
    Require(host.RunPass(256).Executed==256 && host.RunPass(256).Executed==256 && host.RunPass(4).Executed==4,"actual mission512 plus4 to516");
    check(host.Session().Threads()[1].Commands==516 && host.Session().Threads()[1].IP==205545 && host.WorldRevision()==3,
        "actual unchanged main53/mission516 checkpoint with paired initial source garage world revision3");
    const auto before = entities.Revision();
    const auto enexRevision = host.EntryExits().Revision(), garageRevision = host.Garages().Revision();
    const auto worldJournal = host.Events().size();
    const auto session = host.Events().front().Id.Session;
    const auto first = host.RunPass(1);
    std::int32_t reference = -1; Require(host.Session().ReadGlobal(3460,reference),"actual pickup output");
    const auto* pickup = entities.ResolvePickup({reference}); Require(pickup,"actual pickup allocation");
    check(first.Executed==1 && first.Opcode==0x0213 && first.IP==205563 && pickup->Model==1277 && pickup->Type==3 && reference==65568,
        "actual command517 0213@205545 writes first save-token slot32/generation1, not a property alias");
    const auto firstPickup = *pickup;
    NativeScriptPickupRequest request{{session,570,205545},-43,3,pickup->AuthoredPosition,host.Session().Metadata().UsedObjects.at(43)};
    const auto revision = entities.Revision();
    check(host.CreatePickup(request).Reference.Value==reference && entities.Revision()==revision,"actual request identity replay does not allocate or retime");
    auto changed=request; ++changed.Type;
    check(host.CreatePickup(changed).Result.Status==NativeScriptServiceStatus::Error && entities.Revision()==revision,"ordinary replay type mismatch atomic");
    changed=request; changed.UsedObjectName[0]='X';
    check(host.CreatePickup(changed).Result.Status==NativeScriptServiceStatus::Error && entities.Revision()==revision,"ordinary replay model-table identity mismatch atomic");
    check(host.SetCameraBehindPlayer({request.Id}).Status==NativeScriptServiceStatus::Error &&
        host.SetEntryExitFlag({request.Id,0,0,10,0x4000,1}).Status==NativeScriptServiceStatus::Error &&
        host.DeactivateGarage({request.Id,host.Garages().Entries()[13].Name}).Status==NativeScriptServiceStatus::Error &&
        host.CreateLockedProperty({request.Id,request.Position,{}}).Result.Status==NativeScriptServiceStatus::Error,
        "pickup ID cannot cross world/ENEX/garage/property journals");
    changed=request; changed.Id=host.Events().front().Id;
    check(host.CreatePickup(changed).Result.Status==NativeScriptServiceStatus::Error && entities.Revision()==revision,"world ID cannot allocate ordinary pickup");
    const auto terminal=host.RunPass(1000);
    commands=host.Session().Threads()[1].Commands; opcode=terminal.Opcode; ip=terminal.IP;
    check(terminal.Status==NativeScriptStatus::Unsupported && terminal.Executed==22 && commands==539 && opcode==0x0570 && ip==205876,
        "ACTUAL 13 save pickups plus10 numeric writes, mission539 strict sprite33 service 0570@205876 unadvanced");
    check(static_cast<const NativeScriptThreadState&>(host.State())==main && host.EntryExits().Revision()==enexRevision &&
        host.Garages().Revision()==garageRevision && host.Events().size()==worldJournal && worldJournal==50,
        "new slice preserves main WAIT, source ENEX/garage flags and player/world journal");
    unsigned saves=0,locked=0,sale=0,blips=0;
    for (const auto& p:entities.Pickups()) if (p.Active) { saves+=p.Model==1277 && p.Type==3; locked+=p.Type==17; sale+=p.Type==18; }
    for (const auto& b:entities.Blips()) blips+=b.Active;
    check(saves==13 && locked==26 && sale==6 && blips==32 && entities.Revision()==before+13,"persistent shared620 pool owns45 pickups, old32 blips unchanged");
    const auto& last=host.Session().Threads()[1].LastOutputWrite;
    std::printf("pickup actual commands=%llu opcode=%04X ip=%u outputSequence=%llu outputIP=%u variable=%u value=%d saves=%u locked=%u sale=%u revision=%llu\n",
        static_cast<unsigned long long>(commands),opcode,ip,static_cast<unsigned long long>(last.Sequence),last.IP,last.Variable,last.Value,saves,locked,sale,
        static_cast<unsigned long long>(entities.Revision()));
    const auto frozen=host.Session().Threads()[1];
    check(host.RunPass(10).Executed==0 && host.Session().Threads()[1]==frozen,"terminal unsupported sprite neither advances nor repeats allocations");
    const auto camera=game.Camera().Position;
    NativeScriptPropertyInput live; live.FrameCounter=3;
    Require(host.TickProperties({camera.X,camera.Y,camera.Z},true,live,error) && entities.AdvanceTime(48,error),error);
    check(entities.PickupRequirement().Kind==NativeScriptPickupRequirementKind::None && host.PlayerInfo().Money==0,
        "actual startup player/camera consume owned save actors without fabricated collection or cash");
    std::printf("save geometry triangles=%d images=%zu colScale=%.9f colMin=%.9f,%.9f,%.9f colMax=%.9f,%.9f,%.9f firstPosition=%.9f,%.9f,%.9f\n",
        entities.PreparedSaveModel().stats.triangles,entities.PreparedSaveModel().images.size(),entities.SaveGeometry().Scale,
        entities.SaveGeometry().ColMin[0],entities.SaveGeometry().ColMin[1],entities.SaveGeometry().ColMin[2],
        entities.SaveGeometry().ColMax[0],entities.SaveGeometry().ColMax[1],entities.SaveGeometry().ColMax[2],
        firstPickup.Position.X,firstPickup.Position.Y,firstPickup.Position.Z);
#ifndef NATIVE_PICKUPS_STANDALONE
    RealtimeScriptPickupGpuProbe(host,{reference});
#endif

    // TEST-INPUT: actual model/pool/update code, synthetic isolated proximity.
    NativeScriptPickupRequest test{{7001,1,1},1277,3,{1.09f,-2.09f,3.09f}};
    const NativeScriptPosition near{1,-2,3}, away{40,40,3};
    auto created=fixture.CreatePickup(test,near,100); Require(created.Result.Status==NativeScriptServiceStatus::Ready,created.Result.Message);
    const auto ref=created.Reference; const auto base=fixture.Revision();
    check(fixture.ResolvePickup(ref)->Position==near && fixture.ResolvePickup(ref)->RegenerationTime==100 && fixture.ResolvePickup(ref)->Visible,
        "source signed int16/8 truncation, creation camera visibility and type3 timestamp");
    const auto draw=[&](std::uint32_t frame,NativeScriptPosition ped,NativeScriptPosition cam,bool alive,bool vehicle,NativeScriptPropertyInput in={}) {
        in.FrameCounter=frame; fixture.Tick(ped,cam,alive,vehicle,in); return fixture.AdvanceTime(100+frame,error);
    };
    Require(draw(0,near,near,false,false),error);
    check(fixture.Actors().stats.triangles==fixture.PreparedSaveModel().stats.triangles && fixture.PickupRequirement().Kind==NativeScriptPickupRequirementKind::None,
        "dead player does not collect; real model still renders");
    Require(draw(6,near,near,true,true),error);
    NativeScriptPropertyInput input; input.Busy=true; Require(draw(12,near,near,true,false,input),error);
    input={}; input.Replay=true; Require(draw(18,near,near,true,false,input),error);
    input={}; input.Cutscene=true; Require(draw(24,near,near,true,false,input),error);
    check(fixture.Actors().stats.triangles==0,"source running cutscene suppresses ordinary drawable/collection");
    input={}; input.Widescreen=true; Require(draw(30,near,near,true,false,input),error);
    input={}; input.Coop=true; Require(draw(36,near,near,true,false,input),error);
    check(fixture.Actors().stats.triangles==0,"source coop unarmed-model invisibility and mission gate");
    Require(draw(42,{near.X,near.Y,near.Z+2},near,true,false),error);
    Require(draw(48,{near.X+2,near.Y,near.Z},near,true,false),error);
    check(fixture.ResolvePickup(ref) && fixture.Revision()==base && fixture.PickupRequirement().Kind==NativeScriptPickupRequirementKind::None,
        "vehicle/busy/replay/global/strictZ/proximity gates never remove or emit collected reference");
    Require(draw(64,away,{near.X+100,near.Y,near.Z},true,false),error);
    check(!fixture.ResolvePickup(ref)->Visible && !fixture.ResolvePickup(ref)->ObjectPresent && fixture.Actors().stats.triangles==0,"source32 partition at exact100m retires object only, keeps pickup reference");
    Require(draw(65,away,near,true,false),error);
    check(!fixture.ResolvePickup(ref)->ObjectPresent,"camera return outside slot's32 partition does not eagerly recreate object");
    Require(draw(96,away,near,true,false),error);
    check(fixture.ResolvePickup(ref)->ObjectPresent && fixture.ResolvePickup(ref)->Visible,"source32 partition recreates original model, no respawn timer for type3");
    const auto stable=fixture.ResolvePickup(ref)->Actor.meshes.front().pos;
    check(!draw(90,away,near,true,false) && fixture.ResolvePickup(ref)->Actor.meshes.front().pos==stable,"stale frame/time atomically preserves ordinary actor");
    auto second=test; ++second.Id.Instruction;
    check(fixture.CreatePickup(second,near,200).Result.Status==NativeScriptServiceStatus::Error && fixture.Revision()==base,"shared capacity failure consumes no generation or identity");
    second=test; ++second.Id.Instruction; second.Position.X=std::numeric_limits<float>::quiet_NaN();
    check(fixture.CreatePickup(second,near,200).Result.Status==NativeScriptServiceStatus::Error && fixture.Revision()==base,"nonfinite allocation failure atomic");
    check(!draw(102,near,near,true,false) && fixture.PickupRequirement().Kind==NativeScriptPickupRequirementKind::PlayerTaskEligibility &&
        fixture.PickupRequirement().Pickup.Value==ref.Value && fixture.ResolvePickup(ref) && fixture.Revision()==base &&
        fixture.ResolvePickup(ref)->Actor.meshes.front().pos==stable,"actual eligible proximity emits typed task/event requirement BEFORE shake/removal/collected-ring side effects");
    check(!draw(108,away,near,true,false) && fixture.PickupRequirement().FrameCounter==102,"required collection work remains latched; walking away is not completion");
    check(fixture.RemovePickup(ref) && !fixture.ResolvePickup(ref),"explicit owner release invalidates pickup, does not mean collected");
    second=test; ++second.Id.Instruction;
    const auto reused=fixture.CreatePickup(second,near,200);
    check(reused.Result.Status==NativeScriptServiceStatus::Ready && reused.Reference.Value==ref.Value+65536 &&
        fixture.CreatePickup(test,near,200).Result.Status==NativeScriptServiceStatus::Error,"generation-safe reuse; stale creation ID cannot resurrect released pickup");
    // Real source capacity, not the deliberately small failure fixture above.
    std::int32_t firstCapacityRef=-1;
    for (std::size_t i=0;i<620;++i) {
        auto r=test; r.Id={7002,i+1,1}; const auto allocation=capacity.CreatePickup(r,near,200);
        Require(allocation.Result.Status==NativeScriptServiceStatus::Ready && allocation.Reference.Value==std::int32_t(65536+i),"source620 ascending slots");
        if (!i) firstCapacityRef=allocation.Reference.Value;
    }
    auto full=test; full.Id={7002,621,1};
    const auto exhausted=capacity.CreatePickup(full,near,200);
    check(exhausted.Result.Status==NativeScriptServiceStatus::Ready && exhausted.Reference.Value==-1 && capacity.ResolvePickup({firstCapacityRef}),
        "actual620 occupied nonreclaimable slots return source invalid sentinel, never a manufactured drawable");
    Require(capacity.RemovePickup({firstCapacityRef}),"capacity owner release"); const auto afterRelease=capacity.Revision();
    check(capacity.CreatePickup(full,near,300).Reference.Value==-1 && capacity.Revision()==afterRelease,
        "full-pool result replay remains invalid after release, no delayed allocation under old ID");
    check(rw::TexDictionary::getCurrent()==dictionary && rw::Engine::state==rw::Engine::Started,"sealed creation/pose/visibility/requirements never enter RW parser");
    std::printf("NativePickupsProbe failures=%d actualMission=%llu strict=%04X@%u collection=UNSUPPORTED-task-event-authority save-menu=unported\n",
        failures,static_cast<unsigned long long>(commands),opcode,ip);
    return failures;
}

#ifdef NATIVE_PICKUPS_STANDALONE
int main(int argc,char** argv) try {
    const char* dir=argc>1 ? argv[1] : "/game";
    E2ELoadInfo info{}; char error[512]{};
    Require(StreamPager_Init(dir,info,error,sizeof(error),{.includeStreamed=true,.radius=300,.maxInstances=1200}),error);
    std::uint64_t commands{}; std::uint16_t opcode{}; std::uint32_t ip{};
    const auto failures=NativePickupsProbe(dir,commands,opcode,ip); StreamPager_Shutdown(); return failures ? 1 : 0;
} catch (const std::exception& e) { std::fprintf(stderr,"NativePickupsProbe FAIL %s\n",e.what()); return 2; }
#endif
