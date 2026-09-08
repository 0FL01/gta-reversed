// GxtText implementation: SA GXT MAIN-table reader over OS_File*.
// See GxtText.h for the format contract. All parsing is bounds-checked;
// any truncation or unexpected encoding fails honestly (no invented text).

#include "app/platform/linux/GxtText.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
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

// CRC-32/IEEE table, polynomial 0xEDB88320 (cf. game_sa/Core/KeyGen.h).
// Standard reflect-in table; copied as algorithm constants, not code.
uint32_t CrcTable(uint8 index) {
    static uint32_t table[256] = { 0 };
    static bool built = false;
    if (!built) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1) ? (c >> 1) ^ 0xEDB88320u : (c >> 1);
            }
            table[i] = c;
        }
        built = true;
    }
    return table[index];
}

void SetErr(char* err, std::size_t errSize, const char* msg) {
    if (!err || errSize == 0) {
        return;
    }
    (void)std::snprintf(err, errSize, "%s", msg ? msg : "unknown");
}

uint16 ReadU16LE(const uint8* p) {
    return static_cast<uint16>(p[0] | (p[1] << 8));
}

uint32 ReadU32LE(const uint8* p) {
    return static_cast<uint32>(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24));
}

} // namespace

uint32_t GxtText_Hash(const char* key) {
    uint32_t hash = 0xFFFFFFFFu;
    for (const char* p = key; p && *p; ++p) {
        uint8 ch = static_cast<uint8>(*p);
        if (ch >= 'a' && ch <= 'z') {
            ch = static_cast<uint8>(ch - 0x20); // ToUppercase (KeyGen.h)
        }
        hash = CrcTable(static_cast<uint8>(ch ^ hash)) ^ (hash >> 8);
    }
    return hash;
}

bool GxtText_FileForLang(const char* lang, char* out, std::size_t outSize) {
    if (!lang || !out || outSize == 0) {
        return false;
    }
    char lower[32] = {};
    std::size_t n = 0;
    for (const char* p = lang; *p && n + 1 < sizeof(lower); ++p, ++n) {
        lower[n] = (*p >= 'A' && *p <= 'Z') ? static_cast<char>(*p + 32) : *p;
    }
    const char* base = nullptr;
    if (std::strcmp(lower, "english") == 0 || std::strcmp(lower, "american") == 0) {
        base = "american.gxt";
    } else if (std::strcmp(lower, "french") == 0) {
        base = "french.gxt";
    } else if (std::strcmp(lower, "german") == 0) {
        base = "german.gxt";
    } else if (std::strcmp(lower, "italian") == 0) {
        base = "italian.gxt";
    } else if (std::strcmp(lower, "spanish") == 0) {
        base = "spanish.gxt";
    } else {
        return false;
    }
    (void)std::snprintf(out, outSize, "text/%s", base);
    return true;
}

bool GxtText_Load(const char* gameDir, const char* lang, GxtTable& out, char* err,
                  std::size_t errSize) {
    out = GxtTable{};
    if (!gameDir || !gameDir[0]) {
        SetErr(err, errSize, "no game dir");
        return false;
    }
    char rel[64] = {};
    if (!GxtText_FileForLang(lang ? lang : "english", rel, sizeof(rel))) {
        SetErr(err, errSize, "unknown lang (want english/french/german/italian/spanish)");
        return false;
    }
    OS_SetFilePathOffset(gameDir);
    void* file = nullptr;
    if (OS_FileOpen(FILE_DATA_AREA_DEFAULT, &file, rel, FILE_ACCESS_READ) != 0 || !file) {
        SetErr(err, errSize, "cannot open gxt");
        return false;
    }
    int32 size = OS_FileSize(file);
    if (size < 4 + 8 + 8) {
        OS_FileClose(file);
        SetErr(err, errSize, "gxt too small");
        return false;
    }
    std::vector<uint8> bytes(static_cast<std::size_t>(size));
    bool ok = OS_FileRead(file, bytes.data(), size) == 0;
    OS_FileClose(file);
    if (!ok) {
        SetErr(err, errSize, "gxt read failed");
        return false;
    }
    out.file = rel;
    out.version = ReadU16LE(bytes.data());
    uint16 encoding = ReadU16LE(bytes.data() + 2);
    if (encoding != 8) { // GAME_ENCODING: 8-bit GxtChar (Text.cpp)
        SetErr(err, errSize, "unsupported gxt encoding (want 8-bit)");
        return false;
    }
    // Walk chunks like CText::Load: stop at the first TKEY+TDAT pair (MAIN).
    // TABL is skipped (mission offsets are out of scope for the menu slice).
    std::size_t pos = 4;
    const std::size_t end = bytes.size();
    bool haveKey = false;
    bool haveDat = false;
    std::vector<uint8> tkey;
    while (!haveKey || !haveDat) {
        if (pos + 8 > end) {
            SetErr(err, errSize, "gxt truncated (chunk header)");
            return false;
        }
        const uint8* hdr = bytes.data() + pos;
        uint32 chunkSize = ReadU32LE(hdr + 4);
        pos += 8;
        if (pos + chunkSize > end) {
            SetErr(err, errSize, "gxt truncated (chunk body)");
            return false;
        }
        if (std::memcmp(hdr, "TKEY", 4) == 0 && !haveKey) {
            if (chunkSize == 0 || chunkSize % 8 != 0) {
                SetErr(err, errSize, "bad TKEY size");
                return false;
            }
            tkey.assign(bytes.begin() + pos, bytes.begin() + pos + chunkSize);
            haveKey = true;
        } else if (std::memcmp(hdr, "TDAT", 4) == 0 && !haveDat) {
            if (chunkSize == 0) {
                SetErr(err, errSize, "empty TDAT");
                return false;
            }
            out.tdat.assign(bytes.begin() + pos, bytes.begin() + pos + chunkSize);
            haveDat = true;
        }
        // TABL and anything else: skip (CText::Load seeks past them too).
        pos += chunkSize;
    }
    const std::size_t count = tkey.size() / 8;
    if (count == 0 || count > 1000000) {
        SetErr(err, errSize, "bad TKEY count");
        return false;
    }
    out.entries.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const uint8* e = tkey.data() + i * 8;
        GxtEntry entry;
        entry.offset = ReadU32LE(e);     // string offset (CKeyEntry::string)
        entry.hash = ReadU32LE(e + 4);   // name hash (CKeyEntry::hash)
        if (entry.offset >= out.tdat.size()) {
            SetErr(err, errSize, "TKEY offset out of TDAT range");
            return false;
        }
        out.entries.push_back(entry);
    }
    return true;
}

bool GxtText_Find(const GxtTable& table, const char* key, std::string& textOut) {
    textOut.clear();
    if (!key || !key[0] || key[0] == ' ') {
        return false; // CText::Get rejects empty/blank keys
    }
    const uint32_t want = GxtText_Hash(key);
    // Binary search exactly like CKeyArray::BinarySearch (u16 middle).
    int first = 0;
    int last = static_cast<int>(table.entries.size()) - 1;
    const GxtEntry* found = nullptr;
    while (first <= last) {
        uint16_t middle = static_cast<uint16_t>((first + last) >> 1);
        if (middle >= table.entries.size()) {
            break;
        }
        uint32_t have = table.entries[middle].hash;
        if (want == have) {
            found = &table.entries[middle];
            break;
        }
        if (want > have) {
            first = middle + 1;
        } else {
            last = middle - 1;
        }
    }
    if (!found) {
        return false;
    }
    // NUL-terminated bytes at the TDAT offset (bounds-checked).
    std::size_t pos = found->offset;
    while (pos < table.tdat.size() && table.tdat[pos] != '\0') {
        textOut.push_back(table.tdat[pos]);
        ++pos;
        if (textOut.size() > 4096) {
            textOut.clear();
            return false; // unterminated: refuse instead of inventing
        }
    }
    if (pos >= table.tdat.size()) {
        textOut.clear();
        return false;
    }
    return true;
}
