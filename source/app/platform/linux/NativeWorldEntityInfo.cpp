#include "app/platform/linux/NativeWorldEntityInfo.h"
#include "app/platform/linux/NativeMetadataText.h"
#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
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

namespace {
using NativeMetadataText::Require;
using NativeMetadataText::Lower;
using NativeMetadataText::Key;
using NativeMetadataText::Lines;
using NativeMetadataText::Scan;
using NativeMetadataText::MaxFileBytes;
constexpr size_t MaxTotalBytes = 32 * 1024 * 1024;
constexpr size_t MaxModels = 20000, MaxPlacements = 250000, MaxSources = 1024;

static std::optional<NativeWorldPedModelInfo> PedProperties(
    const std::string& line, const NativeWorldSourceRow& row) {
    Scan scan(line);
    NativeWorldPedModelInfo ped;
    uint32_t cars{}, flags{};
    if (!(scan.Int(ped.ModelId) && scan.Word(ped.Name, 19) && scan.Word(ped.TxdName, 19) &&
        scan.Word(ped.PedTypeName, 23) && scan.Word(ped.StatName, 19) &&
        scan.Word(ped.AnimationGroupName, 19) && scan.Hex(cars) && scan.Hex(flags) &&
        scan.Word(ped.AnimationFileName, 11) && scan.Int(ped.Radio1) && scan.Int(ped.Radio2) &&
        scan.Word(ped.AudioTypeName, 15) && scan.Word(ped.VoiceMinName, 55) &&
        scan.Word(ped.VoiceMaxName, 59))) return std::nullopt;
    ped.Ide = row;
    // Source stores the parsed 32-bit hexadecimal values in uint16 members.
    ped.CarsCanDriveMask = static_cast<uint16_t>(cars);
    ped.PedFlags = static_cast<uint16_t>(flags);
    return ped;
}
static void IdeProperties(const std::string& line, const std::string& section, NativeWorldModelInfo& m) {
    Scan r(line); int id{}, flags{}, on{}, off{}; std::string name, txd, anim; float draw{};
    Require(r.Int(id) && r.Word(name, 23) && r.Word(txd, 23), "invalid IDE model identity");
    if (section == "anim") {
        Require(r.Word(anim, 15) && r.Float(draw) && r.Int(flags), "invalid anim IDE record");
        m.Kind = NativeWorldModelKind::Clump;
        m.HasAnimBlend = anim == "null" ? NativeWorldKnownBool::False : NativeWorldKnownBool::True;
        m.DrawDistance = draw; m.IdeFlags = static_cast<uint32_t>(flags);
        m.TxdName = txd; m.AnimationName = anim; m.TimeOn.reset(); m.TimeOff.reset();
        return;
    }
    if (section != "objs" && section != "tobj") return;
    m.Kind = section == "tobj" ? NativeWorldModelKind::TimeAtomic : NativeWorldModelKind::Atomic;
    m.HasAnimBlend = NativeWorldKnownBool::False;
    const bool modern = r.Float(draw) && r.Int(flags) && (section != "tobj" || (r.Int(on) && r.Int(off)));
    bool known = modern;
    if (!modern || draw < 4.0f) {
        Scan legacy(line); int count{}; float unused{};
        known = legacy.Int(id) && legacy.Word(name, 23) && legacy.Word(txd, 23) && legacy.Int(count);
        if (known && count >= 1 && count <= 3) {
            known = legacy.Float(draw);
            for (int i = 1; i < count; ++i) known = legacy.Float(unused) && known;
            const bool readFlags = legacy.Int(flags);
            // LoadObject count=1 accepts five fields: flags retain the first scan's value.
            known = known && (readFlags || (section == "objs" && count == 1 && modern));
            if (section == "tobj") known = known && legacy.Int(on) && legacy.Int(off);
        } else {
            // LoadObject has no default arm; LoadTimeObject's default is unreachable.
            known = known && modern && section == "objs";
        }
    }
    Require(known, "invalid or unrepresented static IDE numeric grammar");
    m.DrawDistance = draw; m.IdeFlags = static_cast<uint32_t>(flags);
    m.TxdName = txd; m.AnimationName.reset();
    if (section == "tobj") { m.TimeOn = on; m.TimeOff = off; }
    else { m.TimeOn.reset(); m.TimeOff.reset(); }
}
static std::string Resolve(const char* gameDir, std::string relative) {
    Require(gameDir && *gameDir, "missing game directory");
    std::replace(relative.begin(), relative.end(), '\\', '/');
    auto path = std::filesystem::absolute(gameDir);
    Require(!std::filesystem::path(relative).is_absolute(), "absolute metadata asset path");
    for (const auto& part : std::filesystem::path(relative)) {
        Require(part != ".." && part != ".", "invalid metadata asset path");
        std::optional<std::filesystem::path> found;
        for (const auto& entry : std::filesystem::directory_iterator(path)) {
            if (Lower(entry.path().filename().string()) == Lower(part.string())) {
                Require(!found, "ambiguous metadata asset path"); found = entry.path();
            }
        }
        Require(found.has_value(), "missing metadata asset: " + relative); path = *found;
    }
    return path.string();
}
static NativeWorldEntitySourceText Read(const char* gameDir, const std::string& relative, size_t& total) {
    const auto path = Resolve(gameDir, relative);
    struct File { void* Handle{}; ~File() { if (Handle) OS_FileClose(Handle); } } file;
    Require(OS_FileOpen(FILE_DATA_AREA_DEFAULT, &file.Handle, path.c_str(), FILE_ACCESS_READ) == 0 && file.Handle,
            "open metadata asset: " + relative);
    auto size = OS_FileSize(file.Handle);
    Require(size >= 0 && static_cast<size_t>(size) <= MaxFileBytes && static_cast<size_t>(size) <= MaxTotalBytes - total,
            "metadata byte budget");
    total += static_cast<size_t>(size);
    NativeWorldEntitySourceText out{relative, std::string(static_cast<size_t>(size), '\0')};
    if (size) Require(OS_FileRead(file.Handle, out.Text.data(), size) == 0, "short metadata asset read");
    return out;
}
}

bool NativeWorldEntitySourceText::ReadBeforeWorker(const char* gameDir, const std::string& relative,
    size_t& total, NativeWorldEntitySourceText& out, std::string& error) try {
    Require(total <= MaxTotalBytes, "metadata total byte budget");
    auto candidateTotal = total;
    auto candidate = Read(gameDir, relative, candidateTotal);
    out = std::move(candidate);
    total = candidateTotal;
    error.clear();
    return true;
} catch (const std::exception& e) { error = e.what(); return false; }

bool NativeWorldEntityInfo::LoadBeforeWorker(const char* gameDir, const NativeCollisionPopulation& population,
                                           std::string& error) try {
    size_t total = 0;
    std::vector<NativeWorldEntitySourceText> sources;
    bool firstIpl = false;
    for (const char* dat : {"data/default.dat", "data/gta.dat"}) {
        const auto list = Read(gameDir, dat, total);
        Lines(list, [&](const auto& line, const auto&) {
            if (line[0] == '#') return true;
            Scan scan(line); std::string kind, path;
            scan.Word(kind);
            if (kind == "IPL") firstIpl = true;
            if (kind == "IDE") {
                Require(!firstIpl, "IDE declared after Object.dat initialization boundary");
                Require(scan.Word(path) && sources.size() < MaxSources, "invalid IDE declaration");
                sources.push_back(Read(gameDir, path, total));
            }
            return true;
        });
    }
    return LoadSources(population, sources, Read(gameDir, "data/object.dat", total), error);
} catch (const std::exception& e) { error = e.what(); return false; }

bool NativeWorldEntityInfo::LoadSources(const NativeCollisionPopulation& population,
                                      std::span<const NativeWorldEntitySourceText> ideSources,
                                      const NativeWorldEntitySourceText& objectSource, std::string& error) try {
    Require(!ideSources.empty() && ideSources.size() <= MaxSources && population.Models.size() <= MaxModels &&
            population.Instances.size() <= MaxPlacements, "metadata population bounds");
    size_t total = objectSource.Text.size();
    std::map<int, NativeWorldModelInfo> sourceModels;
    std::map<int, NativeWorldPedModelInfo> sourcePeds;
    for (const auto& source : ideSources) {
        Require(source.Text.size() <= MaxTotalBytes && total <= MaxTotalBytes - source.Text.size(), "metadata total byte budget");
        total += source.Text.size();
        std::string section;
        Lines(source, [&](const auto& line, const auto& row) {
            if (line[0] == '#') return true;
            Scan scan(line); std::string first; scan.Word(first);
            if (first == "end") { section.clear(); return true; }
            if (section.empty()) { section = first; return true; }
            if (section != "objs" && section != "tobj" && section != "anim" && section != "cars" &&
                section != "peds" && section != "weap" && section != "hier") return true;
            NativeWorldModelInfo model;
            Scan identity(line);
            Require(identity.Int(model.ModelId) && model.ModelId >= 0 && model.ModelId < static_cast<int>(MaxModels) &&
                    identity.Word(model.Name, 23), "invalid IDE identity: " + row.Source);
            model.Ide = row;
            IdeProperties(line, section, model);
            // Keep the old identity-only import boundary conservative. Only a
            // complete valid LoadPedObject row can authorize ped metadata.
            sourcePeds.erase(model.ModelId);
            if (section == "peds") {
                if (auto ped = PedProperties(line, row)) sourcePeds.emplace(model.ModelId, std::move(*ped));
            }
            model.ObjectInfo = NativeWorldObjectAssignment::Unassigned;
            sourceModels[model.ModelId] = std::move(model); // Later IDE definitions replace earlier IDs before Object.dat.
            return true;
        });
    }
    std::map<uint32_t, std::vector<int>> keys;
    for (const auto& [id, model] : sourceModels) keys[Key(model.Name)].push_back(id);
    size_t objectRows = 0;
    Lines(objectSource, [&](const auto& line, const auto& row) {
        if (line[0] == ';' || line[0] == '#') return true;
        if (line[0] == '*') return false;
        Require(++objectRows <= 20000, "Object.dat record budget");
        Scan scan(line); std::string name; std::array<float, 7> f{}; std::array<int, 5> i{};
        Require(scan.Word(name), "Object.dat name");
        for (auto& value : f) Require(scan.Float(value), "Object.dat required float at line " + std::to_string(row.Line));
        for (auto& value : i) Require(scan.Int(value), "Object.dat required integer at line " + std::to_string(row.Line));
        // ObjectData.cpp only requires >=13 conversions. Optional physics/FX fields
        // affect dedup indices, never assigned-vs-minus-one, so do not parse/invent them.
        const auto found = keys.find(Key(name));
        if (found == keys.end()) return true;
        Require(found->second.size() == 1, "ambiguous Object.dat model CRC key: " + name);
        auto& model = sourceModels.at(found->second.front());
        model.ObjectInfo = NativeWorldObjectAssignment::Assigned;
        model.ObjectRows.push_back(row);
        model.DefaultObjectInfoIndex.reset();
        // Source stores these three integers as uint8 before testing defaults.
        if (f[0] == 99999.0f && f[6] == 1.0f && static_cast<uint8_t>(i[0]) == 0) {
            const auto special = static_cast<uint8_t>(i[1]);
            if (special == 0 || special == 4) model.DefaultObjectInfoIndex =
                static_cast<uint8_t>((special == 4 ? 2 : 0) + (static_cast<uint8_t>(i[2]) == 0 ? 0 : 1));
        }
        return true;
    });
    NativeWorldEntityInfo next;
    next.m_PedModels = std::move(sourcePeds);
    next.m_ModelIdsByKey = std::move(keys);
    for (const auto& [id, definition] : population.Models) {
        Require(id >= 0 && id < static_cast<int>(MaxModels) && !definition.Name.empty() && definition.Name.size() <= 23,
                "invalid population model identity");
        Key(definition.Name);
        const auto found = sourceModels.find(id);
        if (found == sourceModels.end()) continue; // Explicitly unrepresented, not all-buildings.
        const auto& source = found->second;
        Require(Lower(source.Name) == Lower(definition.Name), "population IDE name/ID mismatch");
        Require(definition.TimeModel == (source.Kind == NativeWorldModelKind::TimeAtomic), "population IDE time-kind mismatch");
        auto model = source;
        if (model.Kind != NativeWorldModelKind::Unknown) {
            model.InitialClass = model.ObjectInfo == NativeWorldObjectAssignment::Assigned ? NativeWorldInitialClass::DummyObject :
                model.HasAnimBlend == NativeWorldKnownBool::True ? NativeWorldInitialClass::AnimatedBuilding : NativeWorldInitialClass::Building;
        }
        next.m_Models.emplace(id, std::move(model));
    }
    for (const auto& p : population.Instances) {
        const auto model = population.Models.find(p.ModelId);
        Require(model != population.Models.end() && Lower(model->second.Name) == Lower(p.Model), "IPL model identity mismatch");
        Require(!p.Ipl.empty() && p.Ipl.size() <= 1024 && p.Interior >= 0, "invalid IPL provenance/area");
        for (auto v : p.Position) Require(std::isfinite(v), "nonfinite IPL position");
        float qnorm = 0;
        for (auto v : p.Quaternion) { Require(std::isfinite(v), "nonfinite IPL quaternion"); qnorm += v * v; }
        Require(std::isfinite(qnorm) && qnorm > 0, "invalid IPL quaternion");
        NativeWorldPlacementInfo info;
        info.Identity = NativePlacementIdentity::From(p);
        info.ExportedInterior = p.Interior; info.ExportedFlags = p.Flags; info.Lod = p.Lod;
        info.Area = static_cast<uint8_t>(p.Interior & 255);
        if (p.Binary || population.IncludesStreamed) {
            // StreamPager's source-backed runtime export certifies the full
            // text word too. Legacy/import populations keep it unknown.
            Require(p.Interior == static_cast<int>(p.Flags & 255), "source IPL area/type mismatch");
            info.SourceInstanceType = p.Flags;
            const auto bit = [&](uint32_t mask) {
                return (p.Flags & mask) ? NativeWorldKnownBool::True : NativeWorldKnownBool::False;
            };
            info.AuthoredRedundantStream = bit(1u << 8);
            info.AuthoredDontStream = bit(1u << 9);
            info.AuthoredUnderwater = bit(1u << 10);
            info.AuthoredTunnel = bit(1u << 11);
            info.AuthoredTunnelTransition = bit(1u << 12);
        }
        Require(next.m_Placements.emplace(PlacementKey{p.Ipl, p.Record, p.Binary}, std::move(info)).second,
                "duplicate IPL source record");
    }
    next.m_Loaded = true;
    *this = std::move(next); error.clear(); return true;
} catch (const std::exception& e) { error = e.what(); return false; }

const NativeWorldModelInfo* NativeWorldEntityInfo::FindModel(int modelId, std::string_view name) const {
    const auto found = m_Models.find(modelId);
    return found != m_Models.end() && Lower(found->second.Name) == Lower(name) ? &found->second : nullptr;
}
NativeWorldNameStatus NativeWorldEntityInfo::FindNamespaceModelId(std::string_view name, int& modelId) const {
    if (!m_Loaded) return NativeWorldNameStatus::NotLoaded;
    if (name.empty() || name.size() > 255 || std::ranges::any_of(name,
        [](unsigned char c) { return c < 32 || c >= 127; })) return NativeWorldNameStatus::InvalidName;
    const auto found = m_ModelIdsByKey.find(Key(name));
    if (found == m_ModelIdsByKey.end()) return NativeWorldNameStatus::Missing;
    if (found->second.size() != 1) return NativeWorldNameStatus::Ambiguous;
    modelId = found->second.front();
    return NativeWorldNameStatus::Found;
}
NativeWorldEntityMetadata NativeWorldEntityInfo::Query(const NativeCollisionPlacement& p) const {
    if (!m_Loaded) return {};
    const auto model = m_Models.find(p.ModelId);
    if (model == m_Models.end()) return {NativeWorldInfoStatus::ModelUnrepresented};
    if (Lower(model->second.Name) != Lower(p.Model)) return {NativeWorldInfoStatus::IdentityMismatch};
    const auto found = m_Placements.find(PlacementKey{p.Ipl, p.Record, p.Binary});
    if (found == m_Placements.end()) return {NativeWorldInfoStatus::PlacementUnrepresented};
    const auto& placement = found->second;
    if (!placement.Identity.Matches(p) || placement.ExportedInterior != p.Interior ||
        placement.ExportedFlags != p.Flags || placement.Lod != p.Lod) return {NativeWorldInfoStatus::IdentityMismatch};
    return {NativeWorldInfoStatus::Ready, &model->second, &placement};
}
NativeWorldKnownBool NativeWorldEntityMetadata::InitialBuildingMask() const {
    if (Status != NativeWorldInfoStatus::Ready || !Model) return NativeWorldKnownBool::Unknown;
    switch (Model->InitialClass) {
    case NativeWorldInitialClass::Building:
    case NativeWorldInitialClass::AnimatedBuilding: return NativeWorldKnownBool::True;
    case NativeWorldInitialClass::DummyObject: return NativeWorldKnownBool::False;
    default: return NativeWorldKnownBool::Unknown;
    }
}
