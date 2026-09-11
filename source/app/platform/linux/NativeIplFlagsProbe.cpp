// Read-only actual population differential and in-memory text IPL fixtures.
#include "app/platform/linux/StreamPager.h"
#include "app/platform/linux/RealtimeGameplay.h"
#include <algorithm>
#include <bit>
#include <cstdio>
#include <iostream>
#include <set>
#include <stdexcept>
#include <tuple>

using int32 = int32_t;
using int64 = int64_t;
using uint32 = uint32_t;
using uint64 = uint64_t;
#ifndef __stdcall
#define __stdcall
#endif
#include "oswrapper/oswrapper.h"

static bool s_Fixture{}, s_Injected{}, s_Sealed{};
static std::string s_Text, s_Empty = "inst\nend\n";
static void Require(bool ok, const std::string& error) { if (!ok) throw std::runtime_error(error); }
extern "C" FILE* __real_fopen(const char*, const char*);
extern "C" FILE* __wrap_fopen(const char* path, const char* mode) {
    Require(!s_Sealed && std::string(mode) == "rb", "unexpected file write/post-seal open");
    std::string lower(path);
    for (auto& c : lower) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    if (s_Fixture && lower.ends_with(".ipl")) {
        auto& text = s_Injected ? s_Empty : s_Text;
        s_Injected = true;
        return fmemopen(text.data(), text.size(), "rb");
    }
    return __real_fopen(path, mode);
}
#ifndef NATIVE_IPLFLAGS_PRODUCT_OS
int32 OS_FileOpen(OSFileDataArea, void** out, const char* path, OSFileAccessType access) {
    Require(access == FILE_ACCESS_READ, "OS write"); *out = std::fopen(path, "rb"); return *out ? 0 : 1;
}
int32 OS_FileClose(void* f) { return std::fclose(static_cast<FILE*>(f)); }
int32 OS_FileSize(void* f) {
    auto* file = static_cast<FILE*>(f); auto pos = std::ftell(file);
    std::fseek(file, 0, SEEK_END); auto size = std::ftell(file); std::fseek(file, pos, SEEK_SET);
    Require(size >= 0 && size <= INT32_MAX, "file bounds"); return static_cast<int32>(size);
}
int32 OS_FileRead(void* f, void* out, int32 size) {
    Require(!s_Sealed && size >= 0, "post-seal read");
    return std::fread(out, 1, size, static_cast<FILE*>(f)) == static_cast<size_t>(size) ? 0 : 3;
}
int32 OS_FileGetPosition(void* f) { return static_cast<int32>(std::ftell(static_cast<FILE*>(f))); }
void OS_FileSetPosition(void* f, int32 pos) { Require(!s_Sealed, "post-seal seek"); std::fseek(static_cast<FILE*>(f), pos, SEEK_SET); }
void OS_SetFilePathOffset(const char*) {}
#endif

struct Hash {
    uint64_t Value = 1469598103934665603ull;
    void Add(uint32_t v) { for (int i = 0; i < 4; ++i) { Value ^= (v >> (i * 8)) & 255; Value *= 1099511628211ull; } }
    void Add(float v) { Add(std::bit_cast<uint32_t>(v)); }
    template<typename T> void Range(const T& values) { for (const auto& v : values) Add(v); }
};
static uint64_t Geometry(const WorldShotMesh& mesh) {
    Hash h; h.Range(mesh.pos); h.Range(mesh.nrm); h.Range(mesh.uv); h.Range(mesh.triCol);
    for (auto v : mesh.dayColors) h.Add(uint32_t(v));
    for (auto v : mesh.nightColors) h.Add(uint32_t(v));
    return h.Value;
}
static void Identity(const NativePlacementIdentity& p) {
    std::cout << '\t' << p.Ipl << '\t' << p.Record << '\t' << p.Binary << '\t' << p.ModelId << '\t' << p.Model;
}
static void Population(const NativeCollisionPopulation& pop) {
    for (const auto& p : pop.Instances) {
        std::cout << "IPL"; Identity(NativePlacementIdentity::From(p));
        std::cout << '\t' << p.Interior << '\t' << p.Flags << '\t' << p.Lod;
        for (float v : p.Position) std::cout << '\t' << std::bit_cast<uint32_t>(v);
        for (float v : p.Quaternion) std::cout << '\t' << std::bit_cast<uint32_t>(v);
        std::cout << '\n';
    }
}
static void Predicates(const NativeCollisionSnapshot& snapshot, int window) {
    RealtimeGameplayWorld indexed, exhaustive; std::string error;
    s_Sealed = true;
    Require(indexed.Rebuild(snapshot,error) && exhaustive.Rebuild(snapshot,error,false),error);
    const auto metadata = [](const NativeCollisionHit& h) {
        return std::tuple{h.Model,h.Library,h.Ipl,h.ModelId,h.HeaderId,h.Record,h.PrimitiveIndex,h.Binary,
                          h.ValidatedHeaderId,h.TimeShared,h.Primitive,h.Surface.Material,h.Surface.Flags,h.Surface.Brightness,h.Surface.Light};
    };
    size_t hits = 0;
    for (size_t n = 0; n < 128; ++n) {
        const auto& p = snapshot.Instances[n * snapshot.Instances.size() / 128].Placement.Position;
        RealtimeVec3 from{p[0],p[1],p[2]+10}, to{p[0]+1,p[1]+1,p[2]-10}, a{}, b{};
        NativeCollisionHit ma,mb; float fa = -999, fb = -999;
        const auto check = [&](bool x, bool y) {
            Require(x == y,"COL indexed/exhaustive predicate mismatch");
            if (x) { ++hits; Require(metadata(ma) == metadata(mb),"COL provenance/order mismatch"); }
        };
        check(indexed.Ground(from.X,from.Y,from.Z,to.Z,fa,&ma),exhaustive.Ground(from.X,from.Y,from.Z,to.Z,fb,&mb));
        Require(fa == fb,"COL ground result mismatch");
        check(indexed.Raycast(from,to,a,&ma),exhaustive.Raycast(from,to,b,&mb));
        Require(a.X == b.X && a.Y == b.Y && a.Z == b.Z,"COL ray point mismatch");
        check(indexed.SphereBlocked({p[0],p[1],p[2]},.34f,&ma),exhaustive.SphereBlocked({p[0],p[1],p[2]},.34f,&mb));
        for (bool walkable : {false,true}) {
            fa = fb = -999; a = b = {};
            check(indexed.SweepSphere(from,to,.34f,fa,walkable,INFINITY,&a,&ma),
                  exhaustive.SweepSphere(from,to,.34f,fb,walkable,INFINITY,&b,&mb));
            Require(fa == fb && a.X == b.X && a.Y == b.Y && a.Z == b.Z,"COL sweep fraction/normal mismatch");
        }
    }
    Require(hits > 0 && indexed.TriangleTests() == exhaustive.TriangleTests(),"vacuous or changed COL traversal");
    if (window == 4) {
        float low{}, high{};
        Require(indexed.Ground(1540,-1736,20,5,low) && indexed.Ground(1540,-1740.013672f,20,5,high),"source curb absent");
        Require(low == 12.3828125f && high == 12.546875f,"source curb numeric predicate changed");
        std::cout << "PASS source curb low=12.382812500 high=12.546875000 rise=0.164062500\n";
    }
    s_Sealed = false;
    std::cout << "PASS COL predicates window=" << window << " queries=640 hits=" << hits << " memory-only indexed/exhaustive exact\n";
}
static void Synthetic(const char* game, const NativeCollisionPlacement& model) {
    constexpr uint32_t words[]{0,256,512,1024,2048,4096,0x40000000,0x80000000,257,255,0xffffffff,0};
    s_Text = "inst\n";
    for (auto word : words) {
        // Source scanf uses signed %d; bit 31 must survive the uint32 conversion.
        s_Text += std::to_string(model.ModelId) + ", " + model.Model + ", " +
            std::to_string(std::bit_cast<int32_t>(word)) + ", 20000, 20000, 20, 0, 0, 0, 1, -1\n";
    }
    s_Text += "end\n"; s_Fixture = true;
    char err[512]{}; std::string error; E2ELoadInfo load;
    Require(StreamPager_Init(game, load, err, sizeof(err), {true, 120, 8}), err);
    auto context = NativeCollisionContext::LoadBeforeWorker(game, 120, error); Require(bool(context), error);
    const auto& pop = context->Population;
    for (size_t i = 0; i < std::size(words); ++i) {
        const auto& p = pop.Instances.at(i);
        Require(!p.Binary && p.Record == i && p.ModelId == model.ModelId && p.Flags == words[i] &&
                p.Interior == int(words[i] & 255), "synthetic packed word/ordinal/area");
    }
    WorldShotScene original, moved; E2EPagerFrame frame;
    std::vector<NativePlacementIdentity> ids, replaced;
    Require(StreamPager_Update(20000,20000,20,original,frame,err,sizeof(err),{},&ids), err);
    Require(frame.instances == 8 && !frame.fallback && ids.size() == 8, "candidate cap/fallback");
    for (size_t i = 0; i < ids.size(); ++i) Require(ids[i].Matches(pop.Instances[i]), "equal-distance source order/high-bit residency");
    NativeCollisionSnapshot source, disabled, inside;
    Require(context->Snapshot(20000,20000,source,error), error);
    Require(source.Instances.size() == 9, "same-area high bits source COL / interior excluded");
    Require(context->Assets.Snapshot(pop,20000,20000,120,inside,error,1), error);
    Require(inside.Instances.size() == 1 && inside.Instances[0].Placement.Record == 8, "true interior area selection");
    auto override = std::make_shared<const NativePlacementOverrides>(std::vector<NativePlacementOverride>{
        {NativePlacementIdentity::From(pop.Instances[1]), {20000,20000,25}, source.Instances[1].Basis, false}});
    Require(StreamPager_Update(20000,20000,20,moved,frame,err,sizeof(err),override,&replaced), err);
    Require(ids == replaced && moved.meshes.size() == original.meshes.size(), "override candidate identities/cap");
    for (size_t i = 0; i < ids.size(); ++i) {
        if (i != 1) Require(Geometry(original.meshes[i]) == Geometry(moved.meshes[i]), "same model other record moved");
        else for (size_t j = 0; j < original.meshes[i].pos.size(); ++j)
            Require(std::abs(moved.meshes[i].pos[j] - original.meshes[i].pos[j] - (j % 3 == 2 ? 5 : 0)) < .003f,
                    "exact override DFF transform");
    }
    Require(context->Snapshot(20000,20000,disabled,error,override), error);
    Require(disabled.Instances.size() + 1 == source.Instances.size(), "COL override suppression count");
    for (const auto& i : disabled.Instances) {
        Require(i.Placement.Record != 1, "disabled row survived");
        const auto it = std::ranges::find_if(source.Instances,[&](const auto& before) {
            return NativePlacementIdentity::From(i.Placement).Matches(before.Placement);
        });
        Require(it != source.Instances.end() && it->Model == i.Model && it->Basis == i.Basis &&
                it->Min == i.Min && it->Max == i.Max, "same-ID other record COL changed");
    }
    NativeCollisionPopulation again; Require(StreamPager_CollisionPopulation(again,error), error);
    Require(again.Instances.size() == pop.Instances.size(), "population mutated");
    for (size_t i = 0; i < pop.Instances.size(); ++i) Require(
        NativePlacementIdentity::From(pop.Instances[i]).Matches(again.Instances[i]) &&
        pop.Instances[i].Position == again.Instances[i].Position && pop.Instances[i].Flags == again.Instances[i].Flags,
        "overrides modified authored population");
    StreamPager_Shutdown(); s_Fixture = false;
    std::cout << "PASS synthetic signed/unknown bits, render cap/order, source COL area, exact overrides and immutable population\n";
}
#ifdef NATIVE_IPLFLAGS_PRODUCT_OS
static void Curb(const char* dir, const NativeCollisionContext& context) {
    std::string error; NativeCollisionSnapshot snapshot;
    Require(context.Snapshot(1540,-1736,snapshot,error),error);
    RealtimeGameplayWorld world; Require(world.Rebuild(snapshot,error),error);
    RealtimeGameplay game; Require(game.Initialize(dir,error),error);
    s_Sealed = true;
    for (double hz : {59.7,60.0,120.0}) {
        RealtimeVec3 replay{};
        for (int run = 0; run < 2; ++run) {
            Require(game.Spawn(world,1540,-1736,20,-1.57079632679f,error),error);
            const auto start = game.State().Ped;
            int airborne = 0;
            auto leg = [&](float forward) {
                for (int i = 0; i < int(std::round(2*hz)); ++i) {
                    game.Tick(1/hz,{.Forward=forward},world);
                    const auto p = game.State().Ped;
                    airborne += !game.State().Grounded;
                    for (float z : {.344f,.61f,.88f,1.15f,1.42f})
                        Require(!world.SphereBlocked({p.X,p.Y,p.Z+z},.34f),"source curb body penetration");
                }
            };
            leg(1); const auto peak = game.State().Ped;
            Require(peak.Y < -1739.9f && peak.Z > 12.53f && !airborne,"source curb ascent");
            leg(-1); const auto end = game.State().Ped;
            Require(std::hypot(end.X-start.X,end.Y-start.Y) < .03f && std::abs(end.Z-start.Z) < .005f && !airborne,
                    "source curb descent");
            if (!run) replay = end;
            else Require(end.X == replay.X && end.Y == replay.Y && end.Z == replay.Z,"source curb replay");
            std::printf("PASS source curb Tick hz=%.1f replay=%d peak=%.6f,%.6f,%.6f end=%.6f,%.6f,%.6f air=%d COLinstances=%zu triangles=%zu\n",
                hz,run,peak.X,peak.Y,peak.Z,end.X,end.Y,end.Z,airborne,snapshot.Instances.size(),world.TriangleCount());
        }
    }
    s_Sealed = false;
}
#endif
int main(int argc, char** argv) try {
    Require(argc == 3, "GAME_DIR runtime|offline|synthetic required");
    const bool offline = std::string(argv[2]) == "offline", synthetic = std::string(argv[2]) == "synthetic";
    char err[512]{}; std::string error; E2ELoadInfo load;
    Require(StreamPager_Init(argv[1],load,err,sizeof(err),offline ? StreamPagerOptions{} : StreamPagerOptions{true,900,4096}),err);
#ifdef NATIVE_IPLFLAGS_PRODUCT_OS
    if (std::string(argv[2]) == "curb") {
        auto context = NativeCollisionContext::LoadBeforeWorker(argv[1],900,error); Require(bool(context),error);
        Curb(argv[1],*context); StreamPager_Shutdown(); return 0;
    }
#endif
    NativeCollisionPopulation pop;
    if (offline) Require(!StreamPager_CollisionPopulation(pop,error) && !error.empty(), "offline export must remain unavailable");
    else Require(StreamPager_CollisionPopulation(pop,error),error);
    if (!synthetic) Population(pop);
    std::cout << "LOAD\t" << load.iplTotal << '\t' << load.iplKept << '\t' << load.binaryInstances << '\n';
    std::shared_ptr<const NativeCollisionContext> context;
    if (!offline) { context = NativeCollisionContext::LoadBeforeWorker(argv[1],900,error); Require(bool(context),error); }
    NativeCollisionPlacement fixtureModel;
    int window = 0;
    for (const auto center : {NativeCollisionVector{2488.562255859375f,-1666.864501953125f,15},
                             NativeCollisionVector{2495,-1687,15}, NativeCollisionVector{325,2537,17.5f},
                             NativeCollisionVector{-2026,156,30}, NativeCollisionVector{1540,-1736,20}}) {
        WorldShotScene scene; E2EPagerFrame frame; std::vector<NativePlacementIdentity> ids;
        Require(StreamPager_Update(center[0],center[1],center[2],scene,frame,err,sizeof(err),{},&ids),err);
        Require(ids.size() == scene.meshes.size(),"resident identity mapping");
        if (!synthetic) for (size_t i = 0; i < ids.size(); ++i) {
            std::cout << "RENDER\t" << window; Identity(ids[i]);
            std::cout << '\t' << scene.meshes[i].tris << '\t' << Geometry(scene.meshes[i]) << '\n';
        }
        size_t tris = 0; std::set<std::string> sources, binarySources;
        NativeCollisionSnapshot snapshot;
        if (context) {
            Require(context->Snapshot(center[0],center[1],snapshot,error),error);
            if (!synthetic) Predicates(snapshot,window);
            for (const auto& i : snapshot.Instances) {
                sources.insert(i.Placement.Ipl); if (i.Placement.Binary) binarySources.insert(i.Placement.Ipl);
                tris += i.Model->Faces.size();
                Hash h; h.Range(i.Min); h.Range(i.Max); for (const auto& axis : i.Basis) h.Range(axis);
                for (auto v : i.Model->SourceChunk) h.Add(uint32_t(v));
                if (!synthetic) {
                    std::cout << "COL\t" << window; Identity(NativePlacementIdentity::From(i.Placement));
                    std::cout << '\t' << i.Model->Faces.size() << '\t' << h.Value << '\n';
                }
                if (fixtureModel.ModelId < 0 && std::ranges::any_of(ids,[&](const auto& id) { return id.Matches(i.Placement); }))
                    fixtureModel = i.Placement;
            }
        }
        std::cout << "WINDOW\t" << window++ << '\t' << frame.instances << '\t' << frame.tris << '\t'
                  << snapshot.Instances.size() << '\t' << tris << '\t' << sources.size() << '\t' << binarySources.size()
                  << '\t' << snapshot.InteriorExcluded << '\t' << snapshot.MissingModels << '\t' << snapshot.EmptyModels << '\n';
        if (synthetic) break;
    }
    StreamPager_Shutdown();
    if (synthetic) { Require(fixtureModel.ModelId >= 0,"synthetic source geometry witness"); Synthetic(argv[1],fixtureModel); }
    s_Sealed = true;
    Require(offline || !pop.Instances.empty(),"owned population lost on shutdown");
    return 0;
} catch (const std::exception& e) { std::cerr << "FAIL " << e.what() << '\n'; return 2; }
