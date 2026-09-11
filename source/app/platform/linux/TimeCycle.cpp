// TimeCycle implementation: named-weather daylight rows from timecyc.dat.
// See TimeCycle.h for the contract. Column layout reference (read-only, NOT
// linked): game_sa/TimeCycle.cpp CTimeCycle::Initialise sscanf order — Amb,
// Amb_Obj, Dir, SkyTop, SkyBot, SunCore, ... — and the eTimeType/TimeSamples
// table {0,5,6,7,12,19,20,22,24} in CTimeCycle::Update. Section/row labels
// (//////////// EXTRASUNNY_LA, //Midnight, //5AM, ...) are read from the
// file itself; only the 8-row/24-hour table shape is shared with the game.
// R6r: any section by exact token match (not substring: "SUNNY_LA" must not
// match "EXTRASUNNY_LA").

#include "app/platform/linux/TimeCycle.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
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

bool TimeCycle_LoadWeatherHour(const char* gameDir, const char* weather, int hour,
                               TimeCycleParams& out, char* err, std::size_t errSize) {
    out = TimeCycleParams{};
    if (!gameDir || !gameDir[0]) {
        SetErr(err, errSize, "no game dir");
        return false;
    }
    if (!weather || !weather[0]) {
        SetErr(err, errSize, "no weather section");
        return false;
    }
    for (const char* p = weather; *p; ++p) {
        const char c = *p;
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
        if (!ok) {
            SetErr(err, errSize, "bad weather name (want A-Z0-9_)");
            return false;
        }
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
    // Find the named weather section header: a comment line with 4+ leading
    // slashes whose first token after the slashes is exactly `weather`.
    size_t sec = lines.size();
    for (size_t i = 0; i < lines.size(); ++i) {
        std::string ln = TrimRight(lines[i]);
        size_t b = 0;
        while (b < ln.size() && (ln[b] == ' ' || ln[b] == '\t')) {
            ++b;
        }
        size_t s = b;
        while (s < ln.size() && ln[s] == '/') {
            ++s;
        }
        if (s - b < 4) {
            continue;
        }
        size_t t = s;
        while (t < ln.size() && (ln[t] == ' ' || ln[t] == '\t')) {
            ++t;
        }
        size_t e = t;
        while (e < ln.size() && ln[e] != ' ' && ln[e] != '\t' && ln[e] != '\r') {
            ++e;
        }
        if (t < e && ln.substr(t, e - t) == weather) {
            sec = i;
            break;
        }
    }
    if (sec == lines.size()) {
        char msg[128];
        (void)std::snprintf(msg, sizeof(msg), "section %s not found", weather);
        SetErr(err, errSize, msg);
        return false;
    }
    // Collect the data rows: non-empty lines not starting with '/'.
    // (Label lines like //Midnight and the header are skipped; the next
    // section header stops the scan.)
    struct Row {
        int v[18];
        float farClp = 0.0f; // tokens[27] (FarClp per the header comment)
        float fogSt = 0.0f; // tokens[28] (FogSt per the header comment)
        uint8_t water[4] = {}; // tokens[36..39] (WaterRGBA per header)
        float directionalMult = 0.0f;
        bool hasDirectionalMult = false;
        uint8_t lowCloudColours[3] = {};
        bool hasLowCloudColours = false;
        uint8_t postFx[2][4] = {};
        bool hasPostFx = false;
    };
    std::vector<Row> rows;
    for (size_t i = sec + 1; i < lines.size(); ++i) {
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
        // FarClp/FogSt are tokens[27]/[28] of the whitespace-split row
        // (header: ... PoleShd FarClp FogSt LightOnGround ...). Both come
        // only from these timecyc bytes; a row without them is skipped.
        // R6aa: WaterRGBA are tokens[36..39] (header: ... BottomCloudRGB
        // WaterRGBA Alpha1 ...; the game's own sscanf reads them as %f
        // waterR/waterG/waterB/waterA after the two cloud RGB triples).
        {
            std::vector<std::string> toks;
            {
                size_t p = 0;
                while (p < ln.size()) {
                    while (p < ln.size() && (ln[p] == ' ' || ln[p] == '\t')) {
                        ++p;
                    }
                    if (p >= ln.size()) {
                        break;
                    }
                    size_t q = p;
                    while (q < ln.size() && ln[q] != ' ' && ln[q] != '\t') {
                        ++q;
                    }
                    toks.emplace_back(ln.substr(p, q - p));
                    p = q;
                }
            }
            if (toks.size() < 29) {
                continue;
            }
            // CTimeCycle::Initialise reads these as integers, separately from
            // bottom-cloud RGB and cloud alpha. Keep legacy parsing unchanged
            // on bad optional cloud data; the realtime provider gates it out.
            if (toks.size() >= 33) {
                row.hasLowCloudColours = true;
                for (int k = 0; k < 3; ++k) {
                    char* end = nullptr;
                    const long value = std::strtol(toks[30 + k].c_str(), &end, 10);
                    if (end == toks[30 + k].c_str() || *end || value < 0 || value > 255) {
                        row.hasLowCloudColours = false;
                        break;
                    }
                    row.lowCloudColours[k] = static_cast<uint8_t>(value);
                }
            }
            if (toks.size() > 51) {
                char* end = nullptr;
                const float value = std::strtof(toks[51].c_str(), &end);
                row.hasDirectionalMult = end != toks[51].c_str() && !*end &&
                                         std::isfinite(value) && value >= 0 && value <= 2.55f;
                if (row.hasDirectionalMult) {
                    row.directionalMult = value;
                }
            }
            if (toks.size() >= 48) {
                row.hasPostFx = true;
                for (int pass = 0; pass < 2; ++pass) {
                    for (int k = 0; k < 4; ++k) {
                        const auto& token = toks[40 + pass * 4 + k];
                        char* end = nullptr;
                        const float value = std::strtof(token.c_str(), &end);
                        if (end == token.c_str() || *end || !std::isfinite(value) || value < 0 || value > 255) {
                            row.hasPostFx = false;
                            continue;
                        }
                        // Source order A,R,G,B; integer narrowing after alpha*2
                        // is explicit modulo 256, including PC alpha 255 -> 254.
                        row.postFx[pass][k == 0 ? 3 : k - 1] = static_cast<uint8_t>(static_cast<unsigned>(value * (k == 0 ? 2.0f : 1.0f)) & 255u);
                    }
                }
            }
            char* endFar = nullptr;
            char* endFog = nullptr;
            const float far = std::strtof(toks[27].c_str(), &endFar);
            const float fog = std::strtof(toks[28].c_str(), &endFog);
            if (!endFar || *endFar != '\0' || !endFog || *endFog != '\0' ||
                !std::isfinite(far) || !std::isfinite(fog)) {
                continue;
            }
            row.farClp = far;
            row.fogSt = fog;
            if (toks.size() < 40) {
                continue; // no WaterRGBA on this row: skip, never invent
            }
            float wrgba[4] = {};
            bool wok = true;
            for (int k = 0; k < 4; ++k) {
                char* endW = nullptr;
                const float wv = std::strtof(toks[36 + k].c_str(), &endW);
                if (!endW || *endW != '\0' || !std::isfinite(wv) || wv < 0.0f ||
                    wv > 255.0f) {
                    wok = false;
                    break;
                }
                wrgba[k] = wv;
            }
            if (!wok) {
                continue;
            }
            for (int k = 0; k < 4; ++k) {
                row.water[k] = static_cast<uint8_t>(wrgba[k]);
            }
        }
        rows.push_back(row);
    }
    if (rows.empty()) {
        char msg[128];
        (void)std::snprintf(msg, sizeof(msg), "%s: no data rows", weather);
        SetErr(err, errSize, msg);
        return false;
    }
    int idx = SampleForHour(hour);
    if (idx >= static_cast<int>(rows.size())) {
        idx = static_cast<int>(rows.size()) - 1; // floor-map clamp for N!=8
    }
    const Row& row = rows[static_cast<size_t>(idx)];
    for (int k = 0; k < 18; ++k) {
        if (row.v[k] < 0 || row.v[k] > 255) {
            SetErr(err, errSize, "timecyc value out of 0-255 range");
            return false;
        }
    }
    if (!std::isfinite(row.farClp) || !std::isfinite(row.fogSt) || row.farClp <= 0.0f ||
        !(row.farClp > row.fogSt)) {
        SetErr(err, errSize, "timecyc bad FarClp/FogSt");
        return false;
    }
    out.hour = hour;
    out.sampleIdx = idx;
    if (rows.size() == 8) {
        (void)std::snprintf(out.sampleName, sizeof(out.sampleName), "%s", kSampleNames[idx]);
    } else {
        (void)std::snprintf(out.sampleName, sizeof(out.sampleName), "row%d", idx);
    }
    // Column order per CTimeCycle::Initialise: Amb(0-2), Amb_Obj(3-5),
    // Dir(6-8), SkyTop(9-11), SkyBot(12-14), SunCore(15-17).
    for (int k = 0; k < 3; ++k) {
        out.amb[k] = static_cast<uint8_t>(row.v[k]);
        out.ambObjects[k] = static_cast<uint8_t>(row.v[3 + k]);
        out.dir[k] = static_cast<uint8_t>(row.v[6 + k]);
        out.skyTop[k] = static_cast<uint8_t>(row.v[9 + k]);
        out.skyBot[k] = static_cast<uint8_t>(row.v[12 + k]);
        out.sunCore[k] = static_cast<uint8_t>(row.v[15 + k]);
    }
    out.farClp = row.farClp;
    out.directionalMult = row.directionalMult;
    out.hasDirectionalMult = row.hasDirectionalMult;
    out.hasLowCloudColours = rows.size() == 8 && row.hasLowCloudColours;
    if (out.hasLowCloudColours) {
        for (int k = 0; k < 3; ++k) out.lowCloudColours[k] = row.lowCloudColours[k];
    }
    out.fogSt = row.fogSt;
    out.hasPostFx = rows.size() == 8 && row.hasPostFx;
    if (out.hasPostFx) {
        std::memcpy(out.postFx, row.postFx, sizeof(out.postFx));
    }
    for (int k = 0; k < 4; ++k) {
        out.water[k] = row.water[k];
    }
    return true;
}

bool TimeCycle_LoadHour(const char* gameDir, int hour, TimeCycleParams& out, char* err,
                        std::size_t errSize) {
    return TimeCycle_LoadWeatherHour(gameDir, "EXTRASUNNY_LA", hour, out, err, errSize);
}
