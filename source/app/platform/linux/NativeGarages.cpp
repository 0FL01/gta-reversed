#include "app/platform/linux/NativeGarages.h"
#include <algorithm>
#include <atomic>
#include <charconv>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <map>
#include <numbers>
#include <sstream>
#include <set>
#include <stdexcept>

using int32 = std::int32_t;
using uint32 = std::uint32_t;
using int64 = std::int64_t;
using uint64 = std::uint64_t;
#ifndef __stdcall
#define __stdcall
#endif
#include "oswrapper/oswrapper.h"

namespace {
using V = NativeCollisionVector;
void Require(bool ok, const std::string& why) { if (!ok) throw std::runtime_error(why); }
bool Finite(V v) { return std::ranges::all_of(v, [](float f) { return std::isfinite(f); }); }
V Cross(V a, V b) { return {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]}; }
V Transform(const NativeGarageMatrix& m, V p) {
    V out = m.Position;
    for (int i=0;i<3;++i) for (int j=0;j<3;++j) out[i] += m.Basis[j][i]*p[j];
    return out;
}
std::string Lower(std::string s) { for (auto& c:s) if (c>='A' && c<='Z') c += 'a'-'A'; return s; }
std::string Name(std::span<const char> text) {
    const auto end = std::find(text.begin(), text.end(), '\0');
    return Lower(std::string(text.begin(), end));
}
std::string Clean(std::string line) {
    if (auto p=line.find('#'); p!=line.npos) line.resize(p);
    const auto start=line.find_first_not_of(" \t\r\n");
    return start==line.npos ? std::string{} : line.substr(start,line.find_last_not_of(" \t\r\n")-start+1);
}
std::string Path(const char* root, std::string relative) {
    std::replace(relative.begin(),relative.end(),'\\','/');
    Require(!relative.empty() && relative.front()!='/' && relative.find(':')==relative.npos && relative.find("..") == relative.npos, "invalid garage DAT path");
    auto path=std::filesystem::absolute(root);
    for (const auto& part:std::filesystem::path(relative)) {
        std::filesystem::path match;
        for (const auto& child:std::filesystem::directory_iterator(path)) if (Lower(child.path().filename().string())==Lower(part.string())) {
            Require(match.empty(),"ambiguous garage asset path"); match=child.path();
        }
        Require(!match.empty(),"missing garage asset: "+relative); path=std::move(match);
    }
    return path.string();
}
std::string Read(const std::string& path) {
    struct File { void* Handle=nullptr; ~File() { if (Handle) OS_FileClose(Handle); } } f;
    Require(OS_FileOpen(FILE_DATA_AREA_DEFAULT,&f.Handle,path.c_str(),FILE_ACCESS_READ)==0 && f.Handle,"garage open: "+path);
    const auto size=OS_FileSize(f.Handle);
    Require(size>=0 && size<=16*1024*1024,"garage text size bound");
    std::string text(size,'\0'); Require(OS_FileRead(f.Handle,text.data(),size)==0,"garage text read"); return text;
}
template<class T> T Number(const std::string& text) {
    T n{}; auto [end,ec]=std::from_chars(text.data(),text.data()+text.size(),n);
    Require(ec==std::errc{} && end==text.data()+text.size(),"garage numeric field");
    if constexpr (std::is_floating_point_v<T>) Require(std::isfinite(n),"nonfinite garage field");
    return n;
}
template<class F> void Sections(const std::string& text, F consume) {
    std::istringstream input(text); std::string line,section; uint32 number=0,row=0;
    while (std::getline(input,line)) {
        ++number; line=Clean(line); if (line.empty()) continue;
        if (Lower(line)=="end") { section.clear(); continue; }
        if (section.empty()) { section=Lower(line); row=0; continue; }
        ++row; std::replace(line.begin(),line.end(),',',' ');
        std::istringstream fields(line); std::vector<std::string> words; std::string word;
        while (fields>>word) { Require(words.size()<128,"garage field bound"); words.push_back(word); }
        consume(section,number,row,words);
    }
}
bool Hideout(int type) {
    return type==16 || type==17 || type==18 || (type>=24 && type<=32) || (type>=39 && type<=42) || type==44 || type==45;
}
bool OpenAtStart(int type) {
    // InitDoorsAtStart translation table, verified by NativeGaragesSourceProbe.
    return type==2 || type==3 || type==4 || type==5;
}
bool ValidEntity(const NativeGarageEntityBounds& entity) {
    if (!entity.Collision || !entity.Collision->Unsupported.empty() || !Finite(entity.Matrix.Position)) return false;
    for (const auto& axis:entity.Matrix.Basis) if (!Finite(axis)) return false;
    for (const auto& s:entity.Collision->Spheres) if (!Finite(s.Center) || !std::isfinite(s.Radius) || s.Radius<0) return false;
    return true;
}
NativeGarageMatrix Matrix(const NativeCollisionPlacement& p) {
    const auto q=p.Quaternion; float norm=0; for (auto v:q) norm+=v*v;
    Require(std::isfinite(norm) && norm>1e-12f && Finite(p.Position),"invalid garage door transform"); norm=std::sqrt(norm);
    const float x=-q[0]/norm,y=-q[1]/norm,z=-q[2]/norm,w=q[3]/norm;
    return {p.Position,{{{1-2*(y*y+z*z),2*(x*y+z*w),2*(x*z-y*w)},
        {2*(x*y-z*w),1-2*(x*x+z*z),2*(y*z+x*w)}, {2*(x*z+y*w),2*(y*z-x*w),1-2*(x*x+y*y)}}}};
}
// The immutable world snapshot intentionally omits empty collision models.
// Retain those valid door bounds too: bounded, requested-name-only startup
// lookup, using the SAME source COL parser. Never called by a frame/service.
std::map<std::string,std::shared_ptr<const NativeCollisionModel>> EmptyDoorBounds(const char* gameDir,std::set<std::string> needed,const NativeCollisionPopulation& population) {
    std::map<std::string,std::shared_ptr<const NativeCollisionModel>> found;
    for (const auto relative:{"models/gta3.img","models/gta_int.img","models/player.img"}) {
        if (needed.empty()) break;
        struct File { void* Handle=nullptr; ~File() { if (Handle) OS_FileClose(Handle); } } f;
        const auto path=Path(gameDir,relative);
        Require(!OS_FileOpen(FILE_DATA_AREA_DEFAULT,&f.Handle,path.c_str(),FILE_ACCESS_READ) && f.Handle,"empty door IMG open");
        const auto size=OS_FileSize(f.Handle); Require(size>=8,"empty door IMG size");
        const auto read=[&](std::size_t offset,std::size_t count) {
            Require(offset<=static_cast<std::size_t>(size) && count<=static_cast<std::size_t>(size)-offset && count<=16*1024*1024,"empty door IMG range");
            std::vector<std::uint8_t> data(count); OS_FileSetPosition(f.Handle,static_cast<int32>(offset));
            Require(!OS_FileRead(f.Handle,data.data(),static_cast<int32>(count)),"empty door IMG read"); return data;
        };
        const auto word=[](const auto& bytes,std::size_t offset,int width=4) {
            uint32 v=0; Require(offset+width<=bytes.size(),"empty door integer range");
            for (int i=0;i<width;++i) v|=uint32(bytes[offset+i])<<(i*8);
            return v;
        };
        const auto header=read(0,8); Require(!std::memcmp(header.data(),"VER2",4),"empty door IMG format");
        const auto count=word(header,4); Require(count<100000,"empty door IMG directory bound");
        const auto directory=read(8,count*32);
        for (std::size_t e=0;e<directory.size() && !needed.empty();e+=32) {
            const auto name=Lower(std::string(reinterpret_cast<const char*>(directory.data()+e+8),strnlen(reinterpret_cast<const char*>(directory.data()+e+8),24)));
            if (!name.ends_with(".col")) continue;
            const std::size_t base=std::size_t(word(directory,e))*2048;
            const std::size_t bytes=std::size_t(word(directory,e+6,2) ? word(directory,e+6,2) : word(directory,e+4,2))*2048;
            for (std::size_t p=0;p+32<=bytes;) {
                const auto h=read(base+p,32);
                if (std::memcmp(h.data(),"COLL",4) && std::memcmp(h.data(),"COL2",4) && std::memcmp(h.data(),"COL3",4) && std::memcmp(h.data(),"COL4",4)) break; // sector slack; catalog already validated
                const std::size_t length=std::size_t(word(h,4))+8; Require(length>=32 && length<=bytes-p,"empty door COL range");
                const auto model=Lower(std::string(reinterpret_cast<const char*>(h.data()+8),strnlen(reinterpret_cast<const char*>(h.data()+8),22)));
                if (needed.contains(model)) {
                    auto col=std::make_shared<NativeCollisionModel>(); std::string error;
                    Require(NativeCollisionAssets::Parse(read(base+p,length),std::string(relative)+":"+name,*col,error),error);
                    Require(col->Empty,"nonempty door unexpectedly missing from collision snapshot");
                    const auto ide=population.Models.find(col->HeaderId);
                    col->ValidatedHeaderId=ide!=population.Models.end() && Lower(ide->second.Name)==col->Name;
                    col->ChunkOffset=p; found.emplace(model,std::move(col)); needed.erase(model);
                }
                p+=length;
            }
        }
    }
    Require(needed.empty(),"source empty-door bounds unavailable"); return found;
}
}

bool NativeGarages::LoadBeforeWorker(const char* gameDir, const NativeCollisionContext& collision, std::string& error) try {
    Require(gameDir && *gameDir && !m_Sealed && !m_Owner,"garage load must precede worker, once");
    Require(collision.Population.IncludesStreamed,"garages require text and streamed IPL population");
    NativeGarages next;
    // CTempColModels::Initialise, TempColModels.cpp:38-55. Actual source
    // constructor geometry; FileLoader.cpp:1483 assigns it to every ped model.
    auto ped=std::make_shared<NativeCollisionModel>();
    ped->Name="ped1"; ped->Library="CTempColModels::Initialise";
    ped->Min={-.35f,-.35f,-1.0f}; ped->Max={.35f,.35f,.95f}; ped->BoundRadius=1;
    for (int i=0;i<3;++i) ped->Spheres.push_back({{0,0,-.2f+i*.4f},.35f,{62,static_cast<std::uint8_t>(i),0,0}});
    next.m_Ped1Collision=std::move(ped);
    std::vector<std::string> ipls,ides;
    for (const auto dat:{"data/default.dat","data/gta.dat"}) {
        std::istringstream input(Read(Path(gameDir,dat))); std::string line;
        while (std::getline(input,line)) {
            std::istringstream fields(Clean(line)); std::string key,path; fields>>key>>path; key=Lower(key);
            if (key=="ipl") ipls.push_back(path); else if (key=="ide") ides.push_back(path);
        }
    }
    next.m_IplFiles=ipls.size();
    for (auto path:ipls) {
        Sections(Read(Path(gameDir,path)),[&](const auto& section,uint32 line,uint32 row,const auto& words) {
            if (section!="grge") return;
            // CFileLoader::LoadGarage accepts exactly eleven conversions. The
            // shipped two nameless SF rows are not registrations.
            if (words.size()<11) { ++next.m_RejectedRows; return; }
            Require(words.size()==11 && next.m_Entries.size()<50,"garage registration capacity/field bound");
            NativeGarageEntry g; g.Ipl=path; std::replace(g.Ipl.begin(),g.Ipl.end(),'\\','/'); g.Line=line; g.Record=row;
            float v[8]; for (int i=0;i<8;++i) v[i]=Number<float>(words[i]);
            const auto flags=Number<uint32>(words[8]),type=Number<uint32>(words[9]);
            Require(type>=1 && type<=45,"unsupported authored garage type");
            g.Type=g.OriginalType=type; g.Flags=((flags&7)<<3)|0x40;
            g.Origin={v[0],v[1],v[2]}; g.Top=v[7]; Require(g.Top>=g.Origin[2],"inverted garage Z bounds");
            g.DirectionA={v[3]-v[0],v[4]-v[1]}; g.DirectionB={v[5]-v[0],v[6]-v[1]};
            g.Width=std::hypot(g.DirectionA[0],g.DirectionA[1]); g.Height=std::hypot(g.DirectionB[0],g.DirectionB[1]);
            Require(g.Width>0 && g.Height>0,"degenerate garage axes");
            for (int i=0;i<2;++i) { g.DirectionA[i]/=g.Width; g.DirectionB[i]/=g.Height; }
            const float x4=v[3]+v[5]-v[0],y4=v[4]+v[6]-v[1];
            g.Rect={std::min({v[0],v[3],v[5],x4}),std::max({v[0],v[3],v[5],x4}),std::min({v[1],v[4],v[6],y4}),std::max({v[1],v[4],v[6],y4})};
            std::copy_n(words[10].begin(),std::min<std::size_t>(7,words[10].size()),g.Name.begin());
            g.DoorState=OpenAtStart(type) ? 1 : 0; g.DoorPosition=OpenAtStart(type) ? 1.0f : 0.0f;
            next.m_Entries.push_back(std::move(g));
        });
    }
    Require(!next.m_Entries.empty(),"no source garages registered");
    std::map<int,bool> doorModels;
    for (const auto& path:ides) Sections(Read(Path(gameDir,path)),[&](const auto& section,uint32,uint32,const auto& words) {
        if (section!="objs" && section!="tobj") return;
        Require(words.size()>=5,"garage IDE field bound");
        const auto flags=Number<uint32>(words[words.size()-(section=="tobj" ? 3 : 1)]);
        // SetAtomicModelInfoFlags: later special types supersede garage-door.
        doorModels[Number<int>(words[0])]=(flags&0x800) && !(flags&(0x2000|0x4000|0x80000|0x100000));
    });
    NativeCollisionPopulation doors; doors.Models=collision.Population.Models; doors.IncludesStreamed=true;
    for (const auto& placement:collision.Population.Instances) if (doorModels[placement.ModelId]) doors.Instances.push_back(placement);
    std::set<int> interiors; for (const auto& p:doors.Instances) interiors.insert(p.Interior);
    std::vector<NativeCollisionInstance> instances;
    for (const auto interior:interiors) {
        NativeCollisionSnapshot snapshot;
        if (!collision.Assets.Snapshot(doors,0,0,20000,snapshot,error,interior)) {
            Require(error=="no source COL in collision window",error); // validated below as real empty models, never no-op
        } else {
            Require(!snapshot.MissingModels,"missing source garage door collision");
            for (auto& instance:snapshot.Instances) instances.push_back(std::move(instance));
        }
    }
    const auto match=[&](const NativeCollisionPlacement& p) { return std::ranges::find_if(instances,[&](const auto& i) { return i.Placement.Ipl==p.Ipl && i.Placement.Record==p.Record && i.Placement.Binary==p.Binary && i.Placement.ModelId==p.ModelId; }); };
    std::set<std::string> empty; for (const auto& p:doors.Instances) if (match(p)==instances.end()) empty.insert(Lower(p.Model));
    const auto emptyBounds=EmptyDoorBounds(gameDir,std::move(empty),collision.Population);
    static std::atomic<uint64> owners{0}; next.m_Owner=++owners;
    for (const auto& p:doors.Instances) {
        const auto instance=match(p);
        NativeGarageDoor d; d.Placement=p;
        d.Collision=instance!=instances.end() ? instance->Model : emptyBounds.at(Lower(p.Model));
        d.Authored=instance!=instances.end() ? NativeGarageMatrix{p.Position,instance->Basis} : Matrix(p); d.SourcePose=d.Authored;
        const auto center=Transform(d.Authored,d.Collision->BoundCenter);
        if (const auto index=FindForDoor(next.m_Entries,center)) d.Garage=NativeGarageRef{next.m_Owner,*index};
        if (d.Garage) {
            const auto& g=next.m_Entries[d.Garage->Index]; d.SourcePose=DoorPose(g,d);
            // Initial object publication includes the first common update.
            // Registry flags retain InitDoorsAtStart until Tick really runs.
            // This prevents an immutable worker override from colliding with
            // an OPEN rotating door after the source clears its collision bit.
            d.CollisionEnabled=UpdateCollisionFlags(g)&0x40;
            d.RequiresDynamicPublication=d.SourcePose!=d.Authored || !d.CollisionEnabled;
        }
        next.m_Doors.push_back(std::move(d));
    }
    *this=std::move(next); error.clear(); return true;
} catch (const std::exception& e) { error=e.what(); return false; }

std::optional<NativeGarageRef> NativeGarages::Find(std::span<const char> name) const {
    const auto key=Name(name);
    for (std::size_t i=0;i<m_Entries.size();++i) if (Name(m_Entries[i].Name)==key) return NativeGarageRef{m_Owner,i};
    return {};
}
const NativeGarageEntry* NativeGarages::Resolve(NativeGarageRef ref) const {
    return m_Owner && ref.Owner==m_Owner && ref.Index<m_Entries.size() ? &m_Entries[ref.Index] : nullptr;
}
NativeScriptServiceResult NativeGarages::Deactivate(std::span<const char> name) {
    if (!m_Owner) return {NativeScriptServiceStatus::Error,"garage registry not initialized"};
    if (const auto ref=Find(name)) { m_Entries[ref->Index].Flags|=2; ++m_Revision; }
    // Original handler explicitly skips a missing name. Never allocate a stub.
    return {NativeScriptServiceStatus::Ready,{}};
}
NativeScriptServiceResult NativeGarages::ChangeType(std::span<const char> name, std::int32_t type) {
    if (!m_Owner) return {NativeScriptServiceStatus::Error,"garage registry not initialized"};
    if (type < 0 || type > 255) return {NativeScriptServiceStatus::Error,"garage type is out of byte range"};
    if (const auto ref=Find(name)) { m_Entries[ref->Index].Type=std::uint8_t(type); ++m_Revision; }
    return {NativeScriptServiceStatus::Ready,{}};
}
bool NativeGarages::Contains(const NativeGarageEntry& g, V p, float margin) {
    const float x=p[0]-g.Origin[0],y=p[1]-g.Origin[1];
    const float a=x*g.DirectionA[0]+y*g.DirectionA[1],b=x*g.DirectionB[0]+y*g.DirectionB[1];
    return p[2]>=g.Origin[2]-margin && p[2]<=g.Top+margin && a>=-margin && a<=g.Width+margin && b>=-margin && b<=g.Height+margin;
}
float NativeGarages::DistanceSquared(const NativeGarageEntry& g, V p) {
    const float x=std::max({g.Rect[0]-p[0],0.0f,p[0]-g.Rect[1]}),y=std::max({g.Rect[2]-p[1],0.0f,p[1]-g.Rect[3]}); return x*x+y*y;
}
std::optional<std::size_t> NativeGarages::FindForDoor(std::span<const NativeGarageEntry> entries,V center) {
    std::optional<std::size_t> result;
    float best=99999.9f;
    for (std::size_t i=0;i<entries.size();++i) {
        const auto& g=entries[i]; if (!Contains(g,center,7.0f)) continue;
        const V middle{g.Origin[0]+g.DirectionA[0]*g.Width*.5f+g.DirectionB[0]*g.Height*.5f,
            g.Origin[1]+g.DirectionA[1]*g.Width*.5f+g.DirectionB[1]*g.Height*.5f,g.Origin[2]};
        float distance=0; for (int j=0;j<3;++j) distance+=(center[j]-middle[j])*(center[j]-middle[j]); distance=std::sqrt(distance);
        if (distance<best) { best=distance; result=i; }
    }
    return result;
}
bool NativeGarages::EntirelyInside(const NativeGarageEntry& g,const NativeGarageEntityBounds& e,float margin) {
    const auto p=e.Matrix.Position;
    if (p[0]<g.Rect[0]-margin || p[0]>g.Rect[1]+margin || p[1]<g.Rect[2]-margin || p[1]>g.Rect[3]+margin || p[2]<g.Origin[2]-margin || p[2]>g.Top+margin) return false;
    return e.Collision && std::ranges::all_of(e.Collision->Spheres,[&](const auto& s) { return Contains(g,Transform(e.Matrix,s.Center),margin-s.Radius); });
}
bool NativeGarages::EntirelyOutside(const NativeGarageEntry& g,const NativeGarageEntityBounds& e,float margin) {
    const auto p=e.Matrix.Position;
    if (p[0]>g.Rect[0]-margin && p[0]<g.Rect[1]+margin && p[1]>g.Rect[2]-margin && p[1]<g.Rect[3]+margin) return false;
    return e.Collision && std::ranges::none_of(e.Collision->Spheres,[&](const auto& s) { return Contains(g,Transform(e.Matrix,s.Center),margin+s.Radius); });
}
NativeGarageMatrix NativeGarages::DoorPose(const NativeGarageEntry& g,const NativeGarageDoor& d) {
    auto m=d.Authored; const float height=d.Collision->Max[2]-d.Collision->Min[2]-.1f;
    if (g.Flags&8) {
        m.Position[2]+=height*g.DoorPosition*.48f;
        const float angle=g.DoorPosition*((g.Flags&16) ? 1.0f : -1.0f)*std::numbers::pi_v<float>*.5f;
        m.Basis[2]={-std::sin(angle)*m.Basis[1][1],std::sin(angle)*m.Basis[1][0],std::cos(angle)};
        m.Basis[0]=Cross(m.Basis[1],m.Basis[2]);
    } else if (g.Type==44) m.Position[2]-=height*g.DoorPosition;
    else if (g.Type==45) m.Position[0]-=g.DoorPosition*m.Basis[0][0]*14.0f;
    else m.Position[2]+=height*g.DoorPosition/1.1f;
    return m;
}
bool NativeGarages::DoorPublished(const NativeGarageEntry& g,const NativeGarageDoor& d,const NativePlacementOverrides* overrides,std::uint8_t sourceFlags) {
    const auto pose=DoorPose(g,d); const bool collision=sourceFlags&0x40;
    if (const auto* applied=overrides ? overrides->Find(d.Placement) : nullptr)
        return applied->Position==pose.Position && applied->Basis==pose.Basis && applied->CollisionEnabled==collision;
    return pose==d.Authored && collision;
}
NativeGarageRequirement NativeGarages::Transition(const NativeGarageEntry& g,const NativeGarageView& view,const NativeGaragePolicy& policy,std::size_t index) {
    if ((g.Flags&2) && g.DoorState==0) return NativeGarageRequirement::None;
    const auto p=view.Vehicle ? view.Vehicle->Matrix.Position : view.Player.Matrix.Position;
    // Independent retail assertions cover each of these actual return paths.
    // No catch-all distance suppression or assumption that every type is idle.
    if (g.Type==1 && g.DoorState==0 && !view.Vehicle)
        return NativeGarageRequirement::None; // 44F0A6: either target differs from null, or the following non-null test fails
    if ((g.Type==2 || g.Type==3 || g.Type==4) && g.DoorState==1 && !view.Vehicle)
        return NativeGarageRequirement::None; // 44EACA -> 44DBF5: IsStaticPlayerCarEntirelyInside false
    if (g.Type==5) {
        if (p[2]>=950) return NativeGarageRequirement::None;
        if (g.DoorState==1 && policy.NoResprays) return NativeGarageRequirement::None;
        if (g.DoorState==1 && !view.Vehicle) {
            // 44E2B9 still evaluates the player's actual source spheres and
            // may change wanted policy on FOOT. No vehicle alone isn't a no-op.
            if (!EntirelyOutside(g,view.Player,0) || policy.LastGaragePlayerWasIn==static_cast<std::int32_t>(index))
                return NativeGarageRequirement::WantedPolicy;
            return NativeGarageRequirement::None; // 44E2EC, then 44E30A
        }
    }
    if ((g.Type==33 || g.Type==34 || g.Type==35) && g.DoorState==0) {
        if (DistanceSquared(g,p)>=3600 || p[2]<=g.Origin[2] || p[2]>=g.Top-2)
            return NativeGarageRequirement::None; // 44FC2D/44FC4D/44FC5F/44FC67
        return NativeGarageRequirement::ImpoundVehicles;
    }
    if (g.DoorState==2 || g.DoorState==3) return NativeGarageRequirement::DoorMotion;
    // Type 22 is the hidden TRICAS record in int_veg.ipl and is absent from
    // the reversed eGarageType enum. Keep its unported body strict nearby;
    // a distant hidden-interior record has no active transition in this view.
    if ((g.Type==19 || g.Type==22) && DistanceSquared(g,p)>=3600.0f) return NativeGarageRequirement::None;
    if (!Hideout(g.Type)) return NativeGarageRequirement::SourceTypeUpdate;
    if (g.DoorState==1) {
        const auto distance=DistanceSquared(g,p);
        // Source OPEN branch continues while inactive. It first queries door
        // obstruction before closing, then vehicle capacity; never fabricate
        // an empty world vehicle pool or an unblocked door.
        if (distance>225.0f || (distance>16.0f && (!view.Vehicle || view.Vehicle->VehicleSubType==10))) return NativeGarageRequirement::DoorObstruction;
        if (view.Vehicle) return NativeGarageRequirement::VehicleCapacity;
        return NativeGarageRequirement::None;
    }
    if (g.DoorState!=0) return NativeGarageRequirement::SourceTypeUpdate;
    if (p[2]>=950) return NativeGarageRequirement::None;
    const auto distance=DistanceSquared(g,p);
    if (distance<12.25f || (distance<100.0f && view.Vehicle && view.Vehicle->VehicleSubType!=10)) return view.Vehicle && g.Type!=44 ? NativeGarageRequirement::VehicleCapacity : NativeGarageRequirement::RestoreAndOpen;
    return NativeGarageRequirement::None;
}
std::uint8_t NativeGarages::UpdateCollisionFlags(const NativeGarageEntry& g) {
    if (!(g.Flags&8) || ((g.Flags&2) && g.DoorState==0)) return g.Flags;
    return g.DoorState==1 || (g.DoorState==3 && g.DoorPosition>.4f) ? g.Flags&~0x40 : g.Flags|0x40;
}
bool NativeGarages::Tick(const NativeGarageView& view,std::string& error) try {
    Require(m_Owner,"garage registry not initialized");
    if (!view.Replay && !view.Coop) Require(ValidEntity(view.Player) && (!view.Vehicle || (ValidEntity(*view.Vehicle) && view.Vehicle->ModelId>=0 && view.Vehicle->VehicleSubType>=0 && view.Vehicle->VehicleSubType<=11)) && Finite(view.Camera),"garage update requires actual finite entity COL/matrices and vehicle subtype");
    Require(!m_LastFrame || view.Frame>*m_LastFrame,"garage frame must advance (no duplicate or stale input publication)");
    NativeGarageFrame frame;
    auto maintenanceIndex=m_MaintenanceIndex;
    std::vector<std::uint8_t> flags; flags.reserve(m_Entries.size());
    for (const auto& g:m_Entries) flags.push_back(g.Flags);
    frame.Camera.Previous=m_Frame.Camera.Garage;
    if (view.Replay || view.Coop) {
        frame.Camera=m_Frame.Camera; frame.Camera.Apply=false;
    } else {
        frame.Camera.Apply=true;
        for (std::size_t i=0;i<m_Entries.size();++i) {
            const auto& g=m_Entries[i]; const NativeGarageRef ref{m_Owner,i};
            // CGarage::Update camera prelude MUST precede inactive/CLOSED.
            if (g.Type!=13 && g.DoorState<=5 && !(g.Flags&32)) {
                const auto& entity=view.Vehicle && view.Vehicle->ModelId==571 ? *view.Vehicle : view.Player;
                if (EntirelyInside(g,entity,.25f)) { frame.Camera.Outside=true; frame.Camera.Garage=ref; }
                if (view.Vehicle) {
                    if (!EntirelyOutside(g,*view.Vehicle,0)) frame.Camera.AvoidFirstPerson=ref;
                    const auto p=view.Vehicle->Matrix.Position;
                    if (view.Vehicle->ModelId==423 && p[0]>=g.Rect[0]-.5f && p[0]<=g.Rect[1]+.5f && p[1]>=g.Rect[2]-.5f && p[1]<=g.Rect[3]+.5f) {
                        frame.Camera.Outside=true; frame.Camera.Garage=ref;
                    }
                }
            }
            NativeGarageUpdate update; update.Garage=ref; update.FromState=update.RequestedState=g.DoorState;
            update.InactiveClosed=(g.Flags&2) && g.DoorState==0;
            flags[i]=UpdateCollisionFlags(g); // source common consumer after camera and inactive/CLOSED
            update.Requirement=Transition(g,view,m_Policy,i);
            if (update.Requirement!=NativeGarageRequirement::None) update.Status=NativeScriptServiceStatus::Unsupported;
            if (update.Requirement==NativeGarageRequirement::RestoreAndOpen) update.RequestedState=3;
            for (std::size_t j=0;j<m_Doors.size();++j) if (m_Doors[j].Garage==ref) update.Doors.push_back(j);
            if (update.Requirement==NativeGarageRequirement::None && std::ranges::any_of(update.Doors,[&](auto j) { return !DoorPublished(g,m_Doors[j],view.PublishedOverrides.get(),flags[i]); })) {
                update.Requirement=NativeGarageRequirement::DoorMotion; update.Status=NativeScriptServiceStatus::Unsupported;
            }
            frame.Updates.push_back(std::move(update));
        }
        if ((view.Frame&15)==12) {
            maintenanceIndex=(m_MaintenanceIndex+1)%50;
            if (maintenanceIndex<m_Entries.size()) {
                frame.Maintenance=NativeGarageRef{m_Owner,maintenanceIndex};
                const auto& g=m_Entries[maintenanceIndex];
                frame.TidyClose=std::abs(g.Rect[0]-view.Camera[0])<40 && std::abs(g.Rect[2]-view.Camera[1])<40;
                NativeGarageUpdate update; update.Garage=*frame.Maintenance;
                update.Status=NativeScriptServiceStatus::Unsupported; update.Requirement=NativeGarageRequirement::TidyUp;
                update.FromState=update.RequestedState=g.DoorState; frame.Updates.push_back(std::move(update));
            }
        }
    }
    // Everything that can fail/allocate is complete before this publication.
    for (std::size_t i=0;i<m_Entries.size();++i) m_Entries[i].Flags=flags[i];
    for (auto& door:m_Doors) if (door.Garage) {
        door.CollisionEnabled=flags[door.Garage->Index]&0x40;
        door.RequiresDynamicPublication=door.SourcePose!=door.Authored || !door.CollisionEnabled;
    }
    m_MaintenanceIndex=maintenanceIndex;
    m_Frame=std::move(frame); m_LastFrame=view.Frame; error.clear(); return true;
} catch (const std::exception& e) { error=e.what(); return false; }
