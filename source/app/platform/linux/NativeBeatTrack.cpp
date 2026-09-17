#include "app/platform/linux/NativeBeatTrack.h"

#include <cstring>
#include <limits>
#include <vector>

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
constexpr std::array<std::int16_t, 14> BeatTrackLookup{
    180, 175, 178, 179, 178, 175, 175, 175, 175, 176, 184, 183, 182, 181
};
constexpr std::size_t TrackInfoSize = 0x1F84;

std::uint32_t Word(const std::uint8_t* p) {
    return std::uint32_t(p[0]) | std::uint32_t(p[1]) << 8 |
        std::uint32_t(p[2]) << 16 | std::uint32_t(p[3]) << 24;
}

bool ReadAll(const char* path, std::vector<std::uint8_t>& bytes, std::string& error) {
    void* file = nullptr;
    if (OS_FileOpen(FILE_DATA_AREA_DEFAULT, &file, path, FILE_ACCESS_READ) != 0 || !file) {
        error = std::string("cannot open ") + path;
        return false;
    }
    const auto size = OS_FileSize(file);
    if (size <= 0 || size > 64 * 1024 * 1024) {
        OS_FileClose(file);
        error = std::string(path) + " size invalid";
        return false;
    }
    bytes.resize(std::size_t(size));
    const bool ok = OS_FileRead(file, bytes.data(), int32(bytes.size())) == 0;
    OS_FileClose(file);
    if (!ok) error = std::string(path) + " read failed";
    return ok;
}

std::uint64_t Hash(const std::uint8_t* bytes, std::size_t size) {
    std::uint64_t hash = 1469598103934665603ull;
    for (std::size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
    return hash;
}
}

bool NativeBeatTrack::LoadBeforeWorker(const char* gameDir, std::string& error) {
    OS_SetFilePathOffset(gameDir);
    std::vector<std::uint8_t> packs, lookups;
    if (!ReadAll("audio/CONFIG/StrmPaks.dat", packs, error) ||
        !ReadAll("audio/CONFIG/TrakLkup.dat", lookups, error)) return false;
    if (packs.size() % 16 || lookups.size() != m_Lookups.size() * 12) {
        error = "beat-track lookup table shape invalid";
        return false;
    }
    std::int32_t beatsPack = -1;
    for (std::size_t i = 0; i < packs.size() / 16; ++i) {
        const auto* name = reinterpret_cast<const char*>(packs.data() + i * 16);
        if (strnlen(name, 16) == 5 && std::memcmp(name, "BEATS", 5) == 0) beatsPack = std::int32_t(i);
    }
    if (beatsPack < 0) {
        error = "BEATS stream pack is absent";
        return false;
    }
    std::array<Lookup, 1922> parsed{};
    for (std::size_t i = 0; i < parsed.size(); ++i) {
        const auto* row = lookups.data() + i * 12;
        parsed[i] = {row[0], Word(row + 4), Word(row + 8)};
    }
    for (const auto track : BeatTrackLookup) {
        if (track < 0 || std::size_t(track) >= parsed.size() || parsed[track].Pack != beatsPack ||
            parsed[track].Size < TrackInfoSize) {
            error = "source beat-track identity is invalid";
            return false;
        }
    }
    m_Lookups = parsed;
    m_GameDir = gameDir;
    m_State = {};
    m_Ready = true;
    error.clear();
    return true;
}

NativeScriptServiceResult NativeBeatTrack::Preload(std::int32_t scriptTrack) {
    if (!m_Ready || scriptTrack < 0 || std::size_t(scriptTrack) >= BeatTrackLookup.size())
        return {NativeScriptServiceStatus::Error, "beat-track request is invalid"};
    const auto sourceTrack = BeatTrackLookup[scriptTrack];
    const auto& lookup = m_Lookups[sourceTrack];
    OS_SetFilePathOffset(m_GameDir.c_str());
    void* file = nullptr;
    if (OS_FileOpen(FILE_DATA_AREA_DEFAULT, &file, "audio/streams/BEATS", FILE_ACCESS_READ) != 0 || !file)
        return {NativeScriptServiceStatus::Error, "cannot open BEATS stream"};
    const auto fileSize = OS_FileSize(file);
    if (lookup.Offset > std::uint32_t(std::numeric_limits<std::int32_t>::max()) ||
        std::uint64_t(lookup.Offset) + lookup.Size > std::uint64_t(fileSize)) {
        OS_FileClose(file);
        return {NativeScriptServiceStatus::Error, "beat-track stream extent is invalid"};
    }
    std::array<std::uint8_t, TrackInfoSize> info{};
    OS_FileSetPosition(file, std::int32_t(lookup.Offset));
    const bool ok = OS_FileRead(file, info.data(), int32(info.size())) == 0;
    OS_FileClose(file);
    if (!ok) return {NativeScriptServiceStatus::Error, "beat-track metadata read failed"};
    m_State = {scriptTrack, sourceTrack, lookup.Offset, lookup.Size, Hash(info.data(), info.size()), 2};
    return {NativeScriptServiceStatus::Ready, {}};
}
