// Independent app-only catalog consumer. No renderer/CMake/product edits required.
#include "app/platform/linux/NativeLodCatalog.h"
#include "app/platform/linux/StreamPager.h"
#include <algorithm>
#include <bit>
#include <cstdio>
#include <iostream>
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
static void Require(bool ok, const std::string& message) { if (!ok) throw std::runtime_error(message); }
int32 OS_FileOpen(OSFileDataArea, void** out, const char* path, OSFileAccessType access) {
    Require(!s_Sealed && access == FILE_ACCESS_READ, "unexpected file access");
    *out = std::fopen(path, "rb"); return *out ? 0 : 1;
}
int32 OS_FileClose(void* f) { return std::fclose(static_cast<FILE*>(f)); }
int32 OS_FileSize(void* f) {
    auto* file = static_cast<FILE*>(f); const auto pos = std::ftell(file);
    std::fseek(file, 0, SEEK_END); const auto size = std::ftell(file); std::fseek(file, pos, SEEK_SET);
    Require(size >= 0 && size <= INT32_MAX, "file size bounds"); return size;
}
int32 OS_FileRead(void* f, void* out, int32 size) {
    Require(!s_Sealed && size >= 0, "post-publication read");
    return std::fread(out, 1, size, static_cast<FILE*>(f)) == size_t(size) ? 0 : 3;
}
int32 OS_FileGetPosition(void* f) { return std::ftell(static_cast<FILE*>(f)); }
void OS_FileSetPosition(void* f, int32 pos) {
    Require(!s_Sealed && pos >= 0, "post-publication seek"); std::fseek(static_cast<FILE*>(f), pos, SEEK_SET);
}
void OS_SetFilePathOffset(const char*) {}

static uint64_t TransformHash(const NativeCollisionPlacement& p) {
    uint64_t hash = 1469598103934665603ull;
    const auto add = [&](float v) {
        const auto bits = std::bit_cast<uint32_t>(v);
        for (int i = 0; i < 4; ++i) { hash ^= (bits >> (i * 8)) & 255; hash *= 1099511628211ull; }
    };
    for (auto v : p.Position) add(v);
    for (auto v : p.Quaternion) add(v);
    return hash;
}
static void Fixtures() {
    std::string error;
    NativeLodSource text, stream;
    text.Key = "data\\maps\\fixture.ipl"; text.Name = "fixture";
    stream.Key = "archive:fixture_stream0.ipl"; stream.Name = "fixture_stream0"; stream.Binary = true;
    for (int i = 0; i < 2; ++i) {
        NativeCollisionPlacement p;
        p.Model = i ? "plain_parent" : "lod_named_child"; p.ModelId = 100 + i;
        p.Ipl = text.Key; p.Record = i; p.Lod = i ? -1 : 1;
        text.Records.push_back(p);
    }
    auto child = text.Records[0]; child.Ipl = stream.Key; child.Binary = true; child.Record = 0;
    stream.Records.push_back(child);
    auto build = [&](std::vector<NativeLodSource> sources) {
        NativeCollisionPopulation population; population.IncludesStreamed = true;
        for (const auto& source : sources) for (const auto& p : source.Records) population.Instances.push_back(p);
        return NativeLodCatalog::Assemble(population, std::move(sources), {}, error);
    };
    auto graph = build({text, stream}); Require(bool(graph), error);
    Require(!graph->DiskValidated() && graph->Nodes()[1].Children.size() == 2 && graph->Nodes()[2].Parent == 1,
            "contextual binding/name-independent fixture");
    auto missing = stream; missing.Name = "unrelated_stream0";
    graph = build({text, missing}); Require(graph && graph->Nodes()[2].Link == NativeLodLinkStatus::UnknownParentSource, "unknown source");
    auto duplicateStem = text; duplicateStem.Key = "elsewhere\\fixture.ipl";
    for (auto& p : duplicateStem.Records) p.Ipl = duplicateStem.Key;
    graph = build({text, duplicateStem, stream});
    Require(graph && graph->Nodes()[4].Link == NativeLodLinkStatus::AmbiguousParentSource &&
            graph->Nodes()[4].ParentSourceCandidates.size() == 2, "ambiguous source");
    auto invalid = stream; invalid.Records[0].Lod = 2;
    graph = build({text, invalid}); Require(graph && graph->Nodes()[2].Link == NativeLodLinkStatus::InvalidIndex, "context bounds");
    invalid.Records[0].Lod = -2;
    graph = build({text, invalid}); Require(graph && graph->Nodes()[2].Link == NativeLodLinkStatus::InvalidIndex, "negative index");
    auto cycle = text; cycle.Records[1].Lod = 0;
    graph = build({cycle, stream}); Require(graph && graph->Nodes()[0].Link == NativeLodLinkStatus::Cycle &&
        graph->Nodes()[1].Link == NativeLodLinkStatus::Cycle && graph->Nodes()[2].Link == NativeLodLinkStatus::InvalidAncestor &&
        !graph->Nodes()[0].Parent && graph->Nodes()[0].CandidateParent == 1, "cycle and invalid ancestor");
    cycle = text; cycle.Records[0].Lod = 0;
    graph = build({cycle}); Require(graph && graph->Nodes()[0].Link == NativeLodLinkStatus::Cycle, "self cycle");
    Require(!build({text, text}) && error.starts_with("Error:"), "duplicate identity rejected");
    auto gap = text; gap.Records[1].Record = 9;
    Require(!build({gap}) && error.starts_with("Error:"), "contextual record gap rejected");
    std::cout << "FIXTURES\tpass\tcontextual,unknown,ambiguous,bounds,negative,cycle,ancestor,self,duplicate,gap\n";
}
int main(int argc, char** argv) try {
    Require(argc == 2, "usage: NativeLodCatalogProbe GAME");
    Fixtures();
    E2ELoadInfo info; char message[512]{}; std::string error;
    Require(StreamPager_Init(argv[1], info, message, sizeof(message), {true, 900.0f, 4096}), message);
    NativeCollisionPopulation population;
    Require(StreamPager_CollisionPopulation(population, error), error);
    StreamPager_Shutdown();
    const auto graph = NativeLodCatalog::LoadBeforeWorker(argv[1], population, error);
    Require(bool(graph), error); Require(graph->DiskValidated(), "raw disk validation missing");
    // Caller lifetime/order and failed-export proof: graph owns everything.
    auto corrupt = population; corrupt.Instances[0].Lod = INT32_MAX;
    Require(!NativeLodCatalog::LoadBeforeWorker(argv[1], corrupt, error) && error.find("mismatch") != std::string::npos,
            "raw source mismatch rejected");
    std::reverse(population.Instances.begin(), population.Instances.end());
    const auto reordered = NativeLodCatalog::LoadBeforeWorker(argv[1], population, error);
    Require(reordered && reordered->Nodes().size() == graph->Nodes().size(), "population order must not define IPL indices");
    population = {}; corrupt = {}; s_Sealed = true;
    for (size_t i = 0; i < graph->Sources().size(); ++i) {
        const auto& s = graph->Sources()[i];
        std::cout << "SOURCE\t" << i << '\t' << s.Key << '\t' << s.Name << '\t' << s.Binary << '\t'
                  << s.Declaration.Source << '\t' << s.Declaration.Line << '\t' << s.Archive << '\t'
                  << s.ArchiveOrder << '\t' << s.DirectoryRecord << '\t' << s.Sector << '\t' << s.Sectors << '\t' << s.Records.size() << '\n';
    }
    size_t edges{}, unknown{}, time{};
    for (size_t i = 0; i < graph->Nodes().size(); ++i) {
        const auto& n = graph->Nodes()[i]; const auto& other = reordered->Nodes()[i];
        Require(n.Identity == other.Identity && n.Parent == other.Parent && n.Children == other.Children, "order-dependent graph");
        Require(graph->Find(n.Identity) == &n, "exact identity lookup");
        auto wrong = n.Identity; wrong.Model += "_wrong"; Require(!graph->Find(wrong), "inexact lookup accepted");
        const auto metadata = graph->Metadata(n);
        Require(metadata.Status == NativeWorldInfoStatus::Ready && metadata.Placement->SourceInstanceType == n.Placement.Flags,
                "owned source metadata unavailable");
        Require(n.RuntimeModelIsLod == NativeWorldKnownBool::Unknown && n.RuntimeBigBuilding == NativeWorldKnownBool::Unknown &&
                n.RuntimeUsesCollision == NativeWorldKnownBool::Unknown, "unproved runtime state invented");
        edges += n.Parent.has_value(); unknown += n.Link != NativeLodLinkStatus::None && n.Link != NativeLodLinkStatus::Bound;
        time += metadata.Model->Kind == NativeWorldModelKind::TimeAtomic;
        std::cout << "NODE\t" << i << '\t' << n.Source << '\t' << n.LocalIndex << '\t' << n.Identity.ModelId << '\t'
                  << n.Identity.Model << '\t' << n.Placement.Flags << '\t' << n.Placement.Lod << '\t' << TransformHash(n.Placement)
                  << '\t' << int(n.Link) << '\t' << (n.ParentSource ? int64_t(*n.ParentSource) : -1)
                  << '\t' << (n.Parent ? int64_t(*n.Parent) : -1) << '\t' << n.Children.size() << '\t' << int(metadata.Model->Kind)
                  << '\t' << int(graph->InitialBigBuildingCondition(n, 1.0f)) << '\t' << int(graph->InitialBigBuildingCondition(n, 2.0f)) << '\n';
        if (n.Link != NativeLodLinkStatus::None && n.Link != NativeLodLinkStatus::Bound)
            std::cout << "ISSUE\t" << i << '\t' << n.Evidence << '\n';
    }
    std::cout << "SUMMARY\t" << graph->Nodes().size() << '\t' << edges << '\t' << unknown << '\t' << time
              << "\towned-after-pager-shutdown-and-population-destruction\tno-render-claim\n";
    return 0;
} catch (const std::exception& e) { std::cerr << "NativeLodCatalogProbe: " << e.what() << '\n'; return 1; }
