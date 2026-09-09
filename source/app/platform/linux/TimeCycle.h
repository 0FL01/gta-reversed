// TimeCycle: timecyc.dat daylight for the Linux native track (R6m/R6r).
// Parses data/timecyc.dat through OS_File* (any named weather section) and
// maps a clock hour 0-23 to one of the 8 stored samples. The sample table is
// the game's own (CTimeCycle::Update TimeSamples {0,5,6,7,12,19,20,22,24}):
// the floor sample is used as-is, no interpolation in this round. Every RGB
// below comes from timecyc bytes; there are no hardcoded colors in this TU.
#pragma once

#include <cstddef>
#include <cstdint>

struct TimeCycleParams {
    int hour = -1; // requested clock hour 0-23
    int sampleIdx = -1; // 0-7 into the section rows
    char sampleName[16] = {}; // e.g. "Midnight", "7AM", "Midday"
    uint8_t amb[3] = {}; // Amb (static ambient RGB)
    uint8_t dir[3] = {}; // Dir (directional/sun RGB)
    uint8_t skyTop[3] = {}; // Sky top RGB
    uint8_t skyBot[3] = {}; // Sky bot RGB
    uint8_t sunCore[3] = {}; // SunCore RGB
    float farClp = 0.0f; // FarClp (tokens[27] per header, timecyc bytes only)
    float fogSt = 0.0f; // FogSt (tokens[28] per header, timecyc bytes only)
};

// Loads the `weather` row for `hour` (0-23) relative to gameDir (sets the
// OS_File path offset itself, like SceneShot). `weather` is the section name
// without the `////////////` prefix (e.g. "CLOUDY_LA"). Returns false with a
// message in err on any failure (missing file/section/row, bad values, hour
// range, bad weather name). If a section holds N!=8 rows, the R6m floor map
// still applies with the index clamped to N-1 (all shipped sections hold 8).
bool TimeCycle_LoadWeatherHour(const char* gameDir, const char* weather, int hour,
                               TimeCycleParams& out, char* err, std::size_t errSize);

// Loads the EXTRASUNNY_LA row for `hour` (0-23) relative to gameDir (sets the
// OS_File path offset itself, like SceneShot). Returns false with a message
// in err on any failure (missing file/section/row, bad values, hour range).
// Bit-for-bit wrapper over TimeCycle_LoadWeatherHour(gameDir,
// "EXTRASUNNY_LA", hour, ...): the default/--hour path without --weather.
bool TimeCycle_LoadHour(const char* gameDir, int hour, TimeCycleParams& out, char* err,
                        std::size_t errSize);
