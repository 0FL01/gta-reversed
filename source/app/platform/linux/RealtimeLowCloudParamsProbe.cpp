// Isolated actual environment + parser; synthetic timecyc stays in memory.
// Owned water.dat/particle.txd are opened read-only through test OS_File shims.
#include "app/platform/linux/RealtimeEnvironment.cpp"
#include "RealtimeLowCloudParamsProbe.source.h"

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

bool LegacyTimeCycle_LoadWeatherHour(const char*, const char*, int, TimeCycleParams&, char*, size_t);

static std::string s_GameDir, s_Timecyc;
static bool s_Synthetic = false, s_ReadError = false, s_OpenError = false;
static unsigned s_IO = 0;
static int s_MemoryFile;

void OS_SetFilePathOffset(const char* path) { ++s_IO; s_GameDir = path ? path : ""; }
int32 OS_FileOpen(OSFileDataArea area, void** file, const char* path, OSFileAccessType access) {
    ++s_IO;
    if (!file || !path || area != FILE_DATA_AREA_DEFAULT || access != FILE_ACCESS_READ) return 1;
    *file = nullptr;
    if (s_OpenError) return 1;
    if (s_Synthetic && !std::strcmp(path, "data/timecyc.dat")) {
        *file = &s_MemoryFile;
    } else {
        *file = std::fopen((s_GameDir + "/" + path).c_str(), "rb");
    }
    return *file ? 0 : 1;
}
int32 OS_FileSize(void* file) {
    ++s_IO;
    if (file == &s_MemoryFile) return static_cast<int32>(s_Timecyc.size());
    auto* fp = static_cast<FILE*>(file);
    const auto position = std::ftell(fp);
    if (position < 0 || std::fseek(fp, 0, SEEK_END)) return -1;
    const auto size = std::ftell(fp);
    if (std::fseek(fp, position, SEEK_SET)) return -1;
    return size >= 0 && size <= INT32_MAX ? static_cast<int32>(size) : -1;
}
int32 OS_FileRead(void* file, void* destination, int32 size) {
    ++s_IO;
    if (s_ReadError || size < 0) return 1;
    if (file == &s_MemoryFile) {
        if (static_cast<size_t>(size) > s_Timecyc.size()) return 1;
        std::memcpy(destination, s_Timecyc.data(), size);
        return 0;
    }
    return std::fread(destination, 1, size, static_cast<FILE*>(file)) == static_cast<size_t>(size) ? 0 : 1;
}
int32 OS_FileClose(void* file) {
    ++s_IO;
    return file == &s_MemoryFile ? 0 : std::fclose(static_cast<FILE*>(file));
}

namespace probe {
static void Require(bool condition, const char* message) {
    if (!condition) { std::fprintf(stderr, "low-cloud-params-probe-fail %s\n", message); std::exit(1); }
}

using Colours = std::array<uint8_t, 3>;
using Rows = std::array<Colours, 8>;
using Table = std::array<Rows, 23>;

static Table TestColours() {
    Table table{};
    for (size_t w = 0; w < table.size(); ++w) for (size_t h = 0; h < 8; ++h)
        for (size_t c = 0; c < 3; ++c) table[w][h][c] = (w * 53 + h * h * 29 + c * 97) % 256;
    return table;
}

// Entirely synthetic rows: no owned asset bytes are written or copied.
static std::string Fixture(const Table& table, int badWeather = -1, int badRow = -1,
    const char* badToken = nullptr, int omitWeather = -1, int rowCount = 8) {
    std::ostringstream text;
    for (size_t w = 0; w < table.size(); ++w) {
        if (static_cast<int>(w) == omitWeather) continue;
        text << "//////////// " << source_low_clouds::Names[w] << '\n';
        const int count = static_cast<int>(w) == badWeather ? rowCount : 8;
        for (int h = 0; h < count; ++h) {
            for (int token = 0; token < 52; ++token) {
                if (token == 31 && static_cast<int>(w) == badWeather && h == badRow && badToken) text << badToken;
                else if (token >= 30 && token <= 32) text << unsigned(table[w][h % 8][token - 30]);
                else if (token == 27) text << "1000";
                else if (token == 28) text << "100";
                else if (token == 51) text << "1";
                else text << (token * 7 + h * 3) % 256;
                text << (token == 51 ? '\n' : ' ');
            }
        }
    }
    return text.str();
}

// Independent source arithmetic: CColourSet stores a uint16 after each sum.
// The provider outputs bytes because source inputs and convex sums are 0..255.
static unsigned Interpolate(unsigned a, unsigned b, float wa, float wb) {
    const volatile float left = float(a) * wa;
    const volatile float right = float(b) * wb;
    return static_cast<uint16_t>(left + right);
}

static Colours Reference(const Table& table, size_t weather, float hour, float z, bool truncateAnchors = true) {
    hour = std::min(hour, 23.999f);
    const auto upper = std::upper_bound(source_low_clouds::Hours.begin(), source_low_clouds::Hours.end(), hour);
    const size_t row = size_t(upper - source_low_clouds::Hours.begin() - 1), next = (row + 1) % 8;
    const float t = (hour - source_low_clouds::Hours[row]) /
        float(source_low_clouds::Hours[row + 1] - source_low_clouds::Hours[row]);
    const float altitude = std::clamp((z - 20.0f) / 200.0f, 0.0f, 1.0f);
    Colours out{};
    for (size_t c = 0; c < 3; ++c) {
        float a = table[weather][row][c], b = table[weather][next][c];
        if (weather == 2 || weather == 3) {
            if (truncateAnchors) {
                a = float(Interpolate(unsigned(a), table[weather - 2][row][c], 1 - altitude, altitude));
                b = float(Interpolate(unsigned(b), table[weather - 2][next][c], 1 - altitude, altitude));
            } else {
                a = a * (1 - altitude) + table[weather - 2][row][c] * altitude;
                b = b * (1 - altitude) + table[weather - 2][next][c] * altitude;
            }
        }
        const volatile float first = a * (1 - t), second = b * t;
        out[c] = static_cast<uint16_t>(first + second);
    }
    return out;
}

static std::string LegacyFields(const TimeCycleParams& p) {
    std::ostringstream text;
    text << p.hour << ' ' << p.sampleIdx << ' ' << p.sampleName << ' ';
    const auto bytes = [&](const auto& values) { for (auto v : values) text << unsigned(v) << ','; };
    bytes(p.amb); bytes(p.dir); bytes(p.skyTop); bytes(p.skyBot); bytes(p.sunCore);
    bytes(p.water); bytes(p.ambObjects);
    text << std::hexfloat << p.farClp << ' ' << p.fogSt << ' ' << p.directionalMult << ' ' << p.hasDirectionalMult;
    return text.str();
}

static uint64_t s_LegacyHash = 14695981039346656037ull;
static unsigned s_LegacyCases = 0;
static void CheckLegacy(const char* gameDir) {
    for (const char* name : source_low_clouds::Names) for (int hour = 0; hour < 24; ++hour) {
        TimeCycleParams old{}, current{};
        char oldError[128]{}, error[128]{};
        const bool before = LegacyTimeCycle_LoadWeatherHour(gameDir, name, hour, old, oldError, sizeof(oldError));
        const bool after = TimeCycle_LoadWeatherHour(gameDir, name, hour, current, error, sizeof(error));
        Require(before == after && !std::strcmp(oldError, error), "legacy parser status/error changed");
        const auto serialized = LegacyFields(current);
        Require(serialized == LegacyFields(old), "legacy renderer/water timecyc fields changed");
        for (const unsigned char byte : serialized) s_LegacyHash = (s_LegacyHash ^ byte) * 1099511628211ull;
        ++s_LegacyCases;
    }
}

static RealtimeLowCloudParams Sentinel() { return {3, 987, {12, 34, 56}, .1f, .2f, .3f, .4f}; }
static void Unavailable(const RealtimeEnvironment& environment, float z = 30) {
    auto out = Sentinel();
    Require(!environment.GetFixedWeatherLowCloudParams(z, out), "unavailable provider accepted");
    Require(out == Sentinel(), "false getter mutated output");
}

static void Synthetic(const char* gameDir) {
    const auto table = TestColours();
    s_Synthetic = true;
    s_Timecyc = Fixture(table);
    CheckLegacy(gameDir);
    std::vector<float> hours;
    for (int i = 0; i < 96; ++i) hours.push_back(i / 4.0f);
    for (int anchor : source_low_clouds::Hours) {
        if (anchor > 0) hours.push_back(std::nextafter(float(anchor), 0.0f));
        if (anchor < 24) hours.push_back(std::nextafter(float(anchor), 24.0f));
    }
    hours.push_back(23.9999f);
    const float altitudes[]{-1000, 0, 19.99f, 20, 20.01f, 37, 70, 120, 163, 219.99f, 220, 220.01f, 10000};
    unsigned cases = 0, truncationWitnesses = 0;
    RealtimeEnvironment environment;
    Unavailable(environment);
    Require(!environment.SetHour(12), "unloaded SetHour accepted");
    char err[256]{};
    for (size_t weather = 0; weather < table.size(); ++weather) {
        Require(environment.Load(gameDir, err, sizeof(err), 12, source_low_clouds::Names[weather]), err);
        const float wind = source_low_clouds::Wind[weather];
        Require(environment.GetWaterState().wavyness == std::min(std::min(wind, 1.0f) + .3f, 1.0f), "water wind mapping changed");
        const auto io = s_IO;
        for (float hour : hours) {
            Require(environment.SetHour(hour), "valid SetHour");
            auto water = environment.GetWaterState();
            water.gameMs = 0xfffffff0u + cases; // includes source uint32 clock wrap
            water.waterTimeOffset = 1234567; // getter must use gameMs, not wave phase
            Require(environment.SetWaterState(water), "water clock publication");
            for (float z : altitudes) {
                RealtimeLowCloudParams out;
                Require(environment.GetFixedWeatherLowCloudParams(z, out), "valid getter rejected");
                Require(out.hour == hour && out.gameMs == water.gameMs, "wrong live clock");
                Require(out.colours == Reference(table, weather, hour, z), "source RGB interpolation");
                Require(out.foggyness == ((weather == 9 || weather == 19) ? 1 : 0), "source fog factor");
                Require(out.cloudCoverage == source_low_clouds::Cloudy[weather], "source coverage factor");
                Require(out.extraSunnyness == source_low_clouds::ExtraSunny[weather], "source extrasunny factor");
                Require(out.wind == wind, "source raw wind (sandstorm must remain 1.5)");
                truncationWitnesses += out.colours != Reference(table, weather, hour, z, false);
                ++cases;
            }
        }
        RealtimeLowCloudParams before, after;
        Require(environment.GetFixedWeatherLowCloudParams(70, before), "before invalid hour");
        for (float bad : {-1.0f, 24.0f, INFINITY, -INFINITY, NAN}) {
            Require(!environment.SetHour(bad), "invalid SetHour accepted");
            Require(environment.GetFixedWeatherLowCloudParams(70, after) && before == after, "invalid SetHour mutated provider");
        }
        for (float bad : {INFINITY, -INFINITY, NAN}) Unavailable(environment, bad);
        Require(s_IO == io, "runtime getter/hour/water-state did IO");
    }
    Require(truncationWitnesses > 0, "oracle lacks two-stage truncation witnesses");
    std::printf("synthetic-source weather=23 cases=%u two-stage-witnesses=%u runtime-io=0\n", cases, truncationWitnesses);

    // Optional bad cloud data does not invalidate existing renderer/water rows.
    for (const char* bad : {"-1", "256", "12.5", "nan", "inf", "12x", "999999999999999999999999999999999999"}) {
        for (int row = 0; row < 8; ++row) {
            s_Timecyc = Fixture(table, 2, row, bad);
            Require(environment.Load(gameDir, err, sizeof(err), 12, source_low_clouds::Names[2]), err);
            Unavailable(environment);
            s_Timecyc = Fixture(table, 0, row, bad);
            Require(environment.Load(gameDir, err, sizeof(err), 12, source_low_clouds::Names[2]), err);
            Unavailable(environment);
        }
    }
    for (int count : {7, 9}) {
        s_Timecyc = Fixture(table, 0, -1, nullptr, -1, count);
        Require(environment.Load(gameDir, err, sizeof(err), 12, source_low_clouds::Names[2]), err);
        Unavailable(environment);
    }
    s_Timecyc = Fixture(table, -1, -1, nullptr, 0);
    Require(environment.Load(gameDir, err, sizeof(err), 12, source_low_clouds::Names[2]), err);
    Unavailable(environment);
    s_Timecyc = Fixture(table);
    Require(environment.Load(gameDir, err, sizeof(err)), err);
    for (const char* bad : {"UNKNOWN", "SUNNY", "sunny_la", "", "SUNNY_LA!", static_cast<const char*>(nullptr)}) {
        Require(!environment.Load(gameDir, err, sizeof(err), 12, bad), "invalid weather accepted");
        Unavailable(environment);
    }
    for (float bad : {-1.0f, 24.0f, INFINITY, NAN}) {
        Require(!environment.Load(gameDir, err, sizeof(err), bad), "invalid Load hour accepted");
        Unavailable(environment);
    }
    Require(!environment.Load(nullptr, err, sizeof(err)), "null game dir accepted");
    Unavailable(environment);
    for (bool* failure : {&s_ReadError, &s_OpenError}) {
        *failure = true;
        Require(!environment.Load(gameDir, err, sizeof(err)), "IO failure accepted");
        *failure = false;
        Unavailable(environment);
    }
    s_Timecyc.clear();
    Require(!environment.Load(gameDir, err, sizeof(err)), "empty timecyc accepted");
    Unavailable(environment);
    s_Timecyc = Fixture(table, 1, -1, nullptr, -1, 7);
    Require(!environment.Load(gameDir, err, sizeof(err), 12, source_low_clouds::Names[1]), "short selected section accepted");
    Unavailable(environment);
    s_Timecyc = Fixture(table);
    Require(environment.Load(gameDir, err, sizeof(err)), err);
    RealtimeLowCloudParams out;
    Require(environment.GetFixedWeatherLowCloudParams(30, out), "successful reload did not restore provider");
    std::puts("errors-ok optional-selected/counterpart-rgb short/extra/missing-rows invalid-weather/hour/z io reload atomic-false");
    s_Synthetic = false;
}

static void Owned(const char* gameDir) {
    CheckLegacy(gameDir);
    // Read independently, without copying any game asset to disk.
    std::ifstream input(std::string(gameDir) + "/data/timecyc.dat");
    Require(bool(input), "owned timecyc open");
    Table table{};
    std::array<size_t, 23> counts{};
    int weather = -1;
    for (std::string line; std::getline(input, line);) {
        const auto first = line.find_first_not_of(" \t\r");
        if (first == std::string::npos) continue;
        if (line.compare(first, 4, "////") == 0) {
            auto token = line.find_first_not_of('/', first);
            std::istringstream nameStream(token == std::string::npos ? "" : line.substr(token));
            std::string name; nameStream >> name; weather = -1;
            for (size_t w = 0; w < table.size(); ++w) if (name == source_low_clouds::Names[w]) weather = int(w);
        } else if (line[first] != '/' && weather >= 0) {
            std::replace(line.begin(), line.end(), ',', ' ');
            std::istringstream values(line);
            std::vector<std::string> tokens;
            for (std::string value; values >> value;) tokens.push_back(value);
            if (tokens.size() < 33) continue;
            const auto row = counts[weather]++;
            if (row < 8) for (size_t c = 0; c < 3; ++c) table[weather][row][c] = std::stoi(tokens[30 + c]);
        }
    }
    RealtimeEnvironment environment;
    char err[256]{};
    unsigned ready = 0, cases = 0;
    for (size_t w = 0; w < table.size(); ++w) {
        if (!environment.Load(gameDir, err, sizeof(err), 12, source_low_clouds::Names[w])) {
            std::printf("owned-unavailable weather=%s reason=%s\n", source_low_clouds::Names[w], err);
            Unavailable(environment);
            continue;
        }
        RealtimeLowCloudParams out;
        if (!environment.GetFixedWeatherLowCloudParams(30, out)) {
            std::printf("owned-unavailable weather=%s reason=cloud-rows\n", source_low_clouds::Names[w]);
            continue;
        }
        Require(counts[w] == 8, "owned oracle row count");
        if (w == 2 || w == 3) Require(counts[w - 2] == 8, "owned clear oracle row count");
        ++ready;
        const auto io = s_IO;
        for (int quarter = 0; quarter < 96; ++quarter) for (float z : {0.f, 20.f, 37.f, 120.f, 219.99f, 220.f, 1000.f}) {
            const float hour = quarter / 4.0f;
            Require(environment.SetHour(hour), "owned SetHour");
            Require(environment.GetFixedWeatherLowCloudParams(z, out), "owned getter");
            Require(out.colours == Reference(table, w, hour, z), "owned source arithmetic");
            ++cases;
        }
        Require(s_IO == io, "owned runtime IO");
    }
    Require(ready >= 20, "too few owned weather sections verified");
    std::printf("owned-source weather-ready=%u cases=%u runtime-io=0\n", ready, cases);
}
} // namespace probe

int main(int argc, char** argv) {
    probe::Require(argc == 2, "usage: RealtimeLowCloudParamsProbe GAME_DIR");
    probe::Synthetic(argv[1]);
    probe::Owned(argv[1]);
    std::printf("legacy-timecyc-fields cases=%u fnv1a=%016llx baseline=%s identical\n",
        probe::s_LegacyCases, static_cast<unsigned long long>(probe::s_LegacyHash), source_low_clouds::Baseline);
    std::puts("realtime-low-cloud-params-probe-ok fixed-weather-only");
}
