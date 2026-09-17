#include "app/platform/linux/NativeEntryExits.h"
#include "app/platform/linux/NativeScriptEntities.h"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <limits>
#include <numbers>
#include <random>
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
constexpr float Radians = std::numbers::pi_v<float> / 180.0f;
void Require(bool ok, const std::string& why) { if (!ok) throw std::runtime_error(why); }
bool Finite(NativeScriptPosition p) { return std::isfinite(p.X) && std::isfinite(p.Y) && std::isfinite(p.Z); }
std::string Lower(std::string s) {
    for (auto& c : s) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return s;
}
std::string Clean(std::string s) {
    if (const auto comment = s.find('#'); comment != s.npos) s.resize(comment);
    const auto first = s.find_first_not_of(" \r\n\t");
    return first == s.npos ? std::string{} : s.substr(first, s.find_last_not_of(" \r\n\t") - first + 1);
}
std::string ReadText(const std::string& path) {
    struct File { void* Handle = nullptr; ~File() { if (Handle) OS_FileClose(Handle); } } file;
    Require(OS_FileOpen(FILE_DATA_AREA_DEFAULT, &file.Handle, path.c_str(), FILE_ACCESS_READ) == 0 && file.Handle, "ENEX open: " + path);
    const auto size = OS_FileSize(file.Handle);
    Require(size >= 0 && size <= 16 * 1024 * 1024, "ENEX text file bound: " + path);
    std::string text(size, '\0');
    Require(OS_FileRead(file.Handle, text.data(), size) == 0, "ENEX read: " + path);
    return text;
}
std::string AssetPath(std::string path) {
    std::replace(path.begin(), path.end(), '\\', '/');
    Require(!path.empty() && path.front() != '/' && path.find(':') == path.npos && path.find("..") == path.npos, "invalid DAT asset path");
    return path;
}
std::string ResolvePath(const char* gameDir, const std::string& relative) {
    auto path = std::filesystem::absolute(gameDir);
    for (const auto& component : std::filesystem::path(relative)) {
        std::filesystem::path match;
        for (const auto& child : std::filesystem::directory_iterator(path)) {
            if (Lower(child.path().filename().string()) != Lower(component.string())) continue;
            Require(match.empty(), "ambiguous case-insensitive ENEX asset path"); match = child.path();
        }
        Require(!match.empty(), "missing ENEX asset path: " + relative); path = std::move(match);
    }
    return path.string();
}
template<typename T> T Number(const std::string& s) {
    T value{};
    const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), value);
    Require(ec == std::errc{} && end == s.data() + s.size(), "invalid ENEX numeric field");
    if constexpr (std::is_floating_point_v<T>) Require(std::isfinite(value), "nonfinite ENEX field");
    return value;
}
NativeEntryExitRect Leaf(std::size_t index) {
    NativeEntryExitRect rect{-3000, -3000, 3000, 3000};
    for (int shift = 6; shift >= 0; shift -= 2) {
        const auto sector = (index >> shift) & 3;
        const float x = (rect.Left + rect.Right) / 2, y = (rect.Bottom + rect.Top) / 2;
        if (sector & 1) rect.Left = x; else rect.Right = x;
        if (sector & 2) rect.Top = y; else rect.Bottom = y;
    }
    return rect;
}
bool Intersects(NativeEntryExitRect a, NativeEntryExitRect b) {
    return a.Left <= b.Right && a.Right >= b.Left && a.Bottom <= b.Top && a.Top >= b.Bottom;
}
bool Time(const NativeEntryExit& e, unsigned hour) {
    return e.TimeOn > e.TimeOff ? hour >= e.TimeOn || hour < e.TimeOff : hour >= e.TimeOn && hour < e.TimeOff;
}
bool Suppressed(const NativeEntryExitView& v) { return v.Cutscene || v.ControlsDisabled || v.Coop || v.Replay || v.Disabled; }
float DistanceSquared(NativeScriptPosition a, NativeScriptPosition b) {
    const float x = a.X-b.X, y = a.Y-b.Y, z = a.Z-b.Z;
    return x*x + y*y + z*z;
}
bool InArea(const NativeEntryExit& e, NativeScriptPosition p) {
    if (e.EntranceAngle != 0) {
        // CEntryExit::TransformEntrancePoint: positive SetRotateZ, not inverse.
        const float x = p.X-e.Center.X, y = p.Y-e.Center.Y;
        const float c = std::cos(e.EntranceAngle), s = std::sin(e.EntranceAngle);
        p.X = e.Center.X + c*x-s*y; p.Y = e.Center.Y + s*x+c*y;
    }
    return p.X >= e.Entrance.Left && p.X <= e.Entrance.Right && p.Y >= e.Entrance.Bottom && p.Y <= e.Entrance.Top && std::abs(p.Z-e.Center.Z) < 1.0f;
}
}

bool NativeEntryExits::ParseRow(std::string_view row, NativeEntryExit& out, bool randomClosed, std::string& error) {
    try {
        Require(row.size() <= 2048, "ENEX row bound");
        auto text = Clean(std::string(row)); std::replace(text.begin(), text.end(), ',', ' ');
        std::istringstream stream(text);
        std::array<std::string, 18> fields;
        for (auto& field : fields) Require(bool(stream >> field), "ENEX requires 18 fields");
        std::string extra; Require(!(stream >> extra), "ENEX extra field");
        std::array<float, 11> f;
        for (std::size_t i = 0; i < f.size(); ++i) f[i] = Number<float>(fields[i]);
        Require(f[4] >= 0 && f[5] >= 0, "negative ENEX range");
        const auto byte = [&](int i) { const int n = Number<int>(fields[i]); Require(n >= 0 && n <= 255, "ENEX byte bound"); return std::uint8_t(n); };
        NativeEntryExit e;
        e.Area = byte(11); e.SkyColor = byte(14); e.Peds = byte(15); e.TimeOn = byte(16); e.TimeOff = byte(17);
        Require(e.TimeOn <= 24 && e.TimeOff <= 24, "ENEX hour bound");
        const auto flags = Number<uint32>(fields[12]); Require(flags <= 65535, "ENEX flags bound");
        e.AuthoredFlags = e.Flags = std::uint16_t(flags);
        auto name = fields[13];
        if (name.front() == '"') { Require(name.size() >= 2 && name.back() == '"', "ENEX quoted name"); name = name.substr(1, name.size()-2); }
        else name.clear(); // source unquoted name passes nullptr
        Require(name.size() < e.Name.size(), "ENEX name bound");
        std::copy(name.begin(), name.end(), e.Name.begin());
        e.Entrance = {f[0]-f[4]/2, f[1]-f[5]/2, f[0]+f[4]/2, f[1]+f[5]/2};
        e.Center = {(e.Entrance.Left+e.Entrance.Right)/2, (e.Entrance.Bottom+e.Entrance.Top)/2, f[2]+1};
        e.Exit = {f[7], f[8], f[9]+1}; e.EntranceAngle = f[3]*Radians; e.ExitAngle = f[10]; e.AuthoredRangeZ = f[6];
        if ((flags & 0x400) && randomClosed) e.TimeOn = e.TimeOff = 0;
        else if (flags & 0x1000) { e.TimeOn = 0; e.TimeOff = 24; }
        if (!(flags & 0x1000)) e.Flags |= 0x4000; // initial burglary global false
        const float c = std::cos(e.EntranceAngle), s = std::sin(e.EntranceAngle);
        const float x = e.Entrance.Right-e.Center.X, y = e.Entrance.Top-e.Center.Y;
        const float u = e.Entrance.Left-e.Center.X, v = e.Entrance.Bottom-e.Center.Y;
        const float ax = e.Center.X+x*c-y*s, ay = e.Center.Y+x*s+y*c;
        const float bx = e.Center.X+u*c-v*s, by = e.Center.Y+u*s+v*c;
        e.Bounds = {std::min(ax,bx), std::min(ay,by), std::max(ax,bx), std::max(ay,by)};
        Require(Finite(e.Center) && Finite(e.Exit) && std::isfinite(ax) && std::isfinite(ay) && std::isfinite(bx) && std::isfinite(by), "ENEX derived geometry overflow");
        out = std::move(e); error.clear(); return true;
    } catch (const std::exception& e) { error = e.what(); return false; }
}

void NativeEntryExits::BuildRegistry() {
    for (auto& leaf : m_Leaves) leaf.clear();
    const auto link = [&](std::size_t i, std::size_t count) {
        auto& e = m_Entries[i]; if (!(e.Flags & 4)) return;
        for (std::size_t j = 0; j < count; ++j) {
            auto& base = m_Entries[j];
            if ((base.Flags & 4) || Lower(std::string(base.Name.data(), 8)) != Lower(std::string(e.Name.data(), 8))) continue;
            e.Link = int(j); if (base.Link != -1) base.Link = int(i);
            base.TimeOn = 0; base.TimeOff = 24; break;
        }
    };
    for (auto& e : m_Entries) e.Link = -1;
    for (std::size_t i = 0; i < m_Entries.size(); ++i) {
        link(i, i+1); // AddOne before later IPL rows exist
        for (std::size_t leaf = 0; leaf < m_Leaves.size(); ++leaf)
            if (Intersects(Leaf(leaf), m_Entries[i].Bounds)) m_Leaves[leaf].push_back(i);
    }
    for (std::size_t i = 0; i < m_Entries.size(); ++i) if (m_Entries[i].Link == -1) link(i, m_Entries.size());
}
bool NativeEntryExits::InitializeRegistry(std::vector<NativeEntryExit> entries, std::string& error) {
    if (m_Registry || m_Sealed) { error = "ENEX registry already initialized/sealed"; return false; }
    if (entries.size() > 400) { error = "ENEX source pool capacity"; return false; }
    try {
        NativeEntryExits prepared; prepared.m_Entries = std::move(entries); prepared.BuildRegistry();
        m_Entries = std::move(prepared.m_Entries); m_Leaves = std::move(prepared.m_Leaves); m_Registry = true;
        error.clear(); return true;
    } catch (const std::exception& e) { error = e.what(); return false; }
}
bool NativeEntryExits::LoadBeforeWorker(const char* gameDir, std::string& error, uint32 seed) {
    if (m_Registry || m_Sealed) { error = "ENEX loading must precede worker, once"; return false; }
    try {
        Require(gameDir && *gameDir, "ENEX game directory"); OS_SetFilePathOffset(gameDir);
        NativeEntryExits prepared;
        std::vector<std::string> ipls, ides;
        for (const auto* dat : {"data/default.dat", "data/gta.dat"}) {
            std::istringstream stream(ReadText(ResolvePath(gameDir, dat))); std::string line;
            while (std::getline(stream, line)) {
                std::istringstream tokens(Clean(line)); std::string kind, path, extra;
                if (!(tokens >> kind) || (kind != "IPL" && kind != "IDE")) continue;
                Require(bool(tokens >> path) && !(tokens >> extra), "DAT path fields");
                (kind == "IPL" ? ipls : ides).push_back(AssetPath(path));
                Require(ipls.size() <= 1024 && ides.size() <= 1024, "DAT list bound");
            }
        }
        std::mt19937 random(seed); // owned startup 50% burglary choice, not retail RNG parity
        for (const auto& path : ipls) {
            std::istringstream stream(ReadText(ResolvePath(gameDir, path))); std::string line; bool enex = false;
            std::size_t lineNo = 0, row = 0;
            while (std::getline(stream, line)) {
                ++lineNo; line = Clean(line); if (line.empty()) continue;
                if (line == "end") { enex = false; continue; }
                if (line == "enex") { enex = true; continue; }
                if (!enex) continue;
                NativeEntryExit e;
                // Consume randomness only for the source unknown-burglary flag.
                Require(ParseRow(line, e, false, error), path + ":" + std::to_string(lineNo) + ": " + error);
                if (e.AuthoredFlags & 0x400) Require(ParseRow(line, e, (random() & 1) != 0, error), error);
                e.IPL = path; e.Line = lineNo; e.Row = ++row;
                prepared.m_Entries.push_back(std::move(e)); Require(prepared.m_Entries.size() <= 400, "ENEX source pool capacity");
            }
        }
        std::string model, txd;
        for (const auto& path : ides) {
            std::istringstream stream(ReadText(ResolvePath(gameDir, path))); std::string line;
            while (std::getline(stream, line)) {
                line = Clean(line); std::replace(line.begin(), line.end(), ',', ' ');
                std::istringstream tokens(line); int id;
                if (!(tokens >> id) || id != MarkerModel) continue;
                Require(model.empty() && bool(tokens >> model >> txd), "ambiguous/malformed ENEX marker IDE binding");
            }
        }
        Require(model == "diamond_3" && Lower(txd) == "diamond", "missing source ENEX marker model 1559 binding");
        // LoadMarker resets the model instance frame; AddMarker/Render clone its
        // first atomic and change material zero, never property COL scaling.
        const NativeScriptStaticModelOptions options{true, true, std::array<float,4>{1,1,0,1}};
        Require(NativeScriptEntities_LoadStaticModel(gameDir, model, Lower(txd), prepared.m_Model, error, options), error);
        Require(!prepared.m_Entries.empty(), "empty ENEX population"); prepared.BuildRegistry();
        prepared.m_Loaded = prepared.m_Registry = true; prepared.m_IPLCount = ipls.size();
        *this = std::move(prepared); error.clear(); return true;
    } catch (const std::exception& e) { error = e.what(); return false; }
}
std::vector<std::size_t> NativeEntryExits::Candidates(NativeEntryExitRect query) const {
    std::vector<std::size_t> result;
    // Source leaf lists prepend on insertion AND on collection: ascending pool
    // order per leaf, reverse depth-first leaf order, duplicates intentional.
    for (std::size_t i = m_Leaves.size(); i-- > 0;)
        if (Intersects(Leaf(i), query)) result.insert(result.end(), m_Leaves[i].begin(), m_Leaves[i].end());
    return result;
}
std::vector<std::size_t> NativeEntryExits::PointCandidates(float x, float y) const {
    NativeEntryExitRect r{-3000,-3000,3000,3000}; std::size_t index = 0;
    for (int level = 0; level < 4; ++level) {
        const float cx = (r.Left+r.Right)/2, cy = (r.Bottom+r.Top)/2;
        const unsigned sector = (x >= cx ? 1 : 0) | (y < cy ? 2 : 0); index = index*4+sector;
        if (sector & 1) r.Left = cx; else r.Right = cx;
        if (sector & 2) r.Top = cy; else r.Bottom = cy;
    }
    return m_Leaves[index];
}
int NativeEntryExits::FindNearest(float x, float y, float radius, int ignoreArea) const {
    if (!Finite({x,y,radius}) || radius < 0 || !std::isfinite(radius*2) || !std::isfinite(std::abs(x)+radius) || !std::isfinite(std::abs(y)+radius)) return -1;
    float best = radius*2; int nearest = -1;
    for (const auto i : Candidates({x-radius,y-radius,x+radius,y+radius})) {
        const auto& e = m_Entries[i];
        if (m_Entries[e.Link == -1 ? i : std::size_t(e.Link)].Area == ignoreArea) continue;
        const float dx = e.Center.X-x, dy = e.Center.Y-y, distance = std::sqrt(dx*dx+dy*dy);
        if (distance < best) { best = distance; nearest = int(i); }
    }
    return nearest;
}
NativeScriptServiceResult NativeEntryExits::SetFlag(const NativeScriptEntryExitFlagRequest& request) {
    // Source declaration: EntryExitManager::SetEntryExitFlagWithIndex. Missing
    // upstream body verified by read-only retail static RE: 09B4 dispatch
    // 0x47EBF8 -> nearest 0x441FF0 (ignoreArea=-1) -> setter 0x441A40,
    // record flags word +0x30; OR / AND-NOT low16 mask, no linked propagation.
    // Native bounds failure is explicit instead of retail's unchecked -1 slot.
    if (!m_Loaded) return {NativeScriptServiceStatus::Unsupported, "ENEX requires owned IPL registry and prepared marker assets"};
    const int index = FindNearest(request.X, request.Y, request.Radius);
    if (index == -1) return {NativeScriptServiceStatus::Error, "ENEX nearest lookup has no valid entrance"};
    auto& flags = m_Entries[std::size_t(index)].Flags;
    const auto mask = std::uint16_t(request.Mask);
    flags = request.State ? std::uint16_t(flags | mask) : std::uint16_t(flags & ~mask);
    ++m_Revision; return {NativeScriptServiceStatus::Ready, {}};
}
NativeScriptServiceResult NativeEntryExits::SetEnabledByName(const NativeScriptEntryExitSwitchRequest& request) {
    if (!m_Loaded) return {NativeScriptServiceStatus::Unsupported, "ENEX requires owned IPL registry and prepared marker assets"};
    const auto name = Lower(std::string(request.Name.data(), request.Name.size()));
    const auto found = std::ranges::find_if(m_Entries, [&](const auto& entry) {
        return Lower(std::string(entry.Name.data(), entry.Name.size())) == name;
    });
    // Source SetEnabledByName is void: names absent from the current pool are a
    // completed no-op. This also preserves the explicit text-only registry
    // boundary for dynamic DFF 2DFX entry-exits.
    if (found == m_Entries.end()) return {NativeScriptServiceStatus::Ready, "ENEX source name is absent from the owned pool"};
    if (request.Enabled) found->Flags |= 0x4000;
    else found->Flags &= std::uint16_t(~0x4000u);
    ++m_Revision;
    return {NativeScriptServiceStatus::Ready, {}};
}
NativeEntryExitActivation NativeEntryExits::Activation(const NativeEntryExitView& view) const {
    if (!m_Registry || Suppressed(view) || view.TransitionState || !view.CanStartMission || view.Hour > 23 || !Finite(view.Player)) return {};
    for (auto i : PointCandidates(view.Player.X, view.Player.Y)) {
        const auto& e = m_Entries[i];
        if (!(e.Flags & 0x4000) || !Time(e, view.Hour) || !InArea(e, view.Player)) continue;
        if (view.Vehicle == NativeEntryExitVehicle::OnFoot) { if (e.Flags & 0x800) continue; }
        else if (view.BigVehicle || (view.Vehicle == NativeEntryExitVehicle::Automobile ? !(e.Flags & 0x20) :
            view.Vehicle == NativeEntryExitVehicle::Bike ? !(e.Flags & 0x40) : true)) continue;
        const auto destination = e.Link == -1 ? i : std::size_t(e.Link); const auto& exit = m_Entries[destination];
        // Stop BEFORE TransitionStarted's backlink/task/camera side effects.
        return {NativeEntryExitActivationStatus::TransitionRequired, {i, destination, e.Center, exit.Exit, exit.ExitAngle, exit.Area, e.Flags}};
    }
    return {};
}
bool NativeEntryExits::Tick(const NativeEntryExitView& view, uint32 gameMs, std::string& error) {
    try {
        Require(m_Loaded, "ENEX Tick needs prepared assets");
        Require(Finite(view.Camera) && Finite(view.Forward) && Finite(view.Player) && view.Hour < 24 && bool(view.SphereVisible), "invalid ENEX frame/camera");
        const auto elapsed = m_HasTime ? gameMs-m_LastTime : gameMs;
        Require(elapsed <= 0x7fffffff, "ENEX backwards/ambiguous frame time");
        const float angle = m_DiamondAngle + float(elapsed)*0.25f; // source timestep = ms/20, angle += timestep*5
        WorldShotScene actors; std::vector<std::size_t> visible;
        auto z = m_MarkerZ, sizes = m_MarkerSize; std::array<bool, 400> shown{};
        if (!Suppressed(view) && !view.TransitionState) {
            const float cx = view.Camera.X+view.Forward.X*30, cy = view.Camera.Y+view.Forward.Y*30;
            for (auto i : Candidates({cx-30,cy-30,cx+30,cy+30})) {
                const auto& e = m_Entries[i];
                if (shown[i] || !(e.Flags & 0x4000) || !(e.Link != -1 ? e.Area == view.Area : e.Area != view.Area) ||
                    !Time(e, view.Hour) || DistanceSquared(e.Center, view.Camera) >= 1600 || !view.SphereVisible(e.Center, 1)) continue;
                auto p = e.Center; p.Z += 1;
                if (DistanceSquared(p, view.Camera) < 1.6f*1.6f) continue; // PlaceMarkerCone
                const float dx = p.X-view.Player.X, dy = p.Y-view.Player.Y;
                const float standard = 2*(1-0.3f*(25-std::clamp(std::sqrt(dx*dx+dy*dy),5.0f,25.0f))/20);
                const bool sameFrame = m_HasTime && gameMs == m_LastTime && m_WasVisible[i];
                const float scale = sameFrame ? sizes[i] : m_WasVisible[i] ? standard : 2.0f;
                if (sameFrame) p.Z = z[i];
                else p.Z += m_WasVisible[i] ? std::sin(angle*Radians)*0.3f : std::sin(angle*Radians*0.3f);
                z[i] = p.Z; sizes[i] = scale; shown[i] = true; visible.push_back(i);
                for (auto mesh : m_Model.meshes) {
                    for (std::size_t v = 0; v < mesh.pos.size(); v += 3) {
                        mesh.pos[v] = mesh.pos[v]*scale+p.X; mesh.pos[v+1] = mesh.pos[v+1]*scale+p.Y; mesh.pos[v+2] = mesh.pos[v+2]*scale+p.Z;
                    }
                    // Material-zero yellow is prepared at startup; preserve the
                    // other authored materials. Parent uses a full-bright pass.
                    for (std::size_t t = 0; t < mesh.surfaces.size(); ++t) {
                        mesh.surfaces[t].ambient = 1; mesh.surfaces[t].diffuse = 0;
                    }
                    actors.stats.triangles += mesh.tris; actors.meshes.push_back(std::move(mesh));
                }
            }
        }
        std::fill(std::begin(actors.bboxMin), std::end(actors.bboxMin), std::numeric_limits<float>::max());
        std::fill(std::begin(actors.bboxMax), std::end(actors.bboxMax), std::numeric_limits<float>::lowest());
        for (const auto& mesh : actors.meshes) for (std::size_t v = 0; v < mesh.pos.size(); ++v) {
            actors.bboxMin[v%3] = std::min(actors.bboxMin[v%3], mesh.pos[v]); actors.bboxMax[v%3] = std::max(actors.bboxMax[v%3], mesh.pos[v]);
        }
        if (actors.meshes.empty()) { std::fill(std::begin(actors.bboxMin),std::end(actors.bboxMin),0); std::fill(std::begin(actors.bboxMax),std::end(actors.bboxMax),0); }
        m_Actors = std::move(actors); m_Visible = std::move(visible); m_WasVisible = shown; m_MarkerZ = z; m_MarkerSize = sizes;
        m_LastTime = gameMs; m_HasTime = true; m_DiamondAngle = angle; error.clear(); return true;
    } catch (const std::exception& e) { error = e.what(); return false; }
}
