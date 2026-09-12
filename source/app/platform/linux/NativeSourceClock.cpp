// See NativeSourceClock.h for the owned source profile and honest limits.
#include "NativeSourceClock.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

constexpr float kFpsNumerator = 1000.0f;
constexpr float kGameCapMs = 300.0f;
constexpr float kStepLenMs = 20.0f;
constexpr float kFloorStep = 0.01f;
constexpr float kMinStep = 0.00001f;
constexpr float kMaxStep = 3.0f;
constexpr float kUint32Limit = 4294967296.0f;
constexpr std::uint8_t kDaysInMonth[12] = {31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

bool CastableToUint32(float value) {
    return std::isfinite(value) && value >= 0.0f && value < kUint32Limit;
}

// Exact source NormaliseGameClock (Clock.cpp:122-162) with unsigned
// narrowing. day>31 (not >=) differs from the Update cascade; month>12
// resets to 1; weekday is untouched by hour carry.
void NormaliseClock(std::uint16_t& seconds, std::uint8_t& minutes, std::uint8_t& hours,
    std::uint8_t& days, std::uint8_t& month) {
    if (seconds >= 60) {
        const unsigned leftMins = static_cast<unsigned>(seconds / 60);
        minutes = static_cast<std::uint8_t>(static_cast<unsigned>(minutes) + leftMins);
        seconds = static_cast<std::uint16_t>(seconds % 60);
    }
    if (minutes >= 60) {
        const unsigned leftHours = static_cast<unsigned>(minutes) / 60u;
        hours = static_cast<std::uint8_t>(static_cast<unsigned>(hours) + leftHours);
        minutes = static_cast<std::uint8_t>(static_cast<unsigned>(minutes) % 60u);
    }
    if (hours >= 24) {
        const unsigned leftDays = static_cast<unsigned>(hours) / 24u;
        days = static_cast<std::uint8_t>(static_cast<unsigned>(days) + leftDays);
        hours = static_cast<std::uint8_t>(static_cast<unsigned>(hours) % 24u);
    }
    if (days > 31) {
        days = 1;
        month = static_cast<std::uint8_t>(static_cast<unsigned>(month) + 1u);
    }
    if (month > 12) {
        month = 1;
    }
}

} // namespace

NativeSourceClock::NativeSourceClock(NativeSourceClockConfig config) : m_Config(config) {
    m_State.Divider = config.DividerCyclesPerMs;
    m_State.MsPerGameMinute = config.MsPerGameMinute;
}

NativeSourceClockStatus NativeSourceClock::Initialise(std::uint64_t tick) noexcept {
    if (m_Config.DividerCyclesPerMs == 0 || m_Config.MsPerGameMinute == 0) {
        return NativeSourceClockStatus::InvalidConfig;
    }
    float retainedStep = 0.0f;
    bool retainedOwnedZero = true;
    if (m_HasEverInitialised) {
        retainedStep = m_RetainedNonClippedStep;
        retainedOwnedZero = m_RetainedOwnedZero;
    }
    NativeSourceClockState fresh;
    fresh.Initialised = true;
    fresh.GameMs = 0;
    fresh.PauseMs = 1;
    fresh.NonClippedMs = 1;
    fresh.PrevGameMs = 0;
    fresh.PrevPrevGameMs = 0;
    fresh.PrevPrevPrevGameMs = 0;
    fresh.PrevPrevPrevPrevGameMs = 0;
    fresh.PrevNonClippedMs = 0;
    fresh.FrameCounter = 0;
    fresh.Fps = 0.0f;
    fresh.TimeStep = 1.0f;
    fresh.OldTimeStep = 1.0f;
    fresh.NonClippedStep = retainedStep;
    fresh.NonClippedStepIsOwnedZero = retainedOwnedZero;
    fresh.TimeScale = 1.0f;
    fresh.UserPause = false;
    fresh.CodePause = false;
    fresh.SnapshotActive = false;
    fresh.DebugEnabled = false;
    fresh.SuspendDepth = 0;
    fresh.RenderStartTick = tick;
    fresh.PauseTick = 0;
    fresh.LastObservedTick = tick;
    fresh.Month = 1;
    fresh.Day = 1;
    fresh.Weekday = 4;
    fresh.Hours = 12;
    fresh.Minutes = 0;
    fresh.Seconds = 0;
    fresh.LastClockTickGameMs = 0;
    fresh.DaysPassed = 0;
    fresh.CalendarRevision = 0;
    fresh.FreezeTime = false;
    fresh.FasterClock = false;
    fresh.AlwaysMidnight = false;
    fresh.OrangeSky = false;
    fresh.Divider = m_Config.DividerCyclesPerMs;
    fresh.MsPerGameMinute = m_Config.MsPerGameMinute;
    m_State = fresh;
    m_Trace = NativeSourceClockLastFrame{};
    m_HasEverInitialised = true;
    return NativeSourceClockStatus::Ok;
}

NativeSourceClockStatus NativeSourceClock::Tick(std::uint64_t tick) noexcept {
    if (!m_State.Initialised) {
        return NativeSourceClockStatus::NotInitialised;
    }
    if (m_Config.DividerCyclesPerMs == 0 || m_Config.MsPerGameMinute == 0) {
        return NativeSourceClockStatus::InvalidConfig;
    }
    if (m_State.SuspendDepth != 0) {
        return NativeSourceClockStatus::Suspended;
    }
    if (tick < m_State.LastObservedTick) {
        return NativeSourceClockStatus::BackwardTick;
    }
    // FPS before history shift (Timer.cpp:210).
    const std::uint32_t fpsDenom = m_State.NonClippedMs - m_State.PrevNonClippedMs;
    const float fps = kFpsNumerator / static_cast<float>(fpsDenom);
    if (tick < m_State.RenderStartTick) {
        return NativeSourceClockStatus::BackwardTick;
    }
    const std::uint64_t rawDelta = tick - m_State.RenderStartTick;
    const float rawFloat = static_cast<float>(rawDelta);
    if (!std::isfinite(rawFloat) || rawFloat < 0.0f) {
        return NativeSourceClockStatus::Overflow;
    }
    const bool paused = m_State.UserPause || m_State.CodePause;
    float scaled = rawFloat;
    if (!paused) {
        scaled = rawFloat * m_State.TimeScale;
        if (!std::isfinite(scaled) || scaled < 0.0f) {
            return NativeSourceClockStatus::Overflow;
        }
    }
    const float floatDivider = static_cast<float>(m_Config.DividerCyclesPerMs);
    const float pauseQuotient = scaled / floatDivider;
    if (!CastableToUint32(pauseQuotient)) {
        return NativeSourceClockStatus::Overflow;
    }
    const std::uint32_t pauseIncrement = static_cast<std::uint32_t>(pauseQuotient);
    const float pausedDelta = paused ? 0.0f : scaled;
    const float frameDelta = pausedDelta / floatDivider;
    if (!CastableToUint32(frameDelta) && frameDelta != 0.0f) {
        // frameDelta==0 is always castable; otherwise require <2^32.
        if (!std::isfinite(frameDelta) || frameDelta < 0.0f || frameDelta >= kUint32Limit) {
            return NativeSourceClockStatus::Overflow;
        }
    }
    if (!std::isfinite(frameDelta) || frameDelta < 0.0f || frameDelta >= kUint32Limit) {
        return NativeSourceClockStatus::Overflow;
    }
    const std::uint32_t nonClippedIncrement = static_cast<std::uint32_t>(frameDelta);
    const float capped = std::min(frameDelta, kGameCapMs);
    if (!std::isfinite(capped) || capped < 0.0f || capped >= kUint32Limit) {
        return NativeSourceClockStatus::Overflow;
    }
    const std::uint32_t clippedIncrement = static_cast<std::uint32_t>(capped);
    if (clippedIncrement > (std::numeric_limits<std::uint32_t>::max)() - m_State.GameMs) {
        return NativeSourceClockStatus::Overflow;
    }
    float stepRaw = frameDelta / kStepLenMs;
    if (!std::isfinite(stepRaw) || stepRaw < 0.0f) {
        return NativeSourceClockStatus::Overflow;
    }
    bool floorApplied = false;
    float stepFloored = stepRaw;
    if (!paused && !m_State.SnapshotActive && stepFloored < kFloorStep) {
        stepFloored = kFloorStep;
        floorApplied = true;
    }
    const float oldStep = m_State.TimeStep;
    float step = stepFloored;
    if (step > kMaxStep) {
        step = kMaxStep;
    } else if (step < kMinStep) {
        step = kMinStep;
    }

    // Calendar phase only when unpaused (Game.cpp unpaused block).
    bool calendarAdvanced = false;
    bool minuteAdvanced = false;
    std::uint8_t newMonth = m_State.Month;
    std::uint8_t newDay = m_State.Day;
    std::uint8_t newWeekday = m_State.Weekday;
    std::uint8_t newHours = m_State.Hours;
    std::uint8_t newMinutes = m_State.Minutes;
    std::uint16_t newSeconds = m_State.Seconds;
    std::uint32_t newLastTick = m_State.LastClockTickGameMs;
    std::uint32_t newDaysPassed = m_State.DaysPassed;
    const std::uint32_t gameNew = m_State.GameMs + clippedIncrement;
    if (!paused) {
        calendarAdvanced = true;
        if (m_State.FreezeTime) {
            newLastTick = gameNew;
        } else {
            const std::uint32_t diffBefore = gameNew - m_State.LastClockTickGameMs;
            const bool trigger =
                (m_Config.MsPerGameMinute < diffBefore) || m_State.FasterClock;
            if (trigger && !m_State.AlwaysMidnight && !m_State.OrangeSky) {
                newMinutes = static_cast<std::uint8_t>(static_cast<unsigned>(newMinutes) + 1u);
                newLastTick = m_State.LastClockTickGameMs + m_Config.MsPerGameMinute;
                if (m_State.FasterClock) {
                    newLastTick = gameNew;
                }
                minuteAdvanced = true;
                if (newMinutes >= 60) {
                    newMinutes = 0;
                    newHours = static_cast<std::uint8_t>(static_cast<unsigned>(newHours) + 1u);
                    if (newHours >= 24) {
                        newHours = 0;
                        newDay = static_cast<std::uint8_t>(static_cast<unsigned>(newDay) + 1u);
                        newWeekday = (newWeekday == 7) ? 1 : static_cast<std::uint8_t>(static_cast<unsigned>(newWeekday) + 1u);
                        ++newDaysPassed;
                        if (newMonth >= 1 && newMonth <= 12) {
                            if (newDay >= kDaysInMonth[newMonth - 1]) {
                                newDay = 1;
                                newMonth = static_cast<std::uint8_t>(static_cast<unsigned>(newMonth) + 1u);
                                if (newMonth > 12) {
                                    newMonth = 1;
                                }
                            }
                        }
                    }
                }
            }
            const std::uint32_t diffAfter = gameNew - newLastTick;
            const std::uint32_t numer = diffAfter * 60u;
            const std::uint32_t quot = numer / m_Config.MsPerGameMinute;
            newSeconds = static_cast<std::uint16_t>(quot);
        }
        if (m_State.FreezeTime) {
            newSeconds = 0;
        }
    }

    NativeSourceClockState next = m_State;
    next.PrevPrevPrevPrevGameMs = m_State.PrevPrevPrevGameMs;
    next.PrevPrevPrevGameMs = m_State.PrevPrevGameMs;
    next.PrevPrevGameMs = m_State.PrevGameMs;
    next.PrevGameMs = m_State.GameMs;
    next.PrevNonClippedMs = m_State.NonClippedMs;
    next.GameMs = gameNew;
    next.PauseMs = m_State.PauseMs + pauseIncrement;
    next.NonClippedMs = m_State.NonClippedMs + nonClippedIncrement;
    next.Fps = fps;
    next.OldTimeStep = oldStep;
    next.TimeStep = step;
    next.NonClippedStep = stepFloored;
    next.NonClippedStepIsOwnedZero = false;
    next.FrameCounter = m_State.FrameCounter + 1u;
    next.DebugEnabled = true;
    next.RenderStartTick = tick;
    next.LastObservedTick = tick;
    next.Month = newMonth;
    next.Day = newDay;
    next.Weekday = newWeekday;
    next.Hours = newHours;
    next.Minutes = newMinutes;
    next.Seconds = newSeconds;
    next.LastClockTickGameMs = newLastTick;
    next.DaysPassed = newDaysPassed;

    NativeSourceClockLastFrame trace;
    trace.Valid = true;
    trace.TickNow = tick;
    trace.TickPrev = m_State.RenderStartTick;
    trace.RawDeltaTicks = rawDelta;
    trace.RawFloatDelta = rawFloat;
    trace.ScaledDelta = scaled;
    trace.PausedDelta = pausedDelta;
    trace.PauseIncrement = pauseIncrement;
    trace.FrameDeltaMs = frameDelta;
    trace.NonClippedIncrement = nonClippedIncrement;
    trace.ClippedIncrement = clippedIncrement;
    trace.NonClippedStepRaw = stepRaw;
    trace.NonClippedStepFloored = stepFloored;
    trace.FloorApplied = floorApplied;
    trace.OldStep = oldStep;
    trace.Step = step;
    trace.Fps = fps;
    trace.FpsDenominator = fpsDenom;
    trace.Paused = paused;
    trace.CalendarAdvanced = calendarAdvanced;
    trace.MinuteAdvanced = minuteAdvanced;

    m_State = next;
    m_Trace = trace;
    m_RetainedNonClippedStep = stepFloored;
    m_RetainedOwnedZero = false;
    return NativeSourceClockStatus::Ok;
}

NativeSourceClockStatus NativeSourceClock::SetUserPause(bool paused) noexcept {
    if (!m_State.Initialised) {
        return NativeSourceClockStatus::NotInitialised;
    }
    m_State.UserPause = paused;
    return NativeSourceClockStatus::Ok;
}

NativeSourceClockStatus NativeSourceClock::SetCodePause(bool paused) noexcept {
    if (!m_State.Initialised) {
        return NativeSourceClockStatus::NotInitialised;
    }
    m_State.CodePause = paused;
    return NativeSourceClockStatus::Ok;
}

NativeSourceClockStatus NativeSourceClock::SetTimeScale(float scale) noexcept {
    if (!m_State.Initialised) {
        return NativeSourceClockStatus::NotInitialised;
    }
    if (!std::isfinite(scale) || scale < 0.0f) {
        return NativeSourceClockStatus::InvalidScale;
    }
    m_State.TimeScale = scale;
    return NativeSourceClockStatus::Ok;
}

NativeSourceClockStatus NativeSourceClock::SetSnapshotActive(bool active) noexcept {
    if (!m_State.Initialised) {
        return NativeSourceClockStatus::NotInitialised;
    }
    m_State.SnapshotActive = active;
    return NativeSourceClockStatus::Ok;
}

NativeSourceClockStatus NativeSourceClock::SetFreezeTime(bool freeze) noexcept {
    if (!m_State.Initialised) {
        return NativeSourceClockStatus::NotInitialised;
    }
    m_State.FreezeTime = freeze;
    return NativeSourceClockStatus::Ok;
}

NativeSourceClockStatus NativeSourceClock::SetFasterClock(bool faster) noexcept {
    if (!m_State.Initialised) {
        return NativeSourceClockStatus::NotInitialised;
    }
    m_State.FasterClock = faster;
    return NativeSourceClockStatus::Ok;
}

NativeSourceClockStatus NativeSourceClock::SetAlwaysMidnight(bool midnight) noexcept {
    if (!m_State.Initialised) {
        return NativeSourceClockStatus::NotInitialised;
    }
    m_State.AlwaysMidnight = midnight;
    return NativeSourceClockStatus::Ok;
}

NativeSourceClockStatus NativeSourceClock::SetOrangeSky(bool orange) noexcept {
    if (!m_State.Initialised) {
        return NativeSourceClockStatus::NotInitialised;
    }
    m_State.OrangeSky = orange;
    return NativeSourceClockStatus::Ok;
}

NativeSourceClockStatus NativeSourceClock::Suspend(std::uint64_t tick) noexcept {
    if (!m_State.Initialised) {
        return NativeSourceClockStatus::NotInitialised;
    }
    if (!m_State.DebugEnabled) {
        return NativeSourceClockStatus::Ok;
    }
    if (tick < m_State.LastObservedTick) {
        return NativeSourceClockStatus::BackwardTick;
    }
    if (m_State.SuspendDepth == (std::numeric_limits<std::uint32_t>::max)()) {
        return NativeSourceClockStatus::Overflow;
    }
    const bool first = (m_State.SuspendDepth == 0);
    ++m_State.SuspendDepth;
    if (first) {
        m_State.PauseTick = tick;
    }
    m_State.LastObservedTick = tick;
    return NativeSourceClockStatus::Ok;
}

NativeSourceClockStatus NativeSourceClock::Resume(std::uint64_t tick) noexcept {
    if (!m_State.Initialised) {
        return NativeSourceClockStatus::NotInitialised;
    }
    if (!m_State.DebugEnabled) {
        return NativeSourceClockStatus::Ok;
    }
    if (m_State.SuspendDepth == 0) {
        return NativeSourceClockStatus::UnbalancedResume;
    }
    if (tick < m_State.LastObservedTick) {
        return NativeSourceClockStatus::BackwardTick;
    }
    if (tick < m_State.PauseTick) {
        return NativeSourceClockStatus::BackwardTick;
    }
    const std::uint64_t delta = tick - m_State.PauseTick;
    const bool last = (m_State.SuspendDepth == 1);
    if (last) {
        if (delta > (std::numeric_limits<std::uint64_t>::max)() - m_State.RenderStartTick) {
            return NativeSourceClockStatus::Overflow;
        }
    }
    --m_State.SuspendDepth;
    m_State.LastObservedTick = tick;
    if (last) {
        m_State.RenderStartTick = m_State.RenderStartTick + delta;
    }
    return NativeSourceClockStatus::Ok;
}

NativeSourceClockStatus NativeSourceClock::Stop() noexcept {
    if (!m_State.Initialised) {
        return NativeSourceClockStatus::NotInitialised;
    }
    if (m_State.SuspendDepth != 0) {
        return NativeSourceClockStatus::StopWhileSuspended;
    }
    m_State.PrevPrevPrevPrevGameMs = m_State.GameMs;
    m_State.PrevPrevPrevGameMs = m_State.GameMs;
    m_State.PrevPrevGameMs = m_State.GameMs;
    m_State.PrevGameMs = m_State.GameMs;
    m_State.PrevNonClippedMs = m_State.NonClippedMs;
    m_State.DebugEnabled = false;
    return NativeSourceClockStatus::Ok;
}

NativeSourceClockStatus NativeSourceClock::SetGameClock(
    std::uint8_t hours, std::uint8_t minutes, std::uint8_t day) noexcept {
    if (!m_State.Initialised) {
        return NativeSourceClockStatus::NotInitialised;
    }
    std::uint16_t seconds = 0;
    std::uint8_t newMinutes = minutes;
    std::uint8_t newHours = hours;
    std::uint8_t newDays = m_State.Day;
    std::uint8_t newMonth = m_State.Month;
    std::uint8_t newWeekday = m_State.Weekday;
    if (day != 0) {
        newWeekday = day;
        newDays = static_cast<std::uint8_t>(static_cast<unsigned>(newDays) + 1u);
    }
    NormaliseClock(seconds, newMinutes, newHours, newDays, newMonth);
    m_State.LastClockTickGameMs = m_State.GameMs;
    m_State.Hours = newHours;
    m_State.Minutes = newMinutes;
    m_State.Seconds = seconds;
    m_State.Day = newDays;
    m_State.Month = newMonth;
    m_State.Weekday = newWeekday;
    ++m_State.CalendarRevision;
    return NativeSourceClockStatus::Ok;
}
