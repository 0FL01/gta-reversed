// Independent recursive/list oracle versus production flattened-leaf lookup.
// Generated rows below contain no game-asset text. Real-asset checks are numeric.
#include "app/platform/linux/RealtimeScriptHost.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <list>
#include <memory>
#include <stdexcept>

namespace {
void Check(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
struct Oracle {
    NativeEntryExitRect Rect;
    unsigned Depth;
    std::list<std::size_t> Items;
    std::array<std::unique_ptr<Oracle>,4> Children;
    NativeEntryExitRect Sector(unsigned n) const {
        auto r = Rect; const float x=(r.Left+r.Right)/2,y=(r.Bottom+r.Top)/2;
        switch (n) { case 0:r.Right=x;r.Bottom=y;break; case 1:r.Left=x;r.Bottom=y;break;
            case 2:r.Right=x;r.Top=y;break; case 3:r.Left=x;r.Top=y;break; }
        return r;
    }
    bool Inside(NativeEntryExitRect r, unsigned n) const {
        const auto s=Sector(n); return s.Left<=r.Right && s.Right>=r.Left && s.Bottom<=r.Top && s.Top>=r.Bottom;
    }
    void Add(std::size_t i, NativeEntryExitRect r) {
        if (!Depth) { Items.push_front(i); return; }
        for (unsigned n=0;n<4;++n) if (Inside(r,n)) {
            if (!Children[n]) Children[n]=std::make_unique<Oracle>(Oracle{Sector(n),Depth-1,{},{}});
            Children[n]->Add(i,r);
        }
    }
    void Query(NativeEntryExitRect r, std::list<std::size_t>& out) const {
        for (auto i:Items) out.push_front(i);
        for (unsigned n=0;n<4;++n) if (Children[n] && Inside(r,n)) Children[n]->Query(r,out);
    }
};
NativeEntryExit Row(const std::string& text, bool closed=false) {
    NativeEntryExit e; std::string error; Check(NativeEntryExits::ParseRow(text,e,closed,error),error.c_str()); return e;
}
void Fixtures() {
    NativeEntryExit untouched; untouched.Area=19; std::string error;
    for (const auto* bad : {"0 0", "nan 0 0 0 2 2 8 10 10 10 0 0 4 \"A\" 0 2 0 24",
        "0 0 0 0 -1 2 8 10 10 10 0 0 4 \"A\" 0 2 0 24", "0 0 0 0 2 2 8 10 10 10 0 0 4 \"TOOLONG8\" 0 2 0 24",
        "0 0 0 0 2 2 8 10 10 10 0 0 4 \"A\" 0 2 0 24 extra"})
        Check(!NativeEntryExits::ParseRow(bad,untouched,false,error) && untouched.Area==19,"bounded parser failure atomic");
    auto burglary=Row("0 0 1 0 2 2 8 10 10 10 90 0 5120 \"B\" 1 2 20 6",true);
    Check(burglary.Flags==5120 && burglary.TimeOn==0 && burglary.TimeOff==0 && burglary.Center.Z==2 && burglary.Exit.Z==11,"constructor burglary random branch/Z offsets");
    burglary=Row("0 0 1 0 2 2 8 10 10 10 90 0 5120 \"B\" 1 2 20 6",false);
    Check(burglary.TimeOff==24,"constructor burglary fallback hours");
    const auto rotated=Row("100 100 0 45 4 2 8 10 10 10 0 0 0 \"R\" 0 2 0 24");
    Check(std::abs(rotated.Bounds.Right-100.70710678f)<0.00002f && std::abs(rotated.Bounds.Top-102.12132034f)<0.00002f,
        "source two-opposite-corners bound, not conventional rotated AABB");
    std::vector<NativeEntryExit> rows{
        Row("-1 1 0 0 2 2 8 10 10 10 0 0 4 \"PAIR\" 0 2 20 6"),
        Row("1 1 0 0 2 2 8 10 10 10 0 0 0 \"TIE\" 0 2 0 24"),
        Row("100 100 0 0 2 2 8 10 10 10 0 7 0 \"pair\" 0 2 22 5"),
        Row("2 1 0 0 2 2 8 10 10 10 0 0 4 \"PAIR\" 0 2 0 24"), rotated
    };
    NativeEntryExits registry; Check(registry.InitializeRegistry(rows,error),error.c_str());
    Check(registry.Entries()[0].Link==2 && registry.Entries()[3].Link==2 && registry.Entries()[2].Link==-1 &&
        registry.Entries()[2].TimeOn==0 && registry.Entries()[2].TimeOff==24,"allocation-time plus post-creation source one-way linking/name case");
    Oracle oracle{{-3000,-3000,3000,3000},4,{},{}};
    for (std::size_t i=0;i<rows.size();++i) oracle.Add(i,rows[i].Bounds);
    std::size_t queries=0;
    for (float x : {-3001.0f,-3000.0f,-375.0f,-1.0f,0.0f,1.0f,100.0f,375.0f,3000.0f})
        for (float y : {-3000.0f,-375.0f,0.0f,1.0f,100.0f,375.0f,3000.0f})
            for (float r : {0.0f,0.5f,1.0f,10.0f,400.0f}) {
                const NativeEntryExitRect rect{x-r,y-r,x+r,y+r}; std::list<std::size_t> expected; oracle.Query(rect,expected);
                Check(registry.Candidates(rect)==std::vector<std::size_t>(expected.begin(),expected.end()),"recursive prepend oracle candidate order/duplicates");
                ++queries;
            }
    Check(registry.FindNearest(0,1,1)==0,"boundary duplicate in same leaf uses ascending pool order on XY tie");
    NativeEntryExits split;
    Check(split.InitializeRegistry({Row("-1 1 0 0 0.2 0.2 8 0 0 0 0 0 0 \"L\" 0 2 0 24"),
        Row("1 1 0 0 0.2 0.2 8 0 0 0 0 0 0 \"R\" 0 2 0 24")},error),error.c_str());
    Check(split.FindNearest(0,1,1)==1,"distinct leaves reverse sector order wins XY tie over pool order");
    Check(registry.FindNearest(-1,1,1,7)==-1,"nearest ignores LINKED area and strict 2r excludes distance exactly 2");
    Check(registry.FindNearest(-2.5f,1,1)==0,"nearest may accept outside query circle/radius (coarse leaf, XY <2r)");
    NativeEntryExitView view; view.Player=registry.Entries()[0].Center; view.Hour=23; view.CanStartMission=true;
    auto activation=registry.Activation(view);
    Check(activation.Status==NativeEntryExitActivationStatus::TransitionRequired && activation.Transition.Destination==2,"overnight source eligibility yields typed destination");
    view.Hour=6; Check(registry.Activation(view).Status==NativeEntryExitActivationStatus::None,"time-off exclusive");
    view.Hour=23; view.Player.Z+=1; Check(registry.Activation(view).Status==NativeEntryExitActivationStatus::None,"source activation Z strict <1");
    view.Player.Z-=1; view.Vehicle=NativeEntryExitVehicle::Other; Check(registry.Activation(view).Status==NativeEntryExitActivationStatus::None,"source vehicle restriction");
    NativeEntryExits rotation; Check(rotation.InitializeRegistry({rotated},error),error.c_str());
    view.Player={101.5f,99,1}; view.Vehicle=NativeEntryExitVehicle::OnFoot;
    Check(rotation.Activation(view).Status==NativeEntryExitActivationStatus::TransitionRequired,"activation applies source positive entrance rotation, not inverse");
    const auto eligible = [&](unsigned flags, NativeEntryExitVehicle vehicle, bool big=false) {
        auto e=rows[1]; e.Flags=std::uint16_t(0x4000|flags); NativeEntryExits test;
        Check(test.InitializeRegistry({e},error),error.c_str()); view.Player=e.Center; view.Vehicle=vehicle; view.BigVehicle=big;
        return test.Activation(view).Status==NativeEntryExitActivationStatus::TransitionRequired;
    };
    Check(eligible(0x80,NativeEntryExitVehicle::OnFoot) && !eligible(0x800,NativeEntryExitVehicle::OnFoot),"TransitionStarted on-foot gate uses 0x800, not similarly named 0x80");
    Check(eligible(0x20,NativeEntryExitVehicle::Automobile) && !eligible(0x20,NativeEntryExitVehicle::Automobile,true) &&
        eligible(0x40,NativeEntryExitVehicle::Bike) && !eligible(0x20,NativeEntryExitVehicle::Bike),"source automobile/bike/big-vehicle eligibility bits");
    registry.SealStartup(); Check(!registry.InitializeRegistry({},error),"sealed startup immutable registry");
    std::printf("enex fixtures PASS recursive-list-oracle=%zu boundaries=ties,2r,linked-area,duplicates,rotation parser=18 fields\n",queries);
}
}
void NativeEntryExitsProbe(RealtimeScriptHost& host) {
    Fixtures(); auto& registry=host.EntryExits(); const auto entries=registry.Entries();
    Check(registry.IPLCount()==52 && entries.size()==376 && registry.Revision()==2,"actual entire DAT IPL population and two writes");
    std::size_t linked=0,external=0,access=0,changed=0;
    for (std::size_t i=0;i<entries.size();++i) {
        const auto& e=entries[i]; linked+=e.Link!=-1; external+=e.Area==0; access+=(e.Flags&0x4000)!=0;
        const auto initial=std::uint16_t(e.AuthoredFlags | ((e.AuthoredFlags&0x1000)?0:0x4000));
        if (e.Flags!=initial) { ++changed; Check((i==190 || i==46) && initial==0x4004 && e.Flags==4,"only two real SCM targets changed their access word"); }
    }
    Check(linked==227 && external==222 && access==337 && changed==2,"actual IPL link/area/access census");
    Check(entries[190].Row==8 && entries[190].Line==467 && entries[190].Link==-1 && entries[190].Area==10 &&
        entries[46].Row==6 && entries[46].Line==231 && entries[46].Link==370 && entries[370].Flags==0x4000,"actual target provenance and unchanged linked destination");
    Check(registry.FindNearest(426.4971923828125f,2530.68896484375f,10)==190 &&
        registry.FindNearest(316.0696105957031f,-1772.56884765625f,10)==46,"actual typed operands resolve source target indices");
    Check(registry.FindNearest(2488.562255859375f,-1666.864501953125f,10)==-1 &&
        registry.FindNearest(2488.562255859375f,-1666.864501953125f,30)==28 && entries[28].Flags==0x4004,"startup entrance distinct and enabled");
    const auto& events=host.Events(); Check(events.size()==9 && events[7].Opcode==0x09B4 && events[8].Opcode==0x09B4 &&
        events[7].Id.IP==201006 && events[8].Id.IP==201054 && events[7].Id.Instruction==180 && events[8].Id.Instruction==184,
        "actual mission command127/131 host journal identities");
    NativeScriptEntryExitFlagRequest replay{events[7].Id,events[7].Arguments[0],events[7].Arguments[1],events[7].Arguments[2],16384,0};
    Check(host.SetEntryExitFlag(replay).Status==NativeScriptServiceStatus::Ready && registry.Revision()==2,"actual replay once");
    replay.State=1; Check(host.SetEntryExitFlag(replay).Status==NativeScriptServiceStatus::Error && registry.Revision()==2,"same ID changed integer state atomic rejection");
    replay.State=0; replay.Mask|=65536; Check(host.SetEntryExitFlag(replay).Status==NativeScriptServiceStatus::Error,"same ID changed high mask bits rejected");
    replay.Id=events.front().Id; Check(host.SetEntryExitFlag(replay).Status==NativeScriptServiceStatus::Error,"world-owned ID rejected by ENEX");
    replay.Id={events.front().Id.Session,171,200868}; Check(host.SetEntryExitFlag(replay).Status==NativeScriptServiceStatus::Error,"property-owned ID rejected by ENEX");
    const auto enexId=events[7].Id;
    Check(host.SetBlipDisplay({enexId,{65536},2}).Status==NativeScriptServiceStatus::Error &&
        host.RequestCollision({enexId,1,2}).Status==NativeScriptServiceStatus::Error,"ENEX-owned ID rejected by property and world");
    replay.Id={910,1,1}; replay.X=9000; replay.Y=9000; replay.Radius=10;
    Check(host.SetEntryExitFlag(replay).Status==NativeScriptServiceStatus::Error && registry.Revision()==2 && host.Events().size()==9,"no-match failure atomic without journal ownership");
    std::string error; Check(!registry.LoadBeforeWorker("/invalid",error),"sealed startup rejects IO before path lookup");
    std::printf("enex actual PASS IPL=%zu records=%zu linked=%zu exterior=%zu access=%zu writes=%zu mission127=190:4004->0004 mission131=46:4004->0004 startup=28:4004 extent=IPL-only\n",
        registry.IPLCount(),entries.size(),linked,external,access,changed);
    // Same previously failed identity may be used now: failures own nothing.
    replay.X=entries[46].Center.X; replay.Y=entries[46].Center.Y; replay.Radius=1; replay.Mask=0x14000; replay.State=-2;
    Check(host.SetEntryExitFlag(replay).Status==NativeScriptServiceStatus::Ready && entries[46].Flags==0x4004 && entries[370].Flags==0x4000,
        "source low16 mask + any nonzero integer state, no linked propagation");
    replay.Id.Instruction++; replay.Mask=0x4000; replay.State=0;
    Check(host.SetEntryExitFlag(replay).Status==NativeScriptServiceStatus::Ready && entries[46].Flags==4,"restore diagnosed access word after independent service fixture");
}
