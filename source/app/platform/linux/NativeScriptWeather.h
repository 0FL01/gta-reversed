#pragma once

#include "app/platform/linux/NativeScriptSession.h"

#include <cstdint>

struct NativeScriptWeatherState {
    std::int32_t Forced = -1;
    std::int32_t Old = 0;
    std::int32_t New = 0;
    std::uint64_t Revision = 0;
    bool operator==(const NativeScriptWeatherState&) const = default;
};

class NativeScriptWeather {
public:
    NativeScriptServiceResult ForceNow(std::int32_t weather);
    NativeScriptServiceResult Release();
    const NativeScriptWeatherState& State() const { return m_State; }

private:
    NativeScriptWeatherState m_State;
};
