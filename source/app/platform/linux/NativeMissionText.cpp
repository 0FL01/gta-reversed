#include "app/platform/linux/NativeMissionText.h"

#include <algorithm>
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
std::uint16_t U16(const std::uint8_t* data) {
    return std::uint16_t(data[0]) | std::uint16_t(data[1]) << 8;
}
std::uint32_t U32(const std::uint8_t* data) {
    return std::uint32_t(data[0]) | std::uint32_t(data[1]) << 8 |
        std::uint32_t(data[2]) << 16 | std::uint32_t(data[3]) << 24;
}
bool NameValid(const std::array<char, 8>& name) {
    const auto end = std::ranges::find(name, '\0');
    if (end == name.begin()) return false;
    return std::ranges::all_of(name.begin(), end, [](char value) {
        const auto c = static_cast<unsigned char>(value);
        return c == '_' || (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z');
    });
}
std::uint32_t KeyHash(const std::array<char, 8>& key) {
    std::uint32_t hash = 0xFFFFFFFFu;
    for (const char raw : key) {
        if (!raw) break;
        auto ch = static_cast<std::uint8_t>(raw);
        if (ch >= 'a' && ch <= 'z') ch = static_cast<std::uint8_t>(ch - 0x20);
        auto crc = static_cast<std::uint32_t>(ch ^ hash) & 0xFFu;
        for (int bit = 0; bit < 8; ++bit) crc = (crc & 1u) ? (crc >> 1u) ^ 0xEDB88320u : crc >> 1u;
        hash = crc ^ (hash >> 8u);
    }
    return hash;
}
}

bool NativeMissionText::LoadBeforeWorker(const char* gameDir, std::string& error) {
    if (m_Loaded || !gameDir || !*gameDir) { error = "mission text owner load contract rejected"; return false; }
    OS_SetFilePathOffset(gameDir);
    void* file = nullptr;
    if (OS_FileOpen(FILE_DATA_AREA_DEFAULT, &file, "text/american.gxt", FILE_ACCESS_READ) != 0 || !file) {
        error = "mission text GXT open failed";
        return false;
    }
    const int32 size = OS_FileSize(file);
    std::vector<std::uint8_t> bytes(size > 0 ? std::size_t(size) : 0);
    const bool read = size > 16 && OS_FileRead(file, bytes.data(), size) == 0;
    OS_FileClose(file);
    if (!read || U16(bytes.data() + 2) != 8 || std::memcmp(bytes.data() + 4, "TABL", 4)) {
        error = "mission text GXT header rejected";
        return false;
    }
    const auto tableBytes = U32(bytes.data() + 8);
    if (!tableBytes || tableBytes % 12 || 12ull + tableBytes > bytes.size()) {
        error = "mission text table directory rejected";
        return false;
    }
    std::vector<NativeMissionTextTable> tables;
    tables.reserve(tableBytes / 12);
    for (std::uint32_t at = 12; at < 12 + tableBytes; at += 12) {
        NativeMissionTextTable table;
        std::copy_n(reinterpret_cast<const char*>(bytes.data() + at), 8, table.Name.data());
        table.Offset = U32(bytes.data() + at + 8);
        if (!NameValid(table.Name) || table.Offset + 8 > bytes.size()) {
            error = "mission text table identity rejected";
            return false;
        }
        std::uint64_t chunkAt = table.Offset;
        if (std::memcmp(bytes.data() + chunkAt, "TKEY", 4)) {
            if (chunkAt + 16 > bytes.size() || std::memcmp(bytes.data() + chunkAt, table.Name.data(), 8) ||
                std::memcmp(bytes.data() + chunkAt + 8, "TKEY", 4)) {
                error = "mission text table identity rejected";
                return false;
            }
            chunkAt += 8;
        }
        const auto keyBytes = U32(bytes.data() + chunkAt + 4);
        const auto dataAt = chunkAt + 8 + keyBytes;
        if (!keyBytes || keyBytes % 8 || dataAt + 8 > bytes.size() ||
            std::memcmp(bytes.data() + dataAt, "TDAT", 4)) {
            error = "mission text table chunks rejected";
            return false;
        }
        const auto textBytes = U32(bytes.data() + dataAt + 4);
        if (dataAt + 8ull + textBytes > bytes.size()) {
            error = "mission text table data rejected";
            return false;
        }
        table.KeyHashes.reserve(keyBytes / 8);
        for (std::uint64_t keyAt = chunkAt + 8; keyAt < dataAt; keyAt += 8) {
            if (U32(bytes.data() + keyAt) >= textBytes) {
                error = "mission text key offset rejected";
                return false;
            }
            table.KeyHashes.push_back(U32(bytes.data() + keyAt + 4));
        }
        tables.push_back(table);
    }
    m_Tables = std::move(tables);
    m_Loaded = true;
    error.clear();
    return true;
}

void NativeMissionText::BeginFrame() {
    m_DrawCount = 0;
    m_DrawBeforeFade = false;
    m_Font = 0;
    m_Style = {};
}

NativeScriptServiceResult NativeMissionText::Select(const std::array<char, 8>& name) {
    if (!m_Loaded || !NameValid(name)) return {NativeScriptServiceStatus::Error, "invalid mission text selection"};
    const auto found = std::ranges::find(m_Tables, name, &NativeMissionTextTable::Name);
    if (found == m_Tables.end()) return {NativeScriptServiceStatus::Error, "mission text table is absent"};
    if (m_Revision == std::numeric_limits<std::uint64_t>::max()) {
        return {NativeScriptServiceStatus::Error, "mission text revision exhausted"};
    }
    m_Active = name;
    ++m_Revision;
    return {NativeScriptServiceStatus::Ready, {}};
}

NativeScriptServiceResult NativeMissionText::SetCommandsEnabled(bool enabled) {
    if (!m_Loaded) return {NativeScriptServiceStatus::Error, "mission text owner is unavailable"};
    if (m_Revision == std::numeric_limits<std::uint64_t>::max()) {
        return {NativeScriptServiceStatus::Error, "mission text revision exhausted"};
    }
    m_CommandsEnabled = enabled;
    ++m_Revision;
    return {NativeScriptServiceStatus::Ready, {}};
}

NativeScriptServiceResult NativeMissionText::SetDrawBeforeFade(bool enabled) {
    if (!m_Loaded) return {NativeScriptServiceStatus::Error, "mission text owner is unavailable"};
    if (m_Revision == std::numeric_limits<std::uint64_t>::max()) {
        return {NativeScriptServiceStatus::Error, "mission text revision exhausted"};
    }
    m_DrawBeforeFade = enabled;
    ++m_Revision;
    return {NativeScriptServiceStatus::Ready, {}};
}

NativeScriptServiceResult NativeMissionText::SetFont(std::int32_t font) {
    if (!m_Loaded || font < 0 || font > 3) {
        return {NativeScriptServiceStatus::Error, "mission text font is outside source range"};
    }
    if (m_Revision == std::numeric_limits<std::uint64_t>::max()) {
        return {NativeScriptServiceStatus::Error, "mission text revision exhausted"};
    }
    m_Font = font;
    ++m_Revision;
    return {NativeScriptServiceStatus::Ready, {}};
}

NativeScriptServiceResult NativeMissionText::SetStyle(std::uint16_t opcode,
    const std::array<float, 2>& floats, const std::array<std::int32_t, 5>& integers) {
    if (!m_Loaded || !std::isfinite(floats[0]) || !std::isfinite(floats[1])) {
        return {NativeScriptServiceStatus::Error, "invalid mission text style"};
    }
    const auto bytesValid = [&](std::size_t first, std::size_t count) {
        return std::ranges::all_of(integers.begin() + first, integers.begin() + first + count,
            [](auto value) { return value >= 0 && value <= 255; });
    };
    const auto boolean = integers[0] == 0 || integers[0] == 1;
    switch (opcode) {
    case 0x033F:
        if (floats[0] < 0 || floats[1] < 0) return {NativeScriptServiceStatus::Error, "negative text scale"};
        m_Style.ScaleX = floats[0]; m_Style.ScaleY = floats[1]; break;
    case 0x0340:
        if (!bytesValid(0, 4)) return {NativeScriptServiceStatus::Error, "text colour outside bytes"};
        for (unsigned i = 0; i < 4; ++i) m_Style.Colour[i] = std::uint8_t(integers[i]);
        break;
    case 0x0341:
        if (!boolean) return {NativeScriptServiceStatus::Error, "text justify outside boolean"};
        m_Style.Justify = integers[0] != 0; break;
    case 0x0342:
        if (!boolean) return {NativeScriptServiceStatus::Error, "text centre outside boolean"};
        m_Style.Centre = integers[0] != 0; break;
    case 0x0343: m_Style.WrapX = floats[0]; break;
    case 0x0344: m_Style.CentreSize = floats[0]; break;
    case 0x0345:
        if (!boolean) return {NativeScriptServiceStatus::Error, "text background outside boolean"};
        m_Style.Background = integers[0] != 0; break;
    case 0x0346:
        if (!bytesValid(0, 4)) return {NativeScriptServiceStatus::Error, "background colour outside bytes"};
        for (unsigned i = 0; i < 4; ++i) m_Style.BackgroundColour[i] = std::uint8_t(integers[i]);
        break;
    case 0x0347:
        if (!boolean) return {NativeScriptServiceStatus::Error, "background text-only outside boolean"};
        m_Style.BackgroundOnlyText = integers[0] != 0; break;
    case 0x0348:
        if (!boolean) return {NativeScriptServiceStatus::Error, "proportional text outside boolean"};
        m_Style.Proportional = integers[0] != 0; break;
    case 0x060D:
        if (integers[0] < -128 || integers[0] > 127 || !bytesValid(1, 4)) {
            return {NativeScriptServiceStatus::Error, "text drop shadow outside source range"};
        }
        m_Style.DropShadow = static_cast<std::int8_t>(integers[0]);
        for (unsigned i = 0; i < 4; ++i) m_Style.DropShadowColour[i] = std::uint8_t(integers[i + 1]);
        break;
    default: return {NativeScriptServiceStatus::Error, "unknown mission text style opcode"};
    }
    if (m_Revision == std::numeric_limits<std::uint64_t>::max()) return {NativeScriptServiceStatus::Error, "mission text revision exhausted"};
    ++m_Revision;
    return {NativeScriptServiceStatus::Ready, {}};
}

NativeScriptServiceResult NativeMissionText::Display(float x, float y, const std::array<char, 8>& key) {
    if (!m_Loaded || !m_CommandsEnabled || !std::isfinite(x) || !std::isfinite(y) || !NameValid(key)) {
        return {NativeScriptServiceStatus::Error, "invalid mission text display"};
    }
    const auto table = std::ranges::find(m_Tables, m_Active, &NativeMissionTextTable::Name);
    const auto hash = KeyHash(key);
    if (table == m_Tables.end() || std::ranges::find(table->KeyHashes, hash) == table->KeyHashes.end()) {
        return {NativeScriptServiceStatus::Error, "mission text key is absent"};
    }
    if (m_DrawCount == m_Draws.size()) return {NativeScriptServiceStatus::Error, "mission text draw capacity exhausted"};
    if (m_Revision == std::numeric_limits<std::uint64_t>::max()) {
        return {NativeScriptServiceStatus::Error, "mission text revision exhausted"};
    }
    m_Draws[m_DrawCount++] = {key, x, y, m_Style};
    ++m_Revision;
    return {NativeScriptServiceStatus::Ready, {}};
}
