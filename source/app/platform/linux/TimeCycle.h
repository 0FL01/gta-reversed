// TimeCycle: timecyc.dat daylight for the Linux native track (R6m).
// Parses data/timecyc.dat through OS_File* (section EXTRASUNNY_LA only) and
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
};

// Loads the EXTRASUNNY_LA row for `hour` (0-23) relative to gameDir (sets the
// OS_File path offset itself, like SceneShot). Returns false with a message
// in err on any failure (missing file/section/row, bad values, hour range).
bool TimeCycle_LoadHour(const char* gameDir, int hour, TimeCycleParams& out, char* err,
                        std::size_t errSize);
