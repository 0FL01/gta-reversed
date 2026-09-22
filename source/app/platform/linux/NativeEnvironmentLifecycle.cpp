#include "NativeEnvironmentLifecycle.h"

#include <algorithm>
#include <cmath>

namespace {
struct WeatherProfile {
    const char* Name;
    float Wind;
    float Rain;
    float Fog;
    float Cloud;
    float ExtraSunny;
};

constexpr std::array<WeatherProfile, 23> Profiles{{
    {"EXTRASUNNY_LA",0,0,0,0,1},{"SUNNY_LA",.25f,0,0,0,0},{"EXTRASUNNY_SMOG_LA",0,0,0,0,1},
    {"SUNNY_SMOG_LA",.2f,0,0,0,0},{"CLOUDY_LA",.7f,0,0,1,0},{"SUNNY_SF",.25f,0,0,0,0},
    {"EXTRASUNNY_SF",0,0,0,0,1},{"CLOUDY_SF",.7f,0,0,1,0},{"RAINY_SF",1,1,0,1,0},
    {"FOGGY_SF",0,0,1,1,0},{"SUNNY_VEGAS",.2f,0,0,0,0},{"EXTRASUNNY_VEGAS",0,0,0,0,1},
    {"CLOUDY_VEGAS",.4f,0,0,1,0},{"EXTRASUNNY_COUNTRYSIDE",0,0,0,0,1},
    {"SUNNY_COUNTRYSIDE",.3f,0,0,0,0},{"CLOUDY_COUNTRYSIDE",.7f,0,0,1,0},
    {"RAINY_COUNTRYSIDE",1,1,0,1,0},{"EXTRASUNNY_DESERT",0,0,0,0,1},
    {"SUNNY_DESERT",.3f,0,0,0,0},{"SANDSTORM_DESERT",1.5f,0,1,1,0},
    {"UNDERWATER",0,0,0,1,0},{"EXTRACOLOURS_1",0,0,0,1,0},{"EXTRACOLOURS_2",0,0,0,1,0},
}};

float Mix(float a, float b, float t) { return a * (1.0f - t) + b * t; }

NativeEnvironmentStatus Fail(NativeEnvironmentStatus status, const char* text, std::string& error) {
    error = text;
    return status;
}
}

NativeWeatherRegion NativeEnvironmentLifecycle::FindRegion(float x, float y) noexcept {
    if (x > 1000.0f && y > 910.0f) return NativeWeatherRegion::LasVenturas;
    if (x > -850.0f && x < 1000.0f && y > 1280.0f) return NativeWeatherRegion::Desert;
    if (x < -1430.0f && y > -580.0f && y < 1430.0f) return NativeWeatherRegion::SanFierro;
    if (x > 250.0f && x < 3000.0f && y > -3000.0f && y < -850.0f)
        return NativeWeatherRegion::LosSantos;
    return NativeWeatherRegion::Default;
}

NativeEnvironmentStatus NativeEnvironmentLifecycle::Initialize(const char* gameDir, float x, float y,
    float hour, std::int32_t weather, std::string& error) {
    if (!gameDir || !*gameDir) return Fail(NativeEnvironmentStatus::InvalidInput, "game root is empty", error);
    m_GameDir = gameDir;
    return Build(x, y, hour, weather, weather, 0.0f, 0, 0.0f, {}, error);
}

NativeEnvironmentStatus NativeEnvironmentLifecycle::Advance(float x, float y, float hour,
    std::int32_t oldWeather, std::int32_t newWeather, float interpolation, std::uint32_t gameMs,
    float timeStep, std::array<float, 2> currentFlow, std::string& error) {
    if (!m_Published) return Fail(NativeEnvironmentStatus::NotLoaded, "environment is not initialized", error);
    return Build(x, y, hour, oldWeather, newWeather, interpolation, gameMs, timeStep, currentFlow, error);
}

NativeEnvironmentStatus NativeEnvironmentLifecycle::Build(float x, float y, float hour,
    std::int32_t oldWeather, std::int32_t newWeather, float interpolation, std::uint32_t gameMs,
    float timeStep, std::array<float, 2> currentFlow, std::string& error) {
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(hour) || hour < 0 || hour >= 24 ||
        oldWeather < 0 || oldWeather >= std::int32_t(Profiles.size()) ||
        newWeather < 0 || newWeather >= std::int32_t(Profiles.size()) ||
        !std::isfinite(interpolation) || interpolation < 0 || interpolation > 1 ||
        !std::isfinite(timeStep) || timeStep < 0 || timeStep > 3 ||
        !std::isfinite(currentFlow[0]) || !std::isfinite(currentFlow[1]))
        return Fail(NativeEnvironmentStatus::InvalidInput, "environment transition input is invalid", error);
    TimeCycleParams oldParams{}, newParams{};
    char sourceError[256]{};
    const auto clockHour = int(std::floor(hour));
    if (!TimeCycle_LoadWeatherHour(m_GameDir.c_str(), Profiles[oldWeather].Name, clockHour,
            oldParams, sourceError, sizeof(sourceError)) ||
        !TimeCycle_LoadWeatherHour(m_GameDir.c_str(), Profiles[newWeather].Name, clockHour,
            newParams, sourceError, sizeof(sourceError)))
        return Fail(NativeEnvironmentStatus::SourceUnavailable, sourceError, error);
    auto next = std::make_shared<NativeEnvironmentSnapshot>();
    if (m_Published) *next = *m_Published;
    next->Generation = m_Published ? m_Published->Generation + 1 : 1;
    next->Region = FindRegion(x, y);
    next->OldWeather = oldWeather; next->NewWeather = newWeather;
    next->Interpolation = interpolation; next->Hour = hour; next->GameMs = gameMs;
    for (std::size_t i = 0; i < 3; ++i) {
        next->Ambient[i] = Mix(float(oldParams.amb[i]), float(newParams.amb[i]), interpolation) / 255.0f;
        next->Directional[i] = Mix(float(oldParams.dir[i]), float(newParams.dir[i]), interpolation) / 255.0f;
        next->SkyTop[i] = Mix(float(oldParams.skyTop[i]), float(newParams.skyTop[i]), interpolation) / 255.0f;
        next->SkyBottom[i] = Mix(float(oldParams.skyBot[i]), float(newParams.skyBot[i]), interpolation) / 255.0f;
        next->LowCloudColours[i] = std::uint8_t(std::clamp(Mix(float(oldParams.lowCloudColours[i]),
            float(newParams.lowCloudColours[i]), interpolation), 0.0f, 255.0f));
    }
    for (std::size_t i = 0; i < 4; ++i)
        next->Water[i] = Mix(float(oldParams.water[i]), float(newParams.water[i]), interpolation) / 255.0f;
    next->FarClip = Mix(oldParams.farClp, newParams.farClp, interpolation);
    next->FogStart = Mix(oldParams.fogSt, newParams.fogSt, interpolation);
    const auto& oldProfile = Profiles[oldWeather]; const auto& newProfile = Profiles[newWeather];
    next->Wind = Mix(oldProfile.Wind, newProfile.Wind, interpolation);
    next->Rain = Mix(oldProfile.Rain, newProfile.Rain, interpolation);
    next->Foggyness = Mix(oldProfile.Fog, newProfile.Fog, interpolation);
    next->CloudCoverage = Mix(oldProfile.Cloud, newProfile.Cloud, interpolation);
    next->ExtraSunnyness = Mix(oldProfile.ExtraSunny, newProfile.ExtraSunny, interpolation);
    next->WaterWavyness = std::min(next->Wind + 0.3f, 1.0f);
    for (std::size_t axis = 0; axis < 2; ++axis) {
        const double distance = double(timeStep) * double(.04f) * double(currentFlow[axis]);
        next->FirstFlowUv[axis] = float(double(next->FirstFlowUv[axis]) + distance * double(.08f));
        next->SecondFlowUv[axis] = float(double(next->SecondFlowUv[axis]) + distance * double(.04f));
        if (next->FirstFlowUv[axis] > 1.0f) next->FirstFlowUv[axis] -= 1.0f;
        if (next->SecondFlowUv[axis] > 1.0f) next->SecondFlowUv[axis] -= 1.0f;
    }
    if (m_NextEvent == 0) return Fail(NativeEnvironmentStatus::Overflow, "environment event sequence exhausted", error);
    next->Events.push_back({m_NextEvent++, next->Region, oldWeather, newWeather,
        interpolation, hour, gameMs});
    m_Published = std::move(next);
    error.clear();
    return NativeEnvironmentStatus::Ok;
}
