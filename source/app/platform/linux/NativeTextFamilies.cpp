#include "app/platform/linux/NativeTextFamilies.h"

#include "app/platform/linux/GxtText.h"

#include <algorithm>
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
constexpr std::array<const char*, 5> kNames{"american", "french", "german", "italian", "spanish"};

std::uint16_t U16(const std::uint8_t* p) {
    return std::uint16_t(p[0]) | std::uint16_t(p[1]) << 8;
}

std::uint32_t U32(const std::uint8_t* p) {
    return std::uint32_t(p[0]) | std::uint32_t(p[1]) << 8 |
        std::uint32_t(p[2]) << 16 | std::uint32_t(p[3]) << 24;
}

bool SameName(const std::array<char, 8>& value, const char* name) {
    if (!name) return false;
    for (std::size_t i = 0; i < value.size(); ++i) {
        const char right = name[i];
        if (value[i] != right) return false;
        if (!right) return true;
    }
    return std::strlen(name) == value.size();
}

bool ReadFile(const char* relative, std::vector<std::uint8_t>& out) {
    void* file = nullptr;
    if (OS_FileOpen(FILE_DATA_AREA_DEFAULT, &file, relative, FILE_ACCESS_READ) != 0 || !file) return false;
    const int32 size = OS_FileSize(file);
    out.resize(size > 0 ? std::size_t(size) : 0);
    const bool ok = size > 16 && OS_FileRead(file, out.data(), size) == 0;
    OS_FileClose(file);
    return ok;
}

bool ParseLanguage(NativeTextLanguage language, const char* file,
    const std::vector<std::uint8_t>& bytes, NativeTextLanguageFamily& out, std::string& error) {
    if (bytes.size() < 16 || U16(bytes.data() + 2) != 8 || std::memcmp(bytes.data() + 4, "TABL", 4)) {
        error = "text family GXT header rejected";
        return false;
    }
    const auto tableBytes = U32(bytes.data() + 8);
    if (!tableBytes || tableBytes % 12 || 12ull + tableBytes > bytes.size()) {
        error = "text family table directory rejected";
        return false;
    }
    NativeTextLanguageFamily candidate;
    candidate.Language = language;
    candidate.File = file;
    candidate.Version = U16(bytes.data());
    candidate.Tables.reserve(tableBytes / 12);
    for (std::uint32_t at = 12; at < 12 + tableBytes; at += 12) {
        NativeTextTableFamily table;
        std::copy_n(reinterpret_cast<const char*>(bytes.data() + at), 8, table.Name.data());
        std::uint64_t chunk = U32(bytes.data() + at + 8);
        if (chunk + 8 > bytes.size()) { error = "text family table offset rejected"; return false; }
        if (std::memcmp(bytes.data() + chunk, "TKEY", 4)) {
            if (chunk + 16 > bytes.size() || std::memcmp(bytes.data() + chunk, table.Name.data(), 8) ||
                std::memcmp(bytes.data() + chunk + 8, "TKEY", 4)) {
                error = "text family table identity rejected";
                return false;
            }
            chunk += 8;
        }
        const auto keyBytes = U32(bytes.data() + chunk + 4);
        const auto dataAt = chunk + 8ull + keyBytes;
        if (!keyBytes || keyBytes % 8 || dataAt + 8 > bytes.size() ||
            std::memcmp(bytes.data() + dataAt, "TDAT", 4)) {
            error = "text family chunks rejected";
            return false;
        }
        const auto dataBytes = U32(bytes.data() + dataAt + 4);
        if (!dataBytes || dataAt + 8ull + dataBytes > bytes.size()) {
            error = "text family data rejected";
            return false;
        }
        table.Data.assign(bytes.begin() + std::ptrdiff_t(dataAt + 8),
            bytes.begin() + std::ptrdiff_t(dataAt + 8 + dataBytes));
        table.Entries.reserve(keyBytes / 8);
        for (std::uint64_t keyAt = chunk + 8; keyAt < dataAt; keyAt += 8) {
            NativeTextKeyEntry entry{U32(bytes.data() + keyAt + 4), U32(bytes.data() + keyAt)};
            if (entry.Offset >= table.Data.size() ||
                std::find(table.Data.begin() + entry.Offset, table.Data.end(), '\0') == table.Data.end()) {
                error = "text family key offset rejected";
                return false;
            }
            table.Entries.push_back(entry);
        }
        if (!std::ranges::is_sorted(table.Entries, {}, &NativeTextKeyEntry::Hash)) {
            error = "text family key order rejected";
            return false;
        }
        candidate.Tables.push_back(std::move(table));
    }
    if (candidate.Tables.empty() || !SameName(candidate.Tables.front().Name, "MAIN")) {
        error = "text family MAIN table is absent";
        return false;
    }
    out = std::move(candidate);
    return true;
}

void ReplaceToken(std::string& value, const std::string& token, const std::string& replacement) {
    std::size_t at = 0;
    while ((at = value.find(token, at)) != std::string::npos) {
        value.replace(at, token.size(), replacement);
        at += replacement.size();
    }
}
}

bool NativeTextFamilies::Load(const char* gameDir, std::string& error) {
    if (m_Loaded || !gameDir || !*gameDir) { error = "text family load contract rejected"; return false; }
    OS_SetFilePathOffset(gameDir);
    std::array<NativeTextLanguageFamily, 5> candidate;
    for (std::size_t i = 0; i < kNames.size(); ++i) {
        const std::string relative = std::string("text/") + kNames[i] + ".gxt";
        std::vector<std::uint8_t> bytes;
        if (!ReadFile(relative.c_str(), bytes) ||
            !ParseLanguage(static_cast<NativeTextLanguage>(i), relative.c_str(), bytes, candidate[i], error)) {
            if (error.empty()) error = "text family file read failed";
            return false;
        }
    }
    const auto tableCount = candidate.front().Tables.size();
    for (const auto& language : candidate) {
        if (language.Tables.size() != tableCount) {
            error = "text family table count mismatch";
            return false;
        }
        for (std::size_t table = 0; table < tableCount; ++table) {
            if (language.Tables[table].Name != candidate.front().Tables[table].Name) {
                error = "text family table identity mismatch";
                return false;
            }
        }
    }
    m_Languages = std::move(candidate);
    m_Loaded = true;
    error.clear();
    return true;
}

bool NativeTextFamilies::Find(NativeTextLanguage language, const char* tableName, const char* key,
    std::string& out, std::string& error) const {
    if (!m_Loaded || std::size_t(language) >= m_Languages.size() || !tableName || !key || !*key) {
        error = "text family lookup rejected";
        return false;
    }
    return FindHash(language, tableName, GxtText_Hash(key), out, error);
}

bool NativeTextFamilies::FindHash(NativeTextLanguage language, const char* tableName,
    std::uint32_t hash, std::string& out, std::string& error) const {
    if (!m_Loaded || std::size_t(language) >= m_Languages.size() || !tableName) {
        error = "text family hash lookup rejected";
        return false;
    }
    const auto& languageOwner = m_Languages[std::size_t(language)];
    const auto table = std::ranges::find_if(languageOwner.Tables,
        [&](const auto& value) { return SameName(value.Name, tableName); });
    if (table == languageOwner.Tables.end()) { error = "text family table is absent"; return false; }
    const auto entry = std::ranges::lower_bound(table->Entries, hash, {}, &NativeTextKeyEntry::Hash);
    if (entry == table->Entries.end() || entry->Hash != hash) { error = "text family key is absent"; return false; }
    const char* text = table->Data.data() + entry->Offset;
    out.assign(text);
    error.clear();
    return true;
}

bool NativeTextFamilies::Resolve(NativeTextLanguage language, const char* table, const char* key,
    const NativeTextSubstitutions& substitutions, std::string& out, std::string& error) const {
    std::string candidate;
    if (!Find(language, table, key, candidate, error)) return false;
    for (std::size_t i = 0; i < substitutions.Numbers.size(); ++i) {
        if (substitutions.HasNumber[i]) {
            ReplaceToken(candidate, "~" + std::to_string(i + 1) + "~",
                std::to_string(substitutions.Numbers[i]));
        }
    }
    if (!substitutions.String.empty() && !substitutions.HasNumber[0]) {
        const auto at = candidate.find("~1~");
        if (at != std::string::npos) candidate.replace(at, 3, substitutions.String);
    }
    std::size_t at = 0;
    while ((at = candidate.find("~k~~", at)) != std::string::npos) {
        const auto end = candidate.find('~', at + 4);
        if (end == std::string::npos) { error = "text family control token is unterminated"; return false; }
        const std::string name = candidate.substr(at + 4, end - (at + 4));
        const auto binding = substitutions.Controls.find(name);
        if (binding == substitutions.Controls.end()) { error = "text family control binding is absent"; return false; }
        candidate.replace(at, end + 1 - at, binding->second);
        at += binding->second.size();
    }
    out = std::move(candidate);
    error.clear();
    return true;
}
