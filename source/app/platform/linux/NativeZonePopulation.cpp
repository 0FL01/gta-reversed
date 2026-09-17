#include "app/platform/linux/NativeZonePopulation.h"
#include "app/platform/linux/ZoneInfo.h"

#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace {
std::string Lower(std::string value) {
    for (auto& c : value) c = char(std::tolower(static_cast<unsigned char>(c)));
    return value;
}
std::string Name(const std::array<char,8>& value) {
    const auto end=std::find(value.begin(),value.end(),'\0');
    return Lower(std::string(value.begin(),end));
}
}

bool NativeZonePopulation::LoadBeforeWorker(const char* gameDir, std::string& error) try {
    if (m_Loaded) throw std::runtime_error("zone population owner already loaded");
    ZoneData data; char detail[256]{};
    if (!ZoneInfo_Load(gameDir,data,detail,sizeof(detail))) throw std::runtime_error(detail);
    std::vector<NativeZonePopulationEntry> entries;
    entries.reserve(data.zones.size());
    for (const auto& zone : data.zones) {
        if (zone.name.empty() || zone.name.size()>8) throw std::runtime_error("zone population label bound");
        entries.push_back({zone.name,5,15,0,{},false});
    }
    if (entries.empty()) throw std::runtime_error("empty zone population registry");
    m_Entries=std::move(entries); m_Loaded=true; error.clear(); return true;
} catch (const std::exception& exception) { error=exception.what(); return false; }

NativeZonePopulationEntry* NativeZonePopulation::Find(const std::array<char,8>& name) {
    const auto target=Name(name);
    const auto found=std::ranges::find_if(m_Entries,[&](const auto& entry) { return Lower(entry.Label)==target; });
    return found==m_Entries.end() ? nullptr : &*found;
}
NativeScriptServiceResult NativeZonePopulation::Reset() {
    if (!m_Loaded) return {NativeScriptServiceStatus::Unsupported,"zone population registry unavailable"};
    for (auto& entry:m_Entries) { entry.PopulationType=5; entry.Races=15; entry.DealerStrength=0; entry.GangStrength.fill(0); entry.NoCops=false; }
    ++m_Revision; return {NativeScriptServiceStatus::Ready,{}};
}
NativeScriptServiceResult NativeZonePopulation::SetType(const NativeScriptZonePopulationRequest& request) {
    if (!m_Loaded) return {NativeScriptServiceStatus::Unsupported,"zone population registry unavailable"};
    if (request.Value<0 || request.Value>=20) return {NativeScriptServiceStatus::Error,"zone population type outside source enum"};
    if (auto* entry=Find(request.Name)) { entry->PopulationType=std::uint8_t(request.Value); ++m_Revision; }
    return {NativeScriptServiceStatus::Ready,{}};
}
NativeScriptServiceResult NativeZonePopulation::SetRaces(const NativeScriptZonePopulationRequest& request) {
    if (!m_Loaded) return {NativeScriptServiceStatus::Unsupported,"zone population registry unavailable"};
    if (request.Value<0 || request.Value>15) return {NativeScriptServiceStatus::Error,"zone population race mask outside source nibble"};
    if (auto* entry=Find(request.Name)) { entry->Races=std::uint8_t(request.Value); ++m_Revision; }
    return {NativeScriptServiceStatus::Ready,{}};
}
NativeScriptServiceResult NativeZonePopulation::SetDealer(const NativeScriptZonePopulationRequest& request) {
    if (!m_Loaded) return {NativeScriptServiceStatus::Unsupported,"zone population registry unavailable"};
    if (request.Value<0 || request.Value>255) return {NativeScriptServiceStatus::Error,"zone dealer strength outside source byte"};
    if (auto* entry=Find(request.Name)) { entry->DealerStrength=std::uint8_t(request.Value); ++m_Revision; }
    return {NativeScriptServiceStatus::Ready,{}};
}
NativeScriptServiceResult NativeZonePopulation::SetGang(const NativeScriptZoneGangRequest& request) {
    if (!m_Loaded) return {NativeScriptServiceStatus::Unsupported,"zone population registry unavailable"};
    if (request.Gang<0 || request.Gang>=10 || request.Strength<0 || request.Strength>255)
        return {NativeScriptServiceStatus::Error,"zone gang/strength outside source bounds"};
    if (auto* entry=Find(request.Name)) { entry->GangStrength[std::size_t(request.Gang)]=std::uint8_t(request.Strength); ++m_Revision; }
    return {NativeScriptServiceStatus::Ready,{}};
}
NativeScriptServiceResult NativeZonePopulation::SetNoCops(const NativeScriptZonePopulationRequest& request) {
    if (!m_Loaded) return {NativeScriptServiceStatus::Unsupported,"zone population registry unavailable"};
    if (request.Value!=0 && request.Value!=1) return {NativeScriptServiceStatus::Error,"zone no-cops state outside source boolean"};
    if (auto* entry=Find(request.Name)) { entry->NoCops=request.Value!=0; ++m_Revision; }
    return {NativeScriptServiceStatus::Ready,{}};
}
