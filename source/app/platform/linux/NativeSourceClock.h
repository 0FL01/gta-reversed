// Owned deterministic copy of the source timer/calendar phase used by the
// native track. No OS clock, no Godot/RW, no CStats, no serialization.
// Source profile (parent-reviewed, not a blind copy):
//   Timer.cpp:40-78 (Initialise), 86-114 (Suspend/Resume/Stop),
//     186-231 (UpdateVariables/Update);
//   Clock.cpp:27-86 (Initialise/Update), 122-162 (NormaliseGameClock),
//     212-224 (SetGameClock);
//   Game.cpp unpaused phase: CClock::Update runs only when !paused.
// Honest limits: strict binary32 with -fno-fast-math -ffp-contract=off; no
// x87 parity claim. FPS +inf on zero prior increment is a valid diagnostic,
// not JSON. GameMs horizon rejects before uint32 wrap (session horizon); NC
// and Pause accumulators wrap (documented source uint32 behaviour). Fresh
// NonClippedStep is an owned 0 with an explicit label: source Initialise
// never assigns it. No frame-partition invariance claim. No Restore hook.
#pragma once

#include <cstdint>

enum class NativeSourceClockStatus : std::uint8_t {
    Ok,
    NotInitialised,
    InvalidConfig,
    BackwardTick,
    Suspended,
    UnbalancedResume,
    StopWhileSuspended,
    InvalidScale,
    Overflow,
};

struct NativeSourceClockConfig {
    // Ticks per millisecond (default 1e6: caller ticks are nanoseconds).
    std::uint32_t DividerCyclesPerMs = 1000000u;
    std::uint32_t MsPerGameMinute = 1000u;
};

// Plain read-only snapshot. operator== compares members only, never padding.
struct NativeSourceClockState {
    bool Initialised = false;
    std::uint32_t GameMs = 0;
    std::uint32_t PauseMs = 1;
    std::uint32_t NonClippedMs = 1;
    std::uint32_t PrevGameMs = 0;
    std::uint32_t PrevPrevGameMs = 0;
    std::uint32_t PrevPrevPrevGameMs = 0;
    std::uint32_t PrevPrevPrevPrevGameMs = 0;
    std::uint32_t PrevNonClippedMs = 0;
    std::uint32_t FrameCounter = 0;
    float Fps = 0.0f;
    float TimeStep = 1.0f;
    float OldTimeStep = 1.0f;
    float NonClippedStep = 0.0f;
    // True only when NonClippedStep is still the fresh owned zero that source
    // Initialise never assigns. False once a Tick assigns it or a reinit
    // retains a previously assigned value.
    bool NonClippedStepIsOwnedZero = true;
    float TimeScale = 1.0f;
    bool UserPause = false;
    bool CodePause = false;
    bool SnapshotActive = false;
    bool DebugEnabled = false;
    std::uint32_t SuspendDepth = 0;
    std::uint64_t RenderStartTick = 0;
    std::uint64_t PauseTick = 0;
    std::uint64_t LastObservedTick = 0;
    std::uint8_t Month = 1;
    std::uint8_t Day = 1;
    std::uint8_t Weekday = 4;
    std::uint8_t Hours = 12;
    std::uint8_t Minutes = 0;
    std::uint16_t Seconds = 0;
    std::uint32_t LastClockTickGameMs = 0;
    std::uint32_t DaysPassed = 0;
    // Setter observation only: incremented by SetGameClock, never by Tick.
    std::uint64_t CalendarRevision = 0;
    bool FreezeTime = false;
    bool FasterClock = false;
    bool AlwaysMidnight = false;
    bool OrangeSky = false;
    std::uint32_t Divider = 1000000u;
    std::uint32_t MsPerGameMinute = 1000u;
    bool operator==(const NativeSourceClockState&) const = default;
};

// Plain read-only last-frame trace. Distinct 300ms game cap versus the 60ms
// equivalent physics-step cap (Step 3.0). No 60Hz carry/fraction correction.
struct NativeSourceClockLastFrame {
    bool Valid = false;
    std::uint64_t TickNow = 0;
    std::uint64_t TickPrev = 0;
    std::uint64_t RawDeltaTicks = 0;
    float RawFloatDelta = 0.0f;
    // Scaled before the pause zeroing; PausedDelta is 0 when paused.
    float ScaledDelta = 0.0f;
    float PausedDelta = 0.0f;
    std::uint32_t PauseIncrement = 0;
    float FrameDeltaMs = 0.0f;
    std::uint32_t NonClippedIncrement = 0;
    std::uint32_t ClippedIncrement = 0;
    float NonClippedStepRaw = 0.0f;
    float NonClippedStepFloored = 0.0f;
    bool FloorApplied = false;
    float OldStep = 1.0f;
    float Step = 1.0f;
    float Fps = 0.0f;
    std::uint32_t FpsDenominator = 0;
    bool Paused = false;
    bool CalendarAdvanced = false;
    bool MinuteAdvanced = false;
    bool operator==(const NativeSourceClockLastFrame&) const = default;
};

class NativeSourceClock {
public:
    explicit NativeSourceClock(NativeSourceClockConfig config = {});
    NativeSourceClock(const NativeSourceClock&) = delete;
    NativeSourceClock& operator=(const NativeSourceClock&) = delete;
    NativeSourceClock(NativeSourceClock&&) = delete;
    NativeSourceClock& operator=(NativeSourceClock&&) = delete;

    // All methods are noexcept and leave the entire state/trace unchanged on
    // any non-Ok status. No OS clock is read; the caller supplies every tick.
    // Initialise resets every defined source field except NonClippedStep:
    // fresh owners get owned 0, later reinits retain the prior value/label.
    // Reinit is allowed and resets suspend depth. Zero divider/ms-per-minute
    // is InvalidConfig.
    NativeSourceClockStatus Initialise(std::uint64_t tick) noexcept;
    // Equal tick is valid (zero delta). Only a backward tick (< last
    // observed) is BackwardTick. Tick while suspended is Suspended. Float
    // division results are validated finite/nonnegative/<2^32 before any
    // uint32 cast; GameMs overflow is rejected (no silent discard, no wrap).
    NativeSourceClockStatus Tick(std::uint64_t tick) noexcept;
    NativeSourceClockStatus SetUserPause(bool paused) noexcept;
    NativeSourceClockStatus SetCodePause(bool paused) noexcept;
    // Finite and >= 0; 0 is valid (freezes scaled time). NaN/inf/negative is
    // InvalidScale.
    NativeSourceClockStatus SetTimeScale(float scale) noexcept;
    NativeSourceClockStatus SetSnapshotActive(bool active) noexcept;
    NativeSourceClockStatus SetFreezeTime(bool freeze) noexcept;
    NativeSourceClockStatus SetFasterClock(bool faster) noexcept;
    NativeSourceClockStatus SetAlwaysMidnight(bool midnight) noexcept;
    NativeSourceClockStatus SetOrangeSky(bool orange) noexcept;
    // Debug guard is false until the first successful Tick: Suspend/Resume
    // are no-op Ok while false. Once enabled, Suspend nests; the first pins
    // PauseTick and the last rebases RenderStart += now - PauseTick (overflow
    // checked). Unbalanced Resume while enabled is UnbalancedResume.
    NativeSourceClockStatus Suspend(std::uint64_t tick) noexcept;
    NativeSourceClockStatus Resume(std::uint64_t tick) noexcept;
    // Source Stop samples no time: pins the 4 clipped histories plus PrevNC
    // to current, clears Debug, leaves RenderStart. Rejected while suspended.
    NativeSourceClockStatus Stop() noexcept;
    // Copies uint8 inputs; nonzero day sets weekday AND increments the
    // calendar day; Seconds=0; LastTick=GameMs; then exact source Normalise
    // (unsigned narrowing, day>31 unlike Update, month>12 resets to 1,
    // weekday not adjusted by hour carry). Bumps CalendarRevision only.
    NativeSourceClockStatus SetGameClock(std::uint8_t hours, std::uint8_t minutes, std::uint8_t day) noexcept;

    const NativeSourceClockState& State() const noexcept { return m_State; }
    const NativeSourceClockLastFrame& LastFrame() const noexcept { return m_Trace; }
    NativeSourceClockConfig Config() const noexcept { return m_Config; }

private:
    NativeSourceClockConfig m_Config;
    NativeSourceClockState m_State;
    NativeSourceClockLastFrame m_Trace;
    // Retained across Initialise: source never assigns NonClippedStep.
    float m_RetainedNonClippedStep = 0.0f;
    bool m_RetainedOwnedZero = true;
    bool m_HasEverInitialised = false;
};
