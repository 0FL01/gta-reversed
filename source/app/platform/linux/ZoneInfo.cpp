// ZoneInfo implementation: info.zon parse + smallest-zone lookup.
// See ZoneInfo.h for the format contract and game-code references.

#include "app/platform/linux/ZoneInfo.h"

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

void SetErr(char* err, std::size_t errSize, const char* msg) {
    if (!err || errSize == 0) {
        return;
    }
    (void)std::snprintf(err, errSize, "%s", msg ? msg : "zone error");
}

} // namespace

bool ZoneInfo_Contains2D(const ZoneRect& z, double x, double y) {
    return static_cast<double>(z.x1) <= x && x <= static_cast<double>(z.x2) &&
           static_cast<double>(z.y1) <= y && y <= static_cast<double>(z.y2);
}

int ZoneInfo_FindSmallest(const ZoneData& data, double x, double y) {
    int best = -1;
    double bestSize = 0.0;
    for (std::size_t i = 0; i < data.zones.size(); ++i) {
        const ZoneRect& z = data.zones[i];
        if (!ZoneInfo_Contains2D(z, x, y)) {
            continue;
        }
        const double size = static_cast<double>(z.x2 - z.x1) +
                            static_cast<double>(z.y2 - z.y1);
        if (best < 0 || size < bestSize) {
            best = static_cast<int>(i);
            bestSize = size;
        }
    }
    return best;
}

bool ZoneInfo_Load(const char* gameDir, ZoneData& out, char* err, std::size_t errSize) {
    out = ZoneData{};
    if (!gameDir || !gameDir[0]) {
        SetErr(err, errSize, "no game dir");
        return false;
    }
    static const char kSrc[] = "data/info.zon";
    OS_SetFilePathOffset(gameDir);
    void* file = nullptr;
    if (OS_FileOpen(FILE_DATA_AREA_DEFAULT, &file, kSrc, FILE_ACCESS_READ) != 0 ||
        !file) {
        char msg[128] = {};
        (void)std::snprintf(msg, sizeof(msg), "cannot open %s", kSrc);
        SetErr(err, errSize, msg);
        return false;
    }
    int32 size = OS_FileSize(file);
    if (size <= 0 || size > (32 << 20)) {
        OS_FileClose(file);
        SetErr(err, errSize, "bad info.zon size");
        return false;
    }
    std::vector<char> bytes(static_cast<std::size_t>(size) + 1, '\0');
    bool ok = OS_FileRead(file, bytes.data(), size) == 0;
    OS_FileClose(file);
    if (!ok) {
        SetErr(err, errSize, "info.zon read failed");
        return false;
    }
    bytes[static_cast<std::size_t>(size)] = '\0';

    // Split into lines on '\n' (retail files are CRLF; '\r' is sanitized
    // below exactly like CFileLoader::LoadLine does).
    std::vector<std::string> lines;
    {
        std::string cur;
        for (int32 i = 0; i < size; ++i) {
            const char c = bytes[static_cast<std::size_t>(i)];
            if (c == '\n') {
                lines.push_back(cur);
                cur.clear();
            } else {
                cur.push_back(c);
            }
        }
        if (!cur.empty()) {
            lines.push_back(cur);
        }
    }
    // The shipped file is CRLF: the header arrives as "zone\r" (LoadLine
    // would sanitize the '\r' away; accept both spellings here).
    std::string header = lines.empty() ? "" : lines[0];
    while (!header.empty() &&
           static_cast<unsigned char>(header.back()) < static_cast<unsigned char>(' ')) {
        header.pop_back();
    }
    if (header != "zone") {
        SetErr(err, errSize, "info.zon missing zone header");
        return false;
    }
    out.src = kSrc;
    for (std::size_t li = 1; li < lines.size(); ++li) {
        std::string line = lines[li];
        // Sanitize exactly like CFileLoader::LoadLine: ',' and bytes < ' '
        // become spaces, so the comma file scans as whitespace tokens.
        for (std::size_t i = 0; i < line.size(); ++i) {
            const unsigned char c = static_cast<unsigned char>(line[i]);
            if (c < static_cast<unsigned char>(' ') || line[i] == ',') {
                line[i] = ' ';
            }
        }
        // Skip blank lines and '#' comments (same as LoadScene).
        std::size_t first = line.find_first_not_of(" \t");
        if (first == std::string::npos || line[first] == '#') {
            continue;
        }
        if (line.compare(first, 3, "end") == 0 &&
            (first + 3 >= line.size() || line[first + 3] == ' ' ||
             line[first + 3] == '\t')) {
            break;
        }
        // Retail LoadZone contract: 10 tokens, sscanf == 10 (else VERIFY).
        char infoLabel[24] = {};
        int type = 0;
        float minx = 0.0f, miny = 0.0f, minz = 0.0f;
        float maxx = 0.0f, maxy = 0.0f, maxz = 0.0f;
        int level = 0;
        char textLabel[12] = {};
        const int n = std::sscanf(line.c_str(), "%23s %d %f %f %f %f %f %f %d %11s",
                                  infoLabel, &type, &minx, &miny, &minz, &maxx,
                                  &maxy, &maxz, &level, textLabel);
        if (n != 10) {
            char msg[192] = {};
            (void)std::snprintf(msg, sizeof(msg), "info.zon line %lu scans %d/10",
                                static_cast<unsigned long>(li + 1), n);
            SetErr(err, errSize, msg);
            return false;
        }
        ZoneRect z;
        z.name = infoLabel;
        z.key = textLabel;
        z.type = type;
        // Retail CreateZone stores minmax((int16)pos); shipped bounds fit in
        // int16, so float storage keeps file precision with equal ordering.
        z.x1 = minx < maxx ? minx : maxx;
        z.x2 = minx < maxx ? maxx : minx;
        z.y1 = miny < maxy ? miny : maxy;
        z.y2 = miny < maxy ? maxy : miny;
        z.z1 = minz < maxz ? minz : maxz;
        z.z2 = minz < maxz ? maxz : minz;
        z.level = level;
        out.zones.push_back(z);
    }
    if (out.zones.empty()) {
        SetErr(err, errSize, "info.zon has no zone records");
        return false;
    }
    return true;
}
