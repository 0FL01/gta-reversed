// Independent app-only catalog consumer. No renderer/CMake/product edits required.
#include "app/platform/linux/NativeLodCatalog.h"
#include "app/platform/linux/StreamPager.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

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
// Minimal synthetic on-disk catalog proving the malformed-reader gate against
// real LoadBeforeWorker. All bytes are authored here; no /game or tmp reads,
// no copied game data. Fixture root is provided by the Python wrapper as a
// tempdir under artifacts/graphics and only that subtree is ever touched.
namespace {
namespace fs = std::filesystem;
static std::string FixtureLower(std::string text) {
    for (auto& c : text) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return text;
}
static std::string FixtureResolve(const std::string& root, std::string relative) {
    Require(!root.empty(), "fixture dir required");
    std::replace(relative.begin(), relative.end(), '\\', '/');
    Require(!fs::path(relative).is_absolute(), "absolute fixture asset path");
    auto result = fs::absolute(root);
    for (const auto& part : fs::path(relative)) {
        Require(part != ".." && part != ".", "fixture path traversal");
        std::optional<fs::path> found;
        for (const auto& entry : fs::directory_iterator(result)) {
            if (FixtureLower(entry.path().filename().string()) != FixtureLower(part.string())) continue;
            Require(!found, "ambiguous fixture asset path " + relative); found = entry.path();
        }
        Require(found.has_value(), "missing fixture asset " + relative); result = *found;
    }
    return result.string();
}
static void PutU32(std::vector<uint8_t>& out, size_t at, uint32_t value) {
    Require(at <= out.size() && out.size() - at >= 4, "fixture bounds");
    out[at] = uint8_t(value); out[at + 1] = uint8_t(value >> 8);
    out[at + 2] = uint8_t(value >> 16); out[at + 3] = uint8_t(value >> 24);
}
static void PutF32(std::vector<uint8_t>& out, size_t at, float value) {
    PutU32(out, at, std::bit_cast<uint32_t>(value));
}
static void WriteBytes(const fs::path& path, const std::string& bytes) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    Require(bool(file), "fixture write " + path.string());
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    Require(bool(file), "fixture write " + path.string());
}
static void WriteBytes(const fs::path& path, const std::vector<uint8_t>& bytes) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    Require(bool(file), "fixture write " + path.string());
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    Require(bool(file), "fixture write " + path.string());
}
static void PatchU32(const fs::path& path, size_t at, uint32_t value) {
    std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
    Require(bool(file), "fixture patch " + path.string());
    file.seekp(static_cast<std::streamoff>(at));
    Require(bool(file), "fixture patch " + path.string());
    const uint8_t bytes[4] = {uint8_t(value), uint8_t(value >> 8), uint8_t(value >> 16), uint8_t(value >> 24)};
    file.write(reinterpret_cast<const char*>(bytes), 4);
    Require(bool(file), "fixture patch " + path.string());
}
static std::vector<uint8_t> FixtureImg(bool empty) {
    if (empty) {
        std::vector<uint8_t> img(8, 0);
        img[0] = 'V'; img[1] = 'E'; img[2] = 'R'; img[3] = '2';
        return img; // count 0
    }
    // One .ipl member at sector 1: 8 header + 32 directory + pad + 2048 member.
    std::vector<uint8_t> img(4096, 0);
    img[0] = 'V'; img[1] = 'E'; img[2] = 'R'; img[3] = '2';
    PutU32(img, 4, 1);
    PutU32(img, 8, 1); // member sector
    PutU32(img, 12, 1); // sizes -> 1 sector
    const char name[] = "fixture_b.ipl";
    for (size_t i = 0; i < sizeof(name); ++i) img[16 + i] = uint8_t(name[i]);
    const size_t base = 2048;
    img[base] = 'b'; img[base + 1] = 'n'; img[base + 2] = 'r'; img[base + 3] = 'y';
    PutU32(img, base + 4, 1); // numInst
    PutU32(img, base + 28, 76); // offsetInst
    PutU32(img, base + 32, 0); // Stock files use zero/advisory sizeInst.
    const size_t at = base + 76;
    PutF32(img, at, 40.0f); PutF32(img, at + 4, 50.0f); PutF32(img, at + 8, 60.0f);
    PutF32(img, at + 12, 0.0f); PutF32(img, at + 16, 0.0f); PutF32(img, at + 20, 0.0f); PutF32(img, at + 24, 1.0f);
    PutU32(img, at + 28, 1001);
    PutU32(img, at + 32, 0);
    PutU32(img, at + 36, 0xFFFFFFFFu); // Lod -1 sentinel
    return img;
}
static void WriteDiskBaseline(const std::string& root) {
    const fs::path base(root);
    fs::create_directories(base / "data");
    fs::create_directories(base / "MODELS");
    WriteBytes(base / "data" / "default.dat", "IDE data\\fixture.ide\nIMG MODELS\\FIXTURE.IMG\nIPL data\\fixture.ipl\nEXIT\n");
    WriteBytes(base / "data" / "gta.dat", "EXIT\n");
    WriteBytes(base / "data" / "fixture.ide", "objs\n1000 fx_wall_a fx 100 0\n1001 fx_wall_b fx 100 0\nend\n");
    WriteBytes(base / "data" / "object.dat", "* stop\n");
    WriteBytes(base / "data" / "fixture.ipl", "inst\n1000 fx_wall_a 0 10 20 30 0 0 0 1 -1\nend\n");
    WriteBytes(base / "MODELS" / "GTA3.IMG", FixtureImg(true));
    WriteBytes(base / "MODELS" / "GTA_INT.IMG", FixtureImg(true));
    WriteBytes(base / "MODELS" / "FIXTURE.IMG", FixtureImg(false));
}
static NativeCollisionPopulation FixturePopulation(const std::string& root) {
    NativeCollisionPopulation population;
    population.IncludesStreamed = true;
    population.Models[1000] = {"fx_wall_a", false};
    population.Models[1001] = {"fx_wall_b", false};
    NativeCollisionPlacement text;
    text.Model = "fx_wall_a"; text.Ipl = "data\\fixture.ipl";
    text.ModelId = 1000; text.Record = 0;
    text.Position = {10.0f, 20.0f, 30.0f};
    text.Quaternion = {0.0f, 0.0f, 0.0f, 1.0f};
    population.Instances.push_back(text);
    NativeCollisionPlacement binary;
    binary.Model = "fx_wall_b";
    binary.Ipl = FixtureResolve(root, "MODELS\\FIXTURE.IMG") + ":fixture_b.ipl";
    binary.ModelId = 1001; binary.Record = 0; binary.Binary = true;
    binary.Position = {40.0f, 50.0f, 60.0f};
    binary.Quaternion = {0.0f, 0.0f, 0.0f, 1.0f};
    population.Instances.push_back(binary);
    return population;
}
static void SyntheticDiskFixture(const std::string& root) {
    Require(!root.empty() && root.rfind("/game", 0) != 0 && root.rfind("/tmp", 0) != 0,
            "fixture must live under the wrapper tempdir, never /game or tmp");
    Require(fs::path(root).filename().string().rfind("NativeLodCatalog-fixture-", 0) == 0,
            "fixture dir ownership");
    WriteDiskBaseline(root);
    const auto population = FixturePopulation(root);
    std::string error;
    const auto valid = NativeLodCatalog::LoadBeforeWorker(root.c_str(), population, error);
    Require(bool(valid) && error.empty(), "synthetic baseline: " + error);
    Require(valid->DiskValidated(), "synthetic baseline DiskValidated");
    Require(valid->Sources().size() == 2 && valid->Nodes().size() == 2, "synthetic baseline census 2/2");
    const auto unchanged = [&] {
        Require(valid->DiskValidated() && valid->Sources().size() == 2 && valid->Nodes().size() == 2,
                "valid snapshot mutated");
    };
    const fs::path img(FixtureResolve(root, "MODELS\\FIXTURE.IMG"));
    constexpr size_t base = 2048;
    const auto expectFail = [&](const std::string& needle, const std::string& label) {
        std::string err;
        const auto bad = NativeLodCatalog::LoadBeforeWorker(root.c_str(), population, err);
        Require(!bad && !err.empty() && err.find(needle) != std::string::npos,
                "synthetic " + label + " accepted (" + err + ")");
        unchanged();
    };
    // Other-section descriptor outside the IMG member; inst itself stays valid.
    WriteDiskBaseline(root);
    PatchU32(img, base + 8, 1); PatchU32(img, base + 36, 9999); PatchU32(img, base + 40, 40);
    expectFail("outside member", "other-section");
    // Source spans ignore advisory size; a real 40-byte record still fits.
    WriteDiskBaseline(root);
    PatchU32(img, base + 32, 39);
    const auto advisory = NativeLodCatalog::LoadBeforeWorker(root.c_str(), population, error);
    Require(advisory && advisory->DiskValidated(), "advisory size incorrectly treated as record extent: " + error);
    unchanged();
    WriteDiskBaseline(root);
    PatchU32(img, base + 32, 3000);
    expectFail("outside member", "advertised-range");
    WriteDiskBaseline(root);
    PatchU32(img, base + 28, 2020);
    expectFail("inst table bounds", "actual-inst-range");
    WriteDiskBaseline(root);
    PatchU32(img, base + 20, 1); PatchU32(img, base + 60, 2030);
    expectFail("car-generator records out of bounds", "actual-cargen-range");
    // Inst count exceeds the pre-append population budget (99 > 1 remaining).
    WriteDiskBaseline(root);
    PatchU32(img, base + 4, 99);
    expectFail("record budget", "inst-budget");
    // Cumulative text+binary budget: second text record pushes total 3 > 2.
    WriteDiskBaseline(root);
    WriteBytes(fs::path(root) / "data" / "fixture.ipl",
        "inst\n1000 fx_wall_a 0 10 20 30 0 0 0 1 -1\n1000 fx_wall_a 0 11 21 31 0 0 0 1 -1\nend\n");
    expectFail("record budget", "cumulative");
    WriteDiskBaseline(root);
    unchanged();
    std::cout << "DISKFIXTURE\tpass\tbaseline,other-section,advisory-size,advertised-range,actual-inst-range,actual-cargen-range,inst-budget,cumulative\n";
}
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
    // Malformed range: far out-of-range raw LOD indices map to InvalidIndex, never crash.
    auto far = stream; far.Records[0].Lod = INT32_MAX;
    graph = build({text, far});
    Require(graph && graph->Nodes()[2].Link == NativeLodLinkStatus::InvalidIndex, "far range index");
    auto farNegative = stream; farNegative.Records[0].Lod = INT32_MIN;
    graph = build({text, farNegative});
    Require(graph && graph->Nodes()[2].Link == NativeLodLinkStatus::InvalidIndex, "far negative index");
    // Cumulative count: total catalog records are bounded before append by the
    // independent population. Sources publishing beyond it are rejected.
    {
        NativeCollisionPopulation pop; pop.IncludesStreamed = true;
        for (const auto& p : text.Records) pop.Instances.push_back(p);
        auto cumulative = NativeLodCatalog::Assemble(pop, {text, stream}, {}, error);
        Require(!cumulative && error.starts_with("Error:"), "cumulative total beyond population rejected");
    }
    // Partial publication: a population record absent from every source is rejected.
    {
        NativeCollisionPopulation pop; pop.IncludesStreamed = true;
        for (const auto& s : {text, stream}) for (const auto& p : s.Records) pop.Instances.push_back(p);
        auto extra = text.Records[0]; extra.Record = 99;
        pop.Instances.push_back(extra);
        auto partial = NativeLodCatalog::Assemble(pop, {text, stream}, {}, error);
        Require(!partial && error.starts_with("Error:"), "partial publication with uncatalogued population rejected");
    }
    // Permissive pure Assemble: synthetic graphs and unrepresented models stay valid
    // with empty metadata; only LoadBeforeWorker certifies disk metadata.
    Require(!graph->DiskValidated(), "pure Assemble never claims disk validation");
    std::cout << "FIXTURES\tpass\tcontextual,unknown,ambiguous,bounds,negative,cycle,ancestor,self,duplicate,gap,farrange,farnegative,cumulative,partial,permissive\n";
}
int main(int argc, char** argv) try {
    Require(argc == 2 || argc == 3, "usage: NativeLodCatalogProbe GAME [FIXTURE_DIR]");
    Fixtures();
    if (argc == 3) SyntheticDiskFixture(argv[2]);
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
    size_t edges{}, unknown{}, time{}, textPlacements{}, binaryPlacements{};
    std::set<int> distinctIds;
    std::array<size_t, 4> classes{};
    for (size_t i = 0; i < graph->Nodes().size(); ++i) {
        const auto& n = graph->Nodes()[i]; const auto& other = reordered->Nodes()[i];
        Require(n.Identity == other.Identity && n.Parent == other.Parent && n.Children == other.Children, "order-dependent graph");
        Require(graph->Find(n.Identity) == &n, "exact identity lookup");
        auto wrong = n.Identity; wrong.Model += "_wrong"; Require(!graph->Find(wrong), "inexact lookup accepted");
        const auto metadata = graph->Metadata(n);
        Require(metadata.Status == NativeWorldInfoStatus::Ready && metadata.Model && metadata.Placement &&
                    metadata.Placement->SourceInstanceType == n.Placement.Flags,
                "owned source metadata unavailable");
        Require(metadata.Model->Kind != NativeWorldModelKind::Unknown &&
                    metadata.Model->InitialClass != NativeWorldInitialClass::Unknown,
                "disk node lacks static classification");
        Require(!metadata.Model->TxdName.empty(), "disk node lacks authored TXD");
        Require(n.RuntimeModelIsLod == NativeWorldKnownBool::Unknown && n.RuntimeBigBuilding == NativeWorldKnownBool::Unknown &&
                n.RuntimeUsesCollision == NativeWorldKnownBool::Unknown, "unproved runtime state invented");
        edges += n.Parent.has_value(); unknown += n.Link != NativeLodLinkStatus::None && n.Link != NativeLodLinkStatus::Bound;
        time += metadata.Model->Kind == NativeWorldModelKind::TimeAtomic;
        (graph->Sources()[n.Source].Binary ? binaryPlacements : textPlacements)++;
        distinctIds.insert(n.Identity.ModelId);
        classes[static_cast<int>(metadata.Model->InitialClass)]++;
        std::cout << "NODE\t" << i << '\t' << n.Source << '\t' << n.LocalIndex << '\t' << n.Identity.ModelId << '\t'
                  << n.Identity.Model << '\t' << n.Placement.Flags << '\t' << n.Placement.Lod << '\t' << TransformHash(n.Placement)
                  << '\t' << int(n.Link) << '\t' << (n.ParentSource ? int64_t(*n.ParentSource) : -1)
                  << '\t' << (n.Parent ? int64_t(*n.Parent) : -1) << '\t' << n.Children.size() << '\t' << int(metadata.Model->Kind)
                  << '\t' << int(graph->InitialBigBuildingCondition(n, 1.0f)) << '\t' << int(graph->InitialBigBuildingCondition(n, 2.0f)) << '\n';
        if (n.Link != NativeLodLinkStatus::None && n.Link != NativeLodLinkStatus::Bound)
            std::cout << "ISSUE\t" << i << '\t' << n.Evidence << '\n';
    }
    size_t textSources{}, binarySources{}, targets{};
    for (const auto& s : graph->Sources()) (s.Binary ? binarySources : textSources)++;
    for (const auto& n : graph->Nodes()) targets += !n.Children.empty();
    // Integrated source-review census. Numbers are claims to verify against actual
    // disk data, not targets to fit: report any mismatch for parent review.
    std::cout << "CENSUS\t" << graph->Sources().size() << '\t' << textSources << '\t' << binarySources << '\t'
              << graph->Nodes().size() << '\t' << textPlacements << '\t' << binaryPlacements << '\t'
              << edges << '\t' << targets << '\t' << unknown << '\t' << time << '\t' << distinctIds.size() << '\t'
              << classes[1] << '\t' << classes[2] << '\t' << classes[3] << '\t' << classes[0] << '\n';
    Require(graph->Sources().size() == 242 && textSources == 52 && binarySources == 190,
            "integrated source census 242/52/190");
    Require(graph->Nodes().size() == 50935 && textPlacements == 9268 && binaryPlacements == 41667,
            "integrated placement census 50935/9268/41667");
    Require(edges == 6103 && targets == 6086 && unknown == 0, "integrated edge census 6103/6086/0");
    Require(time == 161, "integrated time placements 161");
    // 14259 is the complete static IDE namespace; only 12839 IDs are placed.
    Require(distinctIds.size() == 12839, "integrated placed static model IDs 12839");
    Require(classes[1] == 34759 && classes[2] == 68 && classes[3] == 16108 && classes[0] == 0,
            "integrated class census 34759/68/16108/0");
    std::cout << "SUMMARY\t" << graph->Nodes().size() << '\t' << edges << '\t' << unknown << '\t' << time
              << "\towned-after-pager-shutdown-and-population-destruction\tno-render-claim\n";
    return 0;
} catch (const std::exception& e) { std::cerr << "NativeLodCatalogProbe: " << e.what() << '\n'; return 1; }
