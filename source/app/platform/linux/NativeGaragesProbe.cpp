#include "app/platform/linux/RealtimeScriptHost.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>
#include <rw.h>

namespace {
void Require(bool ok,const std::string& error) { if (!ok) { std::fprintf(stderr,"garage probe: %s\n",error.c_str()); std::exit(2); } }
// Diagnostic-only structured DFF reader. Vehicle camera tests use their real
// embedded COL, not render bounds or a pretend car-sized capsule. No dumps.
std::shared_ptr<const NativeCollisionModel> VehicleCol(const char* dir,int id,const std::string& model) {
    std::ifstream ide(std::string(dir)+"/data/vehicles.ide"); std::string line; bool identity=false;
    while (std::getline(ide,line)) {
        std::replace(line.begin(),line.end(),',',' '); std::istringstream row(line); int number; std::string name;
        if (row>>number>>name && number==id) identity=name==model;
    }
    Require(identity,"actual vehicle IDE identity");
    std::ifstream image(std::string(dir)+"/models/gta3.img",std::ios::binary);
    const auto read=[&](std::size_t offset,std::size_t count) {
        Require(count<=16*1024*1024,"diagnostic vehicle read bound"); std::vector<std::uint8_t> data(count);
        image.seekg(offset); image.read(reinterpret_cast<char*>(data.data()),count); Require(bool(image),"diagnostic vehicle asset read"); return data;
    };
    const auto word=[](const auto& data,std::size_t offset,int n=4) {
        Require(offset+n<=data.size(),"diagnostic DFF integer range"); std::uint32_t value=0;
        for (int i=0;i<n;++i) value|=std::uint32_t(data[offset+i])<<(i*8);
        return value;
    };
    const auto header=read(0,8); Require(!std::memcmp(header.data(),"VER2",4),"diagnostic vehicle IMG format");
    const auto count=word(header,4); Require(count<100000,"vehicle IMG directory bound");
    const auto directory=read(8,count*32); std::vector<std::uint8_t> dff;
    for (std::size_t p=0;p<directory.size();p+=32) {
        const std::string name(reinterpret_cast<const char*>(directory.data()+p+8),strnlen(reinterpret_cast<const char*>(directory.data()+p+8),24));
        if (name==model+".dff") dff=read(std::size_t(word(directory,p))*2048,std::size_t(word(directory,p+6,2) ? word(directory,p+6,2) : word(directory,p+4,2))*2048);
    }
    Require(!dff.empty(),"actual vehicle DFF"); std::shared_ptr<NativeCollisionModel> result;
    const auto visit=[&](auto&& self,std::size_t first,std::size_t end,int depth)->void {
        Require(depth<5,"vehicle DFF nesting bound");
        for (std::size_t p=first;p<end;) {
            Require(p+12<=end,"vehicle DFF header range"); const auto type=word(dff,p),size=word(dff,p+4); Require(size<=end-p-12,"vehicle DFF chunk range");
            if (type==0x253F2FA) {
                Require(!result,"ambiguous embedded vehicle collision"); result=std::make_shared<NativeCollisionModel>(); std::string error;
                Require(NativeCollisionAssets::Parse(std::span(dff).subspan(p+12,size),"models/gta3.img:"+model+".dff/COL-plugin",*result,error),error);
            }
            if (type==0x10 || type==3) self(self,p+12,p+12+size,depth+1);
            p+=12+size;
        }
    };
    visit(visit,0,12+word(dff,4),0); Require(result && !result->Spheres.empty(),"actual vehicle COL spheres"); return result;
}
}
int NativeGaragesProbe(const char* dir,std::uint64_t& commands,std::uint16_t& opcode,std::uint32_t& ip) {
    // Consumer poses below are explicit probe inputs using real/source-built
    // COL. They never teleport the live host player or complete a transition.
    // main.scm progression is measured separately through actual host services.
    int failures=0;
    const auto check=[&](bool ok,const char* label) { std::printf("%s garage %s\n",ok ? "PASS" : "FAIL",label); if (!ok) ++failures; };
    std::string error;
    auto collision=NativeCollisionContext::LoadBeforeWorker(dir,900,error); Require(bool(collision),error);
    const auto carCol=VehicleCol(dir,411,"infernus"),kartCol=VehicleCol(dir,571,"kart"),iceCol=VehicleCol(dir,423,"mrwhoop"),bmxCol=VehicleCol(dir,481,"bmx");
    RealtimeGameplay gameplay; RealtimeScriptHost host(gameplay);
    Require(host.InitializeBeforeWorker(dir,error,collision),error);
    const auto* dictionary=rw::TexDictionary::getCurrent();
    auto& garages=host.Garages();
    check(garages.Entries().size()==50 && garages.Doors().size()==53 && garages.RejectedRows()==2 && garages.IplFiles()==52,"source50 registry +53 real door placements, two nameless rows rejected");
    const std::array<char,8> name{'B','E','A','C','S','V',0,0};
    const auto ref=garages.Find(name); Require(ref.has_value(),"BEACSV registered from IPL");
    const auto initial=*garages.Resolve(*ref);
    check(ref->Index==13 && initial.Type==17 && initial.Flags==0x48 && initial.DoorState==0 && initial.DoorPosition==0 && initial.Line==223 && initial.Record==2,"DAT order, actual type17 closed initialization and flags48");
    const std::array<char,8> duplicate{'s','p','r','l','a','e',0,0};
    std::size_t first=50,count=0;
    for (std::size_t i=0;i<50;++i) { auto n=garages.Entries()[i].Name; for (auto& c:n) if (c>='A' && c<='Z') c+='a'-'A'; if (n==duplicate) { first=std::min(first,i); ++count; } }
    check(count==2 && garages.Find(duplicate)->Index==first,"duplicate SPRLAE first case-insensitive pool match retained");
    check(!garages.Resolve({ref->Owner+1,ref->Index}) && !garages.Resolve({ref->Owner,50}),"foreign generation and out-of-pool garage references rejected");
    const auto doorIt=std::ranges::find_if(garages.Doors(),[&](const auto& d) { return d.Garage==ref; });
    Require(doorIt!=garages.Doors().end(),"bound actual target door");
    const auto door=*doorIt;
    check(door.Placement.ModelId==6517 && door.Placement.Record==81 && door.Placement.Binary && door.Collision->Name=="santagard_law2" && door.Collision->HeaderId==6377 && !door.Collision->ValidatedHeaderId,"streamed door6517 record81 binds correct COL by name despite header6377");
    check(door.CollisionEnabled && !door.RequiresDynamicPublication && door.SourcePose==door.Authored,"closed target actual source matrix/collision need no duplicate drawable");
    // Independent double scalar/quaternion oracle. No native Contains, Matrix,
    // FindForDoor, or cached SourcePose participates in the expected binding.
    bool bindings=true; std::size_t linked=0,empty=0,interior=0,changedPose=0;
    for (const auto& d:garages.Doors()) {
        linked+=d.Garage.has_value(); empty+=d.Collision->Empty; interior+=d.Placement.Interior!=0; changedPose+=d.RequiresDynamicPublication;
        const auto q=d.Placement.Quaternion; const double n=std::sqrt(double(q[0])*q[0]+double(q[1])*q[1]+double(q[2])*q[2]+double(q[3])*q[3]);
        const double x=-q[0]/n,y=-q[1]/n,z=-q[2]/n,w=q[3]/n;
        const double axes[3][3]{{1-2*(y*y+z*z),2*(x*y+z*w),2*(x*z-y*w)},
            {2*(x*y-z*w),1-2*(x*x+z*z),2*(y*z+x*w)}, {2*(x*z+y*w),2*(y*z-x*w),1-2*(x*x+y*y)}};
        double point[3]; for (int i=0;i<3;++i) { point[i]=d.Placement.Position[i]; for (int j=0;j<3;++j) point[i]+=axes[j][i]*d.Collision->BoundCenter[j]; }
        std::optional<std::size_t> wanted; double closest=99999.9;
        for (std::size_t i=0;i<garages.Entries().size();++i) {
            const auto& g=garages.Entries()[i]; const double dx=point[0]-g.Origin[0],dy=point[1]-g.Origin[1];
            const double a=dx*g.DirectionA[0]+dy*g.DirectionA[1],b=dx*g.DirectionB[0]+dy*g.DirectionB[1];
            if (point[2]<g.Origin[2]-7 || point[2]>g.Top+7 || a<-7 || a>g.Width+7 || b<-7 || b>g.Height+7) continue;
            const double cx=g.Origin[0]+g.DirectionA[0]*g.Width*.5+g.DirectionB[0]*g.Height*.5,cy=g.Origin[1]+g.DirectionA[1]*g.Width*.5+g.DirectionB[1]*g.Height*.5;
            const double distance=std::hypot(std::hypot(point[0]-cx,point[1]-cy),point[2]-g.Origin[2]);
            if (distance<closest) { closest=distance; wanted=i; }
        }
        bindings&=wanted==(d.Garage ? std::optional<std::size_t>(d.Garage->Index) : std::nullopt);
    }
    check(bindings && empty==2 && interior==1,"independent scalar oracle agrees on ALL53 real door bindings including empty COL/interior");
    std::printf("garage-source doors=%zu linked=%zu emptyCOL=%zu interior=%zu initialDynamicPublication=%zu\n",garages.Doors().size(),linked,empty,interior,changedPose);
    auto shape=initial; shape.Origin={0,0,0}; shape.Top=4; shape.DirectionA={1,0}; shape.DirectionB={0,1}; shape.Width=shape.Height=10; shape.Rect={0,10,0,10};
    const std::array<NativeGarageEntry,2> ties{shape,shape};
    check(NativeGarages::FindForDoor(ties,{5,5,0})==0 && NativeGarages::Contains(shape,{-7,5,0},7) && !NativeGarages::Contains(shape,{-7.01f,5,0},7),"TEST geometry strict first tie and inclusive7 oriented margin");
    NativeGarageView view;
    view.Player.Collision=garages.Ped1Collision();
    check(view.Player.Collision->Spheres.size()==3 && view.Player.Collision->Min[2]==-1.0f && view.Player.Collision->Spheres[2].Radius==.35f,"actual CTempColModels source ped1 constructor, not nonexistent archive entry or invented hull");
    view.Player.Matrix.Position={door.Authored.Position[0],door.Authored.Position[1]-2,door.Authored.Position[2]};
    view.Camera=view.Player.Matrix.Position;
    Require(garages.Tick(view,error),error);
    const auto initialEntries=garages.Entries();
    check(garages.Frame().Updates[13].Status==NativeScriptServiceStatus::Unsupported && garages.Frame().Updates[13].Requirement==NativeGarageRequirement::RestoreAndOpen && garages.Frame().Updates[13].RequestedState==3,"actual enabled eligible door emits typed restore/open requirement without fake completion");
    auto opened=initial; opened.Flags|=2; opened.DoorState=1; opened.DoorPosition=1;
    check(NativeGarages::UpdateCollisionFlags(opened)==0x0A,"inactive OPEN still clears source rotating-door collision bit");
    auto moving=opened; moving.DoorState=3; moving.DoorPosition=.4f;
    check(NativeGarages::UpdateCollisionFlags(moving)==0x4A,"opening fraction exactly.4 retains source collision");
    moving.DoorPosition=std::nextafter(.4f,1.0f);
    check(NativeGarages::UpdateCollisionFlags(moving)==0x0A,"opening fraction strictly above.4 clears source collision");
    auto far=view; far.Player.Matrix.Position[1]=initial.Rect[2]-5;
    check(NativeGarages::Transition(opened,far)==NativeGarageRequirement::DoorObstruction && NativeGarages::Transition(opened,view)==NativeGarageRequirement::None,"inactive OPEN still evaluates source closing obstruction; near onfoot stays open");
    auto outside=view; outside.Player.Matrix.Position[1]=initial.Rect[2]-3.5f;
    check(NativeGarages::Transition(initial,outside)==NativeGarageRequirement::None,"strict onfoot3.5 boundary");
    outside.Player.Matrix.Position[2]=950;
    check(NativeGarages::Transition(initial,outside)==NativeGarageRequirement::None,"source interior-height950 exclusion");
    auto driving=view; driving.Vehicle=NativeGarageEntityBounds{.Matrix=view.Player.Matrix,.Collision=carCol,.ModelId=411,.VehicleSubType=0};
    auto targetGarage=garages.Entries()[0];
    check(targetGarage.Type==1 && targetGarage.DoorState==0 && targetGarage.TargetVehicleRef<0 &&
        NativeGarages::Transition(targetGarage,driving,garages.Policy(),0)==NativeGarageRequirement::None,
        "retail 44F0A6 closed target-only garage exits when source target reference is null, even with a player car");
    targetGarage.TargetVehicleRef=42;
    check(NativeGarages::Transition(targetGarage,driving,garages.Policy(),0)==
        NativeGarageRequirement::SourceTypeUpdate,
        "target-only garage with a registered target still rejects the unported source body");
    const auto bombShop = garages.Entries()[3];
    check(bombShop.Type==2 && bombShop.DoorState==1 &&
        NativeGarages::DistanceSquared(bombShop,driving.Vehicle->Matrix.Position)>3600 &&
        NativeGarages::Transition(bombShop,driving,garages.Policy(),3)==NativeGarageRequirement::None,
        "retail 44DBE0 skips the open bomb shop when the actual vehicle COL is not entirely inside");
    driving.Vehicle->Matrix.Position[1]=initial.Rect[2]-9;
    check(NativeGarages::Transition(initial,driving)==NativeGarageRequirement::VehicleCapacity,"real model411 embedded COL input reaches source vehicle-capacity dependency inside10m");
    driving.Vehicle->Matrix.Position[1]=initial.Rect[2]-10;
    check(NativeGarages::Transition(initial,driving)==NativeGarageRequirement::None,"source strict vehicle10m boundary");
    driving.Vehicle->Matrix.Position[1]=initial.Rect[2]-5; driving.Vehicle->Collision=bmxCol; driving.Vehicle->ModelId=481; driving.Vehicle->VehicleSubType=10;
    check(NativeGarages::Transition(initial,driving)==NativeGarageRequirement::None,"actual BMX source subtype10 excludes outer vehicle activation band");
    driving.Vehicle->Matrix.Position[1]=initial.Rect[2]-2;
    check(NativeGarages::Transition(initial,driving)==NativeGarageRequirement::VehicleCapacity,"BMX still eligible in strict3.5m inner band; no fake stored-vehicle count");
    const auto firstPass=host.RunPass(1000);
    Require(firstPass.Status==NativeScriptStatus::Waiting,firstPass.Message);
    const auto mainBefore=static_cast<const NativeScriptThreadState&>(host.State()); host.SealStartup();
    const auto prefix=host.RunPass(135);
    Require(prefix.Status==NativeScriptStatus::BudgetYield && prefix.Executed==135,prefix.Message);
    const auto enexRevision=host.EntryExits().Revision(),entitiesRevision=host.Entities().Revision(),worldRevision=host.WorldRevision();
    const auto originalEvents=host.Events().size();
    const auto step=host.RunPass(1);
    check(step.Status==NativeScriptStatus::BudgetYield && step.Executed==1 && step.IP==201140 && host.Session().Threads()[1].Commands==136,"ACTUAL typed02B9@201129 commits command136 next201140");
    check(garages.Resolve(*ref)->Flags==0x4A && garages.Revision()==1 && host.Events().size()==originalEvents+1 && host.Events().back().Reference==13 && host.Events().back().Opcode==0x02B9,"actual host journal owns registered access-byte write once");
    check(host.EntryExits().Revision()==enexRevision && host.Entities().Revision()==entitiesRevision && host.WorldRevision()==worldRevision && static_cast<const NativeScriptThreadState&>(host.State())==mainBefore,"02B9 preserves ENEX/property/world/main state");
    ++view.Frame; Require(garages.Tick(view,error),error);
    check(garages.Frame().Updates[13].InactiveClosed && garages.Frame().Updates[13].Status==NativeScriptServiceStatus::Ready && garages.Frame().Updates[13].Requirement==NativeGarageRequirement::None && garages.Doors()[std::distance(garages.Doors().begin(),doorIt)].SourcePose==door.Authored,"SAME registry flags suppress actual eligible trigger, preserve static pose/collision");
    // Move the actual ped hull inside the now INACTIVE garage: camera prelude
    // must still publish its association, even though type update is skipped.
    view.Player.Matrix.Position={(initial.Rect[0]+initial.Rect[1])*.5f,(initial.Rect[2]+initial.Rect[3])*.5f,initial.Origin[2]+1.5f};
    ++view.Frame; Require(garages.Tick(view,error),error);
    check(garages.Frame().Camera.Apply && garages.Frame().Camera.Outside && garages.Frame().Camera.Garage==ref && garages.Frame().Updates[13].InactiveClosed,"source ped COL camera prelude runs BEFORE inactive+CLOSED gate");
    view.Vehicle=NativeGarageEntityBounds{.Matrix=view.Player.Matrix,.Collision=carCol,.ModelId=411,.VehicleSubType=0};
    ++view.Frame; Require(garages.Tick(view,error),error);
    check(garages.Frame().Camera.AvoidFirstPerson==ref,"actual vehicle COL volume drives source first-person avoidance");
    view.Vehicle->Collision=kartCol; view.Vehicle->ModelId=571; view.Player.Matrix.Position[0]=initial.Rect[0]-20;
    ++view.Frame; Require(garages.Tick(view,error),error);
    check(garages.Frame().Camera.Outside && garages.Frame().Camera.Garage==ref,"actual model571 kart uses VEHICLE COL for camera-inside test, not outside ped hull");
    view.Vehicle->Collision=iceCol; view.Vehicle->ModelId=423; view.Vehicle->Matrix.Position[1]=initial.Rect[2]-.4f;
    ++view.Frame; Require(garages.Tick(view,error),error);
    check(garages.Frame().Camera.Outside && garages.Frame().Camera.Garage==ref,"actual model423 source expanded XY camera special case");
    view.Vehicle.reset();
    const auto savedCamera=garages.Frame().Camera;
    ++view.Frame; view.Replay=true; Require(garages.Tick(view,error),error);
    check(!garages.Frame().Camera.Apply && garages.Frame().Camera.Garage==savedCamera.Garage && garages.Frame().Updates.empty(),"replay suppresses update without clearing existing camera association");
    view.Replay=false; view.Coop=true; ++view.Frame; Require(garages.Tick(view,error),error);
    check(!garages.Frame().Camera.Apply && garages.Frame().Updates.empty(),"cooperative global suppression");
    view.Coop=false;
    check(!garages.Tick(view,error) && garages.Frame().Updates.empty(),"duplicate frame rejects atomically");
    ++view.Frame; view.Player.Matrix.Position[0]=std::numeric_limits<float>::quiet_NaN();
    check(!garages.Tick(view,error) && garages.Frame().Updates.empty(),"nonfinite real hull input rejects atomically");
    view.Player.Matrix.Position={0,0,0}; view.Frame=12; Require(garages.Tick(view,error),error);
    check(garages.Frame().Maintenance && garages.Frame().Maintenance->Index==1 && garages.Frame().Updates.back().Requirement==NativeGarageRequirement::TidyUp && garages.Frame().Updates.back().Status==NativeScriptServiceStatus::Unsupported,"source increment-before-select maintenance is explicit unsupported, not silent empty-world cleanup");
    // Continue actual SCM BEFORE adding any synthetic journal/identity requests.
    const auto terminal=host.RunPass(1000);
    commands=host.Session().Threads()[1].Commands; opcode=terminal.Opcode; ip=terminal.IP;
    std::printf("garage ACTUAL continuation status=%d additional=%zu mission=%llu opcode=%04X ip=%u message=%s\n",int(terminal.Status),terminal.Executed,(unsigned long long)commands,opcode,ip,terminal.Message.c_str());
    check(terminal.Status==NativeScriptStatus::Unsupported && commands==539 && opcode==0x0570 && ip==205876 && terminal.Executed==403,"ACTUAL404 new instructions consume owned effects, strict0570@205876 mission539");
    check(garages.Revision()==13 && host.EntryExits().Revision()==30 && host.Entities().Revision()==109 && host.Events().size()==50,"actual13 garage writes,30 ENEX writes,45 pickups/32 radars with real owned effects");
    const std::array<std::size_t,13> indices{13,34,42,38,39,48,37,5,14,25,28,17,49}; bool exactFlags=true;
    for (std::size_t i=0;i<50;++i) {
        const auto& g=garages.Entries()[i]; const auto& old=initialEntries[i];
        exactFlags&=g.Flags==(old.Flags | (std::ranges::find(indices,i)!=indices.end() ? 2 : 0)) && g.DoorState==old.DoorState && g.DoorPosition==old.DoorPosition && g.TimeToOpen==old.TimeToOpen && g.OriginalType==old.OriginalType;
    }
    check(exactFlags,"independent expected13 actual named writes change ONLY inactive bits, retain door state/fraction/original types");
    const auto& last=host.Session().Threads()[1].LastOutputWrite;
    std::printf("garage actual-last-write sequence=%llu ip=%u variable=%u value=%d\n",(unsigned long long)last.Sequence,last.IP,last.Variable,last.Value);
    check(last.Sequence==462 && last.IP==205866 && last.Variable==7000 && last.Global,"actual last source numeric output before sprite0570");
    const auto faultState=host.Session().Threads()[1]; const auto repeated=host.RunPass(1000);
    check(repeated.Executed==0 && repeated.Status==terminal.Status && repeated.IP==ip && repeated.Opcode==opcode && host.Session().Threads()[1]==faultState && static_cast<const NativeScriptThreadState&>(host.State())==mainBefore,"actual terminal remains unadvanced and main WAIT unchanged");
    const auto revision=garages.Revision(); const auto journal=host.Events().size();
    const auto event=host.Events()[originalEvents];
    NativeScriptGarageRequest request{event.Id,event.Name};
    check(host.DeactivateGarage(request).Status==NativeScriptServiceStatus::Ready && garages.Revision()==revision && host.Events().size()==journal,"actual garage identity replay idempotent");
    request.Name[0]='X';
    check(host.DeactivateGarage(request).Status==NativeScriptServiceStatus::Error && garages.Revision()==revision && host.Events().size()==journal,"changed same-ID name rejected atomically");
    request={host.Events().front().Id,name};
    check(host.DeactivateGarage(request).Status==NativeScriptServiceStatus::Error,"garage cannot steal world identity");
    check(host.SetCameraBehindPlayer({event.Id}).Status==NativeScriptServiceStatus::Error && host.SetEntryExitFlag({event.Id,1,2,10,16384,0}).Status==NativeScriptServiceStatus::Error && host.CreateLockedProperty({event.Id,{1,2,3},{}}).Result.Status==NativeScriptServiceStatus::Error,"world/ENEX/property services cannot steal garage identity");
    request.Id={event.Id.Session,186,201080}; request.Name=name;
    check(host.DeactivateGarage(request).Status==NativeScriptServiceStatus::Error && garages.Revision()==revision,"garage cannot steal actual sale identity");
    request={{9000,1,1},{'n','o','n','e',0,0,0,0}};
    check(host.DeactivateGarage(request).Status==NativeScriptServiceStatus::Ready && garages.Revision()==revision && garages.Entries().size()==50 && host.Events().size()==journal+1,"source missing-name genuine Ready skip, no invented garage");
    check(rw::TexDictionary::getCurrent()==dictionary && rw::Engine::state==rw::Engine::Started,"sealed garage services/consumers borrow no RW state or asset IO");
    std::printf("garage-probe failures=%d firstpass=53 mission-prefix=%llu terminal=%04X@%u mission0-complete=0\n",failures,(unsigned long long)commands,opcode,ip);
    return failures;
}
#ifdef NATIVE_GARAGES_STANDALONE
int main(int argc,char** argv) {
    std::setvbuf(stdout,nullptr,_IONBF,0);
    const char* dir=argc>1 ? argv[1] : "/game"; char error[512]{}; E2ELoadInfo info;
    Require(StreamPager_Init(dir,info,error,sizeof(error),{.includeStreamed=true,.radius=300,.maxInstances=1200}),error);
    std::uint64_t commands{}; std::uint16_t opcode{}; std::uint32_t ip{};
    const int failures=NativeGaragesProbe(dir,commands,opcode,ip);
    StreamPager_Shutdown(); return failures ? 1 : 0;
}
#endif
