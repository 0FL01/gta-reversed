#include "app/platform/linux/NativeScriptWeather.h"

#include <limits>

NativeScriptServiceResult NativeScriptWeather::ForceNow(std::int32_t weather) {
    if (weather < 0 || weather > 22) {
        return {NativeScriptServiceStatus::Error, "weather type is outside source range"};
    }
    if (m_State.Revision == std::numeric_limits<std::uint64_t>::max()) {
        return {NativeScriptServiceStatus::Error, "weather revision exhausted"};
    }
    m_State.Forced = weather;
    m_State.Old = weather;
    m_State.New = weather;
    ++m_State.Revision;
    return {NativeScriptServiceStatus::Ready, {}};
}

NativeScriptServiceResult NativeScriptWeather::Release() {
    if (m_State.Revision == std::numeric_limits<std::uint64_t>::max()) {
        return {NativeScriptServiceStatus::Error, "weather revision exhausted"};
    }
    m_State.Forced = -1;
    ++m_State.Revision;
    return {NativeScriptServiceStatus::Ready, {}};
}
