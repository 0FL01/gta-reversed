// TimeCycle implementation: EXTRASUNNY_LA daylight rows from timecyc.dat.
// See TimeCycle.h for the contract. Column layout reference (read-only, NOT
// linked): game_sa/TimeCycle.cpp CTimeCycle::Initialise sscanf order — Amb,
// Amb_Obj, Dir, SkyTop, SkyBot, SunCore, ... — and the eTimeType/TimeSamples
// table {0,5,6,7,12,19,20,22,24} in CTimeCycle::Update. Section/row labels
// (//////////// EXTRASUNNY_LA, //Midnight, //5AM, ...) are read from the
// file itself; only the 8-row/24-hour table shape is shared with the game.

#include "app/platform/linux/TimeCycle.h"

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
    (void)std::snprintf(err, errSize, "%s", msg ? msg : "timecyc error");
}

// Game's own sample table (CTimeCycle::Update): row i covers
// [TimeSamples[i], TimeSamples[i+1]). No interpolation here: the floor row
// is used as-is, so anchor hours read their exact stored line.
constexpr int kSampleHours[9] = { 0, 5, 6, 7, 12, 19, 20, 22, 24 };
constexpr const char* kSampleNames[8] = {
    "Midnight", "5AM", "6AM", "7AM", "Midday", "7PM", "8PM", "10PM",
};

int SampleForHour(int hour) {
    int idx = 0;
    while (idx + 1 < 8 && hour >= kSampleHours[idx + 1]) {
        ++idx;
    }
    return idx;
}

std::string TrimRight(const std::string& s) {
    size_t e = s.size();
    while (e > 0 && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r')) {
        --e;
    }
    return s.substr(0, e);
}

} // namespace

bool TimeCycle_LoadHour(const char* gameDir, int hour, TimeCycleParams& out, char* err,
                        std::size_t errSize) {
    out = TimeCycleParams{};
    if (!gameDir || !gameDir[0]) {
        SetErr(err, errSize, "no game dir");
        return false;
    }
    if (hour < 0 || hour > 23) {
        SetErr(err, errSize, "hour out of range 0-23");
        return false;
    }
    OS_SetFilePathOffset(gameDir);
    void* file = nullptr;
    if (OS_FileOpen(FILE_DATA_AREA_DEFAULT, &file, "data/timecyc.dat", FILE_ACCESS_READ) != 0 ||
        !file) {
        SetErr(err, errSize, "cannot open data/timecyc.dat");
        return false;
    }
    int32 size = OS_FileSize(file);
    if (size <= 0) {
        OS_FileClose(file);
        SetErr(err, errSize, "empty data/timecyc.dat");
        return false;
    }
    std::vector<char> buf(static_cast<size_t>(size));
    int32 rc = OS_FileRead(file, buf.data(), size);
    OS_FileClose(file);
    if (rc != 0) {
        SetErr(err, errSize, "cannot read data/timecyc.dat");
        return false;
    }
    // Split into lines.
    std::vector<std::string> lines;
    {
        size_t pos = 0;
        while (pos < buf.size()) {
            size_t end = pos;
            while (end < buf.size() && buf[end] != '\n') {
                ++end;
            }
            lines.emplace_back(buf.data() + pos, end - pos);
            pos = end < buf.size() ? end + 1 : buf.size();
        }
    }
    // Find the EXTRASUNNY_LA section header (comment line naming it).
    size_t sec = lines.size();
    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string& ln = lines[i];
        if (!ln.empty() && ln[0] == '/' && ln.find("EXTRASUNNY_LA") != std::string::npos) {
            sec = i;
            break;
        }
    }
    if (sec == lines.size()) {
        SetErr(err, errSize, "section EXTRASUNNY_LA not found");
        return false;
    }
    // Collect the 8 data rows: non-empty lines not starting with '/'.
    // (Label lines like //Midnight and the header are skipped; the next
    // section header stops the scan.)
    struct Row {
        int v[18];
    };
    std::vector<Row> rows;
    for (size_t i = sec + 1; i < lines.size() && rows.size() < 8; ++i) {
        std::string ln = TrimRight(lines[i]);
        size_t b = 0;
        while (b < ln.size() && (ln[b] == ' ' || ln[b] == '\t')) {
            ++b;
        }
        if (b >= ln.size()) {
            continue; // blank
        }
        if (ln[b] == '/') {
            // Weather separators look like `//////////// SUNNY_LA` (4+
            // slashes); label lines like `//Midnight` are just skipped.
            int slashes = 0;
            while (b + static_cast<size_t>(slashes) < ln.size() &&
                   ln[b + static_cast<size_t>(slashes)] == '/') {
                ++slashes;
            }
            if (slashes >= 4 && rows.size() > 0) {
                break; // next weather section
            }
            continue;
        }
        for (char& c : ln) {
            if (c == ',') {
                c = ' '; // same sanitization as CFileLoader::LoadLine
            }
        }
        Row row{};
        int n = std::sscanf(ln.c_str(), "%d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d",
                            &row.v[0], &row.v[1], &row.v[2], &row.v[3], &row.v[4], &row.v[5],
                            &row.v[6], &row.v[7], &row.v[8], &row.v[9], &row.v[10], &row.v[11],
                            &row.v[12], &row.v[13], &row.v[14], &row.v[15], &row.v[16],
                            &row.v[17]);
        if (n != 18) {
            continue; // not a data row (never invent values: skip, like the
                      // game's n<51 warning path skips bad lines)
        }
        rows.push_back(row);
    }
    if (rows.size() != 8) {
        char msg[128];
        (void)std::snprintf(msg, sizeof(msg), "EXTRASUNNY_LA: want 8 rows, got %d",
                            static_cast<int>(rows.size()));
        SetErr(err, errSize, msg);
        return false;
    }
    const int idx = SampleForHour(hour);
    const Row& row = rows[static_cast<size_t>(idx)];
    for (int k = 0; k < 18; ++k) {
        if (row.v[k] < 0 || row.v[k] > 255) {
            SetErr(err, errSize, "timecyc value out of 0-255 range");
            return false;
        }
    }
    out.hour = hour;
    out.sampleIdx = idx;
    (void)std::snprintf(out.sampleName, sizeof(out.sampleName), "%s", kSampleNames[idx]);
    // Column order per CTimeCycle::Initialise: Amb(0-2), Amb_Obj(3-5),
    // Dir(6-8), SkyTop(9-11), SkyBot(12-14), SunCore(15-17).
    for (int k = 0; k < 3; ++k) {
        out.amb[k] = static_cast<uint8_t>(row.v[k]);
        out.dir[k] = static_cast<uint8_t>(row.v[6 + k]);
        out.skyTop[k] = static_cast<uint8_t>(row.v[9 + k]);
        out.skyBot[k] = static_cast<uint8_t>(row.v[12 + k]);
        out.sunCore[k] = static_cast<uint8_t>(row.v[15 + k]);
    }
    return true;
}
