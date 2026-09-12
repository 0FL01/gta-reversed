#include "app/platform/linux/NativeLodCatalog.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <set>
#include <sstream>
#include <stdexcept>

using int32 = int32_t;
using int64 = int64_t;
using uint32 = uint32_t;
using uint64 = uint64_t;
#ifndef __stdcall
#define __stdcall
#endif
#include "oswrapper/oswrapper.h"

namespace {
static void Require(bool ok, const std::string& error) { if (!ok) throw std::runtime_error(error); }
static std::string Lower(std::string text) {
    for (auto& c : text) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return text;
}
static std::string Resolve(const char* gameDir, std::string relative) {
    Require(gameDir && *gameDir, "Error: missing game directory");
    std::replace(relative.begin(), relative.end(), '\\', '/');
    Require(!std::filesystem::path(relative).is_absolute(), "Error: absolute catalog asset path");
    auto result = std::filesystem::absolute(gameDir);
    for (const auto& part : std::filesystem::path(relative)) {
        Require(part != ".." && part != ".", "Error: catalog path traversal");
        std::optional<std::filesystem::path> found;
        for (const auto& entry : std::filesystem::directory_iterator(result)) {
            if (Lower(entry.path().filename().string()) != Lower(part.string())) continue;
            Require(!found, "Error: ambiguous asset path " + relative); found = entry.path();
        }
        Require(found.has_value(), "Error: missing asset " + relative); result = *found;
    }
    return result.string();
}
struct File {
    void* Handle{};
    int32_t Size{};
    explicit File(const std::string& path) {
        Require(OS_FileOpen(FILE_DATA_AREA_DEFAULT, &Handle, path.c_str(), FILE_ACCESS_READ) == 0 && Handle,
                "Error: open catalog " + path);
        Size = OS_FileSize(Handle);
    }
    ~File() { if (Handle) OS_FileClose(Handle); }
    File(const File&) = delete;
    std::vector<uint8_t> Read(uint64_t offset, size_t size) {
        Require(Size >= 0 && size <= 16 * 1024 * 1024 && offset <= uint64_t(Size) && size <= uint64_t(Size) - offset,
                "Error: catalog read bounds");
        std::vector<uint8_t> data(size);
        OS_FileSetPosition(Handle, static_cast<int32_t>(offset));
        Require(!size || OS_FileRead(Handle, data.data(), static_cast<int32_t>(size)) == 0, "Error: short catalog read");
        return data;
    }
};
static uint32_t Word(std::span<const uint8_t> data, size_t at) {
    Require(at <= data.size() && data.size() - at >= 4, "Error: catalog word bounds");
    return uint32_t(data[at]) | uint32_t(data[at + 1]) << 8 | uint32_t(data[at + 2]) << 16 | uint32_t(data[at + 3]) << 24;
}
template<typename F> static void Lines(const char* gameDir, const std::string& relative, F visit) {
    File file(Resolve(gameDir, relative));
    Require(file.Size >= 0 && file.Size <= 8 * 1024 * 1024, "Error: catalog text budget");
    const auto data = file.Read(0, file.Size);
    std::istringstream input(std::string(data.begin(), data.end()));
    std::string line;
    uint32_t number{};
    while (std::getline(input, line)) {
        ++number;
        Require(line.size() < 511 && line.find('\0') == std::string::npos, "Error: catalog text line bounds");
        for (auto& c : line) if (static_cast<unsigned char>(c) < 32 || c == ',') c = ' ';
        const auto begin = line.find_first_not_of(' ');
        if (begin == std::string::npos || line[begin] == '#') continue;
        line.erase(0, begin);
        if (!visit(line, number)) break;
    }
}
static std::string Stem(std::string path) {
    std::replace(path.begin(), path.end(), '\\', '/');
    auto name = std::filesystem::path(path).filename().string();
    auto dot = name.find('.');
    Require(dot != std::string::npos, "Error: IPL source extension missing");
    return Lower(name.substr(0, dot));
}
static void ReadText(const char* gameDir, NativeLodSource& source, size_t remaining) {
    std::string section;
    Lines(gameDir, source.Key, [&](const std::string& line, uint32_t number) {
        std::istringstream scan(line); std::string first; scan >> first;
        if (first == "end") { section.clear(); return true; }
        if (section.empty()) { section = first; return true; }
        if (section != "inst") return true;
        NativeCollisionPlacement p;
        int32_t flags{};
        scan.clear(); scan.str(line);
        scan >> p.ModelId >> p.Model >> flags;
        for (auto& v : p.Position) scan >> v;
        for (auto& v : p.Quaternion) scan >> v;
        scan >> p.Lod;
        Require(!scan.fail(), "Error: invalid IPL " + source.Key + ":" + std::to_string(number));
        p.Ipl = source.Key; p.Record = source.Records.size(); p.Model = Lower(p.Model);
        p.Flags = static_cast<uint32_t>(flags); p.Interior = p.Flags & 255;
        // Cumulative allocation is bounded before append by the independent
        // StreamPager population, not a per-source constant.
        Require(source.Records.size() < remaining, "Error: IPL record budget " + source.Key);
        source.Records.push_back(std::move(p));
        return true;
    });
}
static void ReadBinary(File& file, NativeLodSource& source, const NativeCollisionPopulation& population, size_t totalSoFar) {
    auto data = file.Read(uint64_t(source.Sector) * 2048, size_t(source.Sectors) * 2048);
    Require(data.size() >= 76 && std::memcmp(data.data(), "bnry", 4) == 0, "Error: unsupported archive IPL " + source.Key);
    Require(totalSoFar <= population.Instances.size(), "Error: IPL record budget " + source.Key);
    const size_t remaining = population.Instances.size() - totalSoFar;
    // All six tBinaryIplFile section descriptors must fit the IMG member bytes.
    // Counts at 4,8,12,16,20,24; offsets at 28,36,44,52,60,68; sizes at +4.
    // Follows NativeCarGenerators::ParseBinaryIpl: empty sections (count==0 &&
    // size==0) carry no extent; anything else must declare offset/size in-member.
    constexpr size_t kHeaderSize = 76; // sizeof(tBinaryIplFile) == 0x4C
    for (size_t i = 0; i < 6; ++i) {
        const auto secCount = Word(data, 4 + i * 4);
        const auto secOffset = Word(data, 28 + i * 8);
        const auto secSize = Word(data, 32 + i * 8);
        if (!secCount && !secSize) continue;
        Require(secOffset >= kHeaderSize && secOffset <= data.size() && secSize <= data.size() - secOffset,
                "Error: binary IPL section outside member " + source.Key);
    }
    const auto count = Word(data, 4), start = Word(data, 28);
    Require(count <= remaining, "Error: IPL record budget " + source.Key);
    Require(count <= 250000 && (!count || (start >= kHeaderSize && start <= data.size() && uint64_t(count) * 40 <= data.size() - start)),
            "Error: binary inst table bounds " + source.Key);
    // Source tBinaryIplFile spans use counts, not size fields (all stock sizes
    // are zero). Advertised ranges above do not bound count-derived storage.
    // Car-generator descriptor member size is 0x30 (CFileCarGenerator). Contents are
    // never interpreted here; only bounds are validated.
    const auto carCount = Word(data, 20), carOffset = Word(data, 60);
    Require(!carCount || (carOffset >= kHeaderSize && carOffset <= data.size() &&
            uint64_t(carCount) * 0x30 <= data.size() - carOffset),
            "Error: binary car-generator records out of bounds " + source.Key);
    // No interpreting non-inst sections beyond bounds.
    for (uint32_t r = 0; r < count; ++r) {
        const auto at = start + r * 40;
        NativeCollisionPlacement p;
        p.Ipl = source.Key; p.Record = r; p.Binary = true;
        for (size_t i = 0; i < 3; ++i) p.Position[i] = std::bit_cast<float>(Word(data, at + i * 4));
        for (size_t i = 0; i < 4; ++i) p.Quaternion[i] = std::bit_cast<float>(Word(data, at + 12 + i * 4));
        p.ModelId = std::bit_cast<int32_t>(Word(data, at + 28));
        p.Flags = Word(data, at + 32); p.Interior = p.Flags & 255;
        p.Lod = std::bit_cast<int32_t>(Word(data, at + 36));
        const auto model = population.Models.find(p.ModelId);
        Require(model != population.Models.end(), "Error: binary IPL model missing from full IDE namespace");
        p.Model = Lower(model->second.Name);
        Require(source.Records.size() < remaining, "Error: IPL record budget " + source.Key);
        source.Records.push_back(std::move(p));
    }
}
static std::vector<NativeLodSource> ReadCatalog(const char* gameDir, const NativeCollisionPopulation& population) {
    Require(population.Instances.size() <= 250000, "Error: full source population/catalog required");
    std::vector<NativeLodSource> sources;
    size_t total{};
    // Game.cpp: DEFAULT then main DAT. Streaming::InitImageList 0x4083C0.
    std::vector<std::pair<std::string, NativeWorldSourceRow>> archives{
        {"MODELS\\GTA3.IMG", {"CStreaming::InitImageList@0x4083C0", 0}},
        {"MODELS\\GTA_INT.IMG", {"CStreaming::InitImageList@0x4083C0", 0}}
    };
    bool firstIpl = false;
    for (const char* dat : {"data/default.dat", "data/gta.dat"}) {
        Lines(gameDir, dat, [&](const std::string& line, uint32_t number) {
            std::istringstream scan(line); std::string kind, path; scan >> kind >> path;
            if (kind == "EXIT") return false;
            if (kind == "IMG" && path != "MODELS\\GTA_INT.IMG") {
                Require(!firstIpl, "Unknown: IMG added after Streaming::Init2 boundary");
                archives.push_back({path, {dat, number}});
            }
            if (kind != "IPL") return true;
            firstIpl = true;
            Require(!path.empty() && sources.size() < 1024, "Error: DAT IPL declaration bounds");
            NativeLodSource source;
            source.Key = path; source.Name = Stem(path); source.Declaration = {dat, number};
            Require(total <= population.Instances.size(), "Error: IPL record budget " + source.Key);
            ReadText(gameDir, source, population.Instances.size() - total); total += source.Records.size();
            Require(total <= population.Instances.size(), "Error: IPL record budget " + source.Key);
            sources.push_back(std::move(source));
            return true;
        });
    }
    Require(archives.size() <= 8, "Error: source streaming image slots exceeded");
    std::set<std::string> loaded;
    for (size_t archive = 0; archive < archives.size(); ++archive) {
        const auto& [relative, declaration] = archives[archive];
        const auto absolute = Resolve(gameDir, relative);
        File file(absolute);
        auto header = file.Read(0, 8);
        Require(std::memcmp(header.data(), "VER2", 4) == 0, "Error: IMG version");
        const auto count = Word(header, 4);
        Require(count <= 250000, "Error: IMG directory budget");
        auto directory = file.Read(8, size_t(count) * 32);
        for (uint32_t r = 0; r < count; ++r) {
            const auto* begin = reinterpret_cast<const char*>(directory.data() + r * 32 + 8);
            const auto* end = static_cast<const char*>(std::memchr(begin, 0, 24));
            Require(end, "Error: unterminated IMG name");
            auto name = Lower(std::string(begin, end));
            if (!name.ends_with(".ipl")) continue;
            // Streaming::LoadCdDirectory 0x5B6486 keeps first assigned CD location.
            if (!loaded.insert(Stem(name)).second) continue;
            NativeLodSource source;
            source.Key = absolute + ":" + name; source.Name = Stem(name); source.Binary = true;
            source.Archive = relative; source.ArchiveOrder = archive; source.DirectoryRecord = r;
            source.Declaration = declaration; source.Sector = Word(directory, r * 32);
            const auto sizes = Word(directory, r * 32 + 4);
            source.Sectors = (sizes >> 16) ? sizes >> 16 : sizes & 65535;
            Require(total <= population.Instances.size(), "Error: IPL record budget " + source.Key);
            ReadBinary(file, source, population, total); total += source.Records.size();
            Require(total <= population.Instances.size(), "Error: IPL record budget " + source.Key);
            sources.push_back(std::move(source));
            Require(sources.size() <= 1024, "Error: IPL source budget");
        }
    }
    return sources;
}
}

NativeLodCatalog::Key NativeLodCatalog::IdentityKey(const NativePlacementIdentity& id) {
    return {id.Ipl, id.Model, id.Record, id.ModelId, id.Binary};
}
std::shared_ptr<const NativeLodCatalog> NativeLodCatalog::LoadBeforeWorker(
    const char* gameDir, const NativeCollisionPopulation& population, std::string& error) try {
    NativeWorldEntityInfo metadata;
    if (!metadata.LoadBeforeWorker(gameDir, population, error)) return {};
    auto result = AssembleMutable(population, ReadCatalog(gameDir, population), std::move(metadata), error);
    if (!result) return {};
    for (const auto& node : result->m_Nodes) {
        Require(node.Link == NativeLodLinkStatus::None || node.Link == NativeLodLinkStatus::Bound,
                node.Evidence + " " + node.Identity.Ipl + ":" + std::to_string(node.Identity.Record));
        // Disk validation requires complete source static model/placement metadata for
        // every node. Pure Assemble stays permissive for synthetic graphs and
        // unrepresented models; only this disk path certifies provenance.
        const auto provenance = node.Identity.Ipl + ":" + std::to_string(node.Identity.Record);
        const auto meta = result->m_Metadata.Query(node.Placement);
        Require(meta.Status == NativeWorldInfoStatus::Ready && meta.Model && meta.Placement,
                "Error: catalog placement lacks complete source metadata " + provenance);
        Require(meta.Placement->SourceInstanceType && *meta.Placement->SourceInstanceType == node.Placement.Flags,
                "Error: catalog placement lacks certified source instance type " + provenance);
        Require(meta.Model->Kind != NativeWorldModelKind::Unknown &&
                    meta.Model->InitialClass != NativeWorldInitialClass::Unknown,
                "Error: catalog model lacks static classification " + provenance);
        Require(meta.Model->DrawDistance && meta.Model->IdeFlags,
                "Error: catalog model lacks authored draw/flags " + provenance);
        Require(!meta.Model->TxdName.empty(), "Error: catalog model lacks authored TXD " + provenance);
        switch (meta.Model->Kind) {
        case NativeWorldModelKind::Atomic:
            Require(!meta.Model->AnimationName && !meta.Model->TimeOn && !meta.Model->TimeOff,
                    "Error: atomic model carries non-authored anim/time " + provenance);
            break;
        case NativeWorldModelKind::TimeAtomic:
            // Authored tobj hours are preserved verbatim; no invented 0..24 range.
            Require(meta.Model->TimeOn && meta.Model->TimeOff && !meta.Model->AnimationName,
                    "Error: time model lacks authored hours " + provenance);
            break;
        case NativeWorldModelKind::Clump:
            Require(meta.Model->AnimationName && !meta.Model->TimeOn && !meta.Model->TimeOff,
                    "Error: clump model lacks authored animation " + provenance);
            break;
        default:
            Require(false, "Error: catalog model lacks static kind " + provenance);
            break;
        }
    }
    result->m_DiskValidated = true;
    return result;
} catch (const std::exception& e) { error = e.what(); return {}; }

std::shared_ptr<const NativeLodCatalog> NativeLodCatalog::Assemble(
    const NativeCollisionPopulation& population, std::vector<NativeLodSource> sources,
    NativeWorldEntityInfo metadata, std::string& error) {
    return AssembleMutable(population, std::move(sources), std::move(metadata), error);
}
std::shared_ptr<NativeLodCatalog> NativeLodCatalog::AssembleMutable(
    const NativeCollisionPopulation& population, std::vector<NativeLodSource> sources,
    NativeWorldEntityInfo metadata, std::string& error) try {
    Require(population.IncludesStreamed && !sources.empty() && sources.size() <= 1024 &&
            population.Instances.size() <= 250000, "Error: full source population/catalog required");
    auto result = std::shared_ptr<NativeLodCatalog>(new NativeLodCatalog);
    result->m_Sources = std::move(sources); result->m_Metadata = std::move(metadata);
    std::map<Key, const NativeCollisionPlacement*> exported;
    for (const auto& p : population.Instances) {
        Require(exported.emplace(IdentityKey(NativePlacementIdentity::From(p)), &p).second,
                "Error: duplicate population identity " + p.Ipl + ":" + std::to_string(p.Record));
    }
    std::set<std::pair<std::string, bool>> sourceKeys;
    std::vector<std::vector<size_t>> local(result->m_Sources.size());
    for (size_t s = 0; s < result->m_Sources.size(); ++s) {
        const auto& source = result->m_Sources[s];
        Require(!source.Key.empty() && !source.Name.empty() && sourceKeys.emplace(source.Key, source.Binary).second,
                "Error: duplicate/invalid catalog source " + source.Key);
        for (size_t r = 0; r < source.Records.size(); ++r) {
            const auto& p = source.Records[r];
            Require(p.Ipl == source.Key && p.Record == r && p.Binary == source.Binary,
                    "Error: noncontextual IPL record " + source.Key);
            const auto identity = NativePlacementIdentity::From(p);
            const auto found = exported.find(IdentityKey(identity));
            Require(found != exported.end(), "Error: raw IPL absent from population " + p.Ipl + ":" + std::to_string(r));
            const auto& q = *found->second;
            const auto sameFloat = [](float a, float b) { return std::bit_cast<uint32_t>(a) == std::bit_cast<uint32_t>(b); };
            Require(p.Lod == q.Lod && p.Flags == q.Flags && p.Interior == q.Interior && p.Interior == int(p.Flags & 255) &&
                    std::equal(p.Position.begin(), p.Position.end(), q.Position.begin(), sameFloat) &&
                    std::equal(p.Quaternion.begin(), p.Quaternion.end(), q.Quaternion.begin(), sameFloat),
                    "Error: raw IPL/population mismatch " + p.Ipl + ":" + std::to_string(r));
            for (auto v : p.Position) Require(std::isfinite(v), "Error: nonfinite source position");
            float norm = 0;
            for (auto v : p.Quaternion) { Require(std::isfinite(v), "Error: nonfinite source rotation"); norm += v * v; }
            Require(std::isfinite(norm) && norm > 0, "Error: invalid source quaternion");
            const auto n = result->m_Nodes.size();
            Require(result->m_Index.emplace(IdentityKey(identity), n).second, "Error: duplicate raw IPL identity");
            NativeLodNode node;
            node.Identity = identity; node.Placement = p; node.Source = s; node.LocalIndex = r;
            result->m_Nodes.push_back(std::move(node)); local[s].push_back(n);
        }
    }
    Require(result->m_Nodes.size() == population.Instances.size(), "Error: population has uncatalogued records");
    for (auto& node : result->m_Nodes) {
        const auto& source = result->m_Sources[node.Source];
        if (!source.Binary) node.ParentSourceCandidates.push_back(node.Source);
        else for (size_t s = 0; s < result->m_Sources.size(); ++s) {
            const auto& candidate = result->m_Sources[s];
            // Exact reversed source rule, NOT a model-name LOD heuristic:
            // IplStore.cpp SetupRelatedIpls 0x404DE0 _strnicmp(stem+"_stream").
            if (!candidate.Binary && Lower(source.Name).starts_with(Lower(candidate.Name) + "_stream"))
                node.ParentSourceCandidates.push_back(s);
        }
        if (node.ParentSourceCandidates.size() == 1) node.ParentSource = node.ParentSourceCandidates.front();
        if (node.Placement.Lod == -1) { node.Evidence = "CFileLoader/CIplStore: raw -1 sentinel"; continue; }
        if (node.Placement.Lod < -1) {
            node.Link = NativeLodLinkStatus::InvalidIndex; node.Evidence = "Error: negative non-sentinel raw index"; continue;
        }
        if (!node.ParentSource) {
            node.Link = node.ParentSourceCandidates.empty() ? NativeLodLinkStatus::UnknownParentSource : NativeLodLinkStatus::AmbiguousParentSource;
            node.Evidence = "Unknown: SetupRelatedIpls staticIdx lacks unique text source"; continue;
        }
        const auto& array = local[*node.ParentSource];
        if (size_t(node.Placement.Lod) >= array.size()) {
            node.Link = NativeLodLinkStatus::InvalidIndex; node.Evidence = "Error: raw index outside parent text inst array"; continue;
        }
        node.Parent = node.CandidateParent = array[node.Placement.Lod];
        node.Link = NativeLodLinkStatus::Bound;
        node.Evidence = source.Binary ? "CIplStore::LoadIpl/LoadIplBoundingBox staticIdx array @0x406080/0x405C00" :
            "CFileLoader::LoadScene local array -> LinkLods @0x5B8700/0x5B51E0";
    }
    // Functional graph cycle detection, iterative even for adversarial deep chains.
    std::vector<uint8_t> color(result->m_Nodes.size());
    for (size_t start = 0; start < result->m_Nodes.size(); ++start) {
        if (color[start]) continue;
        std::vector<size_t> path;
        std::optional<size_t> at = start;
        while (at && color[*at] == 0) {
            color[*at] = 1; path.push_back(*at); at = result->m_Nodes[*at].Parent;
        }
        if (at && color[*at] == 1) {
            auto begin = std::find(path.begin(), path.end(), *at);
            for (auto i = begin; i != path.end(); ++i) {
                auto& node = result->m_Nodes[*i]; node.Link = NativeLodLinkStatus::Cycle;
                node.Parent.reset(); node.Evidence = "Error: contextual IPL reference cycle";
            }
        }
        for (auto i = path.rbegin(); i != path.rend(); ++i) {
            auto& node = result->m_Nodes[*i];
            if (node.Parent) {
                const auto status = result->m_Nodes[*node.Parent].Link;
                if (status != NativeLodLinkStatus::Bound && status != NativeLodLinkStatus::None) {
                    node.Link = NativeLodLinkStatus::InvalidAncestor; node.Parent.reset();
                    node.Evidence = "Unknown: parent chain contains invalid/unbound reference";
                }
            }
            color[*i] = 2;
        }
    }
    for (size_t n = 0; n < result->m_Nodes.size(); ++n) {
        if (const auto parent = result->m_Nodes[n].Parent) result->m_Nodes[*parent].Children.push_back(n);
    }
    error.clear(); return result;
} catch (const std::exception& e) { error = e.what(); return {}; }

const NativeLodNode* NativeLodCatalog::Find(const NativePlacementIdentity& identity) const {
    const auto found = m_Index.find(IdentityKey(identity));
    return found == m_Index.end() ? nullptr : &m_Nodes[found->second];
}
NativeWorldKnownBool NativeLodCatalog::InitialBigBuildingCondition(const NativeLodNode& node, float lodMultiplier) const {
    assert(std::isfinite(lodMultiplier) && lodMultiplier > 0);
    if (node.Link != NativeLodLinkStatus::None && node.Link != NativeLodLinkStatus::Bound)
        return NativeWorldKnownBool::Unknown;
    // LoadIplBoundingBox only appends streamed entities with an authored LOD
    // index to ppCurrIplInstance, which defines LinkLods' binary iteration tail.
    if (node.Identity.Binary && node.Link == NativeLodLinkStatus::None)
        return NativeWorldKnownBool::Unknown;
    if (!node.Children.empty()) return NativeWorldKnownBool::True;
    const auto metadata = Metadata(node);
    if (!metadata.Model || !metadata.Model->DrawDistance) return NativeWorldKnownBool::Unknown;
    return *metadata.Model->DrawDistance * lodMultiplier > 300.0f ? NativeWorldKnownBool::True : NativeWorldKnownBool::False;
}

bool NativeLodCatalog::EvaluateLinkLodsChain(const NativePlacementIdentity& childIdentity,
                                             const NativeCollisionAssets& collisions,
                                             const NativeLinkLodsInputs& inputs,
                                             NativeLodChainDecision& out, std::string& error) const try {
    // Pure bounded evaluator: never mutates m_Nodes/m_Sources/m_Metadata.
    // On any rejection out is left unchanged; only a complete decision is published.
    if (!m_DiskValidated) { error = "Error: catalog lacks disk validation"; return false; }
    if (inputs.CacheLoading) { error = "Error: cache loading closure not evaluated"; return false; }
    if (!std::isfinite(inputs.LodMultiplier) || !(inputs.LodMultiplier > 0.0f)) {
        error = "Error: nonfinite/nonpositive LodMultiplier";
        return false;
    }
    const NativeLodNode* childPtr = Find(childIdentity);
    if (!childPtr) { error = "Error: unknown child identity"; return false; }
    const size_t childIdx = static_cast<size_t>(childPtr - m_Nodes.data());
    if (childIdx >= m_Nodes.size()) { error = "Error: child index outside catalog"; return false; }
    const NativeLodNode& child = m_Nodes[childIdx];
    if (!(child.Identity == childIdentity)) { error = "Error: child identity mismatch"; return false; }
    if (child.Link != NativeLodLinkStatus::Bound || !child.Parent) {
        error = "Error: child link not bound";
        return false;
    }
    const size_t parentIdx = *child.Parent;
    if (parentIdx >= m_Nodes.size()) { error = "Error: parent index outside catalog"; return false; }
    const NativeLodNode& parent = m_Nodes[parentIdx];
    // No hardcoded model IDs: every gate below is structural/closure-derived.
    auto lower = [](std::string s) {
        for (auto& c : s) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        return s;
    };
    if (child.Source >= m_Sources.size() || parent.Source >= m_Sources.size()) {
        error = "Error: source index outside catalog";
        return false;
    }
    // Text encounter order: single text source, child record before parent
    // record, catalog source order preserved (FileLoader LoadScene local array
    // -> LinkLods first-bind, then source-order mutable pass 1956-2024).
    if (child.Identity.Binary || parent.Identity.Binary) {
        error = "Error: non-text encounter order";
        return false;
    }
    if (m_Sources[child.Source].Binary || m_Sources[parent.Source].Binary) {
        error = "Error: non-text encounter order";
        return false;
    }
    if (child.Source != parent.Source) { error = "Error: cross-source chain"; return false; }
    if (!(child.LocalIndex < parent.LocalIndex)) { error = "Error: text encounter order"; return false; }
    if (!(childIdx < parentIdx)) { error = "Error: text encounter order"; return false; }
    if (child.Placement.Lod != static_cast<int32_t>(parent.LocalIndex)) {
        error = "Error: Lod index mismatch";
        return false;
    }
    if (parent.Placement.Lod != -1 || parent.Link != NativeLodLinkStatus::None || parent.Parent) {
        error = "Error: parent sibling closure";
        return false;
    }
    if (!child.CandidateParent || *child.CandidateParent != parentIdx) {
        error = "Error: sibling closure";
        return false;
    }
    // Sibling closure: parent exactly one child (this child), child leaf.
    if (!child.Children.empty()) { error = "Error: child sibling closure"; return false; }
    if (parent.Children.size() != 1) {
        error = parent.Children.size() > 1 ? "Error: multichild closure" : "Error: sibling closure";
        return false;
    }
    if (parent.Children.front() != childIdx) { error = "Error: sibling closure"; return false; }
    // Complete unique-model/name closure: each model appears exactly once in
    // the full 50935 population; shared models are rejected, not aliased.
    if (child.Identity.ModelId == parent.Identity.ModelId) {
        error = "Error: shared model closure";
        return false;
    }
    if (lower(child.Identity.Model) == lower(parent.Identity.Model)) {
        error = "Error: shared model closure";
        return false;
    }
    size_t childIdCount{}, parentIdCount{}, childNameCount{}, parentNameCount{};
    for (const auto& n : m_Nodes) {
        if (n.Identity.ModelId == child.Identity.ModelId) ++childIdCount;
        if (n.Identity.ModelId == parent.Identity.ModelId) ++parentIdCount;
        if (lower(n.Identity.Model) == lower(child.Identity.Model)) ++childNameCount;
        if (lower(n.Identity.Model) == lower(parent.Identity.Model)) ++parentNameCount;
        if (childIdCount > 1 && parentIdCount > 1 && childNameCount > 1 && parentNameCount > 1) break;
    }
    if (childIdCount != 1 || parentIdCount != 1 || childNameCount != 1 || parentNameCount != 1) {
        error = "Error: shared model closure";
        return false;
    }
    const auto childMeta = m_Metadata.Query(child.Placement);
    const auto parentMeta = m_Metadata.Query(parent.Placement);
    if (childMeta.Status != NativeWorldInfoStatus::Ready || !childMeta.Model || !childMeta.Placement ||
        parentMeta.Status != NativeWorldInfoStatus::Ready || !parentMeta.Model || !parentMeta.Placement) {
        error = "Error: incomplete source metadata closure";
        return false;
    }
    // Time counterpart residency is unresolved: reject time chains.
    if (childMeta.Model->Kind == NativeWorldModelKind::TimeAtomic ||
        parentMeta.Model->Kind == NativeWorldModelKind::TimeAtomic) {
        error = "Error: time closure";
        return false;
    }
    if (!childMeta.Model->DrawDistance || !parentMeta.Model->DrawDistance) {
        error = "Error: authored draw distance missing";
        return false;
    }
    // Both ends must be initial buildings (Building/AnimatedBuilding).
    // Dummy starts uses-collision false (Building.cpp 14-18 sets true for
    // CBuilding; CEntity 108-126 leaves flags zero and CDummy/CDummyObject
    // never enable it), so a dummy chain cannot carry the accepted
    // not-big/uses-COL decision below.
    if (childMeta.InitialBuildingMask() != NativeWorldKnownBool::True ||
        parentMeta.InitialBuildingMask() != NativeWorldKnownBool::True) {
        error = "Error: unsupported initial building mask closure";
        return false;
    }
    const float childDraw = *childMeta.Model->DrawDistance, parentDraw = *parentMeta.Model->DrawDistance;
    if (!std::isfinite(childDraw) || !std::isfinite(parentDraw) || !(childDraw > 0.0f) || !(parentDraw > 0.0f)) {
        error = "Error: invalid authored draw distance";
        return false;
    }
    // Sibling-lane COL scope: Ready+provenance child, KnownAbsent parent,
    // TimeShared rejected. No IO here; LookupModel is pure/bounded.
    const auto childCol = collisions.LookupModel(child.Identity.Model);
    const auto parentCol = collisions.LookupModel(parent.Identity.Model);
    if (childCol.TimeShared || parentCol.TimeShared) { error = "Error: time-shared COL closure"; return false; }
    if (childCol.Status != NativeCollisionModelStatus::Ready || !childCol.Model) {
        error = "Error: unsupported child COL closure";
        return false;
    }
    if (!childCol.Model->ValidatedHeaderId || !childCol.Error.empty() ||
        !childCol.Model->Unsupported.empty() || childCol.Model->Empty) {
        error = "Error: unsupported child COL closure";
        return false;
    }
    if (childCol.Model->HeaderId != static_cast<uint16_t>(child.Identity.ModelId)) {
        error = "Error: child COL provenance closure";
        return false;
    }
    if (parentCol.Status != NativeCollisionModelStatus::KnownAbsent || parentCol.Model) {
        error = "Error: parent COL not known-absent";
        return false;
    }
    // FileLoader 1991-1994 (0x5B5285): big when it already has LOD children
    // or camera multiplier * draw > 300. SetupBigBuilding (Entity 863-871)
    // disables uses-collision, persists the entity and sets the SEPARATE
    // owns bit (BaseModelInfo.h), never bIsLod.
    const bool childBig = !child.Children.empty() ||
        (inputs.LodMultiplier * childDraw > 300.0f);
    const bool parentBig = !parent.Children.empty() ||
        (inputs.LodMultiplier * parentDraw > 300.0f);
    // Narrow accepted scope: the child stays a normal entity (not big) so it
    // precedes the big parent in the source-order mutable pass. A threshold
    // crossing (e.g. 180*2>300 at multiplier 2) rejects without hardcoding
    // multiplier 1.
    if (childBig || !parentBig) { error = "Error: big-building scope closure"; return false; }
    // FileLoader 1997-2013: single-child LOD aliases the child COL via
    // DeleteCollisionModel + SetColModel(childCOL,false), which clears bIsLod
    // (BaseModelInfo 143-160). The child keeps bIsLod=true. Pointers differ
    // here because the parent is KnownAbsent (null) and the child is Ready.
    // Underwater is LoadObjectInstance 1056 (authored OR) then 1070-1085:
    // child adds the COL-bottom check, parent (KnownAbsent, no COL) keeps the
    // authored bit only. Authored bits must be Known, never flag-decoded.
    if (childMeta.Placement->AuthoredUnderwater == NativeWorldKnownBool::Unknown ||
        parentMeta.Placement->AuthoredUnderwater == NativeWorldKnownBool::Unknown) {
        error = "Error: unknown authored underwater closure";
        return false;
    }
    const bool childUnder = NativeLodSourceChildUnderwater(
        childMeta.Placement->AuthoredUnderwater == NativeWorldKnownBool::True,
        childCol.Model->Min[2], child.Placement.Position[2]);
    const bool parentUnder = parentMeta.Placement->AuthoredUnderwater == NativeWorldKnownBool::True;
    NativeLodChainDecision local;
    local.Child = child.Identity;
    local.Parent = parent.Identity;
    local.ChildNode = childIdx;
    local.ParentNode = parentIdx;
    local.ChildModelId = child.Identity.ModelId;
    local.ParentModelId = parent.Identity.ModelId;
    local.ChildModel = child.Identity.Model;
    local.ParentModel = parent.Identity.Model;
    local.ParentChildren = parent.Children.size();
    local.ChildChildren = child.Children.size();
    local.Link = child.Link;
    local.EdgeKept = true;
    local.CollisionTransferred = true;
    local.EffectiveCol = childCol.Model;
    local.EffectiveColLibrary = childCol.Model->Library;
    local.EffectiveColFaces = static_cast<uint32_t>(childCol.Model->Faces.size());
    local.ChildBigBuilding = childBig ? NativeWorldKnownBool::True : NativeWorldKnownBool::False;
    local.ChildUsesCollision = childBig ? NativeWorldKnownBool::False : NativeWorldKnownBool::True;
    local.ChildIsLod = NativeWorldKnownBool::True;
    local.ParentBigBuilding = parentBig ? NativeWorldKnownBool::True : NativeWorldKnownBool::False;
    local.ParentUsesCollision = parentBig ? NativeWorldKnownBool::False : NativeWorldKnownBool::True;
    local.ParentIsLod = NativeWorldKnownBool::False;
    local.ChildDrawDistance = childDraw;
    local.ParentDrawDistance = parentDraw;
    local.DrawUnchanged = true;
    local.Underwater = parentUnder || childUnder;
    local.UnderwaterPropagated = childUnder;
    local.LinkReason =
        "CFileLoader::LinkLods first-bind SetLod/AddLodChildren then source-order mutable pass 1956-2024 "
        "(0x5B51E0/0x5B5285); text encounter child before parent";
    local.TransferReason =
        "FileLoader 2000-2006 single-child lodMI->DeleteCollisionModel/SetColModel(childCOL,false) clears "
        "bIsLod (BaseModelInfo 143-160); SetOwnsColModel alters SEPARATE bDoWeOwnTheColModel (BaseModelInfo.h); "
        "SetupBigBuilding disables uses-collision (Entity 863-871)";
    local.RelationReason =
        "Renderer single-child: opaque visible child marks/suppresses parent (509-521,695-722), otherwise "
        "visible parent fallback (640-655,1048/1065); no near/far/frustum/residency/GPU claim";
    out = std::move(local);
    error.clear();
    return true;
} catch (const std::exception& e) { error = e.what(); return false; }

bool NativeLodCatalog::EvaluateLodRelation(const NativeLodChainDecision& chain, bool childVisible,
                                           uint8_t childAlpha, bool parentVisible,
                                           NativeLodRelationDecision& out, std::string& error) const try {
    if (!m_DiskValidated) { error = "Error: catalog lacks disk validation"; return false; }
    // The chain must be a decision this catalog could have produced: exact
    // single-child edge, no hardcoded model IDs.
    const NativeLodNode* childPtr = Find(chain.Child);
    const NativeLodNode* parentPtr = Find(chain.Parent);
    if (!childPtr || !parentPtr) { error = "Error: unknown relation chain"; return false; }
    const size_t childIdx = static_cast<size_t>(childPtr - m_Nodes.data());
    const size_t parentIdx = static_cast<size_t>(parentPtr - m_Nodes.data());
    if (childIdx != chain.ChildNode || parentIdx != chain.ParentNode || chain.Link != NativeLodLinkStatus::Bound ||
        !chain.EdgeKept || chain.ParentChildren != 1 || chain.ChildChildren != 0 || !chain.CollisionTransferred) {
        error = "Error: relation chain mismatch";
        return false;
    }
    const NativeLodNode& child = m_Nodes[childIdx];
    const NativeLodNode& parent = m_Nodes[parentIdx];
    if (!child.Parent || *child.Parent != parentIdx || parent.Children.size() != 1 ||
        parent.Children.front() != childIdx || !child.Children.empty()) {
        error = "Error: relation chain mismatch";
        return false;
    }
    // Accepted post-LinkLods states only: tampered/forged decisions (e.g. a
    // childBig=true chain, which would upset normal-before-big order) are
    // rejected with out unchanged.
    if (chain.ChildBigBuilding != NativeWorldKnownBool::False ||
        chain.ParentBigBuilding != NativeWorldKnownBool::True ||
        chain.ChildUsesCollision != NativeWorldKnownBool::True ||
        chain.ParentUsesCollision != NativeWorldKnownBool::False ||
        chain.ChildIsLod != NativeWorldKnownBool::True ||
        chain.ParentIsLod != NativeWorldKnownBool::False) {
        error = "Error: relation chain state mismatch";
        return false;
    }
    // Renderer.cpp 509-521 (SetupMapEntityVisibility): an on-screen,
    // non-occluded child with alpha==255 marks the LOD parent via
    // AddLodChildrenRendered; with <=1 LOD children the child is still
    // submitted VISIBLE even when translucent (alpha<255 branch sets
    // m_bDistanceFade but returns VISIBLE without marking). 695-722
    // (SetupBigBuildingVisibility): a marked single-child parent is diverted
    // to the LodRenderList/STREAMME lane (suppressed, not submitted); an
    // unmarked parent falls back to its own SetupMapEntityVisibility
    // (640-655) and the big-building scan (1048/1065).
    NativeLodRelationDecision local;
    local.ChildSubmitted = childVisible;
    local.ParentMarked = childVisible && childAlpha == 255;
    local.ParentSuppressed = local.ParentMarked;
    local.ParentSubmitted = !local.ParentMarked && parentVisible;
    if (local.ParentMarked) {
        local.Reason =
            "Renderer 509-521 opaque visible child AddLodChildrenRendered + single-child VISIBLE; "
            "695-722 marked parent suppressed to LodRenderList/STREAMME";
    } else if (childVisible) {
        local.Reason =
            "Renderer 509-521 visible translucent child (alpha<255) still VISIBLE without marking; "
            "640-655/1048-1065 visible parent fallback";
    } else {
        local.Reason = "Renderer child not visible, no marking; 640-655/1048-1065 visible parent fallback";
    }
    out = std::move(local);
    error.clear();
    return true;
} catch (const std::exception& e) { error = e.what(); return false; }

bool NativeLodCatalog::SelectResidency(float x, float y, float radius, int area, NativeCatalogResidency& out,
                                       std::string& error) const try {
    // Pure diagnostic residency: never mutates m_Nodes/m_Sources/m_Metadata.
    // On any rejection out is left unchanged; only a complete residency is published.
    if (!m_DiskValidated) { error = "Error: catalog lacks disk validation"; return false; }
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(radius) || !(radius > 0.0f)) {
        error = "Error: invalid residency window";
        return false;
    }
    if (area < 0 || area > 255) { error = "Error: area out of range"; return false; }
    if (m_Nodes.empty()) { error = "Error: empty residency selection"; return false; }

    auto formatIdentity = [](const NativePlacementIdentity& id) {
        return id.Ipl + ":" + std::to_string(id.Record) + " " + id.Model + " (" +
            std::to_string(id.ModelId) + ")" + (id.Binary ? " binary" : " text");
    };

    // Initial window in catalog source order (m_Nodes preserves source order).
    // Area 0: exterior XY disc (Interior low byte == 0 as well as squared
    // distance, no Z cull). Positive area: entire interior via exact low-byte
    // match (position ignored beyond the finite validation above). No name-prefix filters, caps or skips.
    std::vector<char> selected(m_Nodes.size(), 0);
    size_t initialCount = 0;
    const double radiusDouble = static_cast<double>(radius);
    const double radiusSquared = radiusDouble * radiusDouble;
    for (size_t i = 0; i < m_Nodes.size(); ++i) {
        const auto& node = m_Nodes[i];
        bool inWindow = false;
        if (area == 0) {
            if ((node.Placement.Interior & 255) != 0) inWindow = false;
            else {
                const double dx = static_cast<double>(node.Placement.Position[0]) - static_cast<double>(x);
                const double dy = static_cast<double>(node.Placement.Position[1]) - static_cast<double>(y);
                if (dx * dx + dy * dy <= radiusSquared) inWindow = true;
            }
        } else {
            if ((node.Placement.Interior & 255) == area) inWindow = true;
        }
        if (inWindow) { selected[i] = 1; ++initialCount; }
    }
    if (!initialCount) { error = "Error: empty residency selection"; return false; }

    // Bound-parent closure recursively, even outside the initial window.
    std::vector<size_t> work;
    work.reserve(m_Nodes.size());
    for (size_t i = 0; i < m_Nodes.size(); ++i) {
        if (selected[i]) work.push_back(i);
    }
    size_t head = 0;
    while (head < work.size()) {
        const size_t idx = work[head++];
        if (idx >= m_Nodes.size()) { error = "Error: residency index outside catalog"; return false; }
        const auto& node = m_Nodes[idx];
        if (node.Link != NativeLodLinkStatus::Bound) continue;
        if (!node.Parent) { error = "Error: invalid residency link " + formatIdentity(node.Identity); return false; }
        const size_t parent = *node.Parent;
        if (parent >= m_Nodes.size()) {
            error = "Error: invalid residency link " + formatIdentity(node.Identity);
            return false;
        }
        if (!selected[parent]) { selected[parent] = 1; work.push_back(parent); }
    }

    // Whole-candidate validation over the closed set. Any failure rejects the
    // whole candidate with out unchanged; nothing is silently skipped.
    size_t timeModels = 0;
    for (size_t i = 0; i < m_Nodes.size(); ++i) {
        if (!selected[i]) continue;
        const auto& node = m_Nodes[i];
        if (node.Link != NativeLodLinkStatus::None && node.Link != NativeLodLinkStatus::Bound) {
            error = "Error: invalid residency link " + formatIdentity(node.Identity);
            return false;
        }
        if (node.Link == NativeLodLinkStatus::Bound) {
            if (!node.Parent || *node.Parent >= m_Nodes.size()) {
                error = "Error: invalid residency link " + formatIdentity(node.Identity);
                return false;
            }
            const auto& parent = m_Nodes[*node.Parent];
            if (!selected[*node.Parent]) {
                error = "Error: invalid residency link " + formatIdentity(node.Identity);
                return false;
            }
            // Bound Lod binding must address the parent record.
            if (node.Placement.Lod != static_cast<int32_t>(parent.LocalIndex)) {
                error = "Error: invalid residency link " + formatIdentity(node.Identity);
                return false;
            }
            // Cross-area linkage is never admitted.
            if ((node.Placement.Interior & 255) != (parent.Placement.Interior & 255)) {
                error = "Error: cross-area residency link " + formatIdentity(node.Identity) + " -> " +
                    formatIdentity(parent.Identity);
                return false;
            }
        }
        // Area queries admit only that interior (closure included); area 0 is
        // the explicit exterior selection.
        if ((node.Placement.Interior & 255) != area) {
            error = "Error: cross-area residency selection " + formatIdentity(node.Identity);
            return false;
        }
        const auto meta = m_Metadata.Query(node.Placement);
        if (meta.Status != NativeWorldInfoStatus::Ready || !meta.Model || !meta.Placement) {
            error = "Error: unknown residency model " + formatIdentity(node.Identity);
            return false;
        }
        if (meta.Model->Kind != NativeWorldModelKind::Atomic &&
            meta.Model->Kind != NativeWorldModelKind::TimeAtomic) {
            error = std::string(meta.Model->Kind == NativeWorldModelKind::Clump ?
                                    "Error: nonstatic residency model " : "Error: unknown residency model ") +
                formatIdentity(node.Identity);
            return false;
        }
        if (meta.Model->Kind == NativeWorldModelKind::TimeAtomic) ++timeModels;
    }

    // Diagnostic split: any selected node referenced as parent by another
    // selected node is hidden; others are visible. Source order preserved.
    // Hidden is an explicitly labelled lab policy, NOT source runtime LOD:
    // Runtime* fields are never touched (this method is const).
    std::vector<char> isParent(m_Nodes.size(), 0);
    for (size_t i = 0; i < m_Nodes.size(); ++i) {
        if (!selected[i]) continue;
        const auto& node = m_Nodes[i];
        if (node.Link == NativeLodLinkStatus::Bound && node.Parent && *node.Parent < m_Nodes.size() &&
            selected[*node.Parent]) {
            isParent[*node.Parent] = 1;
        }
    }
    NativeCatalogResidency local;
    local.Population = m_Nodes.size();
    local.TimeModels = timeModels;
    for (size_t i = 0; i < m_Nodes.size(); ++i) {
        if (!selected[i]) continue;
        if (isParent[i]) local.HiddenTargets.push_back(m_Nodes[i].Identity);
        else local.Visible.push_back(m_Nodes[i].Identity);
    }
    if (local.Visible.empty() && local.HiddenTargets.empty()) {
        error = "Error: empty residency selection";
        return false;
    }
    if (local.Visible.size() + local.HiddenTargets.size() > local.Population) {
        error = "Error: invalid residency census";
        return false;
    }
    local.ExcludedOutside = local.Population - local.Visible.size() - local.HiddenTargets.size();
    out = std::move(local);
    error.clear();
    return true;
} catch (const std::exception& e) { error = e.what(); return false; }
