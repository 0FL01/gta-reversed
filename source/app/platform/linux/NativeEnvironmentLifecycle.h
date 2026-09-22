#pragma once

#include "TimeCycle.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

enum class NativeWeatherRegion : std::uint8_t { Default, LosSantos, SanFierro, LasVenturas, Desert };
enum class NativeEnvironmentStatus : std::uint8_t { Ok, NotLoaded, InvalidInput, SourceUnavailable, Overflow };

struct NativeEnvironmentEvent {
    std::uint64_t Sequence = 0;
    NativeWeatherRegion Region = NativeWeatherRegion::Default;
    std::int32_t OldWeather = 0, NewWeather = 0;
    float Interpolation = 0.0f, Hour = 0.0f;
    std::uint32_t GameMs = 0;
    bool operator==(const NativeEnvironmentEvent&) const = default;
};

struct NativeEnvironmentSnapshot {
    std::uint64_t Generation = 0;
    NativeWeatherRegion Region = NativeWeatherRegion::Default;
    std::int32_t OldWeather = 0, NewWeather = 0;
    float Interpolation = 0.0f, Hour = 0.0f;
    std::uint32_t GameMs = 0;
    std::array<float, 3> Ambient{}, Directional{}, SkyTop{}, SkyBottom{};
    std::array<std::uint8_t, 3> LowCloudColours{};
    std::array<float, 4> Water{};
    float FarClip = 0.0f, FogStart = 0.0f;
    float Wind = 0.0f, Rain = 0.0f, Foggyness = 0.0f, CloudCoverage = 0.0f, ExtraSunnyness = 0.0f;
    float WaterWavyness = 0.3f;
    std::array<float, 2> FirstFlowUv{}, SecondFlowUv{};
    std::vector<NativeEnvironmentEvent> Events;
    bool PresentationFeedback = false;
    bool operator==(const NativeEnvironmentSnapshot&) const = default;
};

class NativeEnvironmentLifecycle {
public:
    NativeEnvironmentStatus Initialize(const char* gameDir, float x, float y, float hour,
        std::int32_t weather, std::string& error);
    NativeEnvironmentStatus Advance(float x, float y, float hour, std::int32_t oldWeather,
        std::int32_t newWeather, float interpolation, std::uint32_t gameMs,
        float timeStep, std::array<float, 2> currentFlow, std::string& error);
    std::shared_ptr<const NativeEnvironmentSnapshot> LastCommitted() const { return m_Published; }
    static NativeWeatherRegion FindRegion(float x, float y) noexcept;
    static constexpr bool OwnsFrustum = false;
    static constexpr bool OwnsCloudGeometry = false;
    static constexpr bool OwnsWaterGeometry = false;

private:
    NativeEnvironmentStatus Build(float x, float y, float hour, std::int32_t oldWeather,
        std::int32_t newWeather, float interpolation, std::uint32_t gameMs,
        float timeStep, std::array<float, 2> currentFlow, std::string& error);

    std::string m_GameDir;
    std::shared_ptr<const NativeEnvironmentSnapshot> m_Published;
    std::uint64_t m_NextEvent = 1;
};
