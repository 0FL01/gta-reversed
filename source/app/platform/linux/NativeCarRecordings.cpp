#include "app/platform/linux/NativeCarRecordings.h"

#include <charconv>
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
    m_Archive = std::move(archive);
    error.clear();
    return true;
}

NativeScriptServiceResult NativeCarRecordings::Request(std::int32_t id) {
    if (!m_Archive || !m_Entries.contains(id)) return {NativeScriptServiceStatus::Error,"car recording ID is absent"};
    m_Loaded.insert(id);
    return {NativeScriptServiceStatus::Ready,{}};
}
