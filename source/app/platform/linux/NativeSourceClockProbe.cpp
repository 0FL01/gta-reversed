// P2-A02 owned source-clock probe. No OS clock, no Godot/RW/Stats, no
// serialization, no Restore hook. Uses error branches (never assert for side
// effects) so Release (NDEBUG) still executes every clock call.
#include "NativeSourceClock.h"
#include "NativeScriptSession.h"
#include "NativeSourceRng.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>
#include <tuple>
#include <vector>

namespace {

using Status = NativeSourceClockStatus;
std::size_t s_Checks = 0;

void Check(bool condition, const char* message) {
    ++s_Checks;
    if (!condition) {
        std::fprintf(stderr, "native-source-clock FAIL: %s\n", message);
        std::exit(1);
    }
}

void CheckStatus(Status got, Status want, const char* message) {
    ++s_Checks;
    if (got != want) {
        std::fprintf(stderr, "native-source-clock FAIL: %s (status %d != %d)\n",
            message, static_cast<int>(got), static_cast<int>(want));
        std::exit(1);
    }
}

void CheckNear(float got, float want, float tol, const char* message) {
    ++s_Checks;
    if (!std::isfinite(got) || !std::isfinite(want) ||
        std::fabs(got - want) > tol) {
        std::fprintf(stderr, "native-source-clock FAIL: %s (got %.9g want %.9g)\n",
            message, static_cast<double>(got), static_cast<double>(want));
        std::exit(1);
    }
}

void CheckInf(float got, const char* message) {
    ++s_Checks;
    if (!std::isinf(got) || got <= 0.0f) {
        std::fprintf(stderr, "native-source-clock FAIL: %s (got %.9g, want +inf)\n",
            message, static_cast<double>(got));
        std::exit(1);
    }
}

struct DummyServices final : NativeScriptServices {
    NativeScriptServiceResult RequestCollision(const NativeScriptCollisionRequest&) override {
        return {NativeScriptServiceStatus::Unsupported, "clock probe: no collision"};
    }
    NativeScriptServiceResult LoadScene(const NativeScriptSceneRequest&) override {
        return {NativeScriptServiceStatus::Unsupported, "clock probe: no scene"};
    }
    NativeScriptServiceResult CreatePlayer(const NativeScriptPlayerRequest&) override {
        return {NativeScriptServiceStatus::Unsupported, "clock probe: no player"};
    }
};

void CheckInitialState(const NativeSourceClockState& s) {
    Check(s.Initialised, "initialised after Initialise");
    Check(s.GameMs == 0, "initial Game 0");
    Check(s.PauseMs == 1, "initial Pause 1");
    Check(s.NonClippedMs == 1, "initial NC 1");
    Check(s.PrevGameMs == 0 && s.PrevPrevGameMs == 0, "initial clipped history 0");
    Check(s.PrevPrevPrevGameMs == 0 && s.PrevPrevPrevPrevGameMs == 0, "initial deep history 0");
    Check(s.PrevNonClippedMs == 0, "initial PrevNC 0");
    Check(s.FrameCounter == 0, "initial FrameCounter 0");
    Check(s.Fps == 0.0f, "initial FPS 0");
    Check(s.TimeStep == 1.0f && s.OldTimeStep == 1.0f, "initial step/old 1");
    Check(s.TimeScale == 1.0f, "initial scale 1");
    Check(!s.UserPause && !s.CodePause && !s.SnapshotActive, "initial pause/snapshot false");
    Check(!s.DebugEnabled, "initial debug false");
    Check(s.SuspendDepth == 0, "initial suspend depth 0");
    Check(s.Month == 1 && s.Day == 1 && s.Weekday == 4, "initial calendar Jan1 day4");
    Check(s.Hours == 12 && s.Minutes == 0 && s.Seconds == 0, "initial calendar 12:00");
    Check(s.LastClockTickGameMs == 0, "initial lastTick Game 0");
    Check(s.DaysPassed == 0 && s.CalendarRevision == 0, "initial days/revision 0");
    Check(!s.FreezeTime && !s.FasterClock && !s.AlwaysMidnight && !s.OrangeSky,
        "initial freeze/cheats false");
}

void CheckAtomicUnchanged(const NativeSourceClock& clock,
    const NativeSourceClockState& beforeState,
    const NativeSourceClockLastFrame& beforeTrace, const char* message) {
    Check(clock.State() == beforeState, message);
    Check(clock.LastFrame() == beforeTrace, message);
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2 || !argv[1] || !*argv[1]) {
        std::fprintf(stderr, "usage: sa_core_clock_probe <owned-game-dir>\n");
        return 1;
    }
    const char* gameDir = argv[1];

    // Invalid config leaves everything unchanged (never initialised).
    {
        NativeSourceClockConfig bad{};
        bad.DividerCyclesPerMs = 0;
        NativeSourceClock badClock(bad);
        CheckStatus(badClock.Initialise(0), Status::InvalidConfig, "zero divider invalid");
        Check(!badClock.State().Initialised, "bad config never initialises");
        NativeSourceClockConfig badMinute{};
        badMinute.MsPerGameMinute = 0;
        NativeSourceClock badMinuteClock(badMinute);
        CheckStatus(badMinuteClock.Initialise(0), Status::InvalidConfig, "zero msPerMin invalid");
        CheckStatus(badClock.Tick(0), Status::NotInitialised, "tick before init");
        CheckStatus(badClock.Stop(), Status::NotInitialised, "stop before init");
        CheckStatus(badClock.SetTimeScale(1.0f), Status::NotInitialised, "scale before init");
    }

    // Main ms schedule with divider=1, msPerMin=1000.
    NativeSourceClockConfig msCfg;
    msCfg.DividerCyclesPerMs = 1;
    msCfg.MsPerGameMinute = 1000;
    NativeSourceClock clock(msCfg);
    CheckStatus(clock.Initialise(0), Status::Ok, "initialise 0");
    CheckInitialState(clock.State());
    Check(clock.State().Divider == 1 && clock.State().MsPerGameMinute == 1000, "config echo");
    Check(clock.State().NonClippedStep == 0.0f, "fresh NCStep owned 0");
    Check(clock.State().NonClippedStepIsOwnedZero, "fresh owned-zero label");
    Check(!clock.LastFrame().Valid, "trace invalid before first tick");

    // First 50ms: FPS 1000, Game 50, NC 51, Step 2.5.
    CheckStatus(clock.Tick(50), Status::Ok, "tick 50");
    {
        const auto& s = clock.State();
        const auto& t = clock.LastFrame();
        Check(s.GameMs == 50 && s.NonClippedMs == 51, "first 50 game/NC");
        Check(s.PauseMs == 51, "first 50 pause");
        Check(s.Fps == 1000.0f, "first FPS 1000");
        Check(t.FpsDenominator == 1, "first FPS denom 1");
        CheckNear(s.TimeStep, 2.5f, 1e-6f, "first step 2.5");
        CheckNear(s.NonClippedStep, 2.5f, 1e-6f, "first NCStep 2.5");
        Check(s.OldTimeStep == 1.0f, "first old step 1");
        Check(s.FrameCounter == 1, "first frame 1");
        Check(s.DebugEnabled, "debug after first tick");
        Check(s.PrevGameMs == 0 && s.PrevNonClippedMs == 1, "first history shift");
        Check(t.RawDeltaTicks == 50 && t.PauseIncrement == 50, "first trace raw/pause");
        Check(t.NonClippedIncrement == 50 && t.ClippedIncrement == 50, "first trace increments");
        Check(!t.FloorApplied && !t.Paused && t.CalendarAdvanced && !t.MinuteAdvanced,
            "first trace flags");
        Check(s.Seconds == 3, "first 50ms seconds 3");
        Check(s.Minutes == 0 && s.LastClockTickGameMs == 0, "no minute yet");
        Check(!s.NonClippedStepIsOwnedZero, "owned-zero cleared by tick");
    }

    // Next 50ms: FPS 20.
    CheckStatus(clock.Tick(100), Status::Ok, "tick 100");
    {
        const auto& s = clock.State();
        Check(s.Fps == 20.0f, "second FPS 20");
        Check(s.GameMs == 100 && s.NonClippedMs == 101, "second game/NC");
        CheckNear(s.TimeStep, 2.5f, 1e-6f, "second step 2.5");
        Check(s.Seconds == 6, "second seconds 6");
    }

    // +1000 hitch: Game +300 (cap), NC +1000, Step 3, NCStep 50, no minute.
    CheckStatus(clock.Tick(1100), Status::Ok, "hitch tick 1100");
    {
        const auto& s = clock.State();
        const auto& t = clock.LastFrame();
        Check(s.GameMs == 400 && s.NonClippedMs == 1101, "hitch game/NC");
        Check(t.ClippedIncrement == 300 && t.NonClippedIncrement == 1000, "hitch clipped/raw");
        CheckNear(t.NonClippedStepRaw, 50.0f, 1e-6f, "hitch NCStep 50");
        Check(s.TimeStep == 3.0f, "hitch step capped 3");
        Check(s.OldTimeStep == 2.5f, "hitch old step 2.5");
        Check(s.Minutes == 0 && s.LastClockTickGameMs == 0, "no minute until clipped>1000");
        Check(s.Seconds == 24, "hitch seconds 24");
    }

    // Exact 1000: seconds 60, still no minute; then +1 minute.
    CheckStatus(clock.Tick(1400), Status::Ok, "tick 1400");
    Check(clock.State().GameMs == 700, "game 700");
    CheckStatus(clock.Tick(1700), Status::Ok, "tick 1700 game 1000");
    {
        const auto& s = clock.State();
        Check(s.GameMs == 1000, "exact 1000 game");
        Check(s.Seconds == 60, "exact 1000 seconds 60");
        Check(s.Minutes == 0 && s.LastClockTickGameMs == 0, "exact threshold no minute yet");
    }
    CheckStatus(clock.Tick(1701), Status::Ok, "tick 1701 minute");
    {
        const auto& s = clock.State();
        Check(s.GameMs == 1001, "post-threshold game");
        Check(s.Minutes == 1 && s.LastClockTickGameMs == 1000, "one minute, LastTick+=1000");
        Check(s.Seconds == 0, "seconds after minute 0");
    }

    // 1.5ms truncates to 1, step .075 (divider=10).
    {
        NativeSourceClockConfig fracCfg;
        fracCfg.DividerCyclesPerMs = 10;
        fracCfg.MsPerGameMinute = 1000;
        NativeSourceClock frac(fracCfg);
        CheckStatus(frac.Initialise(0), Status::Ok, "frac init");
        CheckStatus(frac.Tick(15), Status::Ok, "frac 1.5ms");
        Check(frac.State().GameMs == 1 && frac.State().NonClippedMs == 2, "1.5ms trunc 1");
        CheckNear(frac.State().NonClippedStep, 0.075f, 1e-6f, "1.5ms step .075");
        CheckNear(frac.LastFrame().FrameDeltaMs, 1.5f, 1e-6f, "1.5ms frameDelta");
    }

    // .1ms floor .01 versus snapshot .005; equal tick valid.
    {
        NativeSourceClockConfig tenthCfg;
        tenthCfg.DividerCyclesPerMs = 10;
        tenthCfg.MsPerGameMinute = 1000;
        NativeSourceClock tenth(tenthCfg);
        CheckStatus(tenth.Initialise(0), Status::Ok, "tenth init");
        CheckStatus(tenth.Tick(1), Status::Ok, "tenth 0.1ms");
        Check(tenth.State().GameMs == 0 && tenth.State().NonClippedMs == 1, ".1ms trunc 0");
        CheckNear(tenth.State().NonClippedStep, 0.01f, 1e-7f, ".1ms floored .01");
        Check(tenth.LastFrame().FloorApplied, ".1ms floor flag");
        CheckStatus(tenth.SetSnapshotActive(true), Status::Ok, "snapshot on");
        CheckStatus(tenth.Tick(2), Status::Ok, "snapshot 0.1ms");
        CheckNear(tenth.State().NonClippedStep, 0.005f, 1e-7f, "snapshot .005");
        // Equal tick is valid zero delta.
        const auto before = tenth.State();
        CheckStatus(tenth.Tick(2), Status::Ok, "equal tick valid");
        Check(tenth.State().GameMs == before.GameMs, "equal tick no game advance");
        Check(tenth.State().FrameCounter == before.FrameCounter + 1u, "equal tick counts frame");
        CheckStatus(tenth.SetSnapshotActive(false), Status::Ok, "snapshot off");
    }

    // Pause freezes Game/NC/calendar but Pause still increments; later FPS inf.
    {
        NativeSourceClock paused(msCfg);
        CheckStatus(paused.Initialise(0), Status::Ok, "pause init");
        CheckStatus(paused.Tick(50), Status::Ok, "pause base tick");
        CheckStatus(paused.SetUserPause(true), Status::Ok, "user pause on");
        const auto before = paused.State();
        const auto beforeTrace = paused.LastFrame();
        (void)beforeTrace;
        CheckStatus(paused.Tick(100), Status::Ok, "paused tick");
        Check(paused.State().GameMs == before.GameMs, "paused game frozen");
        Check(paused.State().NonClippedMs == before.NonClippedMs, "paused NC frozen");
        Check(paused.State().PauseMs == before.PauseMs + 50u, "paused Pause raw increments");
        Check(paused.State().Minutes == before.Minutes, "paused calendar frozen");
        Check(paused.State().Seconds == before.Seconds, "paused seconds frozen");
        Check(paused.LastFrame().Paused && !paused.LastFrame().CalendarAdvanced, "paused trace flags");
        CheckStatus(paused.SetUserPause(false), Status::Ok, "user pause off");
        CheckStatus(paused.SetCodePause(true), Status::Ok, "code pause on");
        CheckStatus(paused.Tick(150), Status::Ok, "code-paused tick");
        Check(paused.State().GameMs == before.GameMs, "code-paused game frozen");
        CheckStatus(paused.SetCodePause(false), Status::Ok, "code pause off");
        // After two paused frames NC==PrevNC so the next FPS denominator is 0.
        CheckStatus(paused.Tick(200), Status::Ok, "post-pause tick");
        CheckInf(paused.LastFrame().Fps, "post-pause FPS inf diagnostic");
        CheckInf(paused.State().Fps, "post-pause state FPS inf");
    }

    // TimeScale .5 / 0 and invalid scales are atomic.
    {
        NativeSourceClock scaled(msCfg);
        CheckStatus(scaled.Initialise(0), Status::Ok, "scale init");
        CheckStatus(scaled.SetTimeScale(0.5f), Status::Ok, "scale .5");
        CheckStatus(scaled.Tick(100), Status::Ok, "scaled tick");
        Check(scaled.State().GameMs == 50, "scale .5 halves game");
        CheckStatus(scaled.SetTimeScale(0.0f), Status::Ok, "scale 0 valid");
        CheckStatus(scaled.Tick(200), Status::Ok, "scale 0 tick");
        Check(scaled.State().GameMs == 50, "scale 0 freezes game");
        Check(scaled.State().PauseMs == 51u, "scale 0 pause frozen");
        const auto before = scaled.State();
        const auto beforeTrace = scaled.LastFrame();
        CheckStatus(scaled.SetTimeScale(-1.0f), Status::InvalidScale, "negative scale");
        CheckAtomicUnchanged(scaled, before, beforeTrace, "negative scale atomic");
        CheckStatus(scaled.SetTimeScale(std::numeric_limits<float>::infinity()),
            Status::InvalidScale, "inf scale");
        CheckAtomicUnchanged(scaled, before, beforeTrace, "inf scale atomic");
        CheckStatus(scaled.SetTimeScale(std::numeric_limits<float>::quiet_NaN()),
            Status::InvalidScale, "nan scale");
        CheckAtomicUnchanged(scaled, before, beforeTrace, "nan scale atomic");
        CheckStatus(scaled.SetTimeScale(1.0f), Status::Ok, "scale restore");
    }

    // Suspend/Resume rebase: Tick200, Suspend200, Resume1200, Tick1250 => delta 50.
    {
        NativeSourceClock sr(msCfg);
        CheckStatus(sr.Initialise(0), Status::Ok, "sr init");
        // Suspend/Resume before the first tick are no-op Ok (debug guard false).
        CheckStatus(sr.Suspend(0), Status::Ok, "pre-tick suspend no-op");
        CheckStatus(sr.Resume(0), Status::Ok, "pre-tick resume no-op");
        Check(sr.State().SuspendDepth == 0, "pre-tick depth stays 0");
        CheckStatus(sr.Tick(200), Status::Ok, "sr tick 200");
        Check(sr.State().GameMs == 200, "sr game 200");
        CheckStatus(sr.Suspend(200), Status::Ok, "sr suspend 200");
        Check(sr.State().SuspendDepth == 1, "sr depth 1");
        // Nested suspend pins only the first tick.
        CheckStatus(sr.Suspend(300), Status::Ok, "sr nested suspend");
        Check(sr.State().SuspendDepth == 2, "sr depth 2");
        Check(sr.State().PauseTick == 200, "first suspend pins PauseTick");
        const auto suspendedState = sr.State();
        const auto suspendedTrace = sr.LastFrame();
        CheckStatus(sr.Tick(400), Status::Suspended, "tick while suspended");
        CheckAtomicUnchanged(sr, suspendedState, suspendedTrace, "suspended tick atomic");
        CheckStatus(sr.Resume(150), Status::BackwardTick, "backward resume");
        CheckAtomicUnchanged(sr, suspendedState, suspendedTrace, "backward resume atomic");
        CheckStatus(sr.Resume(500), Status::Ok, "sr resume nested");
        Check(sr.State().SuspendDepth == 1, "sr depth back to 1");
        Check(sr.State().RenderStartTick == 200, "nested resume does not rebase");
        CheckStatus(sr.Resume(1200), Status::Ok, "sr resume 1200");
        Check(sr.State().SuspendDepth == 0, "sr depth 0");
        Check(sr.State().RenderStartTick == 1200, "last resume rebases to 1200");
        CheckStatus(sr.Tick(1250), Status::Ok, "sr tick 1250");
        Check(sr.LastFrame().RawDeltaTicks == 50, "rebase delta 50 not 1050");
        Check(sr.State().GameMs == 250, "rebase game 250");
        CheckStatus(sr.Resume(1300), Status::UnbalancedResume, "unbalanced resume");
        const auto afterState = sr.State();
        const auto afterTrace = sr.LastFrame();
        CheckAtomicUnchanged(sr, afterState, afterTrace, "unbalanced resume atomic");
        CheckStatus(sr.Tick(1200), Status::BackwardTick, "backward tick");
        CheckAtomicUnchanged(sr, afterState, afterTrace, "backward tick atomic");
        // Stop while suspended is an explicit boundary.
        CheckStatus(sr.Suspend(1300), Status::Ok, "sr suspend again");
        CheckStatus(sr.Stop(), Status::StopWhileSuspended, "stop while suspended");
        Check(sr.State().SuspendDepth == 1, "failed stop keeps depth");
        CheckStatus(sr.Resume(1400), Status::Ok, "sr resume to clear");
    }

    // Stop pins histories + PrevNC, clears debug, leaves RenderStart.
    {
        NativeSourceClock st(msCfg);
        CheckStatus(st.Initialise(10), Status::Ok, "stop init");
        CheckStatus(st.Tick(60), Status::Ok, "stop tick");
        const std::uint64_t renderStart = st.State().RenderStartTick;
        const std::uint32_t game = st.State().GameMs;
        const std::uint32_t clipped = st.State().NonClippedMs;
        CheckStatus(st.Stop(), Status::Ok, "stop ok");
        Check(!st.State().DebugEnabled, "stop clears debug");
        Check(st.State().PrevGameMs == game, "stop pins Prev");
        Check(st.State().PrevPrevGameMs == game, "stop pins PPrev");
        Check(st.State().PrevPrevPrevGameMs == game, "stop pins PPPrev");
        Check(st.State().PrevPrevPrevPrevGameMs == game, "stop pins PPPPrev");
        Check(st.State().PrevNonClippedMs == clipped, "stop pins PrevNC");
        Check(st.State().RenderStartTick == renderStart, "stop leaves RenderStart");
        Check(st.State().GameMs == game && st.State().NonClippedMs == clipped, "stop keeps clocks");
        // Suspend/Resume are no-op again once debug is false.
        CheckStatus(st.Suspend(70), Status::Ok, "post-stop suspend no-op");
        CheckStatus(st.Resume(80), Status::Ok, "post-stop resume no-op");
        // Next tick recomputes FPS from the pinned (equal) histories => +inf.
        CheckStatus(st.Tick(110), Status::Ok, "post-stop tick");
        CheckInf(st.State().Fps, "post-stop FPS inf");
    }

    // Reinit retains prior NCStep; fresh is owned 0.
    {
        NativeSourceClock re(msCfg);
        CheckStatus(re.Initialise(0), Status::Ok, "reinit first");
        Check(re.State().NonClippedStepIsOwnedZero, "reinit fresh owned zero");
        CheckStatus(re.Tick(50), Status::Ok, "reinit tick");
        CheckNear(re.State().NonClippedStep, 2.5f, 1e-6f, "reinit step 2.5");
        CheckStatus(re.Initialise(500), Status::Ok, "reinit second");
        CheckNear(re.State().NonClippedStep, 2.5f, 1e-6f, "reinit retains NCStep");
        Check(!re.State().NonClippedStepIsOwnedZero, "retained label not owned-zero");
        Check(re.State().GameMs == 0 && re.State().FrameCounter == 0, "reinit resets game/frame");
        Check(!re.State().DebugEnabled && re.State().SuspendDepth == 0, "reinit resets debug/depth");
        Check(re.State().RenderStartTick == 500, "reinit render start");
    }

    // SetGameClock: copies, nonzero day bumps weekday+calendar day, Normalise.
    {
        NativeSourceClock sc(msCfg);
        CheckStatus(sc.Initialise(0), Status::Ok, "setclock init");
        CheckStatus(sc.Tick(100), Status::Ok, "setclock tick");
        const std::uint32_t game = sc.State().GameMs;
        CheckStatus(sc.SetGameClock(8, 30, 5), Status::Ok, "set 8:30 day5");
        Check(sc.State().Hours == 8 && sc.State().Minutes == 30, "set copies time");
        Check(sc.State().Weekday == 5 && sc.State().Day == 2, "nonzero day sets weekday+day");
        Check(sc.State().Seconds == 0, "set zeroes seconds");
        Check(sc.State().LastClockTickGameMs == game, "set LastTick=Game");
        Check(sc.State().CalendarRevision == 1, "setter bumps revision");
        // Exact source Normalise: 25:70 overflows minutes->hours->days.
        CheckStatus(sc.SetGameClock(25, 70, 0), Status::Ok, "normalise 25:70");
        Check(sc.State().Hours == 2 && sc.State().Minutes == 10, "normalise hours/minutes");
        Check(sc.State().Day == 3, "normalise hour carry bumps day");
        Check(sc.State().Weekday == 5, "hour carry does not touch weekday");
        Check(sc.State().CalendarRevision == 2, "second setter revision 2");
    }

    // Calendar quirks: quirky >= month roll, Feb 29 table, year wrap, and the
    // blocked/faster/freeze Seconds behaviour.
    {
        NativeSourceClockConfig fastCfg;
        fastCfg.DividerCyclesPerMs = 1;
        fastCfg.MsPerGameMinute = 1;
        NativeSourceClock cal(fastCfg);
        CheckStatus(cal.Initialise(0), Status::Ok, "cal init");
        // Freeze rebases LastTick to Game and zeroes seconds without a minute.
        CheckStatus(cal.Tick(10), Status::Ok, "cal tick");
        CheckStatus(cal.SetFreezeTime(true), Status::Ok, "freeze on");
        const std::uint8_t frozenMinutes = cal.State().Minutes;
        CheckStatus(cal.Tick(20), Status::Ok, "frozen tick");
        Check(cal.State().Minutes == frozenMinutes, "freeze blocks minute");
        Check(cal.State().LastClockTickGameMs == cal.State().GameMs, "freeze rebases LastTick");
        Check(cal.State().Seconds == 0, "freeze seconds 0");
        CheckStatus(cal.SetFreezeTime(false), Status::Ok, "freeze off");
        // Faster clock forces a minute even for a tiny diff and pins LastTick.
        CheckStatus(cal.SetFasterClock(true), Status::Ok, "faster on");
        const std::uint8_t beforeMinutes = cal.State().Minutes;
        CheckStatus(cal.Tick(21), Status::Ok, "faster tick");
        Check(cal.State().Minutes != beforeMinutes ||
                cal.State().LastClockTickGameMs == cal.State().GameMs,
            "faster advances/pins");
        CheckStatus(cal.SetFasterClock(false), Status::Ok, "faster off");
        // Blocked midnight/orange clocks grow Seconds (60 at threshold, more
        // while blocked) without advancing the minute.
        NativeSourceClock blockedExact(msCfg);
        CheckStatus(blockedExact.Initialise(0), Status::Ok, "blocked exact init");
        CheckStatus(blockedExact.SetAlwaysMidnight(true), Status::Ok, "blocked exact midnight");
        CheckStatus(blockedExact.Tick(50), Status::Ok, "blocked 50");
        CheckStatus(blockedExact.Tick(100), Status::Ok, "blocked 100");
        CheckStatus(blockedExact.Tick(1100), Status::Ok, "blocked hitch");
        CheckStatus(blockedExact.Tick(1400), Status::Ok, "blocked 1400");
        CheckStatus(blockedExact.Tick(1700), Status::Ok, "blocked exact 1000");
        Check(blockedExact.State().Seconds == 60, "blocked exact seconds 60");
        Check(blockedExact.State().Minutes == 0, "blocked minute stays 0");
        CheckStatus(blockedExact.Tick(2000), Status::Ok, "blocked past threshold");
        Check(blockedExact.State().Minutes == 0, "blocked still no minute");
        Check(blockedExact.State().Seconds > 60, "blocked seconds grow past 60");
        // Quirky month roll: Jan Day>=31 resets (Jan has 30 owned days here).
        NativeSourceClock month(fastCfg);
        CheckStatus(month.Initialise(0), Status::Ok, "month init");
        std::uint32_t guard = 0;
        while (!(month.State().Month == 2 && month.State().Day == 1)) {
            const std::uint64_t next = month.State().LastObservedTick + 300u;
            CheckStatus(month.Tick(next), Status::Ok, "month drive");
            if (++guard > 200000) {
                Check(false, "january roll took too long");
            }
        }
        Check(month.State().DaysPassed > 0, "month roll counts owned days");
        // February uses the quirky 29-day table entry (28 owned days).
        guard = 0;
        while (!(month.State().Month == 3 && month.State().Day == 1)) {
            const std::uint64_t next = month.State().LastObservedTick + 300u;
            CheckStatus(month.Tick(next), Status::Ok, "february drive");
            if (++guard > 200000) {
                Check(false, "february roll took too long");
            }
        }
        // Year wrap: December rolls to January month 1.
        guard = 0;
        while (!(month.State().Month == 1 && month.State().Day == 1 &&
                 month.State().DaysPassed > 300)) {
            const std::uint64_t next = month.State().LastObservedTick + 300u;
            CheckStatus(month.Tick(next), Status::Ok, "year drive");
            if (++guard > 3000000) {
                Check(false, "year wrap took too long");
            }
        }
        Check(month.State().Month == 1, "year wraps to January");
    }

    // RNG provenance: canonical SeedOnce(1) draws, no clock draw, reseed guard.
    NativeSourceRngProvenance rngBefore{};
    {
        NativeSourceRng rng;
        Check(rng.Readiness() == NativeSourceRngStatus::Unseeded, "rng unseeded");
        Check(!rng.Inspect().Value, "rng unseeded inspect empty");
        Check(rng.Reference().NextRand15().Status == NativeSourceRngStatus::Unseeded,
            "rng unseeded draw fails");
        Check(rng.SeedOnce(1) == NativeSourceRngStatus::Ready, "rng seed once");
        Check(rng.Inspect().Value == NativeSourceRngProvenance{1, 1, 0}, "rng seed provenance");
        const auto ref = rng.Reference();
        constexpr std::uint16_t kCanonical[5] = {41, 18467, 6334, 26500, 19169};
        for (unsigned i = 0; i < 5; ++i) {
            const auto draw = ref.NextRand15();
            char message[64];
            std::snprintf(message, sizeof(message), "rng canonical %u", i);
            Check(draw.Status == NativeSourceRngStatus::Ready, message);
            Check(draw.Value && *draw.Value == kCanonical[i], message);
            // Clock ticks never draw: provenance after each draw plus clock
            // activity must equal the draw-counted provenance.
            NativeSourceClock tickCheck(msCfg);
            CheckStatus(tickCheck.Initialise(0), Status::Ok, "rng guard clock init");
            CheckStatus(tickCheck.Tick(50), Status::Ok, "rng guard clock tick");
            const auto inspect = rng.Inspect();
            Check(inspect.Status == NativeSourceRngStatus::Ready, "rng still ready");
            Check(inspect.Value && inspect.Value->DrawCount == i + 1, "rng draw count exact");
            Check(inspect.Value && inspect.Value->Seed == 1, "rng seed stable");
        }
        rngBefore = *rng.Inspect().Value;
        Check(rng.SeedOnce(2) == NativeSourceRngStatus::AlreadySeeded, "rng reseed rejected");
        Check(rng.Inspect().Value == rngBefore, "rng reseed keeps provenance");
    }

    // Repeated explicit schedule is semantically exact (member ==, no memcmp
    // padding, no timestamps); no frame-partition invariance is claimed.
    {
        const auto run = [&] {
            NativeSourceClock clock(msCfg);
            NativeSourceRng rng;
            Check(rng.SeedOnce(1) == NativeSourceRngStatus::Ready, "trace seed");
            std::vector<std::tuple<NativeSourceClockState, NativeSourceClockLastFrame,
                NativeSourceRngProvenance>> trace;
            const auto frame = [&](std::uint64_t tick) {
                const auto before = rng.Inspect();
                CheckStatus(clock.Tick(tick), Status::Ok, "repeat frame");
                Check(rng.Inspect().Value == before.Value, "clock leaves complete RNG provenance intact");
                Check(rng.Reference().NextRand15().Status == NativeSourceRngStatus::Ready, "scheduled draw");
                trace.emplace_back(clock.State(), clock.LastFrame(), *rng.Inspect().Value);
            };
            CheckStatus(clock.Initialise(0), Status::Ok, "repeat init");
            frame(50);
            frame(100);
            CheckStatus(clock.SetTimeScale(0.5f), Status::Ok, "repeat scale");
            frame(300);
            CheckStatus(clock.SetTimeScale(1.0f), Status::Ok, "repeat scale back");
            CheckStatus(clock.SetUserPause(true), Status::Ok, "repeat pause");
            frame(350);
            CheckStatus(clock.SetUserPause(false), Status::Ok, "repeat unpause");
            frame(400);
            CheckStatus(clock.SetGameClock(8, 15, 0), Status::Ok, "repeat setclock");
            CheckStatus(clock.Suspend(400), Status::Ok, "repeat suspend");
            CheckStatus(clock.Resume(900), Status::Ok, "repeat resume");
            frame(950);
            return trace;
        };
        Check(run() == run(), "every repeated frame/calendar/RNG trace agrees");
    }

    // Other unsigned domains wrap even while the clipped/session domain is safe.
    {
        NativeSourceClock wrapped(msCfg);
        CheckStatus(wrapped.Initialise(0), Status::Ok, "wrap init");
        CheckStatus(wrapped.Tick(4294967040ull), Status::Ok, "largest tested finite delta");
        CheckStatus(wrapped.Tick(4294968040ull), Status::Ok, "nonclipped/pause wrap");
        Check(wrapped.State().GameMs == 600 && wrapped.State().NonClippedMs == 745 &&
            wrapped.State().PauseMs == 745, "wrap does not use clipped-domain arithmetic");
    }

    // Game-horizon overflow: bounded ~14M capped increments, no trace vector.
    {
        NativeSourceClock horizon(msCfg);
        CheckStatus(horizon.Initialise(0), Status::Ok, "horizon init");
        std::uint64_t tick = 0;
        // 14,316,557 * 300ms = 4,294,967,100 (just under 2^32-1).
        constexpr std::uint64_t kFullSteps = 14316557ull;
        for (std::uint64_t i = 0; i < kFullSteps; ++i) {
            tick += 300ull;
            const Status status = horizon.Tick(tick);
            if (status != Status::Ok) {
                std::fprintf(stderr, "native-source-clock FAIL: horizon step %llu status %d\n",
                    static_cast<unsigned long long>(i), static_cast<int>(status));
                return 1;
            }
        }
        Check(horizon.State().GameMs == 4294967100u, "horizon game near max");
        const auto before = horizon.State();
        const auto beforeTrace = horizon.LastFrame();
        CheckStatus(horizon.Tick(tick + 300ull), Status::Overflow, "horizon overflow rejected");
        CheckAtomicUnchanged(horizon, before, beforeTrace, "horizon overflow atomic");
    }

    // Real session seam: LoadMain, feed GameMs, 00C0 setter observation only.
    {
        NativeScriptSession session;
        DummyServices services;
        std::string error;
        Check(session.LoadMain(gameDir, error), error.c_str());
        NativeSourceClock feed(msCfg);
        CheckStatus(feed.Initialise(0), Status::Ok, "feed init");
        CheckStatus(feed.Tick(123), Status::Ok, "feed tick 123");
        Check(feed.State().GameMs == 123, "feed game 123");
        const auto clockBefore = session.State().Clock;
        Check(session.AdvanceTime(feed.State().GameMs, error), error.c_str());
        Check(session.State().TimeMs == 123, "session fed GameMs");
        const auto beforePause = session.State();
        CheckStatus(feed.SetUserPause(true), Status::Ok, "session user pause");
        CheckStatus(feed.Tick(1123), Status::Ok, "session paused tick");
        Check(session.AdvanceTime(feed.State().GameMs, error), error.c_str());
        Check(session.State() == beforePause, "real pause window preserves session time/state");
        CheckStatus(feed.SetUserPause(false), Status::Ok, "session user resume");
        CheckStatus(feed.SetCodePause(true), Status::Ok, "session code pause");
        CheckStatus(feed.Tick(2123), Status::Ok, "session code-paused tick");
        Check(session.AdvanceTime(feed.State().GameMs, error), error.c_str());
        Check(session.State() == beforePause, "code pause preserves session time/state");
        CheckStatus(feed.SetCodePause(false), Status::Ok, "session code resume");
        Check(session.State().Clock == clockBefore, "AdvanceTime leaves 00C0 clock");
        Check(session.State().Clock.Revision == 0, "no automatic 00C0 revision");
        // Real pure prefix reaches the 00C0 setter (8:00, revision 1).
        for (unsigned i = 0; i < 14; ++i) {
            const auto result = session.Step(services);
            Check(result.Status == NativeScriptStatus::Advanced, "real pure prefix advances");
        }
        Check(session.State().Clock.Hours == 8 && session.State().Clock.Minutes == 0,
            "real 00C0 8:00");
        Check(session.State().Clock.Revision == 1, "real 00C0 revision 1");
        Check(session.State().Clock.LastTickMs == 123, "real 00C0 lastTick fed time");
        // Explicit setter seam: mirror the session snapshot into the owned
        // clock; the session itself is never rewritten automatically.
        const auto snapshot = session.State().Clock;
        NativeSourceClock seam(msCfg);
        CheckStatus(seam.Initialise(0), Status::Ok, "seam init");
        CheckStatus(seam.Tick(123), Status::Ok, "seam tick");
        CheckStatus(seam.SetGameClock(snapshot.Hours, snapshot.Minutes, 0),
            Status::Ok, "seam explicit setter");
        Check(seam.State().CalendarRevision == 1, "seam setter revision 1");
        Check(seam.State().Hours == snapshot.Hours, "seam mirrors hours");
        Check(session.State().Clock == snapshot, "no automatic session rewrite");
        Check(session.State().Clock.Revision == 1, "session revision still 1");
    }

    std::printf("native-source-clock-ok checks=%zu profile=Timer40-78,86-114,186-231/"
                "Clock27-86,122-162,212-224/Game-unpaused initial-NCStep-owned-0/"
                "overflow=game-horizon-reject/nc-pause-wrap/x87-not-claimed\n",
        s_Checks);
    return 0;
}
