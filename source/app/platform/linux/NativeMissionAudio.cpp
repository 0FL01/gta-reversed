#include "NativeMissionAudio.h"

#include <algorithm>
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
constexpr std::int32_t FirstMissionAudioId = 42000;
constexpr std::size_t TrackInfoSize = 0x1F84;
constexpr std::uint8_t XorTable[16] = {
    0xEA, 0x3A, 0xC4, 0xA1, 0x9A, 0xA8, 0x14, 0xF3,
    0x48, 0xB0, 0xD7, 0x23, 0x9D, 0xE8, 0xFF, 0xF1,
};

std::uint32_t Word(const std::uint8_t* p) {
    return std::uint32_t(p[0]) | std::uint32_t(p[1]) << 8 |
        std::uint32_t(p[2]) << 16 | std::uint32_t(p[3]) << 24;
}

std::uint64_t LongWord(const std::uint8_t* p) {
    std::uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) value |= std::uint64_t(p[i]) << (i * 8);
    return value;
}

bool OggDuration(std::vector<std::uint8_t>& ogg, std::uint32_t absoluteOffset,
    std::uint32_t& durationMs) {
    for (std::size_t i = 0; i < ogg.size(); ++i) ogg[i] ^= XorTable[(absoluteOffset + i) & 0xF];
    std::size_t pos = 0;
    std::uint32_t rate = 0;
    std::uint64_t lastGranule = 0;
    while (pos + 27 <= ogg.size()) {
        if (std::memcmp(ogg.data() + pos, "OggS", 4) != 0) return false;
        const auto segments = ogg[pos + 26];
        if (pos + 27 + segments > ogg.size()) return false;
        std::size_t body = 0;
        for (unsigned i = 0; i < segments; ++i) body += ogg[pos + 27 + i];
        const auto data = pos + 27 + segments;
        if (data + body > ogg.size()) return false;
        if (!rate) {
            if (body < 16 || ogg[data] != 1 || std::memcmp(ogg.data() + data + 1, "vorbis", 6) != 0)
                return false;
            rate = Word(ogg.data() + data + 12);
            if (!rate) return false;
        }
        const auto granule = LongWord(ogg.data() + pos + 6);
        if (granule != std::numeric_limits<std::uint64_t>::max()) lastGranule = std::max(lastGranule, granule);
        pos = data + body;
    }
    if (pos != ogg.size() || !lastGranule || lastGranule > std::uint64_t(rate) * 3600)
        return false;
    const auto milliseconds = (lastGranule * 1000 + rate - 1) / rate;
    if (!milliseconds || milliseconds > std::numeric_limits<std::uint32_t>::max()) return false;
    durationMs = std::uint32_t(milliseconds);
    return true;
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
    for (std::size_t i = 0; i < size; ++i) { hash ^= bytes[i]; hash *= 1099511628211ull; }
    return hash;
}
}

bool NativeMissionAudio::LoadBeforeWorker(const char* gameDir, std::string& error) {
    OS_SetFilePathOffset(gameDir);
    std::vector<std::uint8_t> packs, lookups;
    if (!ReadAll("audio/CONFIG/StrmPaks.dat", packs, error) ||
        !ReadAll("audio/CONFIG/TrakLkup.dat", lookups, error)) return false;
    if (packs.empty() || packs.size() % 16 || packs.size() / 16 > m_Packs.size() ||
        lookups.size() != m_Lookups.size() * 12) {
        error = "mission-audio stream tables have invalid shape";
        return false;
    }
    m_PackCount = packs.size() / 16;
    for (std::size_t i = 0; i < m_PackCount; ++i)
        std::memcpy(m_Packs[i].data(), packs.data() + i * 16, 16);
    for (std::size_t i = 0; i < m_Lookups.size(); ++i) {
        const auto* row = lookups.data() + i * 12;
        if (row[0] >= m_PackCount) { error = "mission-audio lookup pack is invalid"; return false; }
        m_Lookups[i] = {row[0], Word(row + 4), Word(row + 8)};
    }
    m_GameDir = gameDir;
    m_Slots = {};
    for (auto& slot : m_Slots) slot.AudioId = slot.Pack = -1;
    m_Ready = true;
    error.clear();
    return true;
}

NativeScriptServiceResult NativeMissionAudio::Request(std::int32_t slot, std::int32_t event) {
    // Script payloads use both CAE script-event IDs (42000 + TrackLkup row)
    // and direct source TrackLkup sample IDs (for example DUAL uses 1829).
    const auto index = event >= FirstMissionAudioId ?
        std::int64_t(event) - FirstMissionAudioId : std::int64_t(event);
    if (!m_Ready || slot < 1 || std::size_t(slot) > m_Slots.size() || index < 0 ||
        std::size_t(index) >= m_Lookups.size())
        return {NativeScriptServiceStatus::Error, "mission-audio request is invalid slot=" +
            std::to_string(slot) + " event=" + std::to_string(event)};
    const auto& lookup = m_Lookups[std::size_t(index)];
    if (lookup.Size < TrackInfoSize || lookup.Offset > std::uint32_t(std::numeric_limits<std::int32_t>::max()))
        return {NativeScriptServiceStatus::Error, "mission-audio stream extent is invalid"};
    const auto length = strnlen(m_Packs[lookup.Pack].data(), 16);
    if (!length || length == 16) return {NativeScriptServiceStatus::Error, "mission-audio pack name is invalid"};
    const std::string path = std::string("audio/streams/") +
        std::string(m_Packs[lookup.Pack].data(), length);
    OS_SetFilePathOffset(m_GameDir.c_str());
    void* file = nullptr;
    if (OS_FileOpen(FILE_DATA_AREA_DEFAULT, &file, path.c_str(), FILE_ACCESS_READ) != 0 || !file)
        return {NativeScriptServiceStatus::Error, "mission-audio stream is unavailable"};
    const auto fileSize = OS_FileSize(file);
    if (std::uint64_t(lookup.Offset) + TrackInfoSize + lookup.Size > std::uint64_t(fileSize)) {
        OS_FileClose(file);
        return {NativeScriptServiceStatus::Error, "mission-audio stream range is invalid"};
    }
    std::array<std::uint8_t, TrackInfoSize> info{};
    OS_FileSetPosition(file, std::int32_t(lookup.Offset));
    const bool ok = OS_FileRead(file, info.data(), int32(info.size())) == 0;
    std::vector<std::uint8_t> ogg(lookup.Size);
    OS_FileSetPosition(file, std::int32_t(lookup.Offset + TrackInfoSize));
    const bool audioOk = OS_FileRead(file, ogg.data(), int32(ogg.size())) == 0;
    OS_FileClose(file);
    std::uint32_t durationMs = 0;
    if (!ok || !audioOk)
        return {NativeScriptServiceStatus::Error, "mission-audio stream decode metadata is invalid"};
    const bool decodedDuration = OggDuration(ogg, lookup.Offset + TrackInfoSize, durationMs);
    if (!decodedDuration && event >= FirstMissionAudioId)
        return {NativeScriptServiceStatus::Error, "mission-audio stream decode metadata is invalid"};
    m_Slots[std::size_t(slot - 1)] = {event, lookup.Pack, lookup.Offset, lookup.Size,
        durationMs, 0, Hash(info.data(), info.size()),
        std::make_shared<const std::vector<std::uint8_t>>(std::move(ogg)), true, false, false};
    return {NativeScriptServiceStatus::Ready, {}};
}

NativeScriptServiceResult NativeMissionAudio::Clear(std::int32_t slot) {
    if (slot < 1 || std::size_t(slot) > m_Slots.size())
        return {NativeScriptServiceStatus::Error, "mission-audio clear slot is invalid"};
    m_Slots[std::size_t(slot - 1)] = {};
    m_Slots[std::size_t(slot - 1)].AudioId = m_Slots[std::size_t(slot - 1)].Pack = -1;
    return {NativeScriptServiceStatus::Ready, {}};
}

NativeScriptServiceResult NativeMissionAudio::Play(std::int32_t slot, std::uint32_t nowMs) {
    if (!Loaded(slot)) return {NativeScriptServiceStatus::Error, "mission-audio slot is not loaded"};
    auto& state = m_Slots[std::size_t(slot - 1)];
    if (!state.DurationMs) return {NativeScriptServiceStatus::Unsupported,
        "direct stream sample playback clock is not owned"};
    state.StartedMs = nowMs;
    state.PlaybackRequested = true;
    state.Finished = false;
    return {NativeScriptServiceStatus::Ready, {}};
}

void NativeMissionAudio::Advance(std::uint32_t nowMs) noexcept {
    for (auto& slot : m_Slots) {
        if (slot.PlaybackRequested && !slot.Finished && nowMs - slot.StartedMs >= slot.DurationMs)
            slot.Finished = true;
    }
}

bool NativeMissionAudio::Finished(std::int32_t slot) const noexcept {
    return slot >= 1 && std::size_t(slot) <= m_Slots.size() && m_Slots[std::size_t(slot - 1)].Finished;
}

bool NativeMissionAudio::Loaded(std::int32_t slot) const noexcept {
    return slot >= 1 && std::size_t(slot) <= m_Slots.size() && m_Slots[std::size_t(slot - 1)].Loaded;
}

const NativeMissionAudioSlot* NativeMissionAudio::Slot(std::int32_t slot) const noexcept {
    return slot >= 1 && std::size_t(slot) <= m_Slots.size() ? &m_Slots[std::size_t(slot - 1)] : nullptr;
}
