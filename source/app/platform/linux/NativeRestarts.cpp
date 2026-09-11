#include "app/platform/linux/NativeRestarts.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <numbers>
#include <sstream>
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
bool Valid(NativeRestartKind k) { return k == NativeRestartKind::Hospital || k == NativeRestartKind::Police; }
bool Finite(NativeScriptPosition p) { return std::isfinite(p.X) && std::isfinite(p.Y) && std::isfinite(p.Z); }
bool Finite(const NativeRestartPoint& p) { return Finite(p.Position) && std::isfinite(p.HeadingDegrees); }
void Require(bool ok, const std::string& why) { if (!ok) throw std::runtime_error(why); }
std::string Lower(std::string s) { for (auto& c : s) if (c >= 'A' && c <= 'Z') c += 'a'-'A'; return s; }
std::string Clean(std::string s) {
    if (const auto i = s.find('#'); i != s.npos) s.resize(i);
    std::replace(s.begin(), s.end(), ',', ' ');
    const auto i = s.find_first_not_of(" \r\n\t");
    return i == s.npos ? std::string{} : s.substr(i, s.find_last_not_of(" \r\n\t")-i+1);
}
std::string Read(const char* root, std::string relative) {
    std::replace(relative.begin(), relative.end(), '\\', '/');
    Require(!relative.empty() && relative.front() != '/' && relative.find(':') == relative.npos && relative.find("..") == relative.npos, "restart invalid asset path");
    auto path = std::filesystem::absolute(root);
    for (const auto& part : std::filesystem::path(relative)) {
        std::filesystem::path match;
        for (const auto& child : std::filesystem::directory_iterator(path)) {
            if (Lower(child.path().filename().string()) != Lower(part.string())) continue;
            Require(match.empty(), "restart ambiguous asset case"); match = child.path();
        }
        Require(!match.empty(), "restart missing asset " + relative); path = std::move(match);
    }
    struct File { void* Handle = nullptr; ~File() { if (Handle) OS_FileClose(Handle); } } file;
    Require(OS_FileOpen(FILE_DATA_AREA_DEFAULT, &file.Handle, path.c_str(), FILE_ACCESS_READ) == 0 && file.Handle, "restart asset open");
    const auto size = OS_FileSize(file.Handle);
    Require(size >= 0 && size <= 16*1024*1024, "restart text size bound");
    std::string text(size, '\0'); Require(OS_FileRead(file.Handle, text.data(), size) == 0, "restart asset read");
    return text;
}
float Narrow(float f) {
    Require(std::isfinite(f) && f >= -32768 && f < 32768, "restart map coordinate int16 bound");
    return float(std::int16_t(f));
}
}
bool NativeRestarts::InitializeMapZones(std::span<const NativeRestartMapZone> zones, std::string& error) {
    if (m_Sealed || m_MapCount || zones.size() >= MapCapacity) { error = "restart map initialization/capacity"; return false; }
    try {
        std::array<NativeRestartMapZone, MapCapacity> prepared{};
        prepared[0] = {{-3000,-3000,-2000},{3000,3000,2000},0}; // TheZones::Initialise
        std::size_t n = 1;
        for (auto z : zones) {
            Require(z.Level <= 3, "restart unknown source level");
            z.Min = {Narrow(z.Min.X),Narrow(z.Min.Y),Narrow(z.Min.Z)};
            z.Max = {Narrow(z.Max.X),Narrow(z.Max.Y),Narrow(z.Max.Z)};
            const auto lo = z.Min;
            z.Min = {std::min(lo.X,z.Max.X),std::min(lo.Y,z.Max.Y),std::min(lo.Z,z.Max.Z)};
            z.Max = {std::max(lo.X,z.Max.X),std::max(lo.Y,z.Max.Y),std::max(lo.Z,z.Max.Z)};
            prepared[n++] = z;
        }
        m_MapZones = prepared; m_MapCount = n; ++m_Revision; error.clear(); return true;
    } catch (const std::exception& e) { error = e.what(); return false; }
}
bool NativeRestarts::LoadBeforeWorker(const char* gameDir, std::string& error) {
    if (!gameDir || !*gameDir || m_Sealed || m_MapCount) { error = "restart load requires unsealed startup once"; return false; }
    try {
        std::vector<NativeRestartMapZone> zones;
        for (const auto* dat : {"data/default.dat", "data/gta.dat"}) {
            std::istringstream lines(Read(gameDir,dat)); std::string line;
            while (std::getline(lines,line)) {
                std::istringstream tokens(Clean(line)); std::string kind, path, extra;
                if (!(tokens >> kind)) continue;
                if (kind == "EXIT") break;
                if (kind != "IPL") continue;
                Require(bool(tokens >> path) && !(tokens >> extra), "restart DAT IPL fields");
                const auto text = Read(gameDir,path);
                if (text.starts_with("bnry")) continue;
                std::istringstream rows(text); bool section = false;
                while (std::getline(rows,line)) {
                    line = Clean(line); if (line.empty()) continue;
                    if (line == "end") { section = false; continue; }
                    if (line == "zone") { section = true; continue; }
                    if (!section) continue;
                    NativeRestartMapZone zone; int type, level; std::string name, label;
                    std::istringstream row(line);
                    Require(bool(row >> name >> type >> zone.Min.X >> zone.Min.Y >> zone.Min.Z >> zone.Max.X >> zone.Max.Y >> zone.Max.Z >> level >> label) && !(row >> extra), "restart zone row fields");
                    if (type != 3) continue; // eZoneType::ZONE_TYPE_MAP only
                    Require(level >= 0 && level <= 3, "restart map level"); zone.Level = std::uint8_t(level);
                    zones.push_back(zone); Require(zones.size() < MapCapacity, "restart source map capacity");
                }
            }
        }
        Require(!zones.empty(), "restart DAT contains no map-zone authority");
        return InitializeMapZones(zones,error);
    } catch (const std::exception& e) { error = e.what(); return false; }
}
std::span<const NativeRestartPoint> NativeRestarts::Points(NativeRestartKind kind) const {
    if (!Valid(kind)) return {};
    const auto i = std::size_t(kind); return {m_Points[i].data(),m_Counts[i]};
}
NativeScriptServiceResult NativeRestarts::Add(const NativeScriptRestartRequest& r) {
    if (!Valid(r.Kind) || !Finite(r.Position) || !std::isfinite(r.HeadingDegrees)) return {NativeScriptServiceStatus::Error,"invalid restart operands"};
    // Original016C/D perform FindGroundZForCoord on <= -100 before Add*.
    if (r.Position.Z <= -100) return {NativeScriptServiceStatus::Unsupported,"restart ground sentinel requires source XY ground authority"};
    const auto k = std::size_t(r.Kind);
    if (m_Counts[k] == Capacity) return {NativeScriptServiceStatus::Error,"restart source capacity (10 per kind)"};
    m_Points[k][m_Counts[k]++] = {r.Position,r.HeadingDegrees,r.WhenToUse}; ++m_Revision;
    return {NativeScriptServiceStatus::Ready,{}};
}
NativeScriptServiceResult NativeRestarts::Configure(const NativeRestartPolicy& p) {
    if ((p.OverrideNext && !Finite(*p.OverrideNext)) || (p.MissionBase && !Finite(*p.MissionBase)) ||
        !Finite(p.Hospital.Point) || !Finite(p.Police.Point) || !std::isfinite(p.Hospital.Radius) || !std::isfinite(p.Police.Radius))
        return {NativeScriptServiceStatus::Error,"nonfinite restart policy"};
    m_Policy = p; ++m_Revision; return {NativeScriptServiceStatus::Ready,{}};
}
std::optional<std::uint8_t> NativeRestarts::LevelAt(NativeScriptPosition p) const {
    if (!m_MapCount || !Finite(p)) return {};
    for (std::size_t i = 1; i < m_MapCount; ++i) {
        const auto& z = m_MapZones[i];
        if (p.X >= z.Min.X && p.X <= z.Max.X && p.Y >= z.Min.Y && p.Y <= z.Max.Y && p.Z >= z.Min.Z && p.Z <= z.Max.Z) return z.Level;
    }
    return m_MapZones[0].Level;
}
NativeRestartSelection NativeRestarts::Query(const NativeRestartQuery& q) const {
    if (!Valid(q.Kind) || !Finite(q.Position) || (q.OutsideWorldPosition && !Finite(*q.OutsideWorldPosition)) ||
        (q.CityUnlocked && !std::isfinite(*q.CityUnlocked))) return {NativeRestartStatus::Error,{},"invalid restart query"};
    NativeRestartRequired r; r.Kind = q.Kind; r.RegistryRevision = m_Revision;
    r.Fade = q.Kind == NativeRestartKind::Hospital ? m_Policy.FadeAfterDeath : m_Policy.FadeAfterArrest;
    auto finish = [&]() -> NativeRestartSelection {
        r.ResurrectionPosition = r.Target.Position; r.ResurrectionPosition.Z += 1;
        r.HeadingRadians = r.Target.HeadingDegrees * (std::numbers::pi_v<float>/180.0f);
        if (!Finite(r.ResurrectionPosition) || !std::isfinite(r.HeadingRadians)) return {NativeRestartStatus::Error,{},"restart target overflow"};
        return {NativeRestartStatus::RestartRequired,r,{}};
    };
    if (m_Policy.OverrideNext) { r.Target = *m_Policy.OverrideNext; r.Origin = NativeRestartOrigin::Override; r.ConsumeOverride = true; return finish(); }
    auto point = q.Position;
    if (m_Policy.MissionBase) { point = *m_Policy.MissionBase; r.ConsumeMissionBase = true; }
    else {
        if (!q.Area) return {NativeRestartStatus::Unsupported,{},"restart query requires source area/ENEX authority"};
        if (q.OutsideWorldPosition) point = *q.OutsideWorldPosition;
        else if (*q.Area != 0) return {NativeRestartStatus::Unsupported,{},"interior restart requires ENEX outside-world position"};
    }
    const auto& extra = q.Kind == NativeRestartKind::Hospital ? m_Policy.Hospital : m_Policy.Police;
    const float dx = point.X-extra.Point.Position.X, dy = point.Y-extra.Point.Position.Y;
    if (extra.Radius > 0 && std::sqrt(dx*dx+dy*dy) < extra.Radius) {
        r.Target = extra.Point; r.Origin = NativeRestartOrigin::Extra; return finish();
    }
    const auto level = LevelAt(point);
    if (!level || !q.CityUnlocked) return {NativeRestartStatus::Unsupported,{},"restart nearest requires owned map zones and city-unlocked stat"};
    float closest = q.Kind == NativeRestartKind::Hospital ? 10'000'000.0f : 99'999.898f;
    bool found = false;
    const auto records = Points(q.Kind);
    for (std::size_t i = 0; i < records.size(); ++i) {
        const auto& p = records[i]; if (*q.CityUnlocked < float(p.WhenToUse)) continue;
        const float x = p.Position.X-point.X, y = p.Position.Y-point.Y, z = p.Position.Z-point.Z;
        float distance = std::sqrt(x*x+y*y+z*z);
        if (*level != 0 && *level != *LevelAt(p.Position)) distance *= 6;
        if (distance < closest) { closest = distance; r.Target = p; r.RegistryIndex = i; found = true; }
    }
    if (!found) return {NativeRestartStatus::Unsupported,{},"source selector has no eligible initialized restart target"};
    return finish();
}
bool NativeRestarts::ConsumeSelection(const NativeRestartRequired& r) {
    if (r.RegistryRevision != m_Revision || (r.ConsumeOverride && !m_Policy.OverrideNext) || (r.ConsumeMissionBase && !m_Policy.MissionBase)) return false;
    if (!r.ConsumeOverride && !r.ConsumeMissionBase) return true;
    if (r.ConsumeOverride) m_Policy.OverrideNext.reset();
    if (r.ConsumeMissionBase) m_Policy.MissionBase.reset();
    ++m_Revision; return true;
}
