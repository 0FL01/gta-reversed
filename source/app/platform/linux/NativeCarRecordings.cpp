#include "app/platform/linux/NativeCarRecordings.h"

#include <charconv>
#include <cmath>
#include <cstring>
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
std::uint32_t Word(const std::uint8_t* p) {
    return std::uint32_t(p[0]) | std::uint32_t(p[1]) << 8 | std::uint32_t(p[2]) << 16 | std::uint32_t(p[3]) << 24;
}
std::int16_t Short(const std::uint8_t* p) {
    return static_cast<std::int16_t>(std::uint16_t(p[0]) | std::uint16_t(p[1]) << 8);
}
float Float(const std::uint8_t* p) {
    const auto bits = Word(p);
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}
std::array<float, 3> Cross(const std::array<float, 3>& a, const std::array<float, 3>& b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
std::array<float, 3> Lerp(const std::array<float, 3>& a, const std::array<float, 3>& b, float t) {
    return {a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t};
}
bool ReadAll(const char* path, std::vector<std::uint8_t>& bytes, std::string& error) {
    void* file = nullptr;
    if (OS_FileOpen(FILE_DATA_AREA_DEFAULT,&file,path,FILE_ACCESS_READ) != 0 || !file) {
        error = std::string("cannot open ") + path; return false;
    }
    const auto size = OS_FileSize(file);
    if (size <= 8 || size > 16 * 1024 * 1024) { OS_FileClose(file); error = "car recording archive size invalid"; return false; }
    bytes.resize(std::size_t(size));
    const bool ok = OS_FileRead(file, bytes.data(), int32(bytes.size())) == 0;
    OS_FileClose(file);
    if (!ok) error = "car recording archive read failed";
    return ok;
}
}

bool NativeCarRecordings::LoadBeforeWorker(const char* gameDir, std::string& error) {
    OS_SetFilePathOffset(gameDir);
    std::vector<std::uint8_t> bytes;
    if (!ReadAll("data/Paths/carrec.img", bytes, error)) return false;
    if (std::memcmp(bytes.data(), "VER2", 4) != 0) { error = "car recording archive version"; return false; }
    const auto count = Word(bytes.data() + 4);
    if (!count || count > 1024 || 8ull + std::uint64_t(count) * 32 > bytes.size()) {
        error = "car recording directory bounds"; return false;
    }
    std::map<std::int32_t, NativeCarRecordingEntry> entries;
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto* row = bytes.data() + 8 + i * 32;
        const auto sector = Word(row), sectors = Word(row + 4);
        const std::string_view name(reinterpret_cast<const char*>(row + 8), strnlen(reinterpret_cast<const char*>(row + 8), 24));
        if (name.size() != 13 || name.substr(0, 6) != "carrec" || name.substr(9) != ".rrr" || !sectors) {
            error = "car recording directory identity"; return false;
        }
        std::int32_t id = 0;
        const auto parsed = std::from_chars(name.data() + 6, name.data() + 9, id);
        const std::uint64_t offset = std::uint64_t(sector) * 2048, size = std::uint64_t(sectors) * 2048;
        if (parsed.ec != std::errc{} || parsed.ptr != name.data() + 9 || id <= 0 ||
            offset + size > bytes.size() || !entries.emplace(id, NativeCarRecordingEntry{id,std::uint32_t(offset),std::uint32_t(size)}).second) {
            error = "car recording entry invalid"; return false;
        }
    }
    auto archive = std::make_shared<const std::vector<std::uint8_t>>(std::move(bytes));
    m_Entries = std::move(entries);
    m_Loaded.clear();
    m_Frames.clear();
    m_Playbacks = {};
    m_LastTimeMs = 0;
    m_Archive = std::move(archive);
    error.clear();
    return true;
}

NativeScriptServiceResult NativeCarRecordings::Request(std::int32_t id) {
    if (!m_Archive || !m_Entries.contains(id)) return {NativeScriptServiceStatus::Error,"car recording ID is absent"};
    if (!m_Frames.contains(id)) {
        const auto& entry = m_Entries.at(id);
        std::vector<NativeCarRecordingFrame> frames;
        for (std::uint32_t at = 0; at + 32 <= entry.Size; at += 32) {
            const auto* p = m_Archive->data() + entry.Offset + at;
            const auto time = Word(p);
            if (!frames.empty() && time == 0) break;
            NativeCarRecordingFrame frame;
            frame.Time = time;
            frame.Velocity = {float(Short(p + 4)) / 16383.5f, float(Short(p + 6)) / 16383.5f,
                float(Short(p + 8)) / 16383.5f};
            frame.Right = {float(std::int8_t(p[10])) / 127.0f, float(std::int8_t(p[11])) / 127.0f,
                float(std::int8_t(p[12])) / 127.0f};
            frame.Forward = {float(std::int8_t(p[13])) / 127.0f, float(std::int8_t(p[14])) / 127.0f,
                float(std::int8_t(p[15])) / 127.0f};
            frame.Steering = float(p[16]) / 20.0f;
            frame.Gas = float(p[17]) / 100.0f;
            frame.Brake = float(p[18]) / 100.0f;
            frame.Handbrake = p[19] != 0;
            frame.Position = {Float(p + 20), Float(p + 24), Float(p + 28)};
            if (!std::isfinite(frame.Position[0]) || !std::isfinite(frame.Position[1]) ||
                !std::isfinite(frame.Position[2])) return {NativeScriptServiceStatus::Error,"car recording frame is invalid"};
            frames.push_back(frame);
        }
        if (frames.size() < 2) return {NativeScriptServiceStatus::Error,"car recording has no complete path"};
        for (std::size_t i = 4; i < frames.size(); ++i)
            frames[i - 1].Time = std::uint32_t(float(frames[i].Time + frames[i - 2].Time) / 2.0f);
        for (std::size_t i = 1; i < frames.size(); ++i) {
            if (frames[i].Time <= frames[i - 1].Time)
                return {NativeScriptServiceStatus::Error,"car recording time is not increasing"};
        }
        m_Frames.emplace(id, std::move(frames));
    }
    m_Loaded.insert(id);
    return {NativeScriptServiceStatus::Ready,{}};
}

NativeScriptServiceResult NativeCarRecordings::Start(NativeScriptVehicleRef vehicle,
    std::int32_t id, bool useCarAI, bool looped) {
    if (vehicle.Value < 0 || !m_Loaded.contains(id) || !m_Frames.contains(id))
        return {NativeScriptServiceStatus::Error,"car recording playback request is invalid"};
    for (const auto& playback : m_Playbacks) {
        if (playback.Active && playback.Vehicle.Value == vehicle.Value)
            return {NativeScriptServiceStatus::Error,"car already has active recording playback"};
    }
    for (auto& playback : m_Playbacks) {
        if (!playback.Active) {
            playback = {vehicle, id, 0.0f, 1.0f, true, looped, useCarAI};
            return {NativeScriptServiceStatus::Ready,{}};
        }
    }
    return {NativeScriptServiceStatus::Error,"car recording playback capacity exceeded"};
}

bool NativeCarRecordings::Advance(std::uint32_t nowMs,
    std::vector<NativeCarRecordingUpdate>& updates, std::string& error) {
    if (nowMs < m_LastTimeMs) { error = "car recording time moved backwards"; return false; }
    const auto elapsed = nowMs - m_LastTimeMs;
    m_LastTimeMs = nowMs;
    updates.clear();
    for (auto& playback : m_Playbacks) {
        if (!playback.Active || playback.UseCarAI) continue;
        const auto& frames = m_Frames.at(playback.Recording);
        playback.RunningTime += float(elapsed) * playback.Speed / 4.0f;
        if (playback.RunningTime >= float(frames.back().Time)) {
            if (playback.Looped) playback.RunningTime = 0.0f;
            else {
                playback.Active = false;
                updates.push_back({playback.Vehicle, {}, frames.back().Position, true});
                continue;
            }
        }
        std::size_t current = 0;
        while (current + 1 < frames.size() && float(frames[current + 1].Time) < playback.RunningTime) ++current;
        const auto& a = frames[current];
        const auto& b = frames[std::min(current + 1, frames.size() - 1)];
        const float denominator = float(b.Time - a.Time);
        const float t = denominator > 0.0f ? (playback.RunningTime - float(a.Time)) / denominator : 0.0f;
        const auto right = Lerp(a.Right, b.Right, t), forward = Lerp(a.Forward, b.Forward, t);
        const auto up = Cross(right, forward);
        updates.push_back({playback.Vehicle, {right, forward, up}, Lerp(a.Position, b.Position, t), false});
    }
    error.clear();
    return true;
}

bool NativeCarRecordings::IsPlaybackActive(NativeScriptVehicleRef vehicle) const {
    for (const auto& playback : m_Playbacks)
        if (playback.Active && playback.Vehicle.Value == vehicle.Value) return true;
    return false;
}

NativeCarRecordingInspection NativeCarRecordings::Inspect() const noexcept {
    NativeCarRecordingInspection inspection;
    inspection.Vehicle.Value = -1;
    for (const auto& playback : m_Playbacks) {
        if (!playback.Active) continue;
        ++inspection.Active;
        if (inspection.Recording >= 0) continue;
        inspection.Vehicle = playback.Vehicle;
        inspection.Recording = playback.Recording;
        inspection.RunningTime = playback.RunningTime;
        if (const auto it = m_Frames.find(playback.Recording); it != m_Frames.end() && !it->second.empty())
            inspection.EndTime = it->second.back().Time;
    }
    return inspection;
}

void NativeCarRecordings::Stop(NativeScriptVehicleRef vehicle) noexcept {
    for (auto& playback : m_Playbacks) {
        if (playback.Active && playback.Vehicle.Value == vehicle.Value) playback.Active = false;
    }
}
