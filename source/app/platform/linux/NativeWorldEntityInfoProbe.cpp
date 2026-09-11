// Independent metadata acceptance executable; no product startup integration.
#include "app/platform/linux/NativeWorldEntityInfo.h"
#include "app/platform/linux/StreamPager.h"
#include <cstdio>
#include <iostream>
#include <limits>
#include <stdexcept>

using int32 = int32_t;
using int64 = int64_t;
using uint32 = uint32_t;
using uint64 = uint64_t;
#ifndef __stdcall
#define __stdcall
#endif
#include "oswrapper/oswrapper.h"

static bool s_Sealed{};
static size_t s_Opens{};
static void Require(bool ok, const std::string& message) { if (!ok) throw std::runtime_error(message); }
int32 OS_FileOpen(OSFileDataArea, void** out, const char* path, OSFileAccessType access) {
    Require(!s_Sealed && access == FILE_ACCESS_READ, "unexpected frame IO/write");
    ++s_Opens; *out = std::fopen(path, "rb"); return *out ? 0 : 1;
}
int32 OS_FileClose(void* f) { return std::fclose(static_cast<FILE*>(f)); }
int32 OS_FileSize(void* f) {
    auto* file = static_cast<FILE*>(f); const auto pos = std::ftell(file);
    std::fseek(file, 0, SEEK_END); const auto size = std::ftell(file); std::fseek(file, pos, SEEK_SET);
    Require(size >= 0 && size <= INT32_MAX, "probe file range"); return static_cast<int32>(size);
}
int32 OS_FileRead(void* f, void* out, int32 size) {
    Require(!s_Sealed && size >= 0, "unexpected frame read");
    return std::fread(out, 1, size, static_cast<FILE*>(f)) == static_cast<size_t>(size) ? 0 : 3;
}
int32 OS_FileGetPosition(void* f) { return static_cast<int32>(std::ftell(static_cast<FILE*>(f))); }
void OS_FileSetPosition(void* f, int32 pos) { Require(!s_Sealed, "unexpected frame seek"); std::fseek(static_cast<FILE*>(f), pos, SEEK_SET); }
void OS_SetFilePathOffset(const char*) {}

static NativeCollisionPlacement Placement(int id, const char* name, uint32_t record = 0) {
    NativeCollisionPlacement p; p.ModelId = id; p.Model = name; p.Ipl = "fixture.ipl"; p.Record = record; return p;
}
static void Fixtures() {
    using C = NativeWorldInitialClass;
    using B = NativeWorldKnownBool;
    const std::vector<NativeWorldEntitySourceText> ide{
        {"first.ide", "objs\n10 oldname txd 100 0\n11 plain txd 100 0\n12 lod_dummy txd 100 0\nend\ntobj\n13 door_dy txd 100 0 6 20\n14 door_nt txd 100 0 20 6\nend\n"},
        {"second.ide", "objs\n10 NewName txd 100 0\n15 def0 txd 100 0\n16 def1 txd 100 0\n17 def2 txd 100 0\n18 def3 txd 100 0\nend\nanim\n20 inert txd null 100 0\n21 moving txd NULL 100 0\n22 assignedanim txd anim 100 0\nend\n"}
    };
    const NativeWorldEntitySourceText objects{"object-fixture.dat",
        " ; comment\n# comment\n"
        "OLDNAME 1 2 3 4 5 6 7 0 0 0 0 0\n"
        "nEwNaMe, 1,2,3,4,5,6,7,0,0,0,0,0\n"
        "lod_dummy 1 2 3 4 5 6 7 0 0 0 0 0\n"
        "door_dy 1 2 3 4 5 6 7 0 0 0 0 0\n"
        "def0 99999 2 3 4 5 6 1 256 256 256 0 0\n"
        "def1 99999 2 3 4 5 6 1 0 0 1 0 0\n"
        "def2 99999 2 3 4 5 6 1 0 4 0 0 0\n"
        "def3 99999 2 3 4 5 6 1 0 4 1 0 0\n"
        "NEWNAME 99999 2 3 4 5 6 1 0 4 0 0 0 optional-malformed-is-allowed\n"
        "assignedanim 1 2 3 4 5 6 7 0 0 0 0 0\n"
        "* stop\nplain malformed ignored after terminator\n"};
    NativeCollisionPopulation pop;
    for (auto [id, name] : std::initializer_list<std::pair<int, const char*>>{
        {10,"newname"},{11,"plain"},{12,"lod_dummy"},{13,"door_dy"},{14,"door_nt"},
        {15,"def0"},{16,"def1"},{17,"def2"},{18,"def3"},{20,"inert"},{21,"moving"},{22,"assignedanim"},{99,"unknown"}}) {
        pop.Models[id] = {name, false}; pop.Instances.push_back(Placement(id, name, id));
    }
    pop.Models[13].TimeModel = pop.Models[14].TimeModel = true;
    pop.Instances[0].Interior = 256; // Explicit legacy/import fixture: full word not certified.
    pop.Instances[1].Binary = true; pop.Instances[1].Flags = 0x300;
    NativeWorldEntityInfo info; std::string error;
    Require(info.LoadSources(pop, ide, objects, error), error);
    auto model = [&](int id, const char* name) -> const NativeWorldModelInfo& {
        const auto* value = info.FindModel(id, name); Require(value, "fixture model lookup"); return *value;
    };
    Require(model(10,"NEWNAME").Ide.Source == "second.ide" && model(10,"newname").ObjectRows.size() == 2 &&
            model(10,"newname").ObjectRows.back().Line == 11 && model(10,"newname").DefaultObjectInfoIndex == 2,
            "source ID replacement / uppercase lookup / assignment duplicate order");
    Require(!info.FindModel(10,"oldname"), "old IDE alias survived overwrite");
    Require(model(11,"plain").InitialClass == C::Building && model(12,"lod_dummy").InitialClass == C::DummyObject,
            "source assignment, not LOD/name heuristic");
    Require(model(13,"door_dy").InitialClass == C::DummyObject && model(14,"door_nt").InitialClass == C::Building,
            "ObjectInfo must not inherit from shared/time-named geometry");
    NativeCollisionInstance shared;
    shared.Placement = pop.Instances[4]; // door_nt, whose geometry may be owned by door_dy.
    auto sharedCol = std::make_shared<NativeCollisionModel>(); sharedCol->Name = "door_dy"; sharedCol->HeaderId = 13;
    shared.Model = sharedCol; shared.TimeShared = true;
    Require(info.Query(shared.Placement).Model->InitialClass == C::Building, "time COL owner does not own entity metadata");
    for (int id = 15; id <= 18; ++id) {
        const auto& m = info.Models().at(id);
        Require(m.InitialClass == C::DummyObject && m.DefaultObjectInfoIndex == id - 15, "default assignment is still dummy");
    }
    Require(model(20,"inert").InitialClass == C::Building && model(21,"moving").InitialClass == C::AnimatedBuilding &&
            model(22,"assignedanim").InitialClass == C::DummyObject, "case-sensitive anim null / dummy precedence");
    const auto text = info.Query(pop.Instances[0]), binary = info.Query(pop.Instances[1]);
    Require(text.Placement && !text.Placement->SourceInstanceType && text.Placement->Area == 0 &&
            binary.Placement && binary.Placement->SourceInstanceType == 0x300 && binary.Placement->Area == 0,
            "known/unknown instance metadata");
    Require(text.Placement->UsesCollision == B::Unknown && text.Placement->IsBigBuilding == B::Unknown &&
            text.InitialBuildingMask() == B::False && binary.InitialBuildingMask() == B::True, "class-only filter contract");
    Require(text.Placement->AuthoredRedundantStream == B::Unknown &&
            binary.Placement->AuthoredRedundantStream == B::True && binary.Placement->AuthoredDontStream == B::True &&
            binary.Placement->AuthoredUnderwater == B::False, "legacy unknown versus binary authored bits");
    auto runtime = pop; runtime.IncludesStreamed = true;
    runtime.Instances[0].Interior = 0; runtime.Instances[0].Flags = 0x80001f00;
    NativeWorldEntityInfo sourceInfo;
    Require(sourceInfo.LoadSources(runtime, ide, objects, error), error);
    const auto full = sourceInfo.Query(runtime.Instances[0]);
    Require(full.Placement && full.Placement->SourceInstanceType == 0x80001f00 && full.Placement->Area == 0 &&
            full.Placement->AuthoredRedundantStream == B::True && full.Placement->AuthoredDontStream == B::True &&
            full.Placement->AuthoredUnderwater == B::True && full.Placement->AuthoredTunnel == B::True &&
            full.Placement->AuthoredTunnelTransition == B::True, "runtime full text word and typed authored bits");
    Require(full.Placement->UsesCollision == B::Unknown && full.Placement->IsBigBuilding == B::Unknown &&
            full.Placement->InNormalBuildingSector == B::Unknown && full.Placement->InWorld == B::Unknown,
            "authored flags do not certify current world eligibility");
    auto staleWord = runtime.Instances[0]; staleWord.Flags ^= 0x80000000;
    Require(sourceInfo.Query(staleWord).Status == NativeWorldInfoStatus::IdentityMismatch, "unknown-bit stale metadata");
    runtime.Instances[0].Interior = 1;
    Require(!sourceInfo.LoadSources(runtime, ide, objects, error) &&
            sourceInfo.Query(staleWord).Status == NativeWorldInfoStatus::IdentityMismatch,
            "runtime text area/type mismatch rejects atomically");
    Require(info.Query(pop.Instances.back()).Status == NativeWorldInfoStatus::ModelUnrepresented, "typed unknown");
    auto stale = pop.Instances[0]; stale.ModelId = 11;
    Require(info.Query(stale).Status == NativeWorldInfoStatus::IdentityMismatch, "stale numeric model ID");
    stale = pop.Instances[0]; ++stale.Record;
    Require(info.Query(stale).Status == NativeWorldInfoStatus::PlacementUnrepresented, "source record identity");
    auto moved = pop.Instances[0]; moved.Position = {1,2,3};
    Require(info.Query(moved).Status == NativeWorldInfoStatus::Ready, "replacement position preserves identity");
    const auto* preserved = info.FindModel(10,"newname");
    auto reject = [&](const NativeCollisionPopulation& badPop, const auto& badIde, const auto& badObjects) {
        Require(!info.LoadSources(badPop, badIde, badObjects, error) && !error.empty() &&
                info.FindModel(10,"newname") == preserved && info.Query(pop.Instances[0]).Model == preserved,
                "invalid load must preserve exact owner allocation");
    };
    reject(pop, ide, NativeWorldEntitySourceText{"bad", "plain 1 2 3\n"});
    reject(pop, ide, NativeWorldEntitySourceText{"bad", "plain 1 2 3 4 5 6 7 0 0 0 0 999999999999999999999\n"});
    auto malformed = ide; malformed.push_back({"bad.ide", "objs\n10 newname txd nonsense 0\nend\n"});
    reject(pop, malformed, objects);
    auto ambiguous = ide; ambiguous.push_back({"alias.ide", "cars\n77 NEWNAME txd\nend\n"});
    reject(pop, ambiguous, objects); // Non-static namespace matters to source lookup too.
    auto wrong = pop; wrong.Models[10].Name = "oldname"; reject(wrong, ide, objects);
    wrong = pop; wrong.Instances.push_back(wrong.Instances[0]); reject(wrong, ide, objects);
    wrong = pop; wrong.Instances[0].Position[0] = std::numeric_limits<float>::quiet_NaN(); reject(wrong, ide, objects);
    wrong = pop; wrong.Instances[1].Interior = 1; reject(wrong, ide, objects);
    Require(!info.LoadBeforeWorker("", pop, error) && info.FindModel(10,"newname") == preserved, "failed IO preserves owner");
    std::cout << "PASS synthetic parser, defaults, duplicate order, animation, unknown, identities, atomic failure\n";
}

int main(int argc, char** argv) try {
    Require(argc == 2, "GAME_DIR required"); Fixtures();
    char err[512]{}; std::string error; E2ELoadInfo stats;
    Require(StreamPager_Init(argv[1], stats, err, sizeof(err), {true,900,4096}), err);
    auto context = NativeCollisionContext::LoadBeforeWorker(argv[1], 900, error); Require(bool(context), error);
    NativeWorldEntityInfo info; Require(info.LoadBeforeWorker(argv[1], context->Population, error), error);
    NativeCollisionSnapshot startup, airfield;
    Require(context->Snapshot(2488.562255859375f, -1666.864501953125f, startup, error), error);
    Require(context->Snapshot(325, 2537, airfield, error), error);
    auto legacyPopulation = context->Population;
    for (auto& p : legacyPopulation.Instances) if (!p.Binary) {
        p.Interior = static_cast<int32_t>(p.Flags); p.Flags = 0;
    }
    NativeCollisionSnapshot legacy;
    Require(context->Assets.Snapshot(legacyPopulation,2488.562255859375f,-1666.864501953125f,900,legacy,error),error);
    std::array<size_t,4> legacyCounts{};
    for (const auto& i : legacy.Instances) {
        const auto* m = info.FindModel(i.Placement.ModelId,i.Placement.Model); Require(m,"legacy model lookup");
        ++legacyCounts[static_cast<int>(m->InitialClass)];
    }
    Require(legacy.Instances.size() == 5780 && legacyCounts[0] == 0 &&
            legacyCounts[1] + legacyCounts[2] == 3081 && legacyCounts[3] == 2699,
            "pinned old source-window census");
    StreamPager_Shutdown(); // The metadata owns its copies; no pager pointers.
    s_Sealed = true; const auto opens = s_Opens;
    for (const auto& [id, m] : info.Models()) {
        std::cout << "MODEL\t" << id << '\t' << m.Name << '\t' << static_cast<int>(m.InitialClass) << '\t'
                  << m.Ide.Source << '\t' << m.Ide.Line << '\t' << m.ObjectRows.size() << '\t'
                  << (m.ObjectRows.empty() ? 0 : m.ObjectRows.back().Line) << '\t'
                  << (m.DefaultObjectInfoIndex ? int(*m.DefaultObjectInfoIndex) : -1) << '\n';
    }
    std::array<size_t, 4> all{}, window{};
    for (const auto& p : context->Population.Instances) {
        auto q = info.Query(p); Require(q.Status == NativeWorldInfoStatus::Ready && q.Model, "population query unknown");
        ++all[static_cast<int>(q.Model->InitialClass)];
        Require(q.Placement->SourceInstanceType == p.Flags && q.Placement->Area == p.Interior &&
                q.Placement->UsesCollision == NativeWorldKnownBool::Unknown &&
                q.Placement->IsBigBuilding == NativeWorldKnownBool::Unknown &&
                q.Placement->InNormalBuildingSector == NativeWorldKnownBool::Unknown &&
                q.Placement->InWorld == NativeWorldKnownBool::Unknown, "authored type / invented eligibility");
    }
    for (const auto& i : startup.Instances) {
        auto q = info.Query(i.Placement); Require(q.Status == NativeWorldInfoStatus::Ready, "snapshot query identity");
        ++window[static_cast<int>(q.Model->InitialClass)];
        std::cout << "WINDOW\t" << i.Placement.ModelId << '\n';
        if (!i.Placement.Binary && i.Placement.Flags != 0) {
            Require(i.Placement.Interior == 0 && (i.Placement.Flags & 255) == 0, "addition must be source outdoor");
            std::cout << "ADDED\t" << i.Placement.Ipl << '\t' << i.Placement.Record << '\t'
                      << i.Placement.ModelId << '\t' << i.Placement.Flags << '\n';
        }
        // Even a stale COL header cannot redirect the metadata to a different model.
        auto stale = i; auto col = std::make_shared<NativeCollisionModel>(*i.Model); col->HeaderId = 1224; stale.Model = col;
        Require(info.Query(stale.Placement).Model == q.Model, "COL header ID leaked into model identity");
    }
    Require(window[0] == 0, "startup class coverage");
    bool crate = false, terrain = false;
    for (const auto& i : airfield.Instances) {
        const auto& p = i.Placement;
        if (p.Ipl.find("countn2_stream2.ipl") == std::string::npos) continue;
        const auto q = info.Query(p);
        if (p.ModelId == 1224 && p.Record == 63) {
            Require(p.Model == "woodenbox" && q.InitialBuildingMask() == NativeWorldKnownBool::False &&
                    q.Model->ObjectRows.back().Line == 126, "woodenbox source ground counterexample"); crate = true;
        }
        if (p.ModelId == 16177 && p.Record == 32) {
            Require(p.Model == "ne_bit_07" && q.InitialBuildingMask() == NativeWorldKnownBool::True &&
                    q.Model->ObjectRows.empty() && q.Model->Ide.Line == 179, "Rustler terrain witness classification"); terrain = true;
        }
    }
    Require(crate && terrain && opens == s_Opens, "witness coverage / frame IO");
    for (const auto* label : {"POPULATION", "STARTUP"}) {
        const auto& counts = label[0] == 'P' ? all : window;
        std::cout << label; for (auto count : counts) std::cout << '\t' << count; std::cout << '\n';
    }
    std::cout << "LEGACY"; for (auto count : legacyCounts) std::cout << '\t' << count; std::cout << '\n';
    std::cout << "PASS actual source ownership, woodenbox/ne_bit_07, stale COL IDs, zero post-load IO; ground precision/order/ties unported\n";
    return 0;
} catch (const std::exception& e) { std::cerr << "FAIL " << e.what() << '\n'; return 2; }
