#include "app/platform/linux/NativeCollisionAssets.h"
#include "app/platform/linux/StreamPager.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <limits>
#include <stdexcept>

using int32 = int32_t;
using uint32 = uint32_t;
using int64 = int64_t;
using uint64 = uint64_t;
#ifndef __stdcall
#define __stdcall
#endif
#include "oswrapper/oswrapper.h"

namespace {
using Vec = NativeCollisionVector;
static void Require(bool ok, const std::string& message) { if (!ok) throw std::runtime_error(message); }
static std::string Lower(std::string s) {
    for (auto& c : s) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return s;
}
struct Bytes {
    std::span<const uint8_t> Data;
    void Range(size_t p, size_t n, size_t stride = 1) const {
        Require(p <= Data.size() && n <= (Data.size() - p) / stride, "COL range outside chunk");
    }
    uint16_t U16(size_t p) const { Range(p, 2); return Data[p] | uint16_t(Data[p+1]) << 8; }
    uint32_t U32(size_t p) const { return U16(p) | uint32_t(U16(p+2)) << 16; }
    float Float(size_t p) const {
        const auto bits = U32(p); float v; std::memcpy(&v, &bits, 4);
        Require(std::isfinite(v), "nonfinite COL value"); return v;
    }
    Vec Vector(size_t p) const { return {Float(p), Float(p+4), Float(p+8)}; }
    NativeCollisionSurface Surface(size_t p) const {
        Range(p, 4); return {Data[p], Data[p+1], Data[p+2], Data[p+3]};
    }
};
static void Bounds(Vec a, Vec b) {
    for (int j=0; j<3; ++j) Require(a[j] <= b[j], "inverted COL bounds");
}
static std::string Resolve(const std::string& game, std::string relative) {
    std::replace(relative.begin(), relative.end(), '\\', '/');
    auto current = std::filesystem::absolute(game);
    for (const auto& part : std::filesystem::path(relative)) {
        if (part == "." || part.empty()) continue;
        Require(part != ".." && part != "/", "invalid relative asset path");
        bool found = false;
        for (const auto& entry : std::filesystem::directory_iterator(current)) {
            if (Lower(entry.path().filename().string()) == Lower(part.string())) {
                current = entry.path(); found = true; break;
            }
        }
        Require(found, "missing collision asset: " + relative);
    }
    return current.string();
}
struct File {
    void* Handle{};
    size_t Size{};
    explicit File(const std::string& absolute) {
        Require(OS_FileOpen(FILE_DATA_AREA_DEFAULT, &Handle, absolute.c_str(), FILE_ACCESS_READ) == 0 && Handle,
                "OS_FileOpen absolute collision asset failed: " + absolute);
        const auto size = OS_FileSize(Handle);
        if (size < 0) { OS_FileClose(Handle); Handle = nullptr; throw std::runtime_error("collision file exceeds OS_File range"); }
        Size = static_cast<size_t>(size);
    }
    ~File() { if (Handle) OS_FileClose(Handle); }
    std::vector<uint8_t> Read(size_t offset, size_t count) const {
        Require(offset <= Size && count <= Size-offset, "collision file range");
        std::vector<uint8_t> bytes(count);
        for (size_t p=0; p<count;) {
            const auto n = std::min<size_t>(count-p, 1<<20);
            OS_FileSetPosition(Handle, static_cast<int32>(offset+p));
            Require(OS_FileRead(Handle, bytes.data()+p, static_cast<int32>(n)) == 0, "short collision asset read");
            p += n;
        }
        return bytes;
    }
};
static std::array<Vec, 3> Basis(std::array<float, 4> q) {
    float norm=0;
    for (auto v:q) { Require(std::isfinite(v), "nonfinite IPL quaternion"); norm+=v*v; }
    Require(norm>1e-12f && std::isfinite(norm), "invalid IPL quaternion");
    norm=std::sqrt(norm);
    const float x=-q[0]/norm, y=-q[1]/norm, z=-q[2]/norm, w=q[3]/norm;
    return {Vec{1-2*(y*y+z*z), 2*(x*y+z*w), 2*(x*z-y*w)},
            Vec{2*(x*y-z*w), 1-2*(x*x+z*z), 2*(y*z+x*w)},
            Vec{2*(x*z+y*w), 2*(y*z-x*w), 1-2*(x*x+y*y)}};
}
} // namespace

bool NativeCollisionAssets::Parse(std::span<const uint8_t> chunk, const std::string& library,
                                  NativeCollisionModel& out, std::string& error) try {
    // Same source layouts/arithmetic as ColLoad::ParseV1Chunk/ParseV234Chunk,
    // but preserve surfaces and zero-radius authored ped spheres. Invalid
    // primitives fail the entire model instead of the legacy silent continue.
    Bytes b{chunk}; b.Range(0, 32);
    NativeCollisionModel m;
    m.Version = !std::memcmp(chunk.data(), "COLL", 4) ? 1 : !std::memcmp(chunk.data(), "COL2", 4) ? 2 :
        !std::memcmp(chunk.data(), "COL3", 4) ? 3 : !std::memcmp(chunk.data(), "COL4", 4) ? 4 : 0;
    Require(m.Version != 0, "unknown COL geometry version");
    Require(size_t(b.U32(4))+8 == chunk.size(), "COL chunk length mismatch");
    m.Name=Lower(std::string(reinterpret_cast<const char*>(chunk.data()+8), strnlen(reinterpret_cast<const char*>(chunk.data()+8), 22)));
    Require(!m.Name.empty(), "unnamed COL model");
    m.Library=library; m.HeaderId=b.U16(30);
    m.SourceChunk.assign(chunk.begin(), chunk.end());
    const bool v1=m.Version==1;
    b.Range(32, v1 ? 40 : m.Version==2 ? 76 : m.Version==3 ? 88 : 92);
    m.Min=b.Vector(v1 ? 48:32); m.Max=b.Vector(v1 ? 60:44); Bounds(m.Min,m.Max);
    m.BoundCenter=b.Vector(v1 ? 36:56); m.BoundRadius=b.Float(v1 ? 32:68);
    Require(m.BoundRadius>=0, "negative COL bound radius");
    size_t ns, nb, nf, nv=0, ps, pb, pf, pv, cursor=72;
    auto counted=[&](size_t stride, size_t& start) {
        const size_t n=b.U32(cursor); cursor+=4; start=cursor;
        b.Range(cursor,n,stride); cursor+=n*stride; return n;
    };
    if (v1) {
        ns=counted(20,ps);
        size_t lines; const auto nl=counted(24,lines);
        Require(nl==0, "unsupported source COL suspension lines");
        nb=counted(28,pb); nv=counted(12,pv); nf=counted(16,pf);
    } else {
        ns=b.U16(72); nb=b.U16(74); nf=b.U16(76); m.Flags=b.U32(80);
        Require(chunk[78]==0, "unsupported source COL lines/cones");
        Require((m.Flags & ~uint32_t(1|2|8|16))==0, "unknown COL geometry flags");
        Require((m.Flags&2) || (ns==0 && nb==0 && nf==0), "empty COL flag conflicts with geometry");
        auto array=[&](size_t field, size_t n, size_t stride) {
            const size_t offset=b.U32(field), start=offset+4;
            if (n) { Require(offset!=0 && start>=32+(m.Version==2 ? 76u:m.Version==3 ? 88u:92u), "COL array overlaps header"); b.Range(start,n,stride); }
            return start;
        };
        ps=array(84,ns,20); pb=array(88,nb,28); pf=array(100,nf,8); pv=array(96,nf ? 1:0,6);
        if (nf) {
            size_t end=pf;
            if (m.Flags&8) {
                Require(pf>=4,"COL face group prefix");
                const auto groups=b.U32(pf-4);
                Require(groups <= (pf-4)/28, "COL face group range");
                end=pf-4-size_t(groups)*28;
                for (size_t i=0; i<groups; ++i) {
                    const auto p=end+i*28; Bounds(b.Vector(p),b.Vector(p+12));
                    Require(b.U16(p+24)<=b.U16(p+26) && b.U16(p+26)<nf,"COL face group indices");
                }
            }
            Require(end>=pv, "COL vertices overlap faces");
            size_t bytes=end-pv;
            if (bytes%6==2) bytes-=2;
            Require(bytes%6==0, "COL compressed vertex alignment"); nv=bytes/6;
        }
        if (m.Version>=3) {
            const auto shadows=b.U32(108);
            const auto faces=array(116,shadows,8), verts=array(112,shadows ? 1:0,6);
            uint32_t count=0;
            for (size_t i=0;i<shadows;++i) for (size_t j=0;j<3;++j) count=std::max(count,uint32_t(b.U16(faces+i*8+j*2))+1);
            if (count) b.Range(verts,count,6);
        }
    }
    for (size_t i=0; i<ns; ++i) {
        const size_t p=ps+i*20;
        NativeCollisionModel::Sphere s{b.Vector(p+(v1 ? 4:0)),b.Float(p+(v1 ? 0:12)),b.Surface(p+16)};
        Require(s.Radius>=0, "negative source COL sphere radius"); m.Spheres.push_back(s);
    }
    for (size_t i=0; i<nb; ++i) {
        const size_t p=pb+i*28;
        NativeCollisionModel::Box box{b.Vector(p),b.Vector(p+12),b.Surface(p+24)};
        Bounds(box.Min,box.Max); m.Boxes.push_back(box);
    }
    for (size_t i=0; i<nv; ++i) {
        const size_t p=pv+i*(v1 ? 12:6);
        m.Vertices.push_back(v1 ? b.Vector(p) : Vec{float(int16_t(b.U16(p)))/128, float(int16_t(b.U16(p+2)))/128, float(int16_t(b.U16(p+4)))/128});
    }
    for (size_t i=0; i<nf; ++i) {
        const size_t p=pf+i*(v1 ? 16:8); NativeCollisionModel::Face face;
        for (size_t j=0;j<3;++j) {
            face.Vertices[j]=v1 ? b.U32(p+j*4):b.U16(p+j*2);
            Require(face.Vertices[j]<nv, "COL face vertex index outside array");
        }
        face.Surface=v1 ? b.Surface(p+12):NativeCollisionSurface{chunk[p+6],0,0,chunk[p+7]};
        m.Faces.push_back(face);
    }
    m.Empty=ns==0 && nb==0 && nf==0;
    out=std::move(m); error.clear(); return true;
} catch (const std::exception& e) { error=library+": "+e.what(); return false; }

bool NativeCollisionAssets::Load(const char* gameDir, const NativeCollisionPopulation& population, std::string& error) try {
    Require(gameDir && *gameDir, "missing collision game directory");
    NativeCollisionAssets next;
    auto blob=[&](const std::vector<uint8_t>& bytes, const std::string& library) {
        size_t p=0;
        while (p<bytes.size()) {
            if (std::all_of(bytes.begin()+p,bytes.end(),[](uint8_t c){return c==0;})) break;
            const auto remaining=bytes.size()-p;
            const bool signature=remaining>=4 && (!std::memcmp(bytes.data()+p,"COLL",4) || !std::memcmp(bytes.data()+p,"COL2",4) ||
                !std::memcmp(bytes.data()+p,"COL3",4) || !std::memcmp(bytes.data()+p,"COL4",4));
            // IMG entry sizes are sector-rounded; final sector slack is not
            // specified to be zero. Never accept an unknown COLx as slack.
            if (!signature && p && remaining<2048 && (remaining<3 || std::memcmp(bytes.data()+p,"COL",3))) break;
            // The shipped legacy peds.col has malformed COLL framing after
            // male01 (next fourcc starts one byte before its declared end).
            // Do not guess offsets or feed these dynamic ped templates to the
            // static world. Publish the unsupported tail explicitly; terrain
            // libraries still fail hard on any such framing error.
            if (!signature && std::filesystem::path(library).filename()=="peds.col" && p &&
                remaining>=3 && !std::memcmp(bytes.data()+p,"OLL",3)) {
                ++next.m_Stats.Unsupported;
                next.m_Stats.UnsupportedModels.push_back(library+": unsupported malformed legacy ped tail at "+std::to_string(p)+" ("+std::to_string(remaining)+" bytes)");
                break;
            }
            Require(signature,"unknown COL chunk in "+library+" at "+std::to_string(p)+" remaining="+std::to_string(remaining)+
                " fourcc="+std::to_string(bytes[p])+","+std::to_string(remaining>1 ? bytes[p+1]:0)+","+std::to_string(remaining>2 ? bytes[p+2]:0)+","+std::to_string(remaining>3 ? bytes[p+3]:0));
            Bytes b{std::span(bytes).subspan(p)}; b.Range(0,32);
            const size_t size=size_t(b.U32(4))+8;
            Require(size>=32 && size<=remaining,"COL chunk range in "+library+" at "+std::to_string(p));
            auto model=std::make_shared<NativeCollisionModel>();
            std::string parseError;
            if (!Parse(b.Data.first(size),library,*model,parseError)) {
                // Keep unsupported model identity/bounds where the header is
                // readable; selecting it is an explicit hard failure.
                model->Name=Lower(std::string(reinterpret_cast<const char*>(b.Data.data()+8),strnlen(reinterpret_cast<const char*>(b.Data.data()+8),22)));
                model->Library=library; model->Unsupported=parseError;
                model->HeaderId=b.U16(30);
                model->SourceChunk.assign(b.Data.begin(),b.Data.begin()+size);
                model->Min={-INFINITY,-INFINITY,-INFINITY}; model->Max={INFINITY,INFINITY,INFINITY};
                if (size>=72) {
                    const bool v1=!std::memcmp(b.Data.data(),"COLL",4);
                    model->Min=b.Vector(v1 ? 48:32); model->Max=b.Vector(v1 ? 60:44);
                }
                ++next.m_Stats.Unsupported; next.m_Stats.UnsupportedModels.push_back(model->Name+": "+parseError);
            }
            model->ChunkOffset=static_cast<uint32_t>(p);
            const auto ide=population.Models.find(model->HeaderId);
            model->ValidatedHeaderId=ide!=population.Models.end() && Lower(ide->second.Name)==model->Name;
            next.m_Stats.HeaderNameFallback+=!model->ValidatedHeaderId;
            const auto old=next.m_Models.find(model->Name);
            Require(old==next.m_Models.end(),"ambiguous duplicate COL ownership: "+model->Name+" in "+library);
            next.m_Models.emplace(model->Name,model);
            ++next.m_Stats.Models; next.m_Stats.Empty+=model->Empty;
            next.m_Stats.Spheres+=model->Spheres.size(); next.m_Stats.Boxes+=model->Boxes.size(); next.m_Stats.Triangles+=model->Faces.size();
            p+=size;
        }
    };
    std::vector<std::string> loose;
    for (const auto& e:std::filesystem::directory_iterator(Resolve(gameDir,"models/coll")))
        if (Lower(e.path().extension().string())==".col") loose.push_back(e.path().string());
    std::sort(loose.begin(),loose.end());
    for (const auto& path:loose) { File file(path); blob(file.Read(0,file.Size),path); }
    for (const char* relative:{"models/gta3.img","models/gta_int.img","models/player.img"}) {
        File file(Resolve(gameDir,relative)); const auto header=file.Read(0,8); Bytes h{header};
        Require(!std::memcmp(header.data(),"VER2",4),"unsupported collision IMG version");
        const auto directory=file.Read(8,size_t(h.U32(4))*32); Bytes d{directory};
        for (size_t p=0;p<directory.size();p+=32) {
            const auto name=Lower(std::string(reinterpret_cast<const char*>(directory.data()+p+8),strnlen(reinterpret_cast<const char*>(directory.data()+p+8),24)));
            if (!name.ends_with(".col")) continue;
            const auto sectors=d.U16(p+6) ? d.U16(p+6):d.U16(p+4);
            blob(file.Read(size_t(d.U32(p))*2048,size_t(sectors)*2048),std::string(relative)+":"+name);
        }
    }
    std::map<std::string,bool> timeModels;
    for (const auto& [id,m]:population.Models) timeModels[Lower(m.Name)]=m.TimeModel;
    for (const auto& [name,time]:timeModels) if (time) {
        auto other=name; auto suffix=other.find("_nt"); const bool night=suffix!=std::string::npos;
        if (!night) suffix=other.find("_dy");
        if (suffix==std::string::npos) continue;
        other.replace(suffix,std::string::npos,night ? "_dy":"_nt");
        if (timeModels.contains(other) && timeModels.at(other)) {
            const auto a=next.m_Models.find(name),b=next.m_Models.find(other);
            if (a!=next.m_Models.end() && b!=next.m_Models.end()) {
                // FirstTime loader's SetColModel(...,true) shares to the time
                // partner and clears its ownership bit. Its later chunk is
                // skipped. Within a library the authored chunk order is exact.
                if (a->second->Library==b->second->Library) {
                    next.m_TimePartners[name]=a->second->ChunkOffset<b->second->ChunkOffset ? name:other;
                } else {
                    auto unsupported=std::make_shared<NativeCollisionModel>(*a->second);
                    unsupported->Unsupported="cross-library time-model ownership requires original COL streaming order: "+name+" / "+other;
                    next.m_Models[name]=unsupported; ++next.m_Stats.Unsupported;
                    next.m_Stats.UnsupportedModels.push_back(unsupported->Unsupported);
                }
            } else if (b!=next.m_Models.end()) next.m_TimePartners[name]=other;
        }
    }
    Require(!next.m_Models.empty(),"no source COL assets");
    *this=std::move(next); error.clear(); return true;
} catch (const std::exception& e) { error=e.what(); return false; }

std::shared_ptr<const NativeCollisionContext> NativeCollisionContext::LoadBeforeWorker(
    const char* gameDir, float radius, std::string& error) {
    if (!std::isfinite(radius) || radius <= 0) { error="invalid source COL residency radius"; return {}; }
    auto context=std::make_shared<NativeCollisionContext>();
    context->Radius=radius;
    if (!StreamPager_CollisionPopulation(context->Population,error) ||
        !context->Assets.Load(gameDir,context->Population,error)) return {};
    return context;
}

bool NativeCollisionAssets::Snapshot(const NativeCollisionPopulation& population, float x, float y, float radius,
                                     NativeCollisionSnapshot& out, std::string& error, int interior,
                                     std::shared_ptr<const NativePlacementOverrides> overrides) const try {
    Require(std::isfinite(x) && std::isfinite(y) && std::isfinite(radius) && radius>0,"invalid collision window");
    Require(!m_Models.empty(),"collision assets not loaded");
    Require(population.IncludesStreamed,"collision population lacks binary IPL");
    NativeCollisionSnapshot next;
    next.Overrides = std::move(overrides);
    for (const auto& p:population.Instances) {
        if (p.Interior!=interior) { ++next.InteriorExcluded; continue; }
        const auto* replacement = next.Overrides ? next.Overrides->Find(p) : nullptr;
        if (replacement && !replacement->CollisionEnabled) continue;
        const auto key=Lower(p.Model); auto found=m_Models.find(key); bool shared=false;
        const auto id=population.Models.find(p.ModelId);
        Require(id!=population.Models.end() && Lower(id->second.Name)==key,"IPL/IDE collision identity mismatch: "+key);
        const auto partner=m_TimePartners.find(key);
        if (partner!=m_TimePartners.end()) { found=m_Models.find(partner->second); shared=found!=m_Models.end() && partner->second!=key; }
        if (found==m_Models.end()) { ++next.MissingModels; ++next.KnownAbsence[key]; continue; }
        const auto& model=*found->second;
        // An unsupported parser result has no trustworthy extent. Referencing
        // it must fail even when its malformed header claims it is far away.
        Require(model.Unsupported.empty(),"referenced unsupported COL: "+key+" "+model.Unsupported);
        if (model.Empty) { ++next.EmptyModels; continue; }
        NativeCollisionInstance inst; inst.Placement=p; inst.Model=found->second; inst.TimeShared=shared;
        inst.Basis=replacement ? replacement->Basis : Basis(p.Quaternion);
        if (replacement) inst.Placement.Position = replacement->Position;
        for (auto v:inst.Placement.Position) Require(std::isfinite(v),"nonfinite collision IPL position");
        inst.Min=inst.Max=inst.Placement.Position;
        for (int i=0;i<3;++i) for (int j=0;j<3;++j) {
            const float r=inst.Basis[j][i];
            inst.Min[i]+=r*(r>=0 ? model.Min[j]:model.Max[j]);
            inst.Max[i]+=r*(r>=0 ? model.Max[j]:model.Min[j]);
        }
        if (inst.Max[0]<x-radius || inst.Min[0]>x+radius || inst.Max[1]<y-radius || inst.Min[1]>y+radius) continue;
        next.TimeShared+=shared; next.Instances.push_back(std::move(inst));
    }
    Require(!next.Instances.empty(),"no source COL in collision window");
    out=std::move(next); error.clear(); return true;
} catch (const std::exception& e) { error=e.what(); return false; }
