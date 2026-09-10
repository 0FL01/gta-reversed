// Read-only asset diagnosis, NOT a runtime translation unit. Reuse the exact
// private COL parser until it can be extracted into an owned-data API. Build
// with -ffunction-sections -fdata-sections -Wl,--gc-sections and Collide.cpp;
// the unused legacy OS/global loader is discarded. No librw or OS path mutation.
#include "ColLoad.cpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <stdexcept>

namespace {
using Vec = std::array<float, 3>;
struct AssetModel {
    ColModel Shape;
    std::string Library;
    uint16 HeaderId{};
    std::vector<uint8> SphereMaterials, BoxMaterials, FaceMaterials;
};
struct AssetInstance {
    IplInst Placement;
    std::string Source;
    uint32 Row{};
    int ModelId{};
    bool Binary{};
};
void Require(bool ok, const std::string& message) {
    if (!ok) throw std::runtime_error(message);
}
std::vector<uint8> ReadBytes(std::ifstream& file, uint64 offset, size_t size) {
    std::vector<uint8> bytes(size);
    file.seekg(static_cast<std::streamoff>(offset));
    Require(bool(file.read(reinterpret_cast<char*>(bytes.data()), size)), "short asset read");
    return bytes;
}
std::vector<uint8> ReadBytes(const std::string& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    Require(bool(file), "open failed: " + path);
    const auto size = file.tellg();
    Require(size >= 0, "asset size failed: " + path);
    return ReadBytes(file, 0, static_cast<size_t>(size));
}
std::string GamePath(const std::string& game, const std::string& relative) {
    std::string path;
    Require(ResolveGamePath(game, relative, path), "missing asset: " + relative);
    return path;
}
std::string Text(const std::vector<uint8>& bytes) {
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}
Vec Cross(Vec a, Vec b) {
    return {a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0]};
}
Vec Sub(Vec a, Vec b) { return {a[0]-b[0], a[1]-b[1], a[2]-b[2]}; }
Vec Unit(Vec a) {
    const float n = std::sqrt(a[0]*a[0]+a[1]*a[1]+a[2]*a[2]);
    Require(n > 0, "zero hit normal");
    return {a[0]/n, a[1]/n, a[2]/n};
}

// Geometry is decoded by ColLoad, while provenance/materials lost by that API
// are read alongside it. No raw asset contents are printed or saved.
void ReadModels(const std::vector<uint8>& bytes, const std::string& library,
                std::map<std::string, AssetModel>& models, int& rejected, int& duplicates) {
    for (size_t p = 0; p + 32 <= bytes.size();) {
        const auto* ch = bytes.data() + p;
        const int version = !std::memcmp(ch,"COLL",4) ? 1 :
            !std::memcmp(ch,"COL2",4) ? 2 : !std::memcmp(ch,"COL3",4) ? 3 :
            !std::memcmp(ch,"COL4",4) ? 4 : 0;
        if (!version) break; // IMG sector padding
        const size_t size = size_t(ReadU32LE(ch+4)) + 8;
        Require(size >= 32 && size <= bytes.size()-p, "invalid COL chunk: " + library);
        AssetModel model;
        model.Shape.name = std::string(reinterpret_cast<const char*>(ch+8), strnlen(reinterpret_cast<const char*>(ch+8),22));
        model.Library = library;
        model.HeaderId = ReadU16LE(ch+30);
        std::string key = model.Shape.name;
        ToLowerInPlace(key);
        const bool parsed = version == 1 ? ParseV1Chunk(ch+32,size-32,model.Shape) :
            ParseV234Chunk(ch,size,version,model.Shape);
        if (!parsed) {
            ++rejected;
            if (version==1 || size<108 || (ReadU32LE(ch+80)&2)) {
                std::printf("COL excluded model=%s library=%s reason=nonempty-parser-rejection\n",key.c_str(),library.c_str());
            }
        } else {
            auto materials = [&](size_t offset, size_t count, size_t stride, size_t field, std::vector<uint8>& out) {
                Require(offset <= size && count <= (size-offset)/stride, "surface range: " + key);
                for (size_t i=0; i<count; ++i) out.push_back(ch[offset+i*stride+field]);
            };
            if (version == 1) {
                size_t cursor = 72;
                auto count = [&]() {
                    Require(cursor+4 <= size, "V1 count range: " + key);
                    const uint32 n = ReadU32LE(ch+cursor); cursor += 4; return n;
                };
                const auto ns=count(); materials(cursor,ns,20,16,model.SphereMaterials); cursor+=ns*20;
                const auto nl=count(); cursor+=nl*24;
                const auto nb=count(); materials(cursor,nb,28,24,model.BoxMaterials); cursor+=nb*28;
                const auto nv=count(); cursor+=nv*12;
                const auto nf=count(); materials(cursor,nf,16,12,model.FaceMaterials);
            } else {
                const auto* h=ch+32;
                materials(4+ReadU32LE(h+52),ReadU16LE(h+40),20,16,model.SphereMaterials);
                materials(4+ReadU32LE(h+56),ReadU16LE(h+42),28,24,model.BoxMaterials);
                materials(4+ReadU32LE(h+68),ReadU16LE(h+44),8,6,model.FaceMaterials);
            }
            const bool aligned=model.SphereMaterials.size()==model.Shape.spheres.size() &&
                    model.BoxMaterials.size()==model.Shape.boxes.size() &&
                    model.FaceMaterials.size()==model.Shape.tris.size()/3;
            if (!aligned) {
                ++rejected;
                std::printf("COL excluded model=%s library=%s reason=legacy-parser-discarded-primitives source=%zu/%zu/%zu parsed=%zu/%zu/%zu\n",
                    key.c_str(),library.c_str(),model.SphereMaterials.size(),model.BoxMaterials.size(),model.FaceMaterials.size(),model.Shape.spheres.size(),model.Shape.boxes.size(),model.Shape.tris.size()/3);
            } else if (!models.emplace(key,std::move(model)).second) ++duplicates;
        }
        p += size;
    }
}

struct Hit {
    float Height=-INFINITY;
    Vec Normal{};
    const AssetInstance* Instance{};
    const AssetModel* Model{};
    const char* Primitive="none";
    size_t Index{};
    int Material=-1;
};
Hit Probe(float x,float y,float top,const std::vector<AssetInstance>& instances,
          const std::map<std::string,AssetModel>& models,bool binary,bool conjugate) {
    Hit hit;
    for (const auto& inst:instances) {
        if (inst.Binary && !binary) continue;
        const auto& ip=inst.Placement;
        if (ip.interior || ip.lower.starts_with("lod")) continue;
        const auto found=models.find(ip.lower);
        if (found==models.end()) continue;
        const auto& asset=found->second;
        const auto& model=asset.Shape;
        float q[4]; std::copy_n(ip.quat,4,q);
        if (conjugate) for (int i=0;i<3;++i) q[i]=-q[i];
        Vec basis[3]; QuatToBasis(q,basis[0].data(),basis[1].data(),basis[2].data());
        auto worldVector=[&](Vec v) {
            Vec w{};
            for (int i=0;i<3;++i) for (int j=0;j<3;++j) w[i]+=basis[j][i]*v[j];
            return w;
        };
        Vec low{},high{};
        for (int i=0;i<3;++i) {
            low[i]=high[i]=ip.pos[i];
            for (int j=0;j<3;++j) {
                const float r=basis[j][i];
                low[i]+=r*(r>=0 ? model.bmin[j]:model.bmax[j]);
                high[i]+=r*(r>=0 ? model.bmax[j]:model.bmin[j]);
            }
        }
        if (x<low[0] || x>high[0] || y<low[1] || y>high[1]) continue;
        const Vec relative{x-ip.pos[0],y-ip.pos[1],top-ip.pos[2]};
        Vec origin{},direction{};
        for (int i=0;i<3;++i) {
            for (int j=0;j<3;++j) origin[i]+=basis[i][j]*relative[j];
            direction[i]=-basis[i][2];
        }
        auto accept=[&](float t,Vec normal,const char* primitive,size_t index,int material) {
            if (t<0 || t>top+50 || top-t<=hit.Height) return;
            normal=Unit(worldVector(normal));
            if (std::abs(normal[2])<0.6f) return; // current controller WalkableUp
            hit={top-t,normal,&inst,&asset,primitive,index,material};
        };
        for (size_t i=0;i<model.spheres.size();++i) {
            const auto& s=model.spheres[i]; float t;
            if (Collide::RaySphere(origin.data(),direction.data(),s.c,s.r,t))
                accept(t,{origin[0]+direction[0]*t-s.c[0],origin[1]+direction[1]*t-s.c[1],origin[2]+direction[2]*t-s.c[2]},"sphere",i,asset.SphereMaterials[i]);
        }
        for (size_t i=0;i<model.boxes.size();++i) {
            const auto& b=model.boxes[i]; float t;
            if (!Collide::RayBox(origin.data(),direction.data(),b.mn,b.mx,t)) continue;
            Vec n{}; float closest=INFINITY;
            for (int axis=0;axis<3;++axis) for (int side=0;side<2;++side) {
                const float d=std::abs(origin[axis]+direction[axis]*t-(side ? b.mx[axis]:b.mn[axis]));
                if (d<closest) { closest=d; n={}; n[axis]=side ? 1.0f:-1.0f; }
            }
            accept(t,n,"box",i,asset.BoxMaterials[i]);
        }
        for (size_t i=0;i<model.tris.size()/3;++i) {
            Vec v[3];
            for (int j=0;j<3;++j) std::copy_n(model.verts.data()+model.tris[i*3+j]*3,3,v[j].data());
            float t;
            if (Collide::RayTri(origin.data(),direction.data(),v[0].data(),v[1].data(),v[2].data(),t))
                // CColTrianglePlane uses (C-A) cross (B-A), unlike render winding.
                accept(t,Cross(Sub(v[2],v[0]),Sub(v[1],v[0])),"tri",i,asset.FaceMaterials[i]);
        }
    }
    return hit;
}
void PrintHit(float x,float y,float top,const Hit& hit,const char* mode) {
    if (!hit.Instance) { std::printf("COL mode=%s xy=%.6f,%.6f top=%.3f MISS\n",mode,x,y,top); return; }
    const auto& i=*hit.Instance; const auto& a=*hit.Model; const auto& m=a.Shape;
    std::printf("COL mode=%s xy=%.6f,%.6f top=%.3f ground=%.9f normal=%.6f,%.6f,%.6f material=%d primitive=%s[%zu] model=%s ideId=%d headerId=%u library=%s ipl=%s row=%u binary=%d spheres=%zu boxes=%zu tris=%zu verts=%zu\n",
        mode,x,y,top,hit.Height,hit.Normal[0],hit.Normal[1],hit.Normal[2],hit.Material,hit.Primitive,hit.Index,m.name.c_str(),i.ModelId,a.HeaderId,a.Library.c_str(),i.Source.c_str(),i.Row,i.Binary,m.spheres.size(),m.boxes.size(),m.tris.size()/3,m.verts.size()/3);
    std::printf("  placement=%.9f,%.9f,%.9f rawQuat=%.9f,%.9f,%.9f,%.9f localBounds=%.6f,%.6f,%.6f..%.6f,%.6f,%.6f\n",
        i.Placement.pos[0],i.Placement.pos[1],i.Placement.pos[2],i.Placement.quat[0],i.Placement.quat[1],i.Placement.quat[2],i.Placement.quat[3],m.bmin[0],m.bmin[1],m.bmin[2],m.bmax[0],m.bmax[1],m.bmax[2]);
}
} // namespace

int main(int argc,char** argv) try {
    Require(argc==2,"usage: NativeCollisionAssetsProbe GAME_DIR");
    const std::string game=argv[1];
    std::vector<std::string> idePaths,iplPaths;
    for (const char* dat:{"data/gta.dat","data/default.dat"}) {
        std::istringstream lines(Text(ReadBytes(GamePath(game,dat))));
        for (std::string line;std::getline(lines,line);) {
            std::istringstream row(line); std::string type,path; row>>type>>path;
            if (type=="IDE") idePaths.push_back(path);
            if (type=="IPL") iplPaths.push_back(path);
        }
    }
    std::map<int,std::string> ids;
    std::map<std::string,int> names;
    for (const auto& path:idePaths) {
        std::istringstream lines(Text(ReadBytes(GamePath(game,path))));
        for (std::string line;std::getline(lines,line);) {
            SanitizeLine(line); std::istringstream row(line); int id; std::string name;
            if (row>>id>>name) { ToLowerInPlace(name); ids[id]=name; names[name]=id; }
        }
    }
    std::vector<AssetInstance> instances;
    for (const auto& path:iplPaths) {
        std::vector<IplInst> parsed;
        ParseIplText(Text(ReadBytes(GamePath(game,path))),parsed);
        for (size_t i=0;i<parsed.size();++i) {
            auto id=names.find(parsed[i].lower);
            Require(id!=names.end(),"text IPL missing IDE: "+parsed[i].lower);
            instances.push_back({parsed[i],path,static_cast<uint32>(i),id->second,false});
        }
    }
    const auto textInstances=instances.size();
    std::map<std::string,AssetModel> models;
    int rejected=0,duplicates=0,blobs=0,binaryFiles=0,binaryRows=0;
    std::vector<std::string> loose;
    for (const auto& file:std::filesystem::directory_iterator(GamePath(game,"models/coll"))) {
        std::string ext=file.path().extension().string(); ToLowerInPlace(ext);
        if (ext==".col") loose.push_back(file.path().string());
    }
    std::sort(loose.begin(),loose.end());
    for (const auto& file:loose) { ReadModels(ReadBytes(file),std::filesystem::path(file).filename().string(),models,rejected,duplicates); ++blobs; }
    std::set<std::string> seenIpls;
    for (const char* relative:{"models/gta3.img","models/gta_int.img","models/player.img"}) {
        std::ifstream file(GamePath(game,relative),std::ios::binary);
        const auto head=ReadBytes(file,0,8);
        Require(!std::memcmp(head.data(),"VER2",4),"IMG version");
        const auto directory=ReadBytes(file,8,size_t(ReadU32LE(head.data()+4))*32);
        for (size_t p=0;p<directory.size();p+=32) {
            const auto* entry=directory.data()+p;
            std::string name(reinterpret_cast<const char*>(entry+8),strnlen(reinterpret_cast<const char*>(entry+8),24)); ToLowerInPlace(name);
            if (!name.ends_with(".col") && !name.ends_with(".ipl")) continue;
            const uint16 archived=ReadU16LE(entry+6);
            const size_t size=size_t(archived ? archived:ReadU16LE(entry+4))*2048;
            const auto bytes=ReadBytes(file,uint64(ReadU32LE(entry))*2048,size);
            if (name.ends_with(".col")) { ReadModels(bytes,std::string(relative)+":"+name,models,rejected,duplicates); ++blobs; continue; }
            if (!seenIpls.insert(name).second) continue;
            Require(bytes.size()>=76 && !std::memcmp(bytes.data(),"bnry",4),"binary IPL header");
            const uint32 count=ReadU32LE(bytes.data()+4),offset=ReadU32LE(bytes.data()+28);
            Require(!count || (offset>=76 && offset<=size && count<=(size-offset)/40),"binary IPL range");
            ++binaryFiles; binaryRows+=count;
            for (uint32 i=0;i<count;++i) {
                const auto* row=bytes.data()+offset+i*40;
                const int id=static_cast<int32>(ReadU32LE(row+28));
                Require(ids.contains(id),"binary IPL missing IDE: "+std::to_string(id));
                IplInst inst; inst.model=inst.lower=ids.at(id); inst.interior=ReadU32LE(row+32)&255;
                for (int j=0;j<7;++j) {
                    const float value=ReadF32LE(row+j*4); Require(std::isfinite(value),"IPL transform");
                    if (j<3) inst.pos[j]=value; else inst.quat[j-3]=value;
                }
                instances.push_back({inst,name,i,id,true});
            }
        }
    }
    size_t spheres=0,boxes=0,tris=0,bound=0;
    for (const auto& [name,m]:models) { spheres+=m.Shape.spheres.size(); boxes+=m.Shape.boxes.size(); tris+=m.Shape.tris.size()/3; }
    for (const auto& i:instances) if (!i.Placement.interior && !i.Placement.lower.starts_with("lod") && models.contains(i.Placement.lower)) ++bound;
    std::printf("COL assets blobs=%d models=%zu spheres=%zu boxes=%zu tris=%zu rejectedOrEmpty=%d duplicates=%d textOutdoorNonLod=%zu binaryFiles=%d binaryRows=%d boundOutdoorNonLod=%zu\n",blobs,models.size(),spheres,boxes,tris,rejected,duplicates,textInstances,binaryFiles,binaryRows,bound);
    for (const Vec point:{Vec{2488.562f,-1666.864f,20},Vec{1540,-1736,20}}) {
        PrintHit(point[0],point[1],point[2],Probe(point[0],point[1],point[2],instances,models,false,false),"legacy-text");
        PrintHit(point[0],point[1],point[2],Probe(point[0],point[1],point[2],instances,models,false,true),"text-conjugated");
        const auto hit=Probe(point[0],point[1],point[2],instances,models,true,true);
        PrintHit(point[0],point[1],point[2],hit,"all-conjugated");
        Require(hit.Instance,"required COL ground missed");
        const auto header=ids.find(hit.Model->HeaderId);
        std::printf("  binding headerIdName=%s method=%s\n",header==ids.end() ? "<missing>":header->second.c_str(),
            header!=ids.end() && header->second==hit.Instance->Placement.lower ? "validated-id":"name-resolution");
    }
    for (float dy:{-4.01367f,-3.0f,-2.0f,-1.5f,-1.375f,-1.25f,-1.125f,-1.0f}) {
        const float y=-1736+dy;
        const auto hit=Probe(1540,y,20,instances,models,true,true);
        PrintHit(1540,y,20,hit,"curb-y");
        Require(hit.Instance,"curb COL ground missed");
    }
    for (float dx:{-1.25f,-1.0f,-0.5f,-0.125f,0.125f,1.0f,1.25f,3.0f}) {
        const float x=1540+dx;
        const auto hit=Probe(x,-1736,20,instances,models,true,true);
        PrintHit(x,-1736,20,hit,"curb-x");
        Require(hit.Instance,"curb COL ground missed");
    }
    std::puts("native-collision-assets diagnosis PASS (ray/asset binding only; no controller sweep claim)");
    return 0;
} catch (const std::exception& error) {
    std::fprintf(stderr,"native-collision-assets diagnosis FAIL: %s\n",error.what());
    return 1;
}
