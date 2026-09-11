#include "app/platform/linux/NativeLodCatalog.h"
#include <algorithm>
#include <bit>
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
