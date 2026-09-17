#include "app/platform/linux/NativeCutscene.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <cmath>
#include <limits>

using int8 = int8_t;
using int16 = int16_t;
using int32 = int32_t;
using int64 = int64_t;
using uint8 = uint8_t;
using uint16 = uint16_t;
using uint32 = uint32_t;
using uint64 = uint64_t;

#ifndef __stdcall
#define __stdcall
#endif
#include "oswrapper/oswrapper.h"

namespace {
struct ImgEntry {
    std::uint32_t Sector = 0;
    std::uint32_t Sectors = 0;
    std::array<char, 24> Name{};
};

std::uint32_t U32(const std::uint8_t* data) {
    return std::uint32_t(data[0]) | std::uint32_t(data[1]) << 8 |
        std::uint32_t(data[2]) << 16 | std::uint32_t(data[3]) << 24;
}

std::uint64_t Hash(const std::vector<std::uint8_t>& bytes) {
    std::uint64_t hash = 1469598103934665603ull;
    for (const auto byte : bytes) hash = (hash ^ byte) * 1099511628211ull;
    return hash;
}

bool NameValid(const std::array<char, 8>& name) {
    const auto end = std::ranges::find(name, '\0');
    if (end == name.begin()) return false;
    return std::ranges::all_of(name.begin(), end, [](char value) {
        const auto c = static_cast<unsigned char>(value);
        return c == '_' || (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z');
    });
}

std::string LowerName(const std::array<char, 8>& name, const char* suffix) {
    std::string result;
    for (const auto value : name) {
        if (!value) break;
        result.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(value))));
    }
    result += suffix;
    return result;
}

bool ReadEntry(void* file, const ImgEntry& entry, std::vector<std::uint8_t>& bytes) {
    constexpr std::uint64_t sectorSize = 2048;
    const auto offset = std::uint64_t(entry.Sector) * sectorSize;
    const auto size = std::uint64_t(entry.Sectors) * sectorSize;
    if (!entry.Sectors || offset > std::uint64_t(std::numeric_limits<int32>::max()) ||
        size > std::uint64_t(std::numeric_limits<int32>::max())) return false;
    bytes.resize(std::size_t(size));
    OS_FileSetPosition(file, static_cast<int32>(offset));
    return OS_FileGetPosition(file) == static_cast<int32>(offset) &&
        OS_FileRead(file, bytes.data(), static_cast<int32>(size)) == 0;
}

float CameraDuration(const std::vector<std::uint8_t>& bytes) {
    float maximum = 0.0f;
    std::size_t at = 0;
    while (at < bytes.size() && bytes[at]) {
        const auto line = at;
        while (at < bytes.size() && bytes[at] && bytes[at] != '\r' && bytes[at] != '\n') ++at;
        const auto comma = std::find(bytes.begin() + line, bytes.begin() + at, std::uint8_t(','));
        const auto dot = std::find(bytes.begin() + line, comma, std::uint8_t('.'));
        if (comma != bytes.begin() + at && dot != comma) {
            double value = 0.0;
            for (auto it = bytes.begin() + line; it != comma && *it != '.'; ++it) {
                if (*it < '0' || *it > '9') { value = -1.0; break; }
                value = value * 10.0 + double(*it - '0');
            }
            if (value >= 0.0) {
                double place = 0.1;
                for (auto it = dot + 1; it != comma && *it != 'f'; ++it) {
                    if (*it < '0' || *it > '9') { value = -1.0; break; }
                    value += double(*it - '0') * place;
                    place *= 0.1;
                }
                if (value >= 0.0 && std::isfinite(value)) maximum = std::max(maximum, static_cast<float>(value));
            }
        }
        while (at < bytes.size() && (bytes[at] == '\r' || bytes[at] == '\n')) ++at;
    }
    return maximum;
}
}

bool NativeCutscene::Initialize(const char* gameDir, std::string& error) {
    if (!m_GameDir.empty() || !gameDir || !*gameDir) {
        error = "cutscene owner initialization rejected";
        return false;
    }
    m_GameDir = gameDir;
    error.clear();
    return true;
}

NativeScriptServiceResult NativeCutscene::Load(const std::array<char, 8>& name) {
    if (m_GameDir.empty() || !NameValid(name) || m_Revision == std::numeric_limits<std::uint64_t>::max()) {
        return {NativeScriptServiceStatus::Error, "invalid cutscene load"};
    }
    OS_SetFilePathOffset(m_GameDir.c_str());
    void* file = nullptr;
    if (OS_FileOpen(FILE_DATA_AREA_DEFAULT, &file, "anim/cuts.img", FILE_ACCESS_READ) != 0 || !file) {
        return {NativeScriptServiceStatus::Error, "cutscene IMG open failed"};
    }
    std::array<std::uint8_t, 8> header{};
    bool ok = OS_FileRead(file, header.data(), header.size()) == 0 && !std::memcmp(header.data(), "VER2", 4);
    const auto count = ok ? U32(header.data() + 4) : 0;
    ok = ok && count && count <= 4096;
    std::vector<ImgEntry> entries;
    if (ok) {
        entries.resize(count);
        for (auto& entry : entries) {
            std::array<std::uint8_t, 32> raw{};
            if (OS_FileRead(file, raw.data(), raw.size()) != 0) { ok = false; break; }
            entry.Sector = U32(raw.data());
            entry.Sectors = U32(raw.data() + 4);
            std::copy_n(reinterpret_cast<const char*>(raw.data() + 8), entry.Name.size(), entry.Name.begin());
        }
    }
    const std::array<std::string, 3> wanted{LowerName(name, ".ifp"), LowerName(name, ".cut"), LowerName(name, ".dat")};
    std::array<std::shared_ptr<const std::vector<std::uint8_t>>, 3> payloads;
    std::array<std::uint64_t, 3> hashes{};
    for (std::size_t i = 0; ok && i < wanted.size(); ++i) {
        const auto found = std::ranges::find_if(entries, [&](const auto& entry) {
            const auto end = std::ranges::find(entry.Name, '\0');
            return std::string(entry.Name.begin(), end) == wanted[i];
        });
        if (found == entries.end()) { ok = false; break; }
        auto bytes = std::make_shared<std::vector<std::uint8_t>>();
        ok = ReadEntry(file, *found, *bytes);
        if (ok) {
            hashes[i] = Hash(*bytes);
            payloads[i] = std::move(bytes);
        }
    }
    OS_FileClose(file);
    if (!ok) return {NativeScriptServiceStatus::Error, "cutscene archive identity rejected"};
    auto payload = std::make_shared<NativeCutscenePayload>();
    payload->Name = name;
    payload->Ifp = std::move(payloads[0]);
    payload->Cut = std::move(payloads[1]);
    payload->Dat = std::move(payloads[2]);
    payload->Hashes = hashes;
    payload->DurationSeconds = CameraDuration(*payload->Dat);
    if (!(payload->DurationSeconds > 0.0f) || !std::isfinite(payload->DurationSeconds)) {
        return {NativeScriptServiceStatus::Error, "cutscene camera duration rejected"};
    }
    payload->Revision = m_Revision + 1;
    m_DurationMs = static_cast<std::uint32_t>(std::ceil(payload->DurationSeconds * 1000.0f));
    m_Current = std::move(payload);
    m_Started = false;
    ++m_Revision;
    return {NativeScriptServiceStatus::Ready, {}};
}

NativeScriptServiceResult NativeCutscene::Start() {
    if (!m_Current || m_Started || m_Revision == std::numeric_limits<std::uint64_t>::max()) {
        return {NativeScriptServiceStatus::Error, "invalid cutscene start"};
    }
    m_Started = true;
    m_StartTimeMs = m_LastTimeMs;
    ++m_Revision;
    return {NativeScriptServiceStatus::Ready, {}};
}

NativeScriptServiceResult NativeCutscene::Unload() {
    if (m_GameDir.empty() || m_Revision == std::numeric_limits<std::uint64_t>::max()) {
        return {NativeScriptServiceStatus::Error, "invalid cutscene unload"};
    }
    m_Current.reset();
    m_Started = false;
    m_DurationMs = 0;
    ++m_Revision;
    return {NativeScriptServiceStatus::Ready, {}};
}

void NativeCutscene::AdvanceTime(std::uint32_t nowMs) {
    m_LastTimeMs = nowMs;
}
